#include "../../core/data_document.h"
#include "data_parser_internal.h"
#include "quirewise/status.h"

#ifndef QUIREWISE_STANDALONE_DATA_PARSER
#include "../../core/format_registry.h"
#include "../../core/writer_registry.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QW_YAML_MAX_INPUT ((size_t)256u * 1024u * 1024u)
#define QW_YAML_MAX_OUTPUT QW_DATA_DOCUMENT_MAX_MEMORY

typedef struct QwYamlLine {
    const char *text;
    size_t size;
    size_t indent;
} QwYamlLine;

#define QW_YAML_MAX_ANCHORS 64u

typedef struct QwYamlAnchor {
    char *name;
    size_t name_size;
    QwDataValue value;
} QwYamlAnchor;

typedef struct QwYamlParser {
    const uint8_t *input;
    size_t input_size;
    QwYamlLine *lines;
    size_t line_count;
    size_t line_index;
    QwDataDocument *document;
    QwYamlAnchor anchors[QW_YAML_MAX_ANCHORS];
    size_t anchor_count;
    int error;
} QwYamlParser;

typedef struct QwYamlBuffer {
    char *data;
    size_t size;
    size_t capacity;
    size_t limit;
    int error;
} QwYamlBuffer;

static int yaml_utf8_valid(const uint8_t *data, size_t size)
{
    size_t i = 0u;
    while (i < size) {
        unsigned char first = data[i++];
        unsigned need;
        uint32_t value;
        uint32_t minimum;
        unsigned j;
        if (first < 0x80u) {
            if (!first || (first < 0x20u && first != '\t' &&
                           first != '\n' && first != '\r')) return 0;
            continue;
        }
        if (first >= 0xc2u && first <= 0xdfu) {
            need = 1u; value = first & 0x1fu; minimum = 0x80u;
        } else if (first >= 0xe0u && first <= 0xefu) {
            need = 2u; value = first & 0x0fu; minimum = 0x800u;
        } else if (first >= 0xf0u && first <= 0xf4u) {
            need = 3u; value = first & 0x07u; minimum = 0x10000u;
        } else {
            return 0;
        }
        if ((size_t)need > size - i) return 0;
        for (j = 0u; j < need; ++j) {
            unsigned char next = data[i++];
            if ((next & 0xc0u) != 0x80u) return 0;
            value = (value << 6) | (next & 0x3fu);
        }
        if (value < minimum || value > 0x10ffffu ||
            (value >= 0xd800u && value <= 0xdfffu)) return 0;
    }
    return 1;
}

static void *yaml_allocate(QwYamlParser *parser, size_t size)
{
    void *memory;
    if (!size) size = 1u;
    if (!parser->document ||
        size > QW_DATA_DOCUMENT_MAX_MEMORY - parser->document->allocation_bytes) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return NULL;
    }
    memory = malloc(size);
    if (!memory) {
        parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    parser->document->allocation_bytes += size;
    return memory;
}

static void *yaml_reallocate(QwYamlParser *parser, void *memory,
                             size_t old_size, size_t new_size)
{
    void *grown;
    if (new_size < old_size || !parser->document ||
        new_size - old_size > QW_DATA_DOCUMENT_MAX_MEMORY -
                                  parser->document->allocation_bytes) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return NULL;
    }
    grown = realloc(memory, new_size);
    if (!grown) {
        parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    parser->document->allocation_bytes += new_size - old_size;
    return grown;
}

static void yaml_release(QwYamlParser *parser, void *memory, size_t size)
{
    if (!memory) return;
    free(memory);
    if (parser->document && size <= parser->document->allocation_bytes)
        parser->document->allocation_bytes -= size;
}

static char *yaml_copy(QwYamlParser *parser, const char *text, size_t size)
{
    char *copy;
    if (size == SIZE_MAX) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return NULL;
    }
    copy = (char *)yaml_allocate(parser, size + 1u);
    if (!copy) return NULL;
    if (size) memcpy(copy, text, size);
    copy[size] = '\0';
    return copy;
}

static int yaml_new_value(QwYamlParser *parser, QwDataValue *value,
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

static int yaml_array_reserve(QwYamlParser *parser, QwDataValue *array,
                              size_t needed)
{
    size_t capacity;
    size_t old_size;
    size_t new_size;
    QwDataValue *items;
    if (needed <= array->value.array.capacity) return 1;
    capacity = array->value.array.capacity ? array->value.array.capacity : 4u;
    while (capacity < needed) {
        if (capacity > QW_DATA_DOCUMENT_MAX_NODES / 2u) {
            capacity = QW_DATA_DOCUMENT_MAX_NODES;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < needed || capacity > SIZE_MAX / sizeof(*items)) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    old_size = array->value.array.capacity * sizeof(*items);
    new_size = capacity * sizeof(*items);
    items = (QwDataValue *)yaml_reallocate(parser, array->value.array.items,
                                           old_size, new_size);
    if (!items) return 0;
    array->value.array.items = items;
    array->value.array.capacity = capacity;
    return 1;
}

static int yaml_object_reserve(QwYamlParser *parser, QwDataValue *object,
                               size_t needed)
{
    size_t capacity;
    size_t old_size;
    size_t new_size;
    QwDataMember *items;
    if (needed <= object->value.object.capacity) return 1;
    capacity = object->value.object.capacity ? object->value.object.capacity : 4u;
    while (capacity < needed) {
        if (capacity > QW_DATA_DOCUMENT_MAX_NODES / 2u) {
            capacity = QW_DATA_DOCUMENT_MAX_NODES;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < needed || capacity > SIZE_MAX / sizeof(*items)) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    old_size = object->value.object.capacity * sizeof(*items);
    new_size = capacity * sizeof(*items);
    items = (QwDataMember *)yaml_reallocate(parser, object->value.object.items,
                                            old_size, new_size);
    if (!items) return 0;
    object->value.object.items = items;
    object->value.object.capacity = capacity;
    return 1;
}

static int yaml_array_append(QwYamlParser *parser, QwDataValue *array,
                             QwDataValue *value)
{
    if (array->value.array.count >= QW_DATA_DOCUMENT_MAX_NODES ||
        !yaml_array_reserve(parser, array, array->value.array.count + 1u))
        return 0;
    array->value.array.items[array->value.array.count++] = *value;
    memset(value, 0, sizeof(*value));
    return 1;
}

static int yaml_object_append_owned(QwYamlParser *parser, QwDataValue *object,
                                    char *key, size_t key_size,
                                    QwDataValue *value)
{
    size_t i;
    QwDataMember *member;
    for (i = 0u; i < object->value.object.count; ++i) {
        const QwDataMember *existing = &object->value.object.items[i];
        if (existing->key_size == key_size &&
            memcmp(existing->key, key, key_size) == 0) {
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
    }
    if (object->value.object.count >= QW_DATA_DOCUMENT_MAX_NODES ||
        !yaml_object_reserve(parser, object, object->value.object.count + 1u))
        return 0;
    member = &object->value.object.items[object->value.object.count++];
    member->key = key;
    member->key_size = key_size;
    member->value = *value;
    memset(value, 0, sizeof(*value));
    return 1;
}

static int yaml_clone_value(QwYamlParser *parser, const QwDataValue *source,
                            QwDataValue *target, size_t depth)
{
    size_t index;
    if (!source || !target || depth > QW_DATA_DOCUMENT_MAX_DEPTH) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!yaml_new_value(parser, target, source->kind, depth)) return 0;
    switch (source->kind) {
    case QW_DATA_VALUE_NULL:
        break;
    case QW_DATA_VALUE_BOOLEAN:
        target->value.boolean = source->value.boolean;
        break;
    case QW_DATA_VALUE_NUMBER:
    case QW_DATA_VALUE_TEMPORAL:
        if (source->value.text.size) {
            char *copy = (char *)yaml_allocate(parser,
                                               source->value.text.size + 1u);
            if (!copy) return 0;
            memcpy(copy, source->value.text.data, source->value.text.size);
            copy[source->value.text.size] = '\0';
            target->value.text.data = copy;
            target->value.text.size = source->value.text.size;
        }
        break;
    case QW_DATA_VALUE_STRING:
        if (source->value.text.size) {
            char *copy = (char *)yaml_allocate(parser,
                                               source->value.text.size + 1u);
            if (!copy) return 0;
            memcpy(copy, source->value.text.data, source->value.text.size);
            copy[source->value.text.size] = '\0';
            target->value.text.data = copy;
            target->value.text.size = source->value.text.size;
        }
        break;
    case QW_DATA_VALUE_ARRAY:
        for (index = 0u; index < source->value.array.count; ++index) {
            QwDataValue item;
            memset(&item, 0, sizeof(item));
            if (!yaml_clone_value(parser, &source->value.array.items[index],
                                  &item, depth + 1u) ||
                !yaml_array_append(parser, target, &item)) {
                qw_data_value_dispose(&item);
                return 0;
            }
        }
        break;
    case QW_DATA_VALUE_OBJECT:
        for (index = 0u; index < source->value.object.count; ++index) {
            const QwDataMember *member = &source->value.object.items[index];
            char *key = (char *)yaml_allocate(parser, member->key_size + 1u);
            QwDataValue item;
            memset(&item, 0, sizeof(item));
            if (!key) return 0;
            memcpy(key, member->key, member->key_size);
            key[member->key_size] = '\0';
            if (!yaml_clone_value(parser, &member->value, &item, depth + 1u)) {
                yaml_release(parser, key, member->key_size + 1u);
                return 0;
            }
            if (!yaml_object_append_owned(parser, target, key, member->key_size,
                                          &item)) {
                yaml_release(parser, key, member->key_size + 1u);
                qw_data_value_dispose(&item);
                return 0;
            }
        }
        break;
    }
    return 1;
}

static int yaml_anchor_name_parse(QwYamlParser *parser, const char *text,
                                  size_t size, size_t start, size_t *out_after,
                                  char **out_name, size_t *out_size)
{
    size_t index = start + 1u;
    size_t begin = index;
    char *name;
    while (index < size) {
        char ch = text[index];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
              ch == '.' || ch == '/'))
            break;
        ++index;
    }
    if (index == begin) {
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    {
        size_t name_length = index - begin;
        name = (char *)yaml_allocate(parser, name_length + 1u);
        if (!name) return 0;
        memcpy(name, text + begin, name_length);
        name[name_length] = '\0';
        *out_size = name_length;
    }
    while (index < size && (text[index] == ' ' || text[index] == '\t')) ++index;
    *out_after = index;
    *out_name = name;
    return 1;
}

static int yaml_anchor_store(QwYamlParser *parser, char *name,
                             size_t name_size, const QwDataValue *value)
{
    size_t index;
    QwDataValue clone;
    memset(&clone, 0, sizeof(clone));
    if (!yaml_clone_value(parser, value, &clone, 0u)) {
        yaml_release(parser, name, name_size + 1u);
        return 0;
    }
    for (index = 0u; index < parser->anchor_count; ++index) {
        QwYamlAnchor *existing = &parser->anchors[index];
        if (existing->name_size == name_size &&
            memcmp(existing->name, name, name_size) == 0) {
            qw_data_value_dispose(&existing->value);
            existing->value = clone;
            yaml_release(parser, name, name_size + 1u);
            return 1;
        }
    }
    if (parser->anchor_count >= QW_YAML_MAX_ANCHORS) {
        qw_data_value_dispose(&clone);
        yaml_release(parser, name, name_size + 1u);
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    parser->anchors[parser->anchor_count].name = name;
    parser->anchors[parser->anchor_count].name_size = name_size;
    parser->anchors[parser->anchor_count].value = clone;
    ++parser->anchor_count;
    return 1;
}

static int yaml_anchor_resolve(QwYamlParser *parser, const char *name,
                               size_t name_size, QwDataValue *out_value,
                               size_t depth)
{
    size_t index;
    for (index = 0u; index < parser->anchor_count; ++index) {
        QwYamlAnchor *anchor = &parser->anchors[index];
        if (anchor->name_size == name_size &&
            memcmp(anchor->name, name, name_size) == 0)
            return yaml_clone_value(parser, &anchor->value, out_value, depth);
    }
    parser->error = QW_STATUS_ERR_FORMAT;
    return 0;
}

static size_t yaml_value_node_count(const QwDataValue *value)
{
    size_t total = 1u;
    size_t index;
    if (!value) return 0u;
    if (value->kind == QW_DATA_VALUE_ARRAY) {
        for (index = 0u; index < value->value.array.count; ++index)
            total += yaml_value_node_count(&value->value.array.items[index]);
    } else if (value->kind == QW_DATA_VALUE_OBJECT) {
        for (index = 0u; index < value->value.object.count; ++index)
            total += yaml_value_node_count(&value->value.object.items[index].value);
    }
    return total;
}

static void yaml_anchors_dispose(QwYamlParser *parser)
{
    size_t index;
    size_t nodes = 0u;
    for (index = 0u; index < parser->anchor_count; ++index) {
        nodes += yaml_value_node_count(&parser->anchors[index].value);
        free(parser->anchors[index].name);
        qw_data_value_dispose(&parser->anchors[index].value);
    }
    if (parser->document && nodes <= parser->document->node_count)
        parser->document->node_count -= nodes;
    parser->anchor_count = 0u;
}

static int yaml_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int yaml_append_codepoint(char *output, size_t capacity, size_t *size,
                                 uint32_t codepoint)
{
    uint8_t bytes[4];
    size_t count;
    if (codepoint <= 0x7fu) {
        bytes[0] = (uint8_t)codepoint; count = 1u;
    } else if (codepoint <= 0x7ffu) {
        bytes[0] = (uint8_t)(0xc0u | (codepoint >> 6));
        bytes[1] = (uint8_t)(0x80u | (codepoint & 0x3fu)); count = 2u;
    } else if (codepoint <= 0xffffu &&
               !(codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
        bytes[0] = (uint8_t)(0xe0u | (codepoint >> 12));
        bytes[1] = (uint8_t)(0x80u | ((codepoint >> 6) & 0x3fu));
        bytes[2] = (uint8_t)(0x80u | (codepoint & 0x3fu)); count = 3u;
    } else if (codepoint <= 0x10ffffu) {
        bytes[0] = (uint8_t)(0xf0u | (codepoint >> 18));
        bytes[1] = (uint8_t)(0x80u | ((codepoint >> 12) & 0x3fu));
        bytes[2] = (uint8_t)(0x80u | ((codepoint >> 6) & 0x3fu));
        bytes[3] = (uint8_t)(0x80u | (codepoint & 0x3fu)); count = 4u;
    } else {
        return 0;
    }
    if (count > capacity - *size) return 0;
    memcpy(output + *size, bytes, count);
    *size += count;
    return 1;
}

static int yaml_parse_hex_escape(const char *text, size_t size, size_t *position,
                                 unsigned digits, uint32_t *out_value)
{
    unsigned i;
    uint32_t value = 0u;
    if ((size_t)digits > size - *position) return 0;
    for (i = 0u; i < digits; ++i) {
        int digit = yaml_hex_value(text[(*position)++]);
        if (digit < 0) return 0;
        value = (value << 4) | (unsigned)digit;
    }
    *out_value = value;
    return 1;
}

static char *yaml_decode_quoted(QwYamlParser *parser, const char *text,
                                size_t size, size_t *out_size)
{
    char quote;
    char *decoded;
    size_t position = 1u;
    size_t output_size = 0u;
    if (size < 2u || (text[0] != '\'' && text[0] != '"') ||
        text[size - 1u] != text[0]) {
        parser->error = QW_STATUS_ERR_FORMAT;
        return NULL;
    }
    quote = text[0];
    decoded = (char *)yaml_allocate(parser, size + 1u);
    if (!decoded) return NULL;
    while (position < size - 1u) {
        unsigned char ch = (unsigned char)text[position++];
        if (quote == '\'' && ch == '\'' && position < size - 1u &&
            text[position] == '\'') {
            decoded[output_size++] = '\'';
            ++position;
        } else if (quote == '"' && ch == '\\') {
            uint32_t codepoint;
            unsigned digits;
            if (position >= size - 1u) goto invalid;
            ch = (unsigned char)text[position++];
            switch (ch) {
            case '0': goto invalid;
            case 'a': decoded[output_size++] = '\a'; break;
            case 'b': decoded[output_size++] = '\b'; break;
            case 't': case '\t': decoded[output_size++] = '\t'; break;
            case 'n': decoded[output_size++] = '\n'; break;
            case 'v': decoded[output_size++] = '\v'; break;
            case 'f': decoded[output_size++] = '\f'; break;
            case 'r': decoded[output_size++] = '\r'; break;
            case 'e': decoded[output_size++] = 0x1bu; break;
            case ' ': decoded[output_size++] = ' '; break;
            case '"': decoded[output_size++] = '"'; break;
            case '/': decoded[output_size++] = '/'; break;
            case '\\': decoded[output_size++] = '\\'; break;
            case 'N': codepoint = 0x85u; goto append_codepoint;
            case '_': codepoint = 0xa0u; goto append_codepoint;
            case 'L': codepoint = 0x2028u; goto append_codepoint;
            case 'P': codepoint = 0x2029u; goto append_codepoint;
            case 'x': digits = 2u; break;
            case 'u': digits = 4u; break;
            case 'U': digits = 8u; break;
            default: goto invalid;
            }
            if (ch == 'x' || ch == 'u' || ch == 'U') {
                if (!yaml_parse_hex_escape(text, size - 1u, &position,
                                           digits, &codepoint)) goto invalid;
                if (codepoint >= 0xd800u && codepoint <= 0xdbffu) {
                    uint32_t low;
                    if (ch != 'u' || size - 1u - position < 6u ||
                        text[position] != '\\' || text[position + 1u] != 'u')
                        goto invalid;
                    position += 2u;
                    if (!yaml_parse_hex_escape(text, size - 1u, &position,
                                               4u, &low) ||
                        low < 0xdc00u || low > 0xdfffu) goto invalid;
                    codepoint = 0x10000u + ((codepoint - 0xd800u) << 10) +
                                (low - 0xdc00u);
                }
append_codepoint:
                if (!yaml_append_codepoint(decoded, size, &output_size,
                                           codepoint)) goto invalid;
            }
        } else {
            decoded[output_size++] = (char)ch;
        }
    }
    decoded[output_size] = '\0';
    *out_size = output_size;
    return decoded;
invalid:
    yaml_release(parser, decoded, size + 1u);
    parser->error = QW_STATUS_ERR_FORMAT;
    return NULL;
}

static int yaml_number_valid(const char *text, size_t size)
{
    size_t i = 0u;
    size_t digits = 0u;
    if (i < size && text[i] == '-') ++i;
    if (i >= size) return 0;
    if (text[i] == '0') {
        ++i;
        if (i < size && text[i] >= '0' && text[i] <= '9') return 0;
    } else {
        if (text[i] < '1' || text[i] > '9') return 0;
        while (i < size && text[i] >= '0' && text[i] <= '9') { ++i; ++digits; }
        if (!digits) return 0;
    }
    if (i < size && text[i] == '.') {
        ++i;
        digits = 0u;
        while (i < size && text[i] >= '0' && text[i] <= '9') { ++i; ++digits; }
        if (!digits) return 0;
    }
    if (i < size && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < size && (text[i] == '+' || text[i] == '-')) ++i;
        digits = 0u;
        while (i < size && text[i] >= '0' && text[i] <= '9') { ++i; ++digits; }
        if (!digits) return 0;
    }
    return i == size;
}

static int yaml_text_equal(const char *text, size_t size, const char *word)
{
    size_t i;
    size_t word_size = strlen(word);
    if (size != word_size) return 0;
    for (i = 0u; i < size; ++i) {
        char left = text[i];
        char right = word[i];
        if (left >= 'A' && left <= 'Z') left = (char)(left + ('a' - 'A'));
        if (right >= 'A' && right <= 'Z') right = (char)(right + ('a' - 'A'));
        if (left != right) return 0;
    }
    return 1;
}

static int yaml_parse_scalar(QwYamlParser *parser, const char *text,
                             size_t size, QwDataValue *out_value,
                             size_t depth)
{
    char *copy;
    size_t decoded_size;
    while (size && (text[0] == ' ' || text[0] == '\t')) { ++text; --size; }
    while (size && (text[size - 1u] == ' ' || text[size - 1u] == '\t')) --size;
    if (size && text[0] == '!') {
        size_t tag_size = 1u;
        size_t value_start;
        size_t value_size;
        char *decoded = NULL;
        size_t decoded_size = 0u;
        size_t decoded_alloc = 0u;
        const char *value_text;
        int want_string = 0;
        int want_number = 0;
        int want_boolean = 0;
        int want_null = 0;
        while (tag_size < size && text[tag_size] != ' ' &&
               text[tag_size] != '\t')
            ++tag_size;
        if (tag_size == 5u && memcmp(text, "!!str", 5u) == 0)
            want_string = 1;
        else if (tag_size == 5u && memcmp(text, "!!int", 5u) == 0)
            want_number = 1;
        else if (tag_size == 7u && memcmp(text, "!!float", 7u) == 0)
            want_number = 1;
        else if (tag_size == 6u && memcmp(text, "!!bool", 6u) == 0)
            want_boolean = 1;
        else if (tag_size == 6u && memcmp(text, "!!null", 6u) == 0)
            want_null = 1;
        else {
            parser->error = QW_STATUS_ERR_UNSUPPORTED;
            return 0;
        }
        value_start = tag_size;
        while (value_start < size &&
               (text[value_start] == ' ' || text[value_start] == '\t'))
            ++value_start;
        value_text = text + value_start;
        value_size = size - value_start;
        if (value_size && (value_text[0] == '\'' || value_text[0] == '"')) {
            decoded_alloc = value_size;
            decoded = yaml_decode_quoted(parser, value_text, value_size,
                                         &decoded_size);
            if (!decoded) return 0;
            value_text = decoded;
            value_size = decoded_size;
        }
        if (want_string) {
            char *copy;
            if (!yaml_new_value(parser, out_value, QW_DATA_VALUE_STRING, depth)) {
                if (decoded) yaml_release(parser, decoded, decoded_alloc + 1u);
                return 0;
            }
            copy = (char *)yaml_allocate(parser, value_size + 1u);
            if (!copy) {
                if (decoded) yaml_release(parser, decoded, decoded_alloc + 1u);
                return 0;
            }
            memcpy(copy, value_text, value_size);
            copy[value_size] = '\0';
            out_value->value.text.data = copy;
            out_value->value.text.size = value_size;
        } else if (want_null) {
            if (value_size && !(value_size == 1u && value_text[0] == '~') &&
                !yaml_text_equal(value_text, value_size, "null")) {
                if (decoded) yaml_release(parser, decoded, decoded_alloc + 1u);
                parser->error = QW_STATUS_ERR_FORMAT;
                return 0;
            }
            if (!yaml_new_value(parser, out_value, QW_DATA_VALUE_NULL, depth)) {
                if (decoded) yaml_release(parser, decoded, decoded_alloc + 1u);
                return 0;
            }
        } else if (want_boolean) {
            if (!yaml_text_equal(value_text, value_size, "true") &&
                !yaml_text_equal(value_text, value_size, "false")) {
                if (decoded) yaml_release(parser, decoded, decoded_alloc + 1u);
                parser->error = QW_STATUS_ERR_FORMAT;
                return 0;
            }
            if (!yaml_new_value(parser, out_value, QW_DATA_VALUE_BOOLEAN,
                                depth)) {
                if (decoded) yaml_release(parser, decoded, decoded_alloc + 1u);
                return 0;
            }
            out_value->value.boolean =
                yaml_text_equal(value_text, value_size, "true");
        } else if (want_number) {
            char *copy;
            if (!yaml_number_valid(value_text, value_size)) {
                if (decoded) yaml_release(parser, decoded, decoded_alloc + 1u);
                parser->error = QW_STATUS_ERR_FORMAT;
                return 0;
            }
            if (!yaml_new_value(parser, out_value, QW_DATA_VALUE_NUMBER,
                                depth)) {
                if (decoded) yaml_release(parser, decoded, decoded_alloc + 1u);
                return 0;
            }
            copy = (char *)yaml_allocate(parser, value_size + 1u);
            if (!copy) {
                if (decoded) yaml_release(parser, decoded, decoded_alloc + 1u);
                return 0;
            }
            memcpy(copy, value_text, value_size);
            copy[value_size] = '\0';
            out_value->value.text.data = copy;
            out_value->value.text.size = value_size;
        }
        if (decoded) yaml_release(parser, decoded, decoded_alloc + 1u);
        return 1;
    }
    if (size && (text[0] == '\'' || text[0] == '"')) {
        if (!yaml_new_value(parser, out_value, QW_DATA_VALUE_STRING, depth)) return 0;
        copy = yaml_decode_quoted(parser, text, size, &decoded_size);
        if (!copy) return 0;
        if (memchr(copy, '\0', decoded_size)) {
            yaml_release(parser, copy, size + 1u);
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        out_value->value.text.data = copy;
        out_value->value.text.size = decoded_size;
        return 1;
    }
    if (!size || (size == 1u && text[0] == '~') ||
        yaml_text_equal(text, size, "null"))
        return yaml_new_value(parser, out_value, QW_DATA_VALUE_NULL, depth);
    if (yaml_text_equal(text, size, "true") ||
        yaml_text_equal(text, size, "false")) {
        if (!yaml_new_value(parser, out_value, QW_DATA_VALUE_BOOLEAN, depth)) return 0;
        out_value->value.boolean = yaml_text_equal(text, size, "true");
        return 1;
    }
    if (yaml_number_valid(text, size)) {
        if (!yaml_new_value(parser, out_value, QW_DATA_VALUE_NUMBER, depth)) return 0;
        copy = yaml_copy(parser, text, size);
        if (!copy) return 0;
        out_value->value.text.data = copy;
        out_value->value.text.size = size;
        return 1;
    }
    if (text[0] == '&' || text[0] == '*' || text[0] == '!' ||
        text[0] == '|' || text[0] == '>') {
        parser->error = QW_STATUS_ERR_UNSUPPORTED;
        return 0;
    }
    if (text[0] == '?' &&
        (size == 1u || text[1] == ' ' || text[1] == '\t')) {
        parser->error = QW_STATUS_ERR_UNSUPPORTED;
        return 0;
    }
    if (!yaml_new_value(parser, out_value, QW_DATA_VALUE_STRING, depth)) return 0;
    copy = yaml_copy(parser, text, size);
    if (!copy) return 0;
    out_value->value.text.data = copy;
    out_value->value.text.size = size;
    return 1;
}

static int yaml_parse_key(QwYamlParser *parser, const char *text, size_t size,
                          char **out_key, size_t *out_size)
{
    char *copy;
    while (size && (text[0] == ' ' || text[0] == '\t')) { ++text; --size; }
    while (size && (text[size - 1u] == ' ' || text[size - 1u] == '\t')) --size;
    if (!size) { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
    if (text[0] == '&' || text[0] == '*' || text[0] == '!') {
        parser->error = QW_STATUS_ERR_UNSUPPORTED;
        return 0;
    }
    if (text[0] == '?' &&
        (size == 1u || text[1] == ' ' || text[1] == '\t')) {
        parser->error = QW_STATUS_ERR_UNSUPPORTED;
        return 0;
    }
    if (text[0] == '\'' || text[0] == '"') {
        copy = yaml_decode_quoted(parser, text, size, out_size);
        if (!copy) return 0;
    } else {
        if (memchr(text, '\0', size)) { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
        copy = yaml_copy(parser, text, size);
        if (!copy) return 0;
        *out_size = size;
    }
    if (!*out_size || memchr(copy, '\0', *out_size)) {
        yaml_release(parser, copy, size + 1u);
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    *out_key = copy;
    return 1;
}

static size_t yaml_find_pair_colon(const char *text, size_t size)
{
    char quote = '\0';
    size_t flow_depth = 0u;
    size_t i;
    for (i = 0u; i < size; ++i) {
        char ch = text[i];
        if (quote == '"' && ch == '\\') { ++i; continue; }
        if (quote) {
            if (ch == quote) {
                if (quote == '\'' && i + 1u < size && text[i + 1u] == '\'') {
                    ++i; continue;
                }
                quote = '\0';
            }
            continue;
        }
        if ((i == 0u || flow_depth) && (ch == '\'' || ch == '"')) {
            quote = ch;
            continue;
        }
        if (ch == '[' || ch == '{') { ++flow_depth; continue; }
        if ((ch == ']' || ch == '}') && flow_depth) { --flow_depth; continue; }
        if (flow_depth) continue;
        if (ch == ':' && (i + 1u == size || text[i + 1u] == ' ' ||
                          text[i + 1u] == '\t')) return i;
    }
    return SIZE_MAX;
}

static int yaml_flow_array(QwYamlParser *parser, const char *text, size_t size,
                           size_t *position, QwDataValue *out_value,
                           size_t depth);
static int yaml_flow_object(QwYamlParser *parser, const char *text, size_t size,
                            size_t *position, QwDataValue *out_value,
                            size_t depth);

static void yaml_skip_flow_space(const char *text, size_t size, size_t *position)
{
    while (*position < size && (text[*position] == ' ' || text[*position] == '\t'))
        ++*position;
}

static int yaml_parse_inline(QwYamlParser *parser, const char *text, size_t size,
                             QwDataValue *out_value, size_t depth);

static int yaml_flow_array(QwYamlParser *parser, const char *text, size_t size,
                           size_t *position, QwDataValue *out_value,
                           size_t depth)
{
    if (!yaml_new_value(parser, out_value, QW_DATA_VALUE_ARRAY, depth)) return 0;
    ++*position;
    yaml_skip_flow_space(text, size, position);
    if (*position < size && text[*position] == ']') { ++*position; return 1; }
    for (;;) {
        QwDataValue item;
        size_t start = *position;
        char quote = '\0';
        memset(&item, 0, sizeof(item));
        if (*position >= size || text[*position] == ',' || text[*position] == ']') {
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        if (text[*position] == '[') {
            if (!yaml_flow_array(parser, text, size, position, &item, depth + 1u)) {
                qw_data_value_dispose(&item);
                return 0;
            }
        } else if (text[*position] == '{') {
            if (!yaml_flow_object(parser, text, size, position, &item, depth + 1u)) {
                qw_data_value_dispose(&item);
                return 0;
            }
        } else {
            while (*position < size) {
                char ch = text[*position];
                if (quote == '"' && ch == '\\') { *position += 2u; continue; }
                if (quote) {
                    if (ch == quote) quote = '\0';
                } else if (ch == '\'' || ch == '"') quote = ch;
                else if (ch == ',' || ch == ']') break;
                ++*position;
            }
            if (quote || !yaml_parse_scalar(parser, text + start,
                                             *position - start, &item, depth + 1u)) {
                qw_data_value_dispose(&item);
                if (!parser->error) parser->error = QW_STATUS_ERR_FORMAT;
                return 0;
            }
        }
        if (!yaml_array_append(parser, out_value, &item)) {
            qw_data_value_dispose(&item);
            return 0;
        }
        yaml_skip_flow_space(text, size, position);
        if (*position >= size) { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
        if (text[*position] == ']') { ++*position; return 1; }
        if (text[*position] != ',') { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
        ++*position;
        yaml_skip_flow_space(text, size, position);
        if (*position < size && text[*position] == ']') { ++*position; return 1; }
    }
}

static size_t yaml_find_flow_key_colon(const char *text, size_t size,
                                       size_t start)
{
    char quote = '\0';
    size_t i;
    for (i = start; i < size; ++i) {
        char ch = text[i];
        if (quote == '"' && ch == '\\') { ++i; continue; }
        if (quote) {
            if (ch == quote) {
                if (quote == '\'' && i + 1u < size && text[i + 1u] == '\'') {
                    ++i; continue;
                }
                quote = '\0';
            }
        } else if (i == start && (ch == '\'' || ch == '"')) quote = ch;
        else if (ch == ':') return i;
        else if (ch == ',' || ch == '}') return SIZE_MAX;
    }
    return SIZE_MAX;
}

static int yaml_flow_object(QwYamlParser *parser, const char *text, size_t size,
                            size_t *position, QwDataValue *out_value,
                            size_t depth)
{
    if (!yaml_new_value(parser, out_value, QW_DATA_VALUE_OBJECT, depth)) return 0;
    ++*position;
    yaml_skip_flow_space(text, size, position);
    if (*position < size && text[*position] == '}') { ++*position; return 1; }
    for (;;) {
        size_t colon;
        size_t key_start;
        size_t key_size;
        char *key = NULL;
        QwDataValue item;
        memset(&item, 0, sizeof(item));
        key_start = *position;
        colon = yaml_find_flow_key_colon(text, size, key_start);
        if (colon == SIZE_MAX ||
            !yaml_parse_key(parser, text + key_start, colon - key_start,
                            &key, &key_size)) return 0;
        *position = colon + 1u;
        yaml_skip_flow_space(text, size, position);
        if (*position >= size || text[*position] == ',' || text[*position] == '}') {
            parser->error = QW_STATUS_ERR_FORMAT;
            yaml_release(parser, key, key_size + 1u);
            return 0;
        }
        if (text[*position] == '[') {
            if (!yaml_flow_array(parser, text, size, position, &item, depth + 1u)) {
                yaml_release(parser, key, key_size + 1u);
                qw_data_value_dispose(&item);
                return 0;
            }
        } else if (text[*position] == '{') {
            if (!yaml_flow_object(parser, text, size, position, &item, depth + 1u)) {
                yaml_release(parser, key, key_size + 1u);
                qw_data_value_dispose(&item);
                return 0;
            }
        } else {
            size_t scan = *position;
            char quote = '\0';
            while (scan < size) {
                char ch = text[scan];
                if (quote == '"' && ch == '\\') { scan += 2u; continue; }
                if (quote) { if (ch == quote) quote = '\0'; }
                else if (ch == '\'' || ch == '"') quote = ch;
                else if (ch == ',' || ch == '}') break;
                ++scan;
            }
            if (quote || !yaml_parse_scalar(parser, text + *position,
                                             scan - *position, &item,
                                             depth + 1u)) {
                yaml_release(parser, key, key_size + 1u);
                qw_data_value_dispose(&item);
                if (!parser->error) parser->error = QW_STATUS_ERR_FORMAT;
                return 0;
            }
            *position = scan;
        }
        if (!yaml_object_append_owned(parser, out_value, key, key_size, &item)) {
            yaml_release(parser, key, key_size + 1u);
            qw_data_value_dispose(&item);
            return 0;
        }
        yaml_skip_flow_space(text, size, position);
        if (*position >= size) { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
        if (text[*position] == '}') { ++*position; return 1; }
        if (text[*position] != ',') { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
        ++*position;
        yaml_skip_flow_space(text, size, position);
        if (*position < size && text[*position] == '}') { ++*position; return 1; }
    }
}

static int yaml_parse_inline(QwYamlParser *parser, const char *text, size_t size,
                             QwDataValue *out_value, size_t depth)
{
    size_t position = 0u;
    int parsed;
    yaml_skip_flow_space(text, size, &position);
    if (position >= size) return yaml_parse_scalar(parser, "", 0u, out_value, depth);
    if (text[position] == '*') {
        size_t after;
        char *name = NULL;
        size_t name_size = 0u;
        int ok;
        if (!yaml_anchor_name_parse(parser, text, size, position, &after,
                                    &name, &name_size)) return 0;
        if (after != size) {
            yaml_release(parser, name, name_size + 1u);
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        ok = yaml_anchor_resolve(parser, name, name_size, out_value, depth);
        yaml_release(parser, name, name_size + 1u);
        return ok;
    }
    if (text[position] == '&') {
        size_t after;
        char *name = NULL;
        size_t name_size = 0u;
        int ok;
        if (!yaml_anchor_name_parse(parser, text, size, position, &after,
                                    &name, &name_size)) return 0;
        if (after >= size) {
            yaml_release(parser, name, name_size + 1u);
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        ok = yaml_parse_inline(parser, text + after, size - after, out_value,
                               depth);
        if (ok) ok = yaml_anchor_store(parser, name, name_size, out_value);
        else yaml_release(parser, name, name_size + 1u);
        return ok;
    }
    if (text[position] == '[') {
        parsed = yaml_flow_array(parser, text, size, &position, out_value, depth);
    } else if (text[position] == '{') {
        parsed = yaml_flow_object(parser, text, size, &position, out_value, depth);
    } else {
        parsed = yaml_parse_scalar(parser, text + position, size - position,
                                   out_value, depth);
        position = size;
    }
    yaml_skip_flow_space(text, size, &position);
    if (parsed && position != size) {
        qw_data_value_dispose(out_value);
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    if (!parsed) {
        qw_data_value_dispose(out_value);
        return 0;
    }
    return 1;
}

static int yaml_is_sequence_line(const QwYamlLine *line)
{
    return line->size && line->text[0] == '-' &&
           (line->size == 1u || line->text[1] == ' ' || line->text[1] == '\t');
}

static int yaml_parse_block(QwYamlParser *parser, size_t indent,
                            QwDataValue *out_value, size_t depth);

static int yaml_parse_pair_text(QwYamlParser *parser, const char *text,
                                size_t size, size_t parent_indent,
                                QwDataValue *object, size_t depth)
{
    size_t colon = yaml_find_pair_colon(text, size);
    size_t value_start;
    size_t value_size;
    size_t key_size;
    char *key = NULL;
    QwDataValue value;
    memset(&value, 0, sizeof(value));
    if (colon == SIZE_MAX ||
        !yaml_parse_key(parser, text, colon, &key, &key_size)) {
        if (!parser->error) parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    value_start = colon + 1u;
    while (value_start < size &&
           (text[value_start] == ' ' || text[value_start] == '\t')) ++value_start;
    value_size = size - value_start;
    while (value_size && (text[value_start + value_size - 1u] == ' ' ||
                          text[value_start + value_size - 1u] == '\t')) --value_size;
    {
        char *anchor_name = NULL;
        size_t anchor_size = 0u;
        int anchored = 0;
        if (value_size && text[value_start] == '&') {
            size_t after = 0u;
            if (!yaml_anchor_name_parse(parser, text + value_start, value_size,
                                        0u, &after, &anchor_name,
                                        &anchor_size)) goto failure;
            anchored = 1;
            value_start += after;
            value_size -= after;
            while (value_size && (text[value_start] == ' ' ||
                                  text[value_start] == '\t')) {
                ++value_start;
                --value_size;
            }
        }
        if (value_size) {
            if (!yaml_parse_inline(parser, text + value_start, value_size,
                                   &value, depth + 1u)) {
                if (anchored)
                    yaml_release(parser, anchor_name, anchor_size + 1u);
                goto failure;
            }
        } else if (parser->line_index < parser->line_count &&
                   parser->lines[parser->line_index].indent > parent_indent) {
            size_t child_indent = parser->lines[parser->line_index].indent;
            if (!yaml_parse_block(parser, child_indent, &value, depth + 1u)) {
                if (anchored)
                    yaml_release(parser, anchor_name, anchor_size + 1u);
                goto failure;
            }
        } else if (!yaml_new_value(parser, &value, QW_DATA_VALUE_NULL,
                                   depth + 1u)) {
            if (anchored)
                yaml_release(parser, anchor_name, anchor_size + 1u);
            goto failure;
        }
        if (anchored &&
            !yaml_anchor_store(parser, anchor_name, anchor_size, &value))
            goto failure;
    }
    if (!yaml_object_append_owned(parser, object, key, key_size, &value)) goto failure;
    return 1;
failure:
    yaml_release(parser, key, key_size + 1u);
    qw_data_value_dispose(&value);
    return 0;
}

static int yaml_parse_mapping(QwYamlParser *parser, size_t indent,
                              QwDataValue *out_value, size_t depth)
{
    QwDataValue merges[16];
    size_t merge_count = 0u;
    size_t merge_index;
    if (!yaml_new_value(parser, out_value, QW_DATA_VALUE_OBJECT, depth)) return 0;
    memset(merges, 0, sizeof(merges));
    while (parser->line_index < parser->line_count) {
        QwYamlLine *line = &parser->lines[parser->line_index];
        size_t colon;
        if (line->indent < indent) break;
        if (line->indent != indent || yaml_is_sequence_line(line)) break;
        colon = yaml_find_pair_colon(line->text, line->size);
        if (colon == SIZE_MAX) {
            parser->error = QW_STATUS_ERR_FORMAT;
            goto failure;
        }
        ++parser->line_index;
        if (colon == 2u && line->text[0] == '<' && line->text[1] == '<') {
            size_t value_start = colon + 1u;
            size_t value_size = line->size - value_start;
            QwDataValue merge;
            memset(&merge, 0, sizeof(merge));
            while (value_size && (line->text[value_start] == ' ' ||
                                  line->text[value_start] == '\t')) {
                ++value_start;
                --value_size;
            }
            if (!value_size ||
                !yaml_parse_inline(parser, line->text + value_start,
                                   value_size, &merge, depth + 1u) ||
                merge.kind != QW_DATA_VALUE_OBJECT) {
                qw_data_value_dispose(&merge);
                parser->error = QW_STATUS_ERR_FORMAT;
                goto failure;
            }
            if (merge_count >= sizeof(merges) / sizeof(merges[0])) {
                qw_data_value_dispose(&merge);
                parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
                goto failure;
            }
            merges[merge_count++] = merge;
            continue;
        }
        if (!yaml_parse_pair_text(parser, line->text, line->size, indent,
                                  out_value, depth)) goto failure;
    }
    for (merge_index = 0u; merge_index < merge_count; ++merge_index) {
        QwDataValue *merge = &merges[merge_index];
        size_t member_index;
        for (member_index = 0u; member_index < merge->value.object.count;
             ++member_index) {
            QwDataMember *member = &merge->value.object.items[member_index];
            if (!member->key) continue;
            if (!yaml_object_append_owned(parser, out_value, member->key,
                                          member->key_size, &member->value)) {
                if (parser->error == QW_STATUS_ERR_FORMAT) {
                    parser->error = 0;
                    continue;
                }
                goto cleanup;
            }
            member->key = NULL;
            member->key_size = 0u;
        }
    }
    for (merge_index = 0u; merge_index < merge_count; ++merge_index)
        qw_data_value_dispose(&merges[merge_index]);
    return 1;
cleanup:
failure:
    for (merge_index = 0u; merge_index < merge_count; ++merge_index)
        qw_data_value_dispose(&merges[merge_index]);
    return 0;
}

static int yaml_merge_object(QwYamlParser *parser, QwDataValue *target,
                             QwDataValue *extra)
{
    size_t i;
    if (extra->kind != QW_DATA_VALUE_OBJECT) {
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    for (i = 0u; i < extra->value.object.count; ++i) {
        QwDataMember *member = &extra->value.object.items[i];
        if (!yaml_object_append_owned(parser, target, member->key,
                                      member->key_size, &member->value)) return 0;
        member->key = NULL;
        member->key_size = 0u;
    }
    yaml_release(parser, extra->value.object.items,
                 extra->value.object.capacity * sizeof(QwDataMember));
    if (parser->document->node_count) --parser->document->node_count;
    extra->value.object.items = NULL;
    extra->value.object.count = 0u;
    extra->value.object.capacity = 0u;
    return 1;
}

static int yaml_parse_sequence(QwYamlParser *parser, size_t indent,
                               QwDataValue *out_value, size_t depth)
{
    if (!yaml_new_value(parser, out_value, QW_DATA_VALUE_ARRAY, depth)) return 0;
    while (parser->line_index < parser->line_count) {
        QwYamlLine *line = &parser->lines[parser->line_index];
        const char *item_text;
        size_t item_size;
        QwDataValue item;
        if (line->indent != indent || !yaml_is_sequence_line(line)) break;
        item_text = line->text + 1u;
        item_size = line->size - 1u;
        while (item_size && (*item_text == ' ' || *item_text == '\t')) {
            ++item_text; --item_size;
        }
        {
            char *anchor_name = NULL;
            size_t anchor_size = 0u;
            int anchored = 0;
            if (item_size && *item_text == '&') {
                size_t after = 0u;
                if (!yaml_anchor_name_parse(parser, item_text, item_size, 0u,
                                            &after, &anchor_name,
                                            &anchor_size)) return 0;
                anchored = 1;
                item_text += after;
                item_size -= after;
                while (item_size && (*item_text == ' ' || *item_text == '\t')) {
                    ++item_text;
                    --item_size;
                }
            }
            ++parser->line_index;
            memset(&item, 0, sizeof(item));
            if (!item_size) {
            if (parser->line_index < parser->line_count &&
                parser->lines[parser->line_index].indent > indent) {
                if (!yaml_parse_block(parser,
                        parser->lines[parser->line_index].indent,
                        &item, depth + 1u)) {
                    qw_data_value_dispose(&item);
                    return 0;
                }
            } else if (!yaml_new_value(parser, &item, QW_DATA_VALUE_NULL,
                                       depth + 1u)) return 0;
        } else if (yaml_find_pair_colon(item_text, item_size) != SIZE_MAX) {
            QwDataValue extra;
            memset(&extra, 0, sizeof(extra));
            if (!yaml_new_value(parser, &item, QW_DATA_VALUE_OBJECT, depth + 1u))
                return 0;
            if (!yaml_parse_pair_text(parser, item_text, item_size, indent + 2u,
                                      &item, depth + 1u)) {
                qw_data_value_dispose(&item);
                return 0;
            }
            if (parser->line_index < parser->line_count &&
                parser->lines[parser->line_index].indent > indent) {
                if (parser->lines[parser->line_index].indent != indent + 2u ||
                    !yaml_parse_block(parser, indent + 2u, &extra, depth + 1u) ||
                    !yaml_merge_object(parser, &item, &extra)) {
                    qw_data_value_dispose(&extra);
                    qw_data_value_dispose(&item);
                    if (!parser->error) parser->error = QW_STATUS_ERR_FORMAT;
                    return 0;
                }
                qw_data_value_dispose(&extra);
            }
        } else {
            if (!yaml_parse_inline(parser, item_text, item_size,
                                   &item, depth + 1u)) {
                if (anchored)
                    yaml_release(parser, anchor_name, anchor_size + 1u);
                return 0;
            }
            if (parser->line_index < parser->line_count &&
                parser->lines[parser->line_index].indent > indent) {
                qw_data_value_dispose(&item);
                if (anchored)
                    yaml_release(parser, anchor_name, anchor_size + 1u);
                parser->error = QW_STATUS_ERR_FORMAT;
                return 0;
            }
        }
        if (anchored &&
            !yaml_anchor_store(parser, anchor_name, anchor_size, &item)) {
            qw_data_value_dispose(&item);
            return 0;
        }
        if (!yaml_array_append(parser, out_value, &item)) {
            qw_data_value_dispose(&item);
            return 0;
        }
        }
    }
    return 1;
}

static int yaml_parse_block(QwYamlParser *parser, size_t indent,
                            QwDataValue *out_value, size_t depth)
{
    QwYamlLine *line;
    if (depth > QW_DATA_DOCUMENT_MAX_DEPTH ||
        parser->line_index >= parser->line_count) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    line = &parser->lines[parser->line_index];
    if (line->indent != indent) { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
    if (yaml_is_sequence_line(line))
        return yaml_parse_sequence(parser, indent, out_value, depth);
    if (yaml_find_pair_colon(line->text, line->size) != SIZE_MAX)
        return yaml_parse_mapping(parser, indent, out_value, depth);
    ++parser->line_index;
    return yaml_parse_inline(parser, line->text, line->size, out_value, depth);
}

static void yaml_strip_comment(const char *text, size_t *size)
{
    char quote = '\0';
    int scalar_start = 1;
    size_t i;
    for (i = 0u; i < *size; ++i) {
        char ch = text[i];
        if (quote == '"' && ch == '\\') { ++i; continue; }
        if (quote) {
            if (ch == quote) {
                if (quote == '\'' && i + 1u < *size && text[i + 1u] == '\'') {
                    ++i; continue;
                }
                quote = '\0';
            }
            continue;
        }
        if (scalar_start && (ch == '\'' || ch == '"')) {
            quote = ch;
            scalar_start = 0;
            continue;
        }
        if (ch == '#' && (i == 0u || text[i - 1u] == ' ' || text[i - 1u] == '\t')) {
            *size = i;
            return;
        }
        if (ch == ':' && i + 1u < *size &&
            (text[i + 1u] == ' ' || text[i + 1u] == '\t')) {
            scalar_start = 1;
        } else if (ch == ',' || ch == '[' || ch == '{' ||
                   (ch == '-' && (i == 0u || text[i - 1u] == ' ') &&
                    (i + 1u == *size || text[i + 1u] == ' '))) {
            scalar_start = 1;
        } else if (ch != ' ' && ch != '\t') {
            scalar_start = 0;
        }
    }
}

static int yaml_prepare_lines(QwYamlParser *parser)
{
    size_t physical_count = 0u;
    size_t position = 0u;
    size_t used = 0u;
    int ended = 0;
    while (position < parser->input_size) {
        ++physical_count;
        while (position < parser->input_size && parser->input[position] != '\n' &&
               parser->input[position] != '\r') ++position;
        if (position < parser->input_size && parser->input[position] == '\r') ++position;
        if (position < parser->input_size && parser->input[position] == '\n') ++position;
    }
    if (physical_count > QW_DATA_DOCUMENT_MAX_NODES ||
        physical_count > SIZE_MAX / sizeof(QwYamlLine)) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (physical_count) {
        parser->lines = (QwYamlLine *)calloc(physical_count, sizeof(QwYamlLine));
        if (!parser->lines) { parser->error = QW_STATUS_ERR_OUT_OF_MEMORY; return 0; }
    }
    position = 0u;
    if (parser->input_size >= 3u && parser->input[0] == 0xefu &&
        parser->input[1] == 0xbbu && parser->input[2] == 0xbfu)
        position = 3u;
    while (position < parser->input_size) {
        size_t start = position;
        size_t length;
        size_t indent = 0u;
        while (position < parser->input_size && parser->input[position] != '\n' &&
               parser->input[position] != '\r') ++position;
        length = position - start;
        if (position < parser->input_size && parser->input[position] == '\r') ++position;
        if (position < parser->input_size && parser->input[position] == '\n') ++position;
        while (indent < length && parser->input[start + indent] == ' ') ++indent;
        if (indent < length && parser->input[start + indent] == '\t') {
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        start += indent;
        length -= indent;
        yaml_strip_comment((const char *)parser->input + start, &length);
        while (length && (parser->input[start + length - 1u] == ' ' ||
                          parser->input[start + length - 1u] == '\t')) --length;
        if (!length) continue;
        if (ended) { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
        if (length >= 3u && memcmp(parser->input + start, "---", 3u) == 0 &&
            (length == 3u || parser->input[start + 3u] == ' ' ||
             parser->input[start + 3u] == '\t')) {
            if (used) { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
            if (length == 3u) continue;
            start += 3u;
            length -= 3u;
            while (length && (parser->input[start] == ' ' ||
                              parser->input[start] == '\t')) {
                ++start; --length;
            }
            if (!length) continue;
        }
        if (length == 3u && memcmp(parser->input + start, "...", 3u) == 0) {
            ended = 1;
            continue;
        }
        if (parser->input[start] == '%') {
            parser->error = QW_STATUS_ERR_UNSUPPORTED;
            return 0;
        }
        parser->lines[used].text = (const char *)parser->input + start;
        parser->lines[used].size = length;
        parser->lines[used].indent = indent;
        ++used;
    }
    parser->line_count = used;
    return 1;
}

static int yaml_parse_document(const void *input, size_t size,
                               QwInputFormat input_format,
                               QwDataDocument *out_document)
{
    QwYamlParser parser;
    int rc = QW_STATUS_ERR_FORMAT;
    memset(&parser, 0, sizeof(parser));
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    qw_data_document_init(out_document, input_format);
    if (!input || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (size > QW_YAML_MAX_INPUT) return QW_STATUS_ERR_RESOURCE_LIMIT;
    if (!yaml_utf8_valid((const uint8_t *)input, size)) return QW_STATUS_ERR_FORMAT;
    parser.input = (const uint8_t *)input;
    parser.input_size = size;
    parser.document = out_document;
    if (!yaml_prepare_lines(&parser)) goto failure;
    if (!parser.line_count) {
        if (!yaml_new_value(&parser, &out_document->root,
                            QW_DATA_VALUE_NULL, 0u)) goto failure;
    } else {
        size_t indent = parser.lines[0].indent;
        if (!yaml_parse_block(&parser, indent, &out_document->root, 0u) ||
            parser.line_index != parser.line_count) goto failure;
    }
    yaml_anchors_dispose(&parser);
    out_document->node_count = yaml_value_node_count(&out_document->root);
    if (!qw_data_document_validate(out_document)) goto failure;
    rc = QW_STATUS_OK;
    free(parser.lines);
    return rc;
failure:
    yaml_anchors_dispose(&parser);
    if (parser.lines) free(parser.lines);
    if (parser.error) rc = parser.error;
    qw_data_document_dispose(out_document);
    return rc;
}

static int yaml_probe(const void *input, size_t size)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t position = 0u;
    if (!data || !size || size > QW_YAML_MAX_INPUT) return 0;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    while (position < size) {
        size_t start = position;
        size_t length;
        while (position < size && data[position] != '\n' && data[position] != '\r')
            ++position;
        length = position - start;
        if (position < size && data[position] == '\r') ++position;
        if (position < size && data[position] == '\n') ++position;
        while (length && data[start] == ' ') { ++start; --length; }
        if (length && data[start] == '\t') return 0;
        yaml_strip_comment((const char *)data + start, &length);
        while (length && (data[start + length - 1u] == ' ' ||
                          data[start + length - 1u] == '\t')) --length;
        if (!length) continue;
        if ((length >= 3u && memcmp(data + start, "---", 3u) == 0 &&
             (length == 3u || data[start + 3u] == ' ' ||
              data[start + 3u] == '\t')) ||
            data[start] == '[' || data[start] == '{' ||
            (data[start] == '-' &&
             (length == 1u || data[start + 1u] == ' ' ||
              data[start + 1u] == '\t')) ||
            yaml_find_pair_colon((const char *)data + start, length) != SIZE_MAX)
            return 1;
        return 0;
    }
    return 0;
}

static int yaml_extension_equal(const char *extension, size_t size,
                                const char *expected)
{
    size_t i;
    if (size != strlen(expected)) return 0;
    for (i = 0u; i < size; ++i) {
        char actual = extension[i];
        char wanted = expected[i];
        if (actual >= 'A' && actual <= 'Z') actual = (char)(actual + ('a' - 'A'));
        if (wanted >= 'A' && wanted <= 'Z') wanted = (char)(wanted + ('a' - 'A'));
        if (actual != wanted) return 0;
    }
    return 1;
}

static int yaml_path_probe(const char *path)
{
    const char *base;
    const char *cursor;
    const char *dot;
    size_t extension_size;
    if (!path || !*path) return 0;
    base = path;
    for (cursor = path; *cursor; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') base = cursor + 1u;
    }
    dot = strrchr(base, '.');
    if (!dot) return 0;
    extension_size = strlen(dot);
    return yaml_extension_equal(dot, extension_size, ".yaml") ||
           yaml_extension_equal(dot, extension_size, ".yml");
}

static int yaml_open_data(const void *input, size_t size,
                          QwInputFormat input_format,
                          const QwConvertOptions *options,
                          QwDataDocument *out_document,
                          QwConvertReport *report)
{
    int rc;
    (void)options;
    rc = yaml_parse_document(input, size, input_format, out_document);
    if (rc == QW_STATUS_OK && report) {
        report->format = input_format;
        report->output_format = QW_FORMAT_UNKNOWN;
        report->output_size = 0u;
        (void)snprintf(report->note, sizeof(report->note),
                       "YAML 1.2 data opened through the shared typed value model");
    }
    return rc;
}

int qw_yaml_parse_data_document(const void *data, size_t size,
                                QwDataDocument *out_document)
{
    return yaml_parse_document(data, size, QW_FORMAT_YAML, out_document);
}

#ifndef QUIREWISE_STANDALONE_DATA_PARSER
static int yaml_buffer_append(QwYamlBuffer *buffer, const char *text, size_t size)
{
    size_t needed;
    size_t capacity;
    char *grown;
    if (size > buffer->limit - buffer->size ||
        buffer->size > SIZE_MAX - size - 1u) {
        buffer->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    needed = buffer->size + size + 1u;
    if (needed > buffer->capacity) {
        capacity = buffer->capacity ? buffer->capacity : 256u;
        while (capacity < needed) {
            if (capacity > buffer->limit / 2u) { capacity = buffer->limit + 1u; break; }
            capacity *= 2u;
        }
        if (capacity < needed || capacity <= buffer->capacity) {
            buffer->error = QW_STATUS_ERR_RESOURCE_LIMIT;
            return 0;
        }
        grown = (char *)realloc(buffer->data, capacity);
        if (!grown) { buffer->error = QW_STATUS_ERR_OUT_OF_MEMORY; return 0; }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    if (size) memcpy(buffer->data + buffer->size, text, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
    return 1;
}

static int yaml_buffer_indent(QwYamlBuffer *buffer, size_t indent)
{
    static const char spaces[] = "                                                                ";
    while (indent >= sizeof(spaces) - 1u) {
        if (!yaml_buffer_append(buffer, spaces, sizeof(spaces) - 1u)) return 0;
        indent -= sizeof(spaces) - 1u;
    }
    return !indent || yaml_buffer_append(buffer, spaces, indent);
}

static int yaml_write_string(QwYamlBuffer *buffer, const char *text, size_t size)
{
    size_t i;
    if (!yaml_buffer_append(buffer, "\"", 1u)) return 0;
    for (i = 0u; i < size; ++i) {
        unsigned char ch = (unsigned char)text[i];
        const char *escape = NULL;
        size_t escape_size = 0u;
        char unicode_escape[6];
        static const char hex[] = "0123456789abcdef";
        if (ch == '"') { escape = "\\\""; escape_size = 2u; }
        else if (ch == '\\') { escape = "\\\\"; escape_size = 2u; }
        else if (ch == '\b') { escape = "\\b"; escape_size = 2u; }
        else if (ch == '\f') { escape = "\\f"; escape_size = 2u; }
        else if (ch == '\n') { escape = "\\n"; escape_size = 2u; }
        else if (ch == '\r') { escape = "\\r"; escape_size = 2u; }
        else if (ch == '\t') { escape = "\\t"; escape_size = 2u; }
        if (escape) {
            if (!yaml_buffer_append(buffer, escape, escape_size)) return 0;
        } else if (ch < 0x20u) {
            unicode_escape[0] = '\\'; unicode_escape[1] = 'u';
            unicode_escape[2] = '0'; unicode_escape[3] = '0';
            unicode_escape[4] = hex[ch >> 4]; unicode_escape[5] = hex[ch & 0x0fu];
            if (!yaml_buffer_append(buffer, unicode_escape, sizeof(unicode_escape)))
                return 0;
        } else if (!yaml_buffer_append(buffer, text + i, 1u)) {
            return 0;
        }
    }
    return yaml_buffer_append(buffer, "\"", 1u);
}

static int yaml_write_scalar(QwYamlBuffer *buffer, const QwDataValue *value)
{
    switch (value->kind) {
    case QW_DATA_VALUE_NULL:
        return yaml_buffer_append(buffer, "null", 4u);
    case QW_DATA_VALUE_BOOLEAN:
        return value->value.boolean
            ? yaml_buffer_append(buffer, "true", 4u)
            : yaml_buffer_append(buffer, "false", 5u);
    case QW_DATA_VALUE_NUMBER:
        return yaml_buffer_append(buffer, value->value.text.data,
                                  value->value.text.size);
    case QW_DATA_VALUE_STRING:
    case QW_DATA_VALUE_TEMPORAL:
        return yaml_write_string(buffer, value->value.text.data,
                                 value->value.text.size);
    default:
        return 0;
    }
}

static int yaml_is_empty_collection(const QwDataValue *value)
{
    return (value->kind == QW_DATA_VALUE_ARRAY && !value->value.array.count) ||
           (value->kind == QW_DATA_VALUE_OBJECT && !value->value.object.count);
}

static int yaml_write_block(QwYamlBuffer *buffer, const QwDataValue *value,
                            size_t indent, size_t depth);

static int yaml_write_inline_collection(QwYamlBuffer *buffer,
                                        const QwDataValue *value)
{
    return value->kind == QW_DATA_VALUE_ARRAY
        ? yaml_buffer_append(buffer, "[]", 2u)
        : yaml_buffer_append(buffer, "{}", 2u);
}

static int yaml_write_block(QwYamlBuffer *buffer, const QwDataValue *value,
                            size_t indent, size_t depth)
{
    size_t i;
    if (!value || depth > QW_DATA_DOCUMENT_MAX_DEPTH) return 0;
    if (value->kind != QW_DATA_VALUE_ARRAY &&
        value->kind != QW_DATA_VALUE_OBJECT) {
        return yaml_buffer_indent(buffer, indent) &&
               yaml_write_scalar(buffer, value) &&
               yaml_buffer_append(buffer, "\n", 1u);
    }
    if (yaml_is_empty_collection(value)) {
        return yaml_buffer_indent(buffer, indent) &&
               yaml_write_inline_collection(buffer, value) &&
               yaml_buffer_append(buffer, "\n", 1u);
    }
    if (value->kind == QW_DATA_VALUE_ARRAY) {
        for (i = 0u; i < value->value.array.count; ++i) {
            const QwDataValue *item = &value->value.array.items[i];
            if (!yaml_buffer_indent(buffer, indent) ||
                !yaml_buffer_append(buffer, "-", 1u)) return 0;
            if (item->kind == QW_DATA_VALUE_ARRAY ||
                item->kind == QW_DATA_VALUE_OBJECT) {
                if (yaml_is_empty_collection(item)) {
                    if (!yaml_buffer_append(buffer, " ", 1u) ||
                        !yaml_write_inline_collection(buffer, item) ||
                        !yaml_buffer_append(buffer, "\n", 1u)) return 0;
                } else if (!yaml_buffer_append(buffer, "\n", 1u) ||
                           !yaml_write_block(buffer, item, indent + 2u,
                                             depth + 1u)) return 0;
            } else if (!yaml_buffer_append(buffer, " ", 1u) ||
                       !yaml_write_scalar(buffer, item) ||
                       !yaml_buffer_append(buffer, "\n", 1u)) return 0;
        }
        return 1;
    }
    for (i = 0u; i < value->value.object.count; ++i) {
        const QwDataMember *member = &value->value.object.items[i];
        const QwDataValue *member_value = &member->value;
        if (!yaml_buffer_indent(buffer, indent) ||
            !yaml_write_string(buffer, member->key, member->key_size) ||
            !yaml_buffer_append(buffer, ":", 1u)) return 0;
        if (member_value->kind == QW_DATA_VALUE_ARRAY ||
            member_value->kind == QW_DATA_VALUE_OBJECT) {
            if (yaml_is_empty_collection(member_value)) {
                if (!yaml_buffer_append(buffer, " ", 1u) ||
                    !yaml_write_inline_collection(buffer, member_value) ||
                    !yaml_buffer_append(buffer, "\n", 1u)) return 0;
            } else if (!yaml_buffer_append(buffer, "\n", 1u) ||
                       !yaml_write_block(buffer, member_value, indent + 2u,
                                         depth + 1u)) return 0;
        } else if (!yaml_buffer_append(buffer, " ", 1u) ||
                   !yaml_write_scalar(buffer, member_value) ||
                   !yaml_buffer_append(buffer, "\n", 1u)) return 0;
    }
    return 1;
}

static int yaml_data_write_document(const QwDataDocument *document,
                                    QwOutputFormat output_format,
                                    const QwConvertOptions *options,
                                    uint8_t **out_data, size_t *out_size,
                                    QwConvertReport *report)
{
    QwYamlBuffer buffer;
    (void)options;
    if (!document || !out_data || !out_size || output_format != QW_FORMAT_YAML)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    memset(&buffer, 0, sizeof(buffer));
    buffer.limit = QW_YAML_MAX_OUTPUT;
    if (!yaml_write_block(&buffer, &document->root, 0u, 0u)) {
        int rc = buffer.error ? buffer.error : QW_STATUS_ERR_OUT_OF_MEMORY;
        free(buffer.data);
        return rc;
    }
    *out_data = (uint8_t *)buffer.data;
    *out_size = buffer.size;
    if (report) {
        report->format = document->input_format;
        report->output_format = output_format;
        report->output_size = buffer.size;
        (void)snprintf(report->note, sizeof(report->note),
                       "YAML 1.2 block document written with quoted string scalars");
    }
    return QW_STATUS_OK;
}

const QwDataWriterDescriptor qw_data_yaml_writer = {
    QW_FORMAT_YAML, "YAML data writer", yaml_data_write_document,
    QW_YAML_MAX_OUTPUT
};

const QwFormatDescriptor qw_format_yaml = {
    .id = QW_FORMAT_YAML,
    .name = "YAML",
    .capabilities = 0u,
    .probe = yaml_probe,
    .open_memory = NULL,
    .open_layout = NULL,
    .open_audio = NULL,
    .max_input_bytes = QW_YAML_MAX_INPUT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .convert_to_pdf = NULL,
    .open_data = yaml_open_data
};

const QwPathFormatDescriptor qw_path_format_yaml = {
    .id = QW_FORMAT_YAML,
    .probe_path = yaml_path_probe,
    .decode_document_path = NULL,
    .resource_profile = NULL
};
#endif
