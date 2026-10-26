#include "../../core/data_document.h"
#include "data_parser_internal.h"

#ifndef QUIREWISE_STANDALONE_DATA_PARSER
#include "../../core/format_registry.h"
#include "../../core/writer_registry.h"
#else
#include "quirewise/status.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QW_VCARD_MAX_INPUT ((size_t)256u * 1024u * 1024u)
#define QW_VCARD_MAX_OUTPUT QW_DATA_DOCUMENT_MAX_MEMORY
#define QW_VCARD_MAX_LINE ((size_t)16u * 1024u * 1024u)

#define QW_VCARD_ROOT_KEY "$vcard"
#define QW_VCARD_VERSION_KEY "$version"
#define QW_VCARD_PROPERTIES_KEY "$properties"
#define QW_VCARD_GROUP_KEY "$group"
#define QW_VCARD_NAME_KEY "$name"
#define QW_VCARD_PARAMS_KEY "$params"
#define QW_VCARD_VALUE_KEY "$value"

typedef struct QwVCardBuffer {
    char *data;
    size_t size;
    size_t capacity;
} QwVCardBuffer;

typedef struct QwVCardOutput {
    char *data;
    size_t size;
    size_t capacity;
    int error;
} QwVCardOutput;

typedef struct QwVCardParser {
    const uint8_t *input;
    size_t size;
    QwDataDocument *document;
    QwDataValue *cards;
    QwDataValue current_card;
    QwDataValue *current_properties;
    int in_card;
    int expect_version;
    int version_21;
    int has_fn;
    int error;
} QwVCardParser;

typedef struct QwVCardLineHeader {
    size_t group_start;
    size_t group_size;
    size_t name_start;
    size_t name_size;
    size_t params_start;
    size_t header_size;
    size_t value_start;
    size_t param_count;
} QwVCardLineHeader;

static int vcard_utf8_decode(const uint8_t *data, size_t size,
                             size_t *position, uint32_t *codepoint)
{
    size_t i;
    uint8_t first;
    uint32_t value;
    size_t count;
    if (!data || !position || !codepoint || *position >= size) return 0;
    i = *position;
    first = data[i];
    if (first < 0x80u) { value = first; count = 1u; }
    else if (first >= 0xc2u && first <= 0xdfu) { value = first & 0x1fu; count = 2u; }
    else if (first >= 0xe0u && first <= 0xefu) { value = first & 0x0fu; count = 3u; }
    else if (first >= 0xf0u && first <= 0xf4u) { value = first & 0x07u; count = 4u; }
    else return 0;
    if (count > size - i) return 0;
    if (count > 1u) {
        size_t j;
        for (j = 1u; j < count; ++j) {
            if ((data[i + j] & 0xc0u) != 0x80u) return 0;
            value = (value << 6) | (data[i + j] & 0x3fu);
        }
        if ((first == 0xe0u && data[i + 1u] < 0xa0u) ||
            (first == 0xedu && data[i + 1u] >= 0xa0u) ||
            (first == 0xf0u && data[i + 1u] < 0x90u) ||
            (first == 0xf4u && data[i + 1u] >= 0x90u)) return 0;
    }
    *position = i + count;
    *codepoint = value;
    return 1;
}

static int vcard_content_valid(const char *text, size_t size)
{
    size_t position = 0u;
    if (!text && size) return 0;
    while (position < size) {
        uint32_t codepoint;
        if (!vcard_utf8_decode((const uint8_t *)text, size,
                               &position, &codepoint)) return 0;
        if ((codepoint < 0x20u && codepoint != '\t') ||
            (codepoint >= 0x7fu && codepoint <= 0x9fu)) return 0;
    }
    return 1;
}

static int vcard_ascii_token(const char *text, size_t size)
{
    size_t i;
    if (!text || !size) return 0;
    for (i = 0u; i < size; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-')) return 0;
    }
    return 1;
}

static int vcard_ascii_equal(const char *left, size_t left_size,
                             const char *right)
{
    size_t i;
    size_t right_size = strlen(right);
    if (left_size != right_size) return 0;
    for (i = 0u; i < left_size; ++i) {
        unsigned char a = (unsigned char)left[i];
        unsigned char b = (unsigned char)right[i];
        if (a >= 'a' && a <= 'z') a = (unsigned char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (unsigned char)(b - 'a' + 'A');
        if (a != b) return 0;
    }
    return 1;
}

static void *vcard_tree_allocate(QwVCardParser *parser, size_t size)
{
    void *memory;
    if (!size) size = 1u;
    if (!parser->document ||
        parser->document->allocation_bytes > QW_DATA_DOCUMENT_MAX_MEMORY ||
        size > QW_DATA_DOCUMENT_MAX_MEMORY - parser->document->allocation_bytes) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return NULL;
    }
    memory = malloc(size);
    if (!memory) { parser->error = QW_STATUS_ERR_OUT_OF_MEMORY; return NULL; }
    parser->document->allocation_bytes += size;
    return memory;
}

static void *vcard_tree_reallocate(QwVCardParser *parser, void *memory,
                                   size_t old_size, size_t new_size)
{
    void *grown;
    if (!parser->document || old_size > parser->document->allocation_bytes ||
        parser->document->allocation_bytes > QW_DATA_DOCUMENT_MAX_MEMORY ||
        new_size > QW_DATA_DOCUMENT_MAX_MEMORY -
                       (parser->document->allocation_bytes - old_size)) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return NULL;
    }
    grown = realloc(memory, new_size ? new_size : 1u);
    if (!grown) { parser->error = QW_STATUS_ERR_OUT_OF_MEMORY; return NULL; }
    parser->document->allocation_bytes =
        parser->document->allocation_bytes - old_size + new_size;
    return grown;
}

static void vcard_tree_release(QwVCardParser *parser, void *memory, size_t size)
{
    if (!memory) return;
    free(memory);
    if (parser->document && size <= parser->document->allocation_bytes)
        parser->document->allocation_bytes -= size;
}

static int vcard_new_value(QwVCardParser *parser, QwDataValue *value,
                           QwDataValueKind kind, size_t depth)
{
    if (!value || depth > QW_DATA_DOCUMENT_MAX_DEPTH ||
        parser->document->node_count >= QW_DATA_DOCUMENT_MAX_NODES) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    ++parser->document->node_count;
    memset(value, 0, sizeof(*value));
    value->kind = kind;
    return 1;
}

static int vcard_set_string(QwVCardParser *parser, QwDataValue *value,
                            const char *text, size_t size, size_t depth)
{
    char *copy;
    if (size == SIZE_MAX || !vcard_new_value(parser, value,
            QW_DATA_VALUE_STRING, depth)) return 0;
    copy = (char *)vcard_tree_allocate(parser, size + 1u);
    if (!copy) return 0;
    if (size) memcpy(copy, text, size);
    copy[size] = '\0';
    value->value.text.data = copy;
    value->value.text.size = size;
    return 1;
}

static int vcard_buffer_append(QwVCardBuffer *buffer, const char *text,
                               size_t size)
{
    size_t needed;
    size_t capacity;
    char *grown;
    if (!buffer || (!text && size) || size > SIZE_MAX - buffer->size - 1u ||
        buffer->size > QW_VCARD_MAX_LINE ||
        size > QW_VCARD_MAX_LINE - buffer->size) return 0;
    needed = buffer->size + size + 1u;
    if (needed > buffer->capacity) {
        capacity = buffer->capacity ? buffer->capacity : 128u;
        while (capacity < needed) {
            if (capacity > (QW_VCARD_MAX_LINE + 1u) / 2u) {
                capacity = QW_VCARD_MAX_LINE + 1u;
                break;
            }
            capacity *= 2u;
        }
        if (capacity < needed) return 0;
        grown = (char *)realloc(buffer->data, capacity);
        if (!grown) return 0;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    if (size) memcpy(buffer->data + buffer->size, text, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
    return 1;
}

static void vcard_buffer_reset(QwVCardBuffer *buffer)
{
    if (!buffer) return;
    buffer->size = 0u;
    if (buffer->data) buffer->data[0] = '\0';
}

static void vcard_buffer_dispose(QwVCardBuffer *buffer)
{
    if (!buffer) return;
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static int vcard_array_reserve(QwVCardParser *parser, QwDataValue *array,
                               size_t needed)
{
    size_t capacity;
    QwDataValue *grown;
    if (needed <= array->value.array.capacity) return 1;
    if (needed > QW_DATA_DOCUMENT_MAX_NODES ||
        needed > SIZE_MAX / sizeof(*grown)) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    capacity = array->value.array.capacity ? array->value.array.capacity : 8u;
    while (capacity < needed) {
        if (capacity > QW_DATA_DOCUMENT_MAX_NODES / 2u) {
            capacity = QW_DATA_DOCUMENT_MAX_NODES;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < needed) { parser->error = QW_STATUS_ERR_RESOURCE_LIMIT; return 0; }
    grown = (QwDataValue *)vcard_tree_reallocate(parser,
        array->value.array.items,
        array->value.array.capacity * sizeof(*grown),
        capacity * sizeof(*grown));
    if (!grown) return 0;
    array->value.array.items = grown;
    array->value.array.capacity = capacity;
    return 1;
}

static int vcard_object_reserve(QwVCardParser *parser, QwDataValue *object,
                                size_t needed)
{
    size_t capacity;
    QwDataMember *grown;
    if (needed <= object->value.object.capacity) return 1;
    if (needed > QW_DATA_DOCUMENT_MAX_NODES ||
        needed > SIZE_MAX / sizeof(*grown)) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    capacity = object->value.object.capacity ? object->value.object.capacity : 4u;
    while (capacity < needed) {
        if (capacity > QW_DATA_DOCUMENT_MAX_NODES / 2u) {
            capacity = QW_DATA_DOCUMENT_MAX_NODES;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < needed) { parser->error = QW_STATUS_ERR_RESOURCE_LIMIT; return 0; }
    grown = (QwDataMember *)vcard_tree_reallocate(parser,
        object->value.object.items,
        object->value.object.capacity * sizeof(*grown),
        capacity * sizeof(*grown));
    if (!grown) return 0;
    object->value.object.items = grown;
    object->value.object.capacity = capacity;
    return 1;
}

static int vcard_array_append(QwVCardParser *parser, QwDataValue *array,
                              QwDataValue *value)
{
    if (!array || array->kind != QW_DATA_VALUE_ARRAY || !value ||
        array->value.array.count >= QW_DATA_DOCUMENT_MAX_NODES ||
        !vcard_array_reserve(parser, array, array->value.array.count + 1u))
        return 0;
    array->value.array.items[array->value.array.count++] = *value;
    memset(value, 0, sizeof(*value));
    return 1;
}

static const QwDataValue *vcard_find_member(const QwDataValue *object,
                                            const char *key, size_t key_size)
{
    size_t i;
    const QwDataValue *found = NULL;
    if (!object || object->kind != QW_DATA_VALUE_OBJECT) return NULL;
    for (i = 0u; i < object->value.object.count; ++i) {
        const QwDataMember *member = &object->value.object.items[i];
        if (member->key_size == key_size && memcmp(member->key, key, key_size) == 0) {
            if (found) return NULL;
            found = &member->value;
        }
    }
    return found;
}

static int vcard_object_add(QwVCardParser *parser, QwDataValue *object,
                            const char *key, size_t key_size,
                            QwDataValue *value)
{
    char *key_copy;
    size_t i;
    QwDataMember *member;
    if (!object || object->kind != QW_DATA_VALUE_OBJECT || !key || !value) return 0;
    for (i = 0u; i < object->value.object.count; ++i) {
        QwDataMember *existing = &object->value.object.items[i];
        if (existing->key_size == key_size &&
            memcmp(existing->key, key, key_size) == 0) {
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
    }
    if (object->value.object.count >= QW_DATA_DOCUMENT_MAX_NODES ||
        !vcard_object_reserve(parser, object, object->value.object.count + 1u)) return 0;
    if (key_size == SIZE_MAX) { parser->error = QW_STATUS_ERR_RESOURCE_LIMIT; return 0; }
    key_copy = (char *)vcard_tree_allocate(parser, key_size + 1u);
    if (!key_copy) return 0;
    memcpy(key_copy, key, key_size);
    key_copy[key_size] = '\0';
    member = &object->value.object.items[object->value.object.count++];
    member->key = key_copy;
    member->key_size = key_size;
    member->value = *value;
    memset(value, 0, sizeof(*value));
    return 1;
}

static int vcard_object_add_string(QwVCardParser *parser, QwDataValue *object,
                                   const char *key, size_t key_size,
                                   const char *text, size_t text_size,
                                   size_t depth)
{
    QwDataValue value;
    memset(&value, 0, sizeof(value));
    if (!vcard_set_string(parser, &value, text, text_size, depth) ||
        !vcard_object_add(parser, object, key, key_size, &value)) {
        qw_data_value_dispose(&value);
        return 0;
    }
    return 1;
}

static int vcard_quote_aware_colon(const char *line, size_t size, size_t *colon)
{
    size_t i;
    int quoted = 0;
    int escaped = 0;
    for (i = 0u; i < size; ++i) {
        char ch = line[i];
        if (quoted && ch == '\\' && !escaped) { escaped = 1; continue; }
        if (ch == '"' && !escaped) quoted = !quoted;
        else if (ch == ':' && !quoted) { *colon = i; return 1; }
        escaped = 0;
    }
    return 0;
}

static int vcard_parameter_valid(const char *text, size_t size)
{
    size_t i;
    size_t equals = SIZE_MAX;
    int quoted = 0;
    int escaped = 0;
    if (!text || !size) return 0;
    for (i = 0u; i < size; ++i) {
        char ch = text[i];
        if (quoted && ch == '\\' && !escaped) { escaped = 1; continue; }
        if (ch == '"' && !escaped) quoted = !quoted;
        else if (ch == '=' && !quoted && equals == SIZE_MAX) equals = i;
        escaped = 0;
    }
    if (quoted || equals == SIZE_MAX || equals == 0u || equals + 1u >= size ||
        !vcard_ascii_token(text, equals) || !vcard_content_valid(text, size)) return 0;
    return 1;
}

static int vcard_parameter_valid_21(const char *text, size_t size)
{
    size_t start = 0u;
    size_t i;
    if (vcard_parameter_valid(text, size)) return 1;
    if (!text || !size || !vcard_content_valid(text, size)) return 0;
    for (i = 0u; i <= size; ++i) {
        if (i == size || text[i] == ',') {
            if (!vcard_ascii_token(text + start, i - start)) return 0;
            start = i + 1u;
        }
    }
    return 1;
}

static int vcard_parse_header(const char *line, size_t size,
                              QwVCardLineHeader *header,
                              int allow_legacy_parameters)
{
    size_t colon;
    size_t primary_end;
    size_t i;
    size_t dot = SIZE_MAX;
    int quoted = 0;
    int escaped = 0;
    memset(header, 0, sizeof(*header));
    if (!vcard_quote_aware_colon(line, size, &colon)) return 0;
    for (i = 0u; i < colon; ++i) {
        char ch = line[i];
        if (quoted && ch == '\\' && !escaped) { escaped = 1; continue; }
        if (ch == '"' && !escaped) quoted = !quoted;
        else if (ch == ';' && !quoted) break;
        escaped = 0;
    }
    primary_end = i;
    for (i = 0u; i < primary_end; ++i) {
        if (line[i] == '.') {
            if (dot != SIZE_MAX) return 0;
            dot = i;
        }
    }
    if (dot != SIZE_MAX) {
        if (!vcard_ascii_token(line, dot) ||
            !vcard_ascii_token(line + dot + 1u, primary_end - dot - 1u)) return 0;
        header->group_start = 0u;
        header->group_size = dot;
        header->name_start = dot + 1u;
        header->name_size = primary_end - dot - 1u;
    } else {
        if (!vcard_ascii_token(line, primary_end)) return 0;
        header->name_start = 0u;
        header->name_size = primary_end;
    }
    header->header_size = colon;
    header->value_start = colon + 1u;
    header->params_start = primary_end < colon ? primary_end + 1u : colon;
    if (header->params_start == colon) return 1;
    i = header->params_start;
    while (i < colon) {
        size_t start = i;
        quoted = 0;
        escaped = 0;
        while (i < colon) {
            char ch = line[i];
            if (quoted && ch == '\\' && !escaped) { escaped = 1; ++i; continue; }
            if (ch == '"' && !escaped) quoted = !quoted;
            else if (ch == ';' && !quoted) break;
            escaped = 0;
            ++i;
        }
        if (allow_legacy_parameters
                ? !vcard_parameter_valid_21(line + start, i - start)
                : !vcard_parameter_valid(line + start, i - start)) return 0;
        ++header->param_count;
        if (i < colon) ++i;
    }
    return !quoted;
}

static int vcard_header_has_quoted_printable(
    const char *line, const QwVCardLineHeader *header)
{
    size_t i = header->params_start;
    while (i < header->header_size) {
        size_t start = i;
        size_t end;
        size_t equals;
        while (i < header->header_size && line[i] != ';') ++i;
        end = i;
        for (equals = start; equals < end && line[equals] != '='; ++equals) {}
        {
            size_t value_start = equals < end ? equals + 1u : end;
            size_t value_size = end - value_start;
            if (value_size >= 2u && line[value_start] == '"' &&
                line[end - 1u] == '"') {
                ++value_start;
                value_size -= 2u;
            }
        if (equals < end &&
            vcard_ascii_equal(line + start, equals - start, "ENCODING") &&
            vcard_ascii_equal(line + value_start, value_size,
                              "QUOTED-PRINTABLE")) return 1;
        }
        if (i < header->header_size) ++i;
    }
    return 0;
}

enum {
    VCARD_CHARSET_NONE,
    VCARD_CHARSET_ISO_8859_1,
    VCARD_CHARSET_WINDOWS_1252
};

static int vcard_legacy_charset_parameter(const char *text, size_t size)
{
    size_t equals = 0u;
    size_t value_start;
    size_t value_size;
    while (equals < size && text[equals] != '=') ++equals;
    if (equals == size || !vcard_ascii_equal(text, equals, "CHARSET"))
        return VCARD_CHARSET_NONE;
    value_start = equals + 1u;
    value_size = size - value_start;
    if (value_size >= 2u && text[value_start] == '"' &&
        text[size - 1u] == '"') {
        ++value_start;
        value_size -= 2u;
    }
    if (vcard_ascii_equal(text + value_start, value_size, "ISO-8859-1") ||
        vcard_ascii_equal(text + value_start, value_size, "LATIN1"))
        return VCARD_CHARSET_ISO_8859_1;
    if (vcard_ascii_equal(text + value_start, value_size, "WINDOWS-1252") ||
        vcard_ascii_equal(text + value_start, value_size, "CP1252"))
        return VCARD_CHARSET_WINDOWS_1252;
    return VCARD_CHARSET_NONE;
}

static int vcard_header_legacy_charset(const char *line,
                                       const QwVCardLineHeader *header)
{
    size_t i = header->params_start;
    int charset = VCARD_CHARSET_NONE;
    while (i < header->header_size) {
        size_t start = i;
        size_t end;
        int candidate;
        while (i < header->header_size && line[i] != ';') ++i;
        end = i;
        candidate = vcard_legacy_charset_parameter(line + start, end - start);
        if (candidate != VCARD_CHARSET_NONE) {
            if (charset != VCARD_CHARSET_NONE && charset != candidate) return -1;
            charset = candidate;
        }
        if (i < header->header_size) ++i;
    }
    return charset;
}

static int vcard_append_utf8_codepoint(QwVCardBuffer *buffer,
                                      uint32_t codepoint)
{
    char encoded[4];
    size_t size;
    if (codepoint <= 0x7fu) {
        encoded[0] = (char)codepoint;
        size = 1u;
    } else if (codepoint <= 0x7ffu) {
        encoded[0] = (char)(0xc0u | (codepoint >> 6u));
        encoded[1] = (char)(0x80u | (codepoint & 0x3fu));
        size = 2u;
    } else {
        encoded[0] = (char)(0xe0u | (codepoint >> 12u));
        encoded[1] = (char)(0x80u | ((codepoint >> 6u) & 0x3fu));
        encoded[2] = (char)(0x80u | (codepoint & 0x3fu));
        size = 3u;
    }
    if (buffer->size > QW_VCARD_MAX_LINE ||
        size > QW_VCARD_MAX_LINE - buffer->size)
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    return vcard_buffer_append(buffer, encoded, size)
        ? QW_STATUS_OK : QW_STATUS_ERR_OUT_OF_MEMORY;
}

static int vcard_decode_legacy_value(const char *text, size_t size,
                                     int charset, QwVCardBuffer *decoded)
{
    static const uint16_t windows_1252_high[32] = {
        0x20acu, 0u, 0x201au, 0x0192u, 0x201eu, 0x2026u, 0x2020u, 0x2021u,
        0x02c6u, 0x2030u, 0x0160u, 0x2039u, 0x0152u, 0u, 0x017du, 0u,
        0u, 0x2018u, 0x2019u, 0x201cu, 0x201du, 0x2022u, 0x2013u, 0x2014u,
        0x02dcu, 0x2122u, 0x0161u, 0x203au, 0x0153u, 0u, 0x017eu, 0x0178u
    };
    size_t i;
    for (i = 0u; i < size; ++i) {
        uint32_t codepoint = (uint8_t)text[i];
        int rc;
        if (charset == VCARD_CHARSET_WINDOWS_1252 &&
            codepoint >= 0x80u && codepoint <= 0x9fu) {
            codepoint = windows_1252_high[codepoint - 0x80u];
            if (!codepoint) return QW_STATUS_ERR_FORMAT;
        }
        rc = vcard_append_utf8_codepoint(decoded, codepoint);
        if (rc != QW_STATUS_OK) return rc;
    }
    return QW_STATUS_OK;
}

static int vcard_qp_soft_break(const QwVCardParser *parser,
                               const QwVCardBuffer *line)
{
    QwVCardLineHeader header;
    if (!parser->version_21 || !line || !line->size ||
        line->data[line->size - 1u] != '=' ||
        !vcard_parse_header(line->data, line->size, &header, 1)) return 0;
    return vcard_header_has_quoted_printable(line->data, &header);
}

static int vcard_append_parameters(QwVCardParser *parser,
                                   const char *line,
                                   const QwVCardLineHeader *header,
                                   QwDataValue *parameters,
                                   size_t depth,
                                   int normalize_legacy_charset)
{
    size_t i;
    if (!vcard_new_value(parser, parameters, QW_DATA_VALUE_ARRAY, depth)) return 0;
    if (header->params_start == header->header_size) return 1;
    i = header->params_start;
    while (i < header->header_size) {
        size_t start = i;
        const char *parameter_text;
        size_t parameter_size;
        QwDataValue parameter;
        int quoted = 0;
        int escaped = 0;
        while (i < header->header_size) {
            char ch = line[i];
            if (quoted && ch == '\\' && !escaped) { escaped = 1; ++i; continue; }
            if (ch == '"' && !escaped) quoted = !quoted;
            else if (ch == ';' && !quoted) break;
            escaped = 0;
            ++i;
        }
        parameter_text = line + start;
        parameter_size = i - start;
        if (normalize_legacy_charset &&
            vcard_legacy_charset_parameter(parameter_text, parameter_size) !=
                VCARD_CHARSET_NONE) {
            parameter_text = "CHARSET=UTF-8";
            parameter_size = 13u;
        }
        memset(&parameter, 0, sizeof(parameter));
        if (!vcard_set_string(parser, &parameter, parameter_text,
                              parameter_size, depth + 1u) ||
            !vcard_array_append(parser, parameters, &parameter)) {
            qw_data_value_dispose(&parameter);
            return 0;
        }
        if (i < header->header_size) ++i;
    }
    return 1;
}

static int vcard_property_append(QwVCardParser *parser, const char *line,
                                 size_t size, const QwVCardLineHeader *header)
{
    QwDataValue property = {0};
    QwDataValue parameters = {0};
    QwVCardBuffer decoded = {0};
    const char *value_text = line + header->value_start;
    size_t value_size = size - header->value_start;
    int charset = VCARD_CHARSET_NONE;
    int normalize_charset = 0;
    int rc;

    if (parser->version_21 &&
        !vcard_header_has_quoted_printable(line, header)) {
        charset = vcard_header_legacy_charset(line, header);
        if (charset < 0) {
            parser->error = QW_STATUS_ERR_FORMAT;
            goto failure;
        }
        if (charset != VCARD_CHARSET_NONE) {
            rc = vcard_decode_legacy_value(value_text, value_size, charset,
                                           &decoded);
            if (rc != QW_STATUS_OK) {
                parser->error = rc;
                goto failure;
            }
            value_text = decoded.data;
            value_size = decoded.size;
            normalize_charset = 1;
        }
    }
    if (!vcard_content_valid(value_text, value_size)) {
        parser->error = QW_STATUS_ERR_FORMAT;
        goto failure;
    }
    if (!vcard_new_value(parser, &property, QW_DATA_VALUE_OBJECT, 4u) ||
        !vcard_object_add_string(parser, &property, QW_VCARD_GROUP_KEY,
            sizeof(QW_VCARD_GROUP_KEY) - 1u,
            line + header->group_start, header->group_size, 5u) ||
        !vcard_object_add_string(parser, &property, QW_VCARD_NAME_KEY,
            sizeof(QW_VCARD_NAME_KEY) - 1u,
            line + header->name_start, header->name_size, 5u) ||
        !vcard_append_parameters(parser, line, header, &parameters, 5u,
                                 normalize_charset) ||
        !vcard_object_add(parser, &property, QW_VCARD_PARAMS_KEY,
            sizeof(QW_VCARD_PARAMS_KEY) - 1u, &parameters) ||
        !vcard_object_add_string(parser, &property, QW_VCARD_VALUE_KEY,
            sizeof(QW_VCARD_VALUE_KEY) - 1u,
            value_text, value_size, 5u) ||
        !vcard_array_append(parser, parser->current_properties, &property)) {
        goto failure;
    }
    vcard_buffer_dispose(&decoded);
    if (vcard_ascii_equal(line + header->name_start,
                          header->name_size, "FN")) parser->has_fn = 1;
    return 1;

failure:
    vcard_buffer_dispose(&decoded);
    qw_data_value_dispose(&property);
    qw_data_value_dispose(&parameters);
    return 0;
}

static int vcard_start_card(QwVCardParser *parser, const char *version,
                            size_t version_size)
{
    QwDataValue version_value;
    QwDataValue properties;
    memset(&version_value, 0, sizeof(version_value));
    memset(&properties, 0, sizeof(properties));
    if (!vcard_new_value(parser, &parser->current_card,
                         QW_DATA_VALUE_OBJECT, 2u) ||
        !vcard_set_string(parser, &version_value, version, version_size, 3u) ||
        !vcard_object_add(parser, &parser->current_card, QW_VCARD_VERSION_KEY,
            sizeof(QW_VCARD_VERSION_KEY) - 1u, &version_value) ||
        !vcard_new_value(parser, &properties, QW_DATA_VALUE_ARRAY, 3u) ||
        !vcard_object_add(parser, &parser->current_card, QW_VCARD_PROPERTIES_KEY,
            sizeof(QW_VCARD_PROPERTIES_KEY) - 1u, &properties)) {
        qw_data_value_dispose(&version_value);
        qw_data_value_dispose(&properties);
        qw_data_value_dispose(&parser->current_card);
        return 0;
    }
    parser->current_properties =
        &parser->current_card.value.object.items[1u].value;
    parser->version_21 = version_size == 3u &&
                         memcmp(version, "2.1", 3u) == 0;
    return 1;
}

static int vcard_process_line(QwVCardParser *parser,
                              const char *line, size_t size)
{
    QwVCardLineHeader header;
    size_t value_size;
    int is_begin;
    int is_begin_name;
    int is_end;
    int is_end_name;
    int is_version;
    if (!size || !vcard_parse_header(line, size, &header,
                                      parser->version_21)) {
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    value_size = size - header.value_start;
    is_begin_name = vcard_ascii_equal(line + header.name_start,
                                      header.name_size, "BEGIN");
    is_end_name = vcard_ascii_equal(line + header.name_start,
                                    header.name_size, "END");
    is_begin = header.group_size == 0u && header.param_count == 0u &&
        is_begin_name &&
        vcard_ascii_equal(line + header.value_start, value_size, "VCARD");
    is_end = header.group_size == 0u && header.param_count == 0u &&
        is_end_name &&
        vcard_ascii_equal(line + header.value_start, value_size, "VCARD");
    is_version = header.group_size == 0u && header.param_count == 0u &&
        vcard_ascii_equal(line + header.name_start, header.name_size, "VERSION");
    if ((is_begin_name && !is_begin) || (is_end_name && !is_end)) {
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    if (!parser->in_card) {
        if (!is_begin) { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
        parser->in_card = 1;
        parser->expect_version = 1;
        parser->version_21 = 0;
        parser->has_fn = 0;
        return 1;
    }
    if (parser->expect_version) {
        if (!is_version) { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
        if ((value_size != 3u || memcmp(line + header.value_start, "2.1", 3u) != 0) &&
            (value_size != 3u || memcmp(line + header.value_start, "3.0", 3u) != 0) &&
            (value_size != 3u || memcmp(line + header.value_start, "4.0", 3u) != 0)) {
            parser->error = QW_STATUS_ERR_UNSUPPORTED;
            return 0;
        }
        if (!vcard_start_card(parser, line + header.value_start, value_size)) return 0;
        parser->expect_version = 0;
        return 1;
    }
    if (is_begin || is_version) { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
    if (is_end) {
        if (!parser->has_fn ||
            !vcard_array_append(parser, parser->cards, &parser->current_card)) {
            if (!parser->error) parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        parser->current_properties = NULL;
        parser->in_card = 0;
        return 1;
    }
    return vcard_property_append(parser, line, size, &header);
}

static int vcard_parse_document(const void *input, size_t size,
                                QwInputFormat input_format,
                                QwDataDocument *out_document)
{
    QwVCardParser parser;
    QwVCardBuffer logical_line;
    QwDataValue root_cards;
    size_t position = 0u;
    int have_logical_line = 0;
    int rc = QW_STATUS_ERR_FORMAT;
    memset(&parser, 0, sizeof(parser));
    memset(&logical_line, 0, sizeof(logical_line));
    memset(&root_cards, 0, sizeof(root_cards));
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    qw_data_document_init(out_document, input_format);
    if (!input || !size) {
        qw_data_document_dispose(out_document);
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (size > QW_VCARD_MAX_INPUT) {
        qw_data_document_dispose(out_document);
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    parser.input = (const uint8_t *)input;
    parser.size = size;
    parser.document = out_document;
    if (size >= 3u && parser.input[0] == 0xefu && parser.input[1] == 0xbbu &&
        parser.input[2] == 0xbfu) position = 3u;
    if (!vcard_new_value(&parser, &out_document->root,
                         QW_DATA_VALUE_OBJECT, 0u) ||
        !vcard_new_value(&parser, &root_cards, QW_DATA_VALUE_ARRAY, 1u) ||
        !vcard_object_add(&parser, &out_document->root, QW_VCARD_ROOT_KEY,
            sizeof(QW_VCARD_ROOT_KEY) - 1u, &root_cards)) goto failure;
    parser.cards = &out_document->root.value.object.items[0u].value;
    while (position < size) {
        size_t start = position;
        size_t length;
        while (position < size && parser.input[position] != '\r' &&
               parser.input[position] != '\n') ++position;
        length = position - start;
        if (position < size && parser.input[position] == '\r') {
            if (position + 1u >= size || parser.input[position + 1u] != '\n') {
                parser.error = QW_STATUS_ERR_FORMAT;
                goto failure;
            }
            position += 2u;
        } else if (position < size) {
            ++position;
        }
        if (have_logical_line && vcard_qp_soft_break(&parser, &logical_line)) {
            size_t continuation_start = start;
            size_t continuation_size = length;
            --logical_line.size;
            logical_line.data[logical_line.size] = '\0';
            if (continuation_size &&
                (parser.input[continuation_start] == ' ' ||
                 parser.input[continuation_start] == '\t')) {
                ++continuation_start;
                --continuation_size;
            }
            if (!vcard_buffer_append(&logical_line,
                    (const char *)parser.input + continuation_start,
                    continuation_size)) {
                parser.error = QW_STATUS_ERR_RESOURCE_LIMIT;
                goto failure;
            }
        } else if (length && (parser.input[start] == ' ' || parser.input[start] == '\t')) {
            if (!have_logical_line ||
                !vcard_buffer_append(&logical_line,
                    (const char *)parser.input + start + 1u, length - 1u)) {
                parser.error = have_logical_line ? QW_STATUS_ERR_RESOURCE_LIMIT : QW_STATUS_ERR_FORMAT;
                goto failure;
            }
        } else {
            if (have_logical_line &&
                !vcard_process_line(&parser, logical_line.data, logical_line.size))
                goto failure;
            vcard_buffer_reset(&logical_line);
            if (!vcard_buffer_append(&logical_line,
                    (const char *)parser.input + start, length)) {
                parser.error = QW_STATUS_ERR_RESOURCE_LIMIT;
                goto failure;
            }
            have_logical_line = 1;
        }
    }
    if (have_logical_line &&
        !vcard_process_line(&parser, logical_line.data, logical_line.size)) goto failure;
    if (parser.in_card || !parser.cards->value.array.count ||
        !qw_data_document_validate(out_document)) {
        parser.error = QW_STATUS_ERR_FORMAT;
        goto failure;
    }
    rc = QW_STATUS_OK;
    vcard_buffer_dispose(&logical_line);
    return rc;
failure:
    vcard_buffer_dispose(&logical_line);
    qw_data_value_dispose(&parser.current_card);
    qw_data_value_dispose(&root_cards);
    if (parser.error) rc = parser.error;
    qw_data_document_dispose(out_document);
    return rc;
}

int qw_vcard_parse_data_document(const void *data, size_t size,
                                 QwDataDocument *out_document)
{
    return vcard_parse_document(data, size, QW_FORMAT_VCF, out_document);
}

#ifndef QUIREWISE_STANDALONE_DATA_PARSER
static int vcard_output_append(QwVCardOutput *output,
                               const char *text, size_t size)
{
    size_t needed;
    size_t capacity;
    char *grown;
    if (output->size > QW_VCARD_MAX_OUTPUT ||
        size > QW_VCARD_MAX_OUTPUT - output->size ||
        size > SIZE_MAX - output->size - 1u) {
        output->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    needed = output->size + size + 1u;
    if (needed > output->capacity) {
        capacity = output->capacity ? output->capacity : 256u;
        while (capacity < needed) {
            if (capacity > (QW_VCARD_MAX_OUTPUT + 1u) / 2u) {
                capacity = QW_VCARD_MAX_OUTPUT + 1u;
                break;
            }
            capacity *= 2u;
        }
        if (capacity < needed) {
            output->error = QW_STATUS_ERR_RESOURCE_LIMIT;
            return 0;
        }
        grown = (char *)realloc(output->data, capacity);
        if (!grown) { output->error = QW_STATUS_ERR_OUT_OF_MEMORY; return 0; }
        output->data = grown;
        output->capacity = capacity;
    }
    if (size) memcpy(output->data + output->size, text, size);
    output->size += size;
    output->data[output->size] = '\0';
    return 1;
}

static int vcard_write_folded_line(QwVCardOutput *output,
                                   const char *line, size_t size)
{
    size_t position = 0u;
    size_t width = 0u;
    if (!size || !vcard_content_valid(line, size)) {
        output->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    while (position < size) {
        size_t budget = width ? 74u : 75u;
        size_t count = size - position;
        if (count > budget) count = budget;
        while (count && position + count < size &&
               (((unsigned char)line[position + count] & 0xc0u) == 0x80u)) --count;
        if (!count) { output->error = QW_STATUS_ERR_FORMAT; return 0; }
        if (width && !vcard_output_append(output, " ", 1u)) return 0;
        if (!vcard_output_append(output, line + position, count)) return 0;
        position += count;
        width = position < size ? 1u : 0u;
        if (position < size && !vcard_output_append(output, "\r\n", 2u)) return 0;
    }
    return vcard_output_append(output, "\r\n", 2u);
}

static const QwDataValue *vcard_schema_field(const QwDataValue *object,
                                             const char *key)
{
    return vcard_find_member(object, key, strlen(key));
}

static int vcard_string_field(const QwDataValue *object, const char *key,
                              const QwDataValue **out_value)
{
    const QwDataValue *value = vcard_schema_field(object, key);
    if (!value || value->kind != QW_DATA_VALUE_STRING ||
        !value->value.text.data ||
        !vcard_content_valid(value->value.text.data, value->value.text.size)) return 0;
    *out_value = value;
    return 1;
}

static int vcard_write_property(QwVCardOutput *output,
                                const QwDataValue *property,
                                int version_21,
                                int *has_fn)
{
    const QwDataValue *group;
    const QwDataValue *name;
    const QwDataValue *parameters;
    const QwDataValue *value;
    QwVCardBuffer line;
    size_t i;
    memset(&line, 0, sizeof(line));
    if (!property || property->kind != QW_DATA_VALUE_OBJECT ||
        property->value.object.count != 4u ||
        !vcard_string_field(property, QW_VCARD_GROUP_KEY, &group) ||
        !vcard_string_field(property, QW_VCARD_NAME_KEY, &name) ||
        !vcard_string_field(property, QW_VCARD_VALUE_KEY, &value) ||
        !vcard_ascii_token(name->value.text.data, name->value.text.size) ||
        vcard_ascii_equal(name->value.text.data, name->value.text.size, "BEGIN") ||
        vcard_ascii_equal(name->value.text.data, name->value.text.size, "END") ||
        vcard_ascii_equal(name->value.text.data, name->value.text.size, "VERSION") ||
        (group->value.text.size &&
         !vcard_ascii_token(group->value.text.data, group->value.text.size))) {
        output->error = QW_STATUS_ERR_UNSUPPORTED;
        return 0;
    }
    parameters = vcard_schema_field(property, QW_VCARD_PARAMS_KEY);
    if (!parameters || parameters->kind != QW_DATA_VALUE_ARRAY) {
        output->error = QW_STATUS_ERR_UNSUPPORTED;
        return 0;
    }
    if (group->value.text.size &&
        (!vcard_buffer_append(&line, group->value.text.data, group->value.text.size) ||
         !vcard_buffer_append(&line, ".", 1u))) goto out_of_memory;
    if (!vcard_buffer_append(&line, name->value.text.data, name->value.text.size))
        goto out_of_memory;
    for (i = 0u; i < parameters->value.array.count; ++i) {
        const QwDataValue *parameter = &parameters->value.array.items[i];
        if (parameter->kind != QW_DATA_VALUE_STRING ||
            !(version_21
                ? vcard_parameter_valid_21(parameter->value.text.data,
                                           parameter->value.text.size)
                : vcard_parameter_valid(parameter->value.text.data,
                                        parameter->value.text.size))) {
            output->error = QW_STATUS_ERR_UNSUPPORTED;
            goto failure;
        }
        if (!vcard_buffer_append(&line, ";", 1u) ||
            !vcard_buffer_append(&line, parameter->value.text.data,
                                 parameter->value.text.size)) goto out_of_memory;
    }
    if (!vcard_buffer_append(&line, ":", 1u) ||
        !vcard_buffer_append(&line, value->value.text.data,
                             value->value.text.size)) goto out_of_memory;
    if (!vcard_write_folded_line(output, line.data, line.size)) goto failure;
    if (vcard_ascii_equal(name->value.text.data, name->value.text.size, "FN"))
        *has_fn = 1;
    vcard_buffer_dispose(&line);
    return 1;
out_of_memory:
    output->error = QW_STATUS_ERR_OUT_OF_MEMORY;
failure:
    vcard_buffer_dispose(&line);
    return 0;
}

static int vcard_write_document(const QwDataDocument *document,
                                QwOutputFormat output_format,
                                const QwConvertOptions *options,
                                uint8_t **out_data, size_t *out_size,
                                QwConvertReport *report)
{
    const QwDataValue *cards;
    QwVCardOutput output;
    size_t card_index;
    int rc = QW_STATUS_ERR_UNSUPPORTED;
    (void)options;
    if (!document || !out_data || !out_size || output_format != QW_FORMAT_VCF)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    if (document->root.kind != QW_DATA_VALUE_OBJECT ||
        document->root.value.object.count != 1u) return QW_STATUS_ERR_UNSUPPORTED;
    cards = vcard_schema_field(&document->root, QW_VCARD_ROOT_KEY);
    if (!cards || cards->kind != QW_DATA_VALUE_ARRAY || !cards->value.array.count) return QW_STATUS_ERR_UNSUPPORTED;
    memset(&output, 0, sizeof(output));
    for (card_index = 0u; card_index < cards->value.array.count; ++card_index) {
        const QwDataValue *card = &cards->value.array.items[card_index];
        const QwDataValue *version;
        const QwDataValue *properties;
        size_t property_index;
        int has_fn = 0;
        int version_21;
        if (!card || card->kind != QW_DATA_VALUE_OBJECT ||
            card->value.object.count != 2u ||
            !vcard_string_field(card, QW_VCARD_VERSION_KEY, &version) ||
            (!vcard_ascii_equal(version->value.text.data,
                                version->value.text.size, "2.1") &&
             !vcard_ascii_equal(version->value.text.data,
                                version->value.text.size, "3.0") &&
             !vcard_ascii_equal(version->value.text.data,
                                version->value.text.size, "4.0"))) {
            output.error = QW_STATUS_ERR_UNSUPPORTED;
            goto cleanup;
        }
        version_21 = vcard_ascii_equal(version->value.text.data,
                                       version->value.text.size, "2.1");
        properties = vcard_schema_field(card, QW_VCARD_PROPERTIES_KEY);
        if (!properties || properties->kind != QW_DATA_VALUE_ARRAY) {
            output.error = QW_STATUS_ERR_UNSUPPORTED;
            goto cleanup;
        }
        if (!vcard_write_folded_line(&output, "BEGIN:VCARD", 11u)) goto cleanup;
        {
            QwVCardBuffer version_line;
            memset(&version_line, 0, sizeof(version_line));
            if (!vcard_buffer_append(&version_line, "VERSION:", 8u) ||
                !vcard_buffer_append(&version_line, version->value.text.data,
                                     version->value.text.size) ||
                !vcard_write_folded_line(&output, version_line.data,
                                         version_line.size)) {
                vcard_buffer_dispose(&version_line);
                goto cleanup;
            }
            vcard_buffer_dispose(&version_line);
        }
        for (property_index = 0u; property_index < properties->value.array.count;
             ++property_index) {
            if (!vcard_write_property(&output,
                    &properties->value.array.items[property_index],
                    version_21, &has_fn)) goto cleanup;
        }
        if (!has_fn || !vcard_write_folded_line(&output, "END:VCARD", 9u)) {
            output.error = QW_STATUS_ERR_UNSUPPORTED;
            goto cleanup;
        }
    }
    *out_data = (uint8_t *)output.data;
    *out_size = output.size;
    if (report) {
        report->format = document->input_format;
        report->output_format = output_format;
        report->output_size = output.size;
        (void)snprintf(report->note, sizeof(report->note),
            "vCard 2.1/3.0/4.0 records preserve ordered properties and parameter values");
    }
    return QW_STATUS_OK;
cleanup:
    rc = output.error ? output.error : QW_STATUS_ERR_UNSUPPORTED;
    free(output.data);
    return rc;
}

static int vcard_probe(const void *input, size_t size)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t position = 0u;
    static const char begin[] = "BEGIN:VCARD";
    size_t i;
    if (!data || size < sizeof(begin) - 1u || size > QW_VCARD_MAX_INPUT) return 0;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    if (size - position < sizeof(begin) - 1u) return 0;
    for (i = 0u; i < sizeof(begin) - 1u; ++i) {
        uint8_t ch = data[position + i];
        if (ch >= 'a' && ch <= 'z') ch = (uint8_t)(ch - 'a' + 'A');
        if (ch != (uint8_t)begin[i]) return 0;
    }
    return size - position == sizeof(begin) - 1u ||
           data[position + sizeof(begin) - 1u] == '\r' ||
           data[position + sizeof(begin) - 1u] == '\n';
}

static int vcard_extension_equal(const char *extension, size_t size,
                                 const char *expected)
{
    size_t i;
    size_t expected_size = strlen(expected);
    if (size != expected_size) return 0;
    for (i = 0u; i < size; ++i) {
        unsigned char a = (unsigned char)extension[i];
        unsigned char b = (unsigned char)expected[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static int vcard_path_probe(const char *path)
{
    const char *base;
    const char *cursor;
    const char *dot;
    if (!path || !*path) return 0;
    base = path;
    for (cursor = path; *cursor; ++cursor)
        if (*cursor == '/' || *cursor == '\\') base = cursor + 1u;
    dot = strrchr(base, '.');
    return dot && (vcard_extension_equal(dot, strlen(dot), ".vcf") ||
                   vcard_extension_equal(dot, strlen(dot), ".vcard"));
}

static int vcard_open_data(const void *input, size_t size,
                           QwInputFormat input_format,
                           const QwConvertOptions *options,
                           QwDataDocument *out_document,
                           QwConvertReport *report)
{
    int rc;
    (void)options;
    rc = vcard_parse_document(input, size, input_format, out_document);
    if (rc == QW_STATUS_OK && report) {
        report->format = input_format;
        report->output_format = QW_FORMAT_UNKNOWN;
        report->output_size = 0u;
        (void)snprintf(report->note, sizeof(report->note),
            "vCard 2.1/3.0/4.0 records preserve ordered properties, groups, parameters and values");
    }
    return rc;
}

enum {
    QW_VCARD_TABLE_COLUMNS = 8u,
    QW_VCARD_TABLE_MAX_ROWS = 1000000u,
    QW_VCARD_TABLE_MAX_CELLS = 1000000u,
    QW_VCARD_TABLE_MAX_CELL_BYTES = 1024u * 1024u,
    QW_VCARD_TABLE_MAX_CONTENT_BYTES = 256u * 1024u * 1024u
};

typedef struct QwVCardTableField {
    const char *text;
    size_t size;
} QwVCardTableField;

typedef struct QwVCardTableProperty {
    QwVCardBuffer line;
    const char *version;
    size_t version_size;
    const char *group;
    size_t group_size;
    const char *name;
    size_t name_size;
    const char *value;
    size_t value_size;
    size_t parameter_count;
    size_t next_parameter;
    size_t card_id;
    size_t property_order;
    int active;
    int no_parameters;
} QwVCardTableProperty;

static const char *const vcard_table_headers[QW_VCARD_TABLE_COLUMNS] = {
    "CARD", "VERSION", "PROPERTY_ORDER", "GROUP", "NAME",
    "PARAM_ORDER", "PARAM", "VALUE"
};

static int vcard_spreadsheet_add_row(QwSpreadsheetDocument *document,
                                     size_t sheet_index, size_t row,
                                     const QwVCardTableField *fields)
{
    size_t column;
    for (column = 0u; column < QW_VCARD_TABLE_COLUMNS; ++column) {
        const char *text = fields[column].text ? fields[column].text : "";
        if (fields[column].size > QW_VCARD_TABLE_MAX_CELL_BYTES ||
            document->total_cell_count >= QW_VCARD_TABLE_MAX_CELLS ||
            document->content_bytes > QW_VCARD_TABLE_MAX_CONTENT_BYTES ||
            fields[column].size > QW_VCARD_TABLE_MAX_CONTENT_BYTES -
                                      document->content_bytes)
            return QW_STATUS_ERR_RESOURCE_LIMIT;
        if (!qw_spreadsheet_document_add_cell(document, sheet_index, row,
                column, QW_SPREADSHEET_VALUE_STRING,
                (const uint8_t *)text, fields[column].size, NULL, 0u)) {
            return QW_STATUS_ERR_OUT_OF_MEMORY;
        }
    }
    return QW_STATUS_OK;
}

static int vcard_open_spreadsheet(const void *input, size_t size,
                                  QwInputFormat input_format,
                                  const QwConvertOptions *options,
                                  QwSpreadsheetDocument *out_document,
                                  QwConvertReport *report)
{
    QwDataDocument data;
    const QwDataValue *cards;
    QwVCardTableField fields[QW_VCARD_TABLE_COLUMNS];
    size_t sheet_index;
    size_t row = 0u;
    size_t card_index;
    int rc;
    (void)options;
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    qw_spreadsheet_document_init(out_document, input_format);
    rc = vcard_parse_document(input, size, input_format, &data);
    if (rc != QW_STATUS_OK) {
        qw_data_document_dispose(&data);
        return rc;
    }
    if (!qw_spreadsheet_document_add_sheet(out_document,
            (const uint8_t *)"vCard", 5u, &sheet_index)) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (size_t column = 0u; column < QW_VCARD_TABLE_COLUMNS; ++column) {
        fields[column].text = vcard_table_headers[column];
        fields[column].size = strlen(vcard_table_headers[column]);
    }
    rc = vcard_spreadsheet_add_row(out_document, sheet_index, row++, fields);
    if (rc != QW_STATUS_OK) goto cleanup;
    cards = vcard_schema_field(&data.root, QW_VCARD_ROOT_KEY);
    if (!cards || cards->kind != QW_DATA_VALUE_ARRAY) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    for (card_index = 0u; card_index < cards->value.array.count; ++card_index) {
        const QwDataValue *card = &cards->value.array.items[card_index];
        const QwDataValue *version;
        const QwDataValue *properties;
        char card_text[32];
        int card_length = snprintf(card_text, sizeof(card_text), "%zu",
                                   card_index + 1u);
        size_t property_index;
        if (card_length < 0 || (size_t)card_length >= sizeof(card_text) ||
            !vcard_string_field(card, QW_VCARD_VERSION_KEY, &version) ||
            !(properties = vcard_schema_field(card, QW_VCARD_PROPERTIES_KEY)) ||
            properties->kind != QW_DATA_VALUE_ARRAY) {
            rc = QW_STATUS_ERR_FORMAT;
            goto cleanup;
        }
        for (property_index = 0u;
             property_index < properties->value.array.count; ++property_index) {
            const QwDataValue *property =
                &properties->value.array.items[property_index];
            const QwDataValue *group;
            const QwDataValue *name;
            const QwDataValue *value;
            const QwDataValue *parameters;
            char property_text[32];
            int property_length = snprintf(property_text,
                sizeof(property_text), "%zu", property_index);
            size_t parameter_rows;
            size_t parameter_index;
            if (property_length < 0 ||
                (size_t)property_length >= sizeof(property_text) ||
                !vcard_string_field(property, QW_VCARD_GROUP_KEY, &group) ||
                !vcard_string_field(property, QW_VCARD_NAME_KEY, &name) ||
                !vcard_string_field(property, QW_VCARD_VALUE_KEY, &value) ||
                !(parameters = vcard_schema_field(property, QW_VCARD_PARAMS_KEY)) ||
                parameters->kind != QW_DATA_VALUE_ARRAY) {
                rc = QW_STATUS_ERR_FORMAT;
                goto cleanup;
            }
            parameter_rows = parameters->value.array.count
                ? parameters->value.array.count : 1u;
            for (parameter_index = 0u; parameter_index < parameter_rows;
                 ++parameter_index) {
                const QwDataValue *parameter = NULL;
                char parameter_text[32];
                int parameter_length = 0;
                fields[0] = (QwVCardTableField){card_text, (size_t)card_length};
                fields[1] = (QwVCardTableField){version->value.text.data,
                                                version->value.text.size};
                fields[2] = (QwVCardTableField){property_text,
                                                (size_t)property_length};
                fields[3] = (QwVCardTableField){group->value.text.data,
                                                group->value.text.size};
                fields[4] = (QwVCardTableField){name->value.text.data,
                                                name->value.text.size};
                fields[5] = (QwVCardTableField){"", 0u};
                fields[6] = (QwVCardTableField){"", 0u};
                fields[7] = (QwVCardTableField){value->value.text.data,
                                                value->value.text.size};
                if (parameters->value.array.count) {
                    parameter = &parameters->value.array.items[parameter_index];
                    parameter_length = snprintf(parameter_text,
                        sizeof(parameter_text), "%zu", parameter_index);
                    if (parameter_length < 0 ||
                        (size_t)parameter_length >= sizeof(parameter_text) ||
                        parameter->kind != QW_DATA_VALUE_STRING) {
                        rc = QW_STATUS_ERR_FORMAT;
                        goto cleanup;
                    }
                    fields[5] = (QwVCardTableField){parameter_text,
                                                   (size_t)parameter_length};
                    fields[6] = (QwVCardTableField){
                        parameter->value.text.data, parameter->value.text.size};
                }
                if (row >= QW_VCARD_TABLE_MAX_ROWS) {
                    rc = QW_STATUS_ERR_RESOURCE_LIMIT;
                    goto cleanup;
                }
                rc = vcard_spreadsheet_add_row(out_document, sheet_index,
                                               row++, fields);
                if (rc != QW_STATUS_OK) goto cleanup;
            }
        }
    }
    if (!qw_spreadsheet_document_validate(out_document)) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    if (report) {
        report->format = input_format;
        report->output_format = QW_FORMAT_UNKNOWN;
        report->output_size = 0u;
        (void)snprintf(report->note, sizeof(report->note),
            "vCard exported as a property table; parameters use repeated rows");
    }
    rc = QW_STATUS_OK;
cleanup:
    qw_data_document_dispose(&data);
    if (rc != QW_STATUS_OK) qw_spreadsheet_document_dispose(out_document);
    return rc;
}

static const QwSpreadsheetCell *vcard_spreadsheet_cell_at(
    const QwSpreadsheetSheet *sheet, size_t row, size_t column)
{
    size_t low = 0u;
    size_t high = sheet->cell_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        const QwSpreadsheetCell *cell = &sheet->cells[middle];
        if (cell->row < row ||
            (cell->row == row && cell->column < column)) low = middle + 1u;
        else high = middle;
    }
    if (low < sheet->cell_count && sheet->cells[low].row == row &&
        sheet->cells[low].column == column) return &sheet->cells[low];
    return NULL;
}

static int vcard_spreadsheet_get_text(const QwSpreadsheetSheet *sheet,
                                      size_t row, size_t column,
                                      const char **text, size_t *size)
{
    const QwSpreadsheetCell *cell =
        vcard_spreadsheet_cell_at(sheet, row, column);
    if (!cell) {
        *text = "";
        *size = 0u;
        return 1;
    }
    if (cell->formula_size ||
        (cell->value_kind != QW_SPREADSHEET_VALUE_STRING &&
         cell->value_kind != QW_SPREADSHEET_VALUE_EMPTY) ||
        !vcard_content_valid((const char *)cell->value_utf8,
                             cell->value_size)) return 0;
    *text = cell->value_size ? (const char *)cell->value_utf8 : "";
    *size = cell->value_size;
    return 1;
}

static int vcard_table_parse_index(const char *text, size_t size,
                                   size_t *out_value)
{
    size_t value = 0u;
    size_t i;
    if (!text || !size || !out_value) return 0;
    for (i = 0u; i < size; ++i) {
        unsigned digit;
        if (text[i] < '0' || text[i] > '9') return 0;
        digit = (unsigned)(text[i] - '0');
        if (value > (SIZE_MAX - digit) / 10u) return 0;
        value = value * 10u + digit;
    }
    *out_value = value;
    return 1;
}

static int vcard_table_append(QwVCardParser *parser, QwVCardBuffer *line,
                              const char *text, size_t size)
{
    if (line->size > QW_VCARD_MAX_LINE ||
        size > QW_VCARD_MAX_LINE - line->size) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!vcard_buffer_append(line, text, size)) {
        parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

static int vcard_table_flush_property(QwVCardParser *parser,
                                      QwVCardTableProperty *property)
{
    QwVCardLineHeader header;
    int previous_version_21;
    int version_21;
    int ok;
    if (!property->active) return 1;
    previous_version_21 = parser->version_21;
    version_21 = vcard_ascii_equal(property->version,
                                   property->version_size, "2.1");
    parser->version_21 = version_21;
    ok = vcard_table_append(parser, &property->line, ":", 1u);
    if (ok) {
        ok = vcard_table_append(parser, &property->line, property->value,
                                property->value_size) &&
             vcard_parse_header(property->line.data, property->line.size,
                                &header, version_21) &&
             header.param_count == property->parameter_count &&
             vcard_property_append(parser, property->line.data,
                                   property->line.size, &header);
    }
    parser->version_21 = previous_version_21;
    if (!ok) {
        if (!parser->error) parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    vcard_buffer_reset(&property->line);
    property->active = 0;
    return 1;
}

static int vcard_table_finish_card(QwVCardParser *parser,
                                   QwVCardTableProperty *property)
{
    if (!vcard_table_flush_property(parser, property) || !parser->has_fn ||
        !vcard_array_append(parser, parser->cards, &parser->current_card)) {
        if (!parser->error) parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    parser->current_properties = NULL;
    return 1;
}

static int vcard_write_spreadsheet(const QwSpreadsheetDocument *spreadsheet,
                                   QwOutputFormat output_format,
                                   const QwConvertOptions *options,
                                   uint8_t **out_data, size_t *out_size,
                                   QwConvertReport *report)
{
    QwDataDocument data;
    QwVCardParser parser;
    QwVCardTableProperty property;
    const QwSpreadsheetSheet *sheet;
    const char *text[QW_VCARD_TABLE_COLUMNS];
    size_t lengths[QW_VCARD_TABLE_COLUMNS];
    size_t last_row;
    size_t row;
    size_t expected_card = 1u;
    size_t expected_property = 0u;
    size_t current_card = 0u;
    size_t current_property = 0u;
    const char *current_version = NULL;
    size_t current_version_size = 0u;
    int card_started = 0;
    int rc = QW_STATUS_ERR_UNSUPPORTED;
    (void)options;
    if (!spreadsheet || !out_data || !out_size || output_format != QW_FORMAT_VCF)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    if (!qw_spreadsheet_document_validate(spreadsheet) ||
        spreadsheet->sheet_count != 1u) return QW_STATUS_ERR_UNSUPPORTED;
    sheet = &spreadsheet->sheets[0u];
    if (!sheet->cell_count ||
        sheet->cells[sheet->cell_count - 1u].row >= QW_VCARD_TABLE_MAX_ROWS)
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    last_row = sheet->cells[sheet->cell_count - 1u].row;
    if (last_row == 0u) return QW_STATUS_ERR_UNSUPPORTED;
    for (size_t i = 0u; i < sheet->cell_count; ++i) {
        if (sheet->cells[i].column >= QW_VCARD_TABLE_COLUMNS)
            return QW_STATUS_ERR_UNSUPPORTED;
    }
    for (size_t column = 0u; column < QW_VCARD_TABLE_COLUMNS; ++column) {
        if (!vcard_spreadsheet_get_text(sheet, 0u, column,
                &text[column], &lengths[column]) ||
            strlen(vcard_table_headers[column]) != lengths[column] ||
            memcmp(text[column], vcard_table_headers[column], lengths[column]))
            return QW_STATUS_ERR_UNSUPPORTED;
    }
    memset(&data, 0, sizeof(data));
    memset(&parser, 0, sizeof(parser));
    memset(&property, 0, sizeof(property));
    qw_data_document_init(&data, spreadsheet->input_format);
    parser.document = &data;
    if (!vcard_new_value(&parser, &data.root, QW_DATA_VALUE_OBJECT, 0u) ||
        !vcard_new_value(&parser, &parser.current_card,
                         QW_DATA_VALUE_ARRAY, 1u) ||
        !vcard_object_add(&parser, &data.root, QW_VCARD_ROOT_KEY,
            sizeof(QW_VCARD_ROOT_KEY) - 1u, &parser.current_card)) {
        rc = parser.error ? parser.error : QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    parser.cards = &data.root.value.object.items[0u].value;
    for (row = 1u; row <= last_row; ++row) {
        size_t card_id;
        size_t property_order;
        size_t parameter_order = 0u;
        int has_parameter_order;
        for (size_t column = 0u; column < QW_VCARD_TABLE_COLUMNS; ++column) {
            if (!vcard_spreadsheet_get_text(sheet, row, column,
                    &text[column], &lengths[column])) {
                rc = QW_STATUS_ERR_UNSUPPORTED;
                goto cleanup;
            }
        }
        if (!vcard_table_parse_index(text[0], lengths[0], &card_id) ||
            !card_id || card_id > QW_VCARD_TABLE_MAX_ROWS ||
            !vcard_table_parse_index(text[2], lengths[2], &property_order) ||
            property_order >= QW_VCARD_TABLE_MAX_ROWS ||
            !lengths[1] || !lengths[4] ||
            (!vcard_ascii_equal(text[1], lengths[1], "2.1") &&
            (!vcard_ascii_equal(text[1], lengths[1], "3.0") &&
              !vcard_ascii_equal(text[1], lengths[1], "4.0")))) {
            rc = QW_STATUS_ERR_FORMAT;
            goto cleanup;
        }
        has_parameter_order = lengths[5] != 0u;
        if (has_parameter_order != (lengths[6] != 0u) ||
            (has_parameter_order &&
             !vcard_table_parse_index(text[5], lengths[5], &parameter_order))) {
            rc = QW_STATUS_ERR_FORMAT;
            goto cleanup;
        }
        if (!card_started || card_id != current_card ||
            property_order != current_property) {
            if (property.active && !vcard_table_flush_property(&parser, &property)) {
                rc = parser.error;
                goto cleanup;
            }
            if (!card_started || card_id != current_card) {
                if (card_started && !vcard_table_finish_card(&parser, &property)) {
                    rc = parser.error;
                    goto cleanup;
                }
                if (card_id != expected_card || property_order != 0u) {
                    rc = QW_STATUS_ERR_FORMAT;
                    goto cleanup;
                }
                if (!vcard_start_card(&parser, text[1], lengths[1])) {
                    rc = parser.error ? parser.error : QW_STATUS_ERR_OUT_OF_MEMORY;
                    goto cleanup;
                }
                parser.has_fn = 0;
                card_started = 1;
                current_card = card_id;
                current_version = text[1];
                current_version_size = lengths[1];
                expected_property = 0u;
                ++expected_card;
            } else {
                if (property_order != expected_property ||
                    lengths[1] != current_version_size ||
                    memcmp(text[1], current_version, lengths[1])) {
                    rc = QW_STATUS_ERR_FORMAT;
                    goto cleanup;
                }
            }
            current_property = property_order;
            expected_property = property_order + 1u;
            property.card_id = card_id;
            property.property_order = property_order;
            property.version = text[1];
            property.version_size = lengths[1];
            property.group = text[3];
            property.group_size = lengths[3];
            property.name = text[4];
            property.name_size = lengths[4];
            property.value = text[7];
            property.value_size = lengths[7];
            property.parameter_count = 0u;
            property.next_parameter = 0u;
            property.no_parameters = !has_parameter_order;
            if (!vcard_table_append(&parser, &property.line,
                                    text[3], lengths[3]) ||
                (lengths[3] && !vcard_table_append(&parser, &property.line,
                                                   ".", 1u)) ||
                !vcard_table_append(&parser, &property.line,
                                    text[4], lengths[4])) {
                rc = parser.error;
                goto cleanup;
            }
            property.active = 1;
        } else {
            if (property.card_id != card_id ||
                property.property_order != property_order ||
                property.version_size != lengths[1] ||
                memcmp(property.version, text[1], lengths[1]) ||
                property.group_size != lengths[3] ||
                memcmp(property.group, text[3], lengths[3]) ||
                property.name_size != lengths[4] ||
                memcmp(property.name, text[4], lengths[4]) ||
                property.value_size != lengths[7] ||
                memcmp(property.value, text[7], lengths[7]) ||
                property.no_parameters ||
                (!has_parameter_order && property.parameter_count)) {
                rc = QW_STATUS_ERR_FORMAT;
                goto cleanup;
            }
        }
        if (has_parameter_order) {
            if (parameter_order != property.next_parameter ||
                !(vcard_ascii_equal(text[1], lengths[1], "2.1")
                    ? vcard_parameter_valid_21(text[6], lengths[6])
                    : vcard_parameter_valid(text[6], lengths[6])) ||
                !vcard_table_append(&parser, &property.line, ";", 1u) ||
                !vcard_table_append(&parser, &property.line,
                                    text[6], lengths[6])) {
                rc = parser.error ? parser.error : QW_STATUS_ERR_FORMAT;
                goto cleanup;
            }
            ++property.next_parameter;
            ++property.parameter_count;
        }
    }
    if (!card_started || !vcard_table_finish_card(&parser, &property) ||
        !qw_data_document_validate(&data)) {
        rc = parser.error ? parser.error : QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    rc = vcard_write_document(&data, output_format, options,
                              out_data, out_size, report);
cleanup:
    vcard_buffer_dispose(&property.line);
    qw_data_document_dispose(&data);
    return rc;
}

const QwSpreadsheetWriterDescriptor qw_spreadsheet_vcard_writer = {
    QW_FORMAT_VCF, "vCard property-table writer", vcard_write_spreadsheet,
    QW_VCARD_MAX_OUTPUT
};

const QwDataWriterDescriptor qw_data_vcard_writer = {
    QW_FORMAT_VCF, "vCard data writer", vcard_write_document,
    QW_VCARD_MAX_OUTPUT
};

const QwFormatDescriptor qw_format_vcard = {
    .id = QW_FORMAT_VCF,
    .name = "vCard",
    .capabilities = 0u,
    .probe = vcard_probe,
    .open_memory = NULL,
    .open_layout = NULL,
    .open_audio = NULL,
    .max_input_bytes = QW_VCARD_MAX_INPUT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .convert_to_pdf = NULL,
    .open_data = vcard_open_data,
    .open_spreadsheet = vcard_open_spreadsheet
};

const QwPathFormatDescriptor qw_path_format_vcard = {
    .id = QW_FORMAT_VCF,
    .probe_path = vcard_path_probe,
    .decode_document_path = NULL,
    .resource_profile = NULL
};
#endif
