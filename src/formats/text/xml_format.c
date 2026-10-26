#include "../../core/data_document.h"
#include "../../core/xml_parser.h"
#include "../../core/format_registry.h"
#ifndef QUIREWISE_STANDALONE_DATA_PARSER
#include "../../core/writer_registry.h"
#endif
#include "xml_format_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QW_XML_MAX_INPUT QW_FORMAT_INPUT_LIMIT_TEXT
#define QW_XML_MAX_OUTPUT QW_DATA_DOCUMENT_MAX_MEMORY
#define QW_XML_DOCUMENT_KEY "$xml"
#define QW_XML_NAME_KEY "$name"
#define QW_XML_ATTRIBUTES_KEY "$attributes"
#define QW_XML_CONTENT_KEY "$content"

typedef struct QwXmlParser {
    const uint8_t *input;
    size_t size;
    size_t position;
    QwDataDocument *document;
    int error;
} QwXmlParser;

typedef struct QwXmlBuffer {
    char *data;
    size_t size;
    size_t capacity;
} QwXmlBuffer;

typedef struct QwXmlOutput {
    char *data;
    size_t size;
    size_t capacity;
    size_t limit;
    int error;
} QwXmlOutput;

static void *xml_allocate(QwXmlParser *parser, size_t size)
{
    void *memory;
    if (!size) size = 1u;
    if (!parser->document ||
        size > QW_DATA_DOCUMENT_MAX_MEMORY - parser->document->allocation_bytes) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return NULL;
    }
    memory = malloc(size);
    if (!memory) { parser->error = QW_STATUS_ERR_OUT_OF_MEMORY; return NULL; }
    parser->document->allocation_bytes += size;
    return memory;
}

static void *xml_reallocate(QwXmlParser *parser, void *memory,
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
    if (!grown) { parser->error = QW_STATUS_ERR_OUT_OF_MEMORY; return NULL; }
    parser->document->allocation_bytes += new_size - old_size;
    return grown;
}

static void xml_release(QwXmlParser *parser, void *memory, size_t size)
{
    if (!memory) return;
    free(memory);
    if (parser->document && size <= parser->document->allocation_bytes)
        parser->document->allocation_bytes -= size;
}

static int xml_buffer_reserve(QwXmlParser *parser, QwXmlBuffer *buffer,
                              size_t needed)
{
    size_t capacity;
    char *grown;
    if (needed <= buffer->capacity) return 1;
    capacity = buffer->capacity ? buffer->capacity : 32u;
    while (capacity < needed) {
        if (capacity > QW_DATA_DOCUMENT_MAX_MEMORY / 2u) {
            capacity = QW_DATA_DOCUMENT_MAX_MEMORY;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < needed) { parser->error = QW_STATUS_ERR_RESOURCE_LIMIT; return 0; }
    grown = (char *)xml_reallocate(parser, buffer->data,
                                   buffer->capacity, capacity);
    if (!grown) return 0;
    buffer->data = grown;
    buffer->capacity = capacity;
    return 1;
}

static int xml_buffer_append(QwXmlParser *parser, QwXmlBuffer *buffer,
                             const char *text, size_t size)
{
    if (size > SIZE_MAX - buffer->size - 1u ||
        !xml_buffer_reserve(parser, buffer, buffer->size + size + 1u)) return 0;
    if (size) memcpy(buffer->data + buffer->size, text, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
    return 1;
}

static int xml_buffer_finish(QwXmlParser *parser, QwXmlBuffer *buffer)
{
    if (!xml_buffer_reserve(parser, buffer, buffer->size + 1u)) return 0;
    buffer->data[buffer->size] = '\0';
    return 1;
}

static void xml_buffer_dispose(QwXmlParser *parser, QwXmlBuffer *buffer)
{
    xml_release(parser, buffer->data, buffer->capacity);
    memset(buffer, 0, sizeof(*buffer));
}

static int xml_decode_utf8(const uint8_t *data, size_t size,
                           size_t *position, uint32_t *out_codepoint)
{
    uint8_t first;
    uint32_t codepoint;
    size_t count;
    size_t i;
    if (*position >= size) return 0;
    first = data[*position];
    if (first < 0x80u) { codepoint = first; count = 1u; }
    else if (first >= 0xc2u && first <= 0xdfu) {
        codepoint = first & 0x1fu; count = 2u;
    } else if (first >= 0xe0u && first <= 0xefu) {
        codepoint = first & 0x0fu; count = 3u;
    } else if (first >= 0xf0u && first <= 0xf4u) {
        codepoint = first & 0x07u; count = 4u;
    } else return 0;
    if (count > size - *position) return 0;
    for (i = 1u; i < count; ++i) {
        uint8_t next = data[*position + i];
        if ((next & 0xc0u) != 0x80u) return 0;
        codepoint = (codepoint << 6) | (next & 0x3fu);
    }
    *position += count;
    *out_codepoint = codepoint;
    return 1;
}

static int xml_character_valid(uint32_t cp)
{
    return cp == 0x9u || cp == 0xau || cp == 0xdu ||
           (cp >= 0x20u && cp <= 0xd7ffu) ||
           (cp >= 0xe000u && cp <= 0xfffdu) ||
           (cp >= 0x10000u && cp <= 0x10ffffu);
}

static int xml_utf8_valid(const uint8_t *data, size_t size)
{
    size_t position = 0u;
    while (position < size) {
        uint32_t cp;
        if (!xml_decode_utf8(data, size, &position, &cp) ||
            !xml_character_valid(cp)) return 0;
    }
    return 1;
}

static int xml_name_start(uint32_t cp)
{
    return cp == ':' || cp == '_' || (cp >= 'A' && cp <= 'Z') ||
           (cp >= 'a' && cp <= 'z') ||
           (cp >= 0xc0u && cp <= 0xd6u) || (cp >= 0xd8u && cp <= 0xf6u) ||
           (cp >= 0xf8u && cp <= 0x2ffu) || (cp >= 0x370u && cp <= 0x37du) ||
           (cp >= 0x37fu && cp <= 0x1fffu) ||
           (cp >= 0x200cu && cp <= 0x200du) ||
           (cp >= 0x2070u && cp <= 0x218fu) ||
           (cp >= 0x2c00u && cp <= 0x2fefu) ||
           (cp >= 0x3001u && cp <= 0xd7ffu) ||
           (cp >= 0xf900u && cp <= 0xfdcfu) ||
           (cp >= 0xfdf0u && cp <= 0xfffdu) ||
           (cp >= 0x10000u && cp <= 0xeffffu);
}

static int xml_name_char(uint32_t cp)
{
    return xml_name_start(cp) || cp == '-' || cp == '.' ||
           (cp >= '0' && cp <= '9') || cp == 0xb7u ||
           (cp >= 0x300u && cp <= 0x36fu) ||
           (cp >= 0x203fu && cp <= 0x2040u);
}

static int xml_new_value(QwXmlParser *parser, QwDataValue *value,
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

static int xml_set_text(QwXmlParser *parser, QwDataValue *value,
                        const char *text, size_t size, size_t depth)
{
    char *copy;
    if (!xml_new_value(parser, value, QW_DATA_VALUE_STRING, depth)) return 0;
    if (size == SIZE_MAX) { parser->error = QW_STATUS_ERR_RESOURCE_LIMIT; return 0; }
    copy = (char *)xml_allocate(parser, size + 1u);
    if (!copy) return 0;
    if (size) memcpy(copy, text, size);
    copy[size] = '\0';
    value->value.text.data = copy;
    value->value.text.size = size;
    return 1;
}

static int xml_array_reserve(QwXmlParser *parser, QwDataValue *array,
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
    items = (QwDataValue *)xml_reallocate(parser, array->value.array.items,
                                          old_size, new_size);
    if (!items) return 0;
    array->value.array.items = items;
    array->value.array.capacity = capacity;
    return 1;
}

static int xml_object_reserve(QwXmlParser *parser, QwDataValue *object,
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
    items = (QwDataMember *)xml_reallocate(parser, object->value.object.items,
                                           old_size, new_size);
    if (!items) return 0;
    object->value.object.items = items;
    object->value.object.capacity = capacity;
    return 1;
}

static QwDataMember *xml_object_find(QwDataValue *object,
                                    const char *key, size_t key_size)
{
    size_t i;
    if (!object || object->kind != QW_DATA_VALUE_OBJECT) return NULL;
    for (i = 0u; i < object->value.object.count; ++i) {
        QwDataMember *member = &object->value.object.items[i];
        if (member->key_size == key_size &&
            memcmp(member->key, key, key_size) == 0) return member;
    }
    return NULL;
}

static int xml_object_append_owned(QwXmlParser *parser, QwDataValue *object,
                                   char *key, size_t key_size,
                                   size_t key_allocation, QwDataValue *value)
{
    QwDataMember *member;
    if (xml_object_find(object, key, key_size)) {
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    if (object->value.object.count >= QW_DATA_DOCUMENT_MAX_NODES ||
        !xml_object_reserve(parser, object, object->value.object.count + 1u))
        return 0;
    member = &object->value.object.items[object->value.object.count++];
    member->key = key;
    member->key_size = key_size;
    member->value = *value;
    (void)key_allocation;
    memset(value, 0, sizeof(*value));
    return 1;
}

static int xml_array_append(QwXmlParser *parser, QwDataValue *array,
                            QwDataValue *value)
{
    if (array->value.array.count >= QW_DATA_DOCUMENT_MAX_NODES ||
        !xml_array_reserve(parser, array, array->value.array.count + 1u))
        return 0;
    array->value.array.items[array->value.array.count++] = *value;
    memset(value, 0, sizeof(*value));
    return 1;
}

static int xml_encode_codepoint(QwXmlParser *parser, QwXmlBuffer *buffer,
                                uint32_t cp)
{
    char bytes[4];
    size_t count;
    if (!xml_character_valid(cp)) {
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    if (cp <= 0x7fu) { bytes[0] = (char)cp; count = 1u; }
    else if (cp <= 0x7ffu) {
        bytes[0] = (char)(0xc0u | (cp >> 6));
        bytes[1] = (char)(0x80u | (cp & 0x3fu));
        count = 2u;
    } else if (cp <= 0xffffu) {
        bytes[0] = (char)(0xe0u | (cp >> 12));
        bytes[1] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        bytes[2] = (char)(0x80u | (cp & 0x3fu));
        count = 3u;
    } else {
        bytes[0] = (char)(0xf0u | (cp >> 18));
        bytes[1] = (char)(0x80u | ((cp >> 12) & 0x3fu));
        bytes[2] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        bytes[3] = (char)(0x80u | (cp & 0x3fu));
        count = 4u;
    }
    return xml_buffer_append(parser, buffer, bytes, count);
}

static int xml_read_name(QwXmlParser *parser, QwXmlBuffer *name)
{
    int first = 1;
    size_t start;
    memset(name, 0, sizeof(*name));
    start = parser->position;
    while (parser->position < parser->size) {
        size_t next = parser->position;
        uint32_t cp;
        if (!xml_decode_utf8(parser->input, parser->size, &next, &cp)) break;
        if (first ? !xml_name_start(cp) : !xml_name_char(cp)) break;
        if (!xml_buffer_append(parser, name,
                (const char *)parser->input + parser->position,
                next - parser->position)) {
            xml_buffer_dispose(parser, name);
            return 0;
        }
        parser->position = next;
        first = 0;
    }
    if (parser->position == start || !xml_buffer_finish(parser, name)) {
        xml_buffer_dispose(parser, name);
        if (!parser->error) parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    return 1;
}

static int xml_name_valid(const char *name, size_t size)
{
    size_t position = 0u;
    int first = 1;
    if (!name || !size) return 0;
    while (position < size) {
        size_t next = position;
        uint32_t cp;
        if (!xml_decode_utf8((const uint8_t *)name, size, &next, &cp) ||
            (first ? !xml_name_start(cp) : !xml_name_char(cp))) return 0;
        first = 0;
        position = next;
    }
    return !first;
}

static void xml_skip_space(QwXmlParser *parser)
{
    while (parser->position < parser->size) {
        uint8_t ch = parser->input[parser->position];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
        ++parser->position;
    }
}

static int xml_starts(const QwXmlParser *parser, const char *text, size_t size)
{
    return parser->position <= parser->size &&
           size <= parser->size - parser->position &&
           memcmp(parser->input + parser->position, text, size) == 0;
}

static int xml_append_member(QwXmlParser *parser, QwDataValue *object,
                             const char *key, size_t key_size,
                             QwDataValue *value)
{
    char *copy;
    if (key_size == SIZE_MAX) { parser->error = QW_STATUS_ERR_RESOURCE_LIMIT; return 0; }
    copy = (char *)xml_allocate(parser, key_size + 1u);
    if (!copy) return 0;
    if (key_size) memcpy(copy, key, key_size);
    copy[key_size] = '\0';
    if (!xml_object_append_owned(parser, object, copy, key_size,
                                 key_size + 1u, value)) {
        xml_release(parser, copy, key_size + 1u);
        return 0;
    }
    return 1;
}

static int xml_append_text_value(QwXmlParser *parser, QwDataValue *array,
                                 const char *text, size_t size, size_t depth)
{
    QwDataValue value;
    memset(&value, 0, sizeof(value));
    if (!size) return 1;
    if (!xml_set_text(parser, &value, text, size, depth) ||
        !xml_array_append(parser, array, &value)) {
        qw_data_value_dispose(&value);
        return 0;
    }
    return 1;
}

static int xml_parse_entity(QwXmlParser *parser, QwXmlBuffer *buffer)
{
    size_t start;
    size_t length;
    uint32_t cp = 0u;
    unsigned base = 10u;
    size_t i;
    if (parser->position >= parser->size || parser->input[parser->position] != '&')
        return 0;
    ++parser->position;
    start = parser->position;
    while (parser->position < parser->size && parser->input[parser->position] != ';' &&
           parser->position - start <= 32u) ++parser->position;
    if (parser->position >= parser->size || parser->input[parser->position] != ';') {
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    length = parser->position - start;
    ++parser->position;
    if (length == 2u && memcmp(parser->input + start, "lt", 2u) == 0) cp = '<';
    else if (length == 2u && memcmp(parser->input + start, "gt", 2u) == 0) cp = '>';
    else if (length == 3u && memcmp(parser->input + start, "amp", 3u) == 0) cp = '&';
    else if (length == 4u && memcmp(parser->input + start, "apos", 4u) == 0) cp = '\'';
    else if (length == 4u && memcmp(parser->input + start, "quot", 4u) == 0) cp = '"';
    else if (length >= 2u && parser->input[start] == '#') {
        i = start + 1u;
        if (i < start + length && (parser->input[i] == 'x' || parser->input[i] == 'X')) {
            base = 16u;
            ++i;
        }
        if (i >= start + length) { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
        for (; i < start + length; ++i) {
            unsigned digit;
            uint8_t ch = parser->input[i];
            if (ch >= '0' && ch <= '9') digit = (unsigned)(ch - '0');
            else if (base == 16u && ch >= 'a' && ch <= 'f') digit = (unsigned)(ch - 'a' + 10);
            else if (base == 16u && ch >= 'A' && ch <= 'F') digit = (unsigned)(ch - 'A' + 10);
            else { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
            if (digit >= base || cp > (0x10ffffu - digit) / base) {
                parser->error = QW_STATUS_ERR_FORMAT;
                return 0;
            }
            cp = cp * base + digit;
        }
    } else {
        parser->error = QW_STATUS_ERR_UNSUPPORTED;
        return 0;
    }
    return xml_encode_codepoint(parser, buffer, cp);
}

static int xml_skip_comment(QwXmlParser *parser)
{
    if (!xml_starts(parser, "<!--", 4u)) return 0;
    parser->position += 4u;
    while (parser->position + 2u < parser->size) {
        if (xml_starts(parser, "-->", 3u)) {
            parser->position += 3u;
            return 1;
        }
        if (xml_starts(parser, "--", 2u)) {
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        ++parser->position;
    }
    parser->error = QW_STATUS_ERR_FORMAT;
    return 0;
}

static int xml_skip_processing_instruction(QwXmlParser *parser)
{
    if (!xml_starts(parser, "<?", 2u)) return 0;
    parser->position += 2u;
    while (parser->position + 1u < parser->size) {
        if (xml_starts(parser, "?>", 2u)) {
            parser->position += 2u;
            return 1;
        }
        ++parser->position;
    }
    parser->error = QW_STATUS_ERR_FORMAT;
    return 0;
}

static int xml_parse_text(QwXmlParser *parser, QwXmlBuffer *text)
{
    memset(text, 0, sizeof(*text));
    while (parser->position < parser->size && parser->input[parser->position] != '<') {
        size_t next;
        uint32_t cp;
        if (xml_starts(parser, "]]>", 3u)) {
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        if (parser->input[parser->position] == '&') {
            if (!xml_parse_entity(parser, text)) return 0;
            continue;
        }
        if (parser->input[parser->position] == '\r') {
            ++parser->position;
            if (parser->position < parser->size && parser->input[parser->position] == '\n')
                ++parser->position;
            if (!xml_buffer_append(parser, text, "\n", 1u)) return 0;
            continue;
        }
        next = parser->position;
        if (!xml_decode_utf8(parser->input, parser->size, &next, &cp) ||
            !xml_character_valid(cp) ||
            !xml_buffer_append(parser, text,
                (const char *)parser->input + parser->position,
                next - parser->position)) {
            if (!parser->error) parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        parser->position = next;
    }
    return xml_buffer_finish(parser, text);
}

static int xml_parse_attribute_value(QwXmlParser *parser, QwXmlBuffer *text)
{
    uint8_t quote;
    memset(text, 0, sizeof(*text));
    if (parser->position >= parser->size ||
        (parser->input[parser->position] != '\'' && parser->input[parser->position] != '"')) {
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    quote = parser->input[parser->position++];
    while (parser->position < parser->size) {
        size_t next;
        uint32_t cp;
        uint8_t ch = parser->input[parser->position];
        if (ch == quote) {
            ++parser->position;
            return xml_buffer_finish(parser, text);
        }
        if (ch == '<') { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
        if (ch == '&') {
            if (!xml_parse_entity(parser, text)) return 0;
            continue;
        }
        if (ch == '\r') {
            ++parser->position;
            if (parser->position < parser->size && parser->input[parser->position] == '\n')
                ++parser->position;
            if (!xml_buffer_append(parser, text, " ", 1u)) return 0;
            continue;
        }
        if (ch == '\t' || ch == '\n') {
            ++parser->position;
            if (!xml_buffer_append(parser, text, " ", 1u)) return 0;
            continue;
        }
        next = parser->position;
        if (!xml_decode_utf8(parser->input, parser->size, &next, &cp) ||
            !xml_character_valid(cp) ||
            !xml_buffer_append(parser, text,
                (const char *)parser->input + parser->position,
                next - parser->position)) {
            if (!parser->error) parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        parser->position = next;
    }
    parser->error = QW_STATUS_ERR_FORMAT;
    return 0;
}

static int xml_parse_element(QwXmlParser *parser, QwDataValue *element,
                             size_t depth);

static int xml_parse_cdata(QwXmlParser *parser, QwXmlBuffer *text)
{
    memset(text, 0, sizeof(*text));
    if (!xml_starts(parser, "<![CDATA[", 9u)) return 0;
    parser->position += 9u;
    while (parser->position < parser->size) {
        size_t next;
        uint32_t cp;
        if (xml_starts(parser, "]]>", 3u)) {
            parser->position += 3u;
            return xml_buffer_finish(parser, text);
        }
        if (parser->input[parser->position] == '\r') {
            ++parser->position;
            if (parser->position < parser->size && parser->input[parser->position] == '\n')
                ++parser->position;
            if (!xml_buffer_append(parser, text, "\n", 1u)) return 0;
            continue;
        }
        next = parser->position;
        if (!xml_decode_utf8(parser->input, parser->size, &next, &cp) ||
            !xml_character_valid(cp) ||
            !xml_buffer_append(parser, text,
                (const char *)parser->input + parser->position,
                next - parser->position)) {
            if (!parser->error) parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        parser->position = next;
    }
    parser->error = QW_STATUS_ERR_FORMAT;
    return 0;
}

static int xml_skip_misc(QwXmlParser *parser)
{
    for (;;) {
        xml_skip_space(parser);
        if (xml_starts(parser, "<!--", 4u)) {
            if (!xml_skip_comment(parser)) return 0;
        } else if (xml_starts(parser, "<?", 2u)) {
            if (!xml_skip_processing_instruction(parser)) return 0;
        } else if (xml_starts(parser, "<!", 2u)) {
            parser->error = QW_STATUS_ERR_UNSUPPORTED;
            return 0;
        } else {
            return 1;
        }
    }
}

static int xml_parse_element(QwXmlParser *parser, QwDataValue *element,
                             size_t depth)
{
    QwXmlBuffer name;
    QwDataValue name_value;
    QwDataValue field;
    QwDataValue *attributes;
    QwDataValue *content;
    int self_closing = 0;
    memset(&name, 0, sizeof(name));
    memset(&name_value, 0, sizeof(name_value));
    memset(&field, 0, sizeof(field));
    attributes = NULL;
    content = NULL;
    if (depth > QW_DATA_DOCUMENT_MAX_DEPTH || parser->position >= parser->size ||
        parser->input[parser->position] != '<' ||
        xml_starts(parser, "</", 2u) || xml_starts(parser, "<!", 2u) ||
        xml_starts(parser, "<?", 2u)) {
        parser->error = depth > QW_DATA_DOCUMENT_MAX_DEPTH
            ? QW_STATUS_ERR_RESOURCE_LIMIT : QW_STATUS_ERR_FORMAT;
        return 0;
    }
    ++parser->position;
    if (!xml_read_name(parser, &name)) goto failure;
    if (!xml_new_value(parser, element, QW_DATA_VALUE_OBJECT, depth) ||
        !xml_set_text(parser, &name_value, name.data, name.size, depth + 1u) ||
        !xml_append_member(parser, element, QW_XML_NAME_KEY,
                           sizeof(QW_XML_NAME_KEY) - 1u, &name_value)) goto failure;
    qw_data_value_dispose(&name_value);
    xml_buffer_dispose(parser, &name);

    if (!xml_new_value(parser, &field, QW_DATA_VALUE_OBJECT, depth + 1u) ||
        !xml_append_member(parser, element, QW_XML_ATTRIBUTES_KEY,
                           sizeof(QW_XML_ATTRIBUTES_KEY) - 1u, &field)) goto failure;
    attributes = &element->value.object.items[1u].value;
    if (!xml_new_value(parser, &field, QW_DATA_VALUE_ARRAY, depth + 1u) ||
        !xml_append_member(parser, element, QW_XML_CONTENT_KEY,
                           sizeof(QW_XML_CONTENT_KEY) - 1u, &field)) goto failure;
    content = &element->value.object.items[2u].value;

    for (;;) {
        size_t before_space = parser->position;
        xml_skip_space(parser);
        if (parser->position >= parser->size) goto malformed;
        if (xml_starts(parser, "/>" , 2u)) {
            parser->position += 2u;
            self_closing = 1;
            break;
        }
        if (parser->input[parser->position] == '>') {
            ++parser->position;
            break;
        }
        if (parser->position == before_space) goto malformed;
        {
            QwXmlBuffer attribute_name;
            QwXmlBuffer attribute_text;
            QwDataValue attribute_value;
            memset(&attribute_name, 0, sizeof(attribute_name));
            memset(&attribute_text, 0, sizeof(attribute_text));
            memset(&attribute_value, 0, sizeof(attribute_value));
            if (!xml_read_name(parser, &attribute_name)) goto attribute_failure;
            xml_skip_space(parser);
            if (parser->position >= parser->size ||
                parser->input[parser->position++] != '=') goto attribute_failure;
            xml_skip_space(parser);
            if (!xml_parse_attribute_value(parser, &attribute_text)) goto attribute_failure;
            if (!xml_set_text(parser, &attribute_value, attribute_text.data,
                              attribute_text.size, depth + 2u) ||
                !xml_object_append_owned(parser, attributes,
                    attribute_name.data, attribute_name.size,
                    attribute_name.capacity, &attribute_value)) goto attribute_failure;
            attribute_name.data = NULL;
            attribute_name.size = attribute_name.capacity = 0u;
            xml_buffer_dispose(parser, &attribute_name);
            xml_buffer_dispose(parser, &attribute_text);
            qw_data_value_dispose(&attribute_value);
            continue;
attribute_failure:
            if (!parser->error) parser->error = QW_STATUS_ERR_FORMAT;
            xml_buffer_dispose(parser, &attribute_name);
            xml_buffer_dispose(parser, &attribute_text);
            qw_data_value_dispose(&attribute_value);
            goto failure;
        }
    }
    if (self_closing) {
        return 1;
    }
    for (;;) {
        if (parser->position >= parser->size) goto malformed;
        if (xml_starts(parser, "</", 2u)) {
            QwXmlBuffer close_name;
            int matches;
            parser->position += 2u;
            memset(&close_name, 0, sizeof(close_name));
            if (!xml_read_name(parser, &close_name)) goto failure;
            matches = close_name.size == element->value.object.items[0].value.value.text.size &&
                memcmp(close_name.data,
                       element->value.object.items[0].value.value.text.data,
                       close_name.size) == 0;
            xml_buffer_dispose(parser, &close_name);
            xml_skip_space(parser);
            if (!matches || parser->position >= parser->size ||
                parser->input[parser->position++] != '>') goto malformed;
            break;
        }
        if (xml_starts(parser, "<!--", 4u)) {
            if (!xml_skip_comment(parser)) goto failure;
            continue;
        }
        if (xml_starts(parser, "<?", 2u)) {
            if (!xml_skip_processing_instruction(parser)) goto failure;
            continue;
        }
        if (xml_starts(parser, "<![CDATA[", 9u)) {
            QwXmlBuffer cdata;
            memset(&cdata, 0, sizeof(cdata));
            if (!xml_parse_cdata(parser, &cdata) ||
                !xml_append_text_value(parser, content, cdata.data,
                                       cdata.size, depth + 2u)) {
                xml_buffer_dispose(parser, &cdata);
                goto failure;
            }
            xml_buffer_dispose(parser, &cdata);
            continue;
        }
        if (xml_starts(parser, "<!", 2u)) {
            parser->error = QW_STATUS_ERR_UNSUPPORTED;
            goto failure;
        }
        if (parser->input[parser->position] == '<') {
            QwDataValue child;
            memset(&child, 0, sizeof(child));
            if (!xml_parse_element(parser, &child, depth + 2u) ||
                !xml_array_append(parser, content, &child)) {
                qw_data_value_dispose(&child);
                goto failure;
            }
            continue;
        }
        {
            QwXmlBuffer text;
            memset(&text, 0, sizeof(text));
            if (!xml_parse_text(parser, &text) ||
                !xml_append_text_value(parser, content, text.data,
                                       text.size, depth + 2u)) {
                xml_buffer_dispose(parser, &text);
                goto failure;
            }
            xml_buffer_dispose(parser, &text);
        }
    }
    return 1;
malformed:
    if (!parser->error) parser->error = QW_STATUS_ERR_FORMAT;
failure:
    xml_buffer_dispose(parser, &name);
    qw_data_value_dispose(&name_value);
    qw_data_value_dispose(&field);
    return 0;
}

static int xml_parse_document_normalized(const void *input, size_t size,
                                        QwInputFormat input_format,
                                        QwDataDocument *out_document)
{
    QwXmlParser parser;
    QwDataValue root_element;
    int rc = QW_STATUS_ERR_FORMAT;
    memset(&parser, 0, sizeof(parser));
    memset(&root_element, 0, sizeof(root_element));
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    qw_data_document_init(out_document, input_format);
    if (!input || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (size > QW_XML_MAX_INPUT) return QW_STATUS_ERR_RESOURCE_LIMIT;
    if (!xml_utf8_valid((const uint8_t *)input, size)) return QW_STATUS_ERR_FORMAT;
    parser.input = (const uint8_t *)input;
    parser.size = size;
    parser.document = out_document;
    if (size >= 3u && parser.input[0] == 0xefu && parser.input[1] == 0xbbu &&
        parser.input[2] == 0xbfu) parser.position = 3u;
    if (!xml_new_value(&parser, &out_document->root,
                       QW_DATA_VALUE_OBJECT, 0u) ||
        !xml_skip_misc(&parser) ||
        !xml_parse_element(&parser, &root_element, 1u) ||
        !xml_append_member(&parser, &out_document->root,
                           QW_XML_DOCUMENT_KEY,
                           sizeof(QW_XML_DOCUMENT_KEY) - 1u,
                           &root_element) ||
        !xml_skip_misc(&parser) || parser.position != parser.size ||
        !qw_data_document_validate(out_document)) goto failure;
    rc = QW_STATUS_OK;
    return rc;
failure:
    qw_data_value_dispose(&root_element);
    if (parser.error) rc = parser.error;
    qw_data_document_dispose(out_document);
    return rc;
}

static int xml_core_error_to_page(const QwXmlError *error)
{
    if (!error) return QW_STATUS_ERR_FORMAT;
    switch (error->code) {
    case QW_XML_ERROR_ARGUMENT:
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    case QW_XML_ERROR_RESOURCE_LIMIT:
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    case QW_XML_ERROR_OUT_OF_MEMORY:
        return QW_STATUS_ERR_OUT_OF_MEMORY;
    case QW_XML_ERROR_UNSUPPORTED:
        return QW_STATUS_ERR_UNSUPPORTED;
    case QW_XML_ERROR_NONE:
    case QW_XML_ERROR_FORMAT:
    case QW_XML_ERROR_ENCODING:
    default:
        return QW_STATUS_ERR_FORMAT;
    }
}

static int xml_parse_document(const void *input, size_t size,
                              QwInputFormat input_format,
                              QwDataDocument *out_document)
{
    QwXmlError error;
    QwXmlNode *tree;
    uint8_t *normalized = NULL;
    size_t normalized_size = 0u;
    int rc;
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    qw_data_document_init(out_document, input_format);
    if (!input || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;

    tree = qw_xml_parse(input, size, &error);
    if (!tree) {
        rc = xml_core_error_to_page(&error);
        qw_data_document_dispose(out_document);
        return rc;
    }
    if (!qw_xml_serialize(tree, &normalized, &normalized_size, &error)) {
        rc = xml_core_error_to_page(&error);
        qw_xml_drop(tree);
        qw_data_document_dispose(out_document);
        return rc;
    }
    qw_xml_drop(tree);

    /* The legacy data-shape mapper now receives only XML emitted by the
     * standards parser, keeping its representation separate from grammar. */
    rc = xml_parse_document_normalized(normalized, normalized_size,
                                       input_format, out_document);
    free(normalized);
    return rc;
}

int qw_xml_parse_data(const void *input, size_t size,
                      QwDataDocument *out_document)
{
    return xml_parse_document(input, size, QW_FORMAT_XML, out_document);
}

int qw_xml_parse_data_format(const void *input, size_t size,
                             QwInputFormat input_format,
                             QwDataDocument *out_document)
{
    if (input_format != QW_FORMAT_XML && input_format != QW_FORMAT_OPML) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    return xml_parse_document(input, size, input_format, out_document);
}

#ifndef QUIREWISE_STANDALONE_DATA_PARSER
static int xml_output_append(QwXmlOutput *output, const char *text, size_t size)
{
    size_t needed;
    size_t capacity;
    char *grown;
    if (output->size > output->limit || size > output->limit - output->size ||
        size > SIZE_MAX - output->size - 1u) {
        output->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    needed = output->size + size + 1u;
    if (needed > output->capacity) {
        capacity = output->capacity ? output->capacity : 256u;
        while (capacity < needed) {
            if (capacity > output->limit / 2u) {
                capacity = output->limit + 1u;
                break;
            }
            capacity *= 2u;
        }
        if (capacity < needed || capacity <= output->capacity) {
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

static int xml_write_escaped(QwXmlOutput *output, const char *text,
                             size_t size, int attribute)
{
    size_t position = 0u;
    while (position < size) {
        uint32_t cp;
        size_t next = position;
        const char *escaped = NULL;
        size_t escaped_size = 0u;
        if (!xml_decode_utf8((const uint8_t *)text, size, &next, &cp) ||
            !xml_character_valid(cp)) {
            output->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        if (cp == '&') { escaped = "&amp;"; escaped_size = 5u; }
        else if (cp == '<') { escaped = "&lt;"; escaped_size = 4u; }
        else if (cp == '>') { escaped = "&gt;"; escaped_size = 4u; }
        else if (attribute && cp == '"') { escaped = "&quot;"; escaped_size = 6u; }
        else if (attribute && cp == '\'') { escaped = "&apos;"; escaped_size = 6u; }
        else if (cp == '\r') { escaped = "&#xD;"; escaped_size = 5u; }
        else if (attribute && cp == '\n') { escaped = "&#xA;"; escaped_size = 5u; }
        else if (attribute && cp == '\t') { escaped = "&#x9;"; escaped_size = 5u; }
        if (escaped) {
            if (!xml_output_append(output, escaped, escaped_size)) return 0;
        } else if (!xml_output_append(output, text + position, next - position)) {
            return 0;
        }
        position = next;
    }
    return 1;
}

static const QwDataValue *xml_schema_field(const QwDataValue *object,
                                           const char *key, size_t key_size)
{
    size_t i;
    const QwDataValue *found = NULL;
    if (!object || object->kind != QW_DATA_VALUE_OBJECT) return NULL;
    for (i = 0u; i < object->value.object.count; ++i) {
        const QwDataMember *member = &object->value.object.items[i];
        if (member->key_size == key_size &&
            memcmp(member->key, key, key_size) == 0) {
            if (found) return NULL;
            found = &member->value;
        }
    }
    return found;
}

static int xml_write_element(QwXmlOutput *output, const QwDataValue *element,
                             size_t depth)
{
    const QwDataValue *name;
    const QwDataValue *attributes;
    const QwDataValue *content;
    size_t i;
    if (!element || element->kind != QW_DATA_VALUE_OBJECT ||
        element->value.object.count != 3u ||
        depth > QW_DATA_DOCUMENT_MAX_DEPTH) {
        output->error = QW_STATUS_ERR_UNSUPPORTED;
        return 0;
    }
    name = xml_schema_field(element, QW_XML_NAME_KEY,
                            sizeof(QW_XML_NAME_KEY) - 1u);
    attributes = xml_schema_field(element, QW_XML_ATTRIBUTES_KEY,
                                  sizeof(QW_XML_ATTRIBUTES_KEY) - 1u);
    content = xml_schema_field(element, QW_XML_CONTENT_KEY,
                               sizeof(QW_XML_CONTENT_KEY) - 1u);
    if (!name || name->kind != QW_DATA_VALUE_STRING ||
        !xml_name_valid(name->value.text.data, name->value.text.size) ||
        !attributes || attributes->kind != QW_DATA_VALUE_OBJECT ||
        !content || content->kind != QW_DATA_VALUE_ARRAY) {
        output->error = QW_STATUS_ERR_UNSUPPORTED;
        return 0;
    }
    if (!xml_output_append(output, "<", 1u) ||
        !xml_output_append(output, name->value.text.data, name->value.text.size)) return 0;
    for (i = 0u; i < attributes->value.object.count; ++i) {
        const QwDataMember *attribute = &attributes->value.object.items[i];
        if (!xml_name_valid(attribute->key, attribute->key_size) ||
            attribute->value.kind != QW_DATA_VALUE_STRING) {
            output->error = QW_STATUS_ERR_UNSUPPORTED;
            return 0;
        }
        if (!xml_output_append(output, " ", 1u) ||
            !xml_output_append(output, attribute->key, attribute->key_size) ||
            !xml_output_append(output, "=\"", 2u) ||
            !xml_write_escaped(output, attribute->value.value.text.data,
                               attribute->value.value.text.size, 1) ||
            !xml_output_append(output, "\"", 1u)) return 0;
    }
    if (!content->value.array.count) return xml_output_append(output, "/>" , 2u);
    if (!xml_output_append(output, ">", 1u)) return 0;
    for (i = 0u; i < content->value.array.count; ++i) {
        const QwDataValue *child = &content->value.array.items[i];
        if (child->kind == QW_DATA_VALUE_STRING) {
            if (!xml_write_escaped(output, child->value.text.data,
                                   child->value.text.size, 0)) return 0;
        } else if (child->kind == QW_DATA_VALUE_OBJECT) {
            if (!xml_write_element(output, child, depth + 1u)) return 0;
        } else {
            output->error = QW_STATUS_ERR_UNSUPPORTED;
            return 0;
        }
    }
    return xml_output_append(output, "</", 2u) &&
           xml_output_append(output, name->value.text.data, name->value.text.size) &&
           xml_output_append(output, ">", 1u);
}

static int xml_write_document(const QwDataDocument *document,
                              QwOutputFormat output_format,
                              const QwConvertOptions *options,
                              uint8_t **out_data, size_t *out_size,
                              QwConvertReport *report)
{
    static const char declaration[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    const QwDataValue *root;
    QwXmlOutput output;
    int rc;
    (void)options;
    if (!document || !out_data || !out_size || output_format != QW_FORMAT_XML)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    root = xml_schema_field(&document->root, QW_XML_DOCUMENT_KEY,
                            sizeof(QW_XML_DOCUMENT_KEY) - 1u);
    if (document->root.kind != QW_DATA_VALUE_OBJECT ||
        document->root.value.object.count != 1u || !root) return QW_STATUS_ERR_UNSUPPORTED;
    memset(&output, 0, sizeof(output));
    output.limit = QW_XML_MAX_OUTPUT;
    if (!xml_output_append(&output, declaration, sizeof(declaration) - 1u) ||
        !xml_write_element(&output, root, 1u)) {
        rc = output.error ? output.error : QW_STATUS_ERR_OUT_OF_MEMORY;
        free(output.data);
        return rc;
    }
    *out_data = (uint8_t *)output.data;
    *out_size = output.size;
    if (report) {
        report->format = document->input_format;
        report->output_format = output_format;
        report->output_size = output.size;
        (void)snprintf(report->note, sizeof(report->note),
            "XML round trips through the ordered $xml/$name/$attributes/$content data shape");
    }
    return QW_STATUS_OK;
}

static int xml_probe(const void *input, size_t size)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t position = 0u;
    if (!data || !size || size > QW_XML_MAX_INPUT) return 0;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    for (;;) {
        while (position < size && (data[position] == ' ' || data[position] == '\t' ||
               data[position] == '\r' || data[position] == '\n')) ++position;
        if (position + 4u <= size && memcmp(data + position, "<!--", 4u) == 0) {
            position += 4u;
            while (position + 2u < size && memcmp(data + position, "-->", 3u) != 0)
                ++position;
            if (position + 2u >= size) return 0;
            position += 3u;
            continue;
        }
        if (position + 2u <= size && data[position] == '<' && data[position + 1u] == '?') {
            position += 2u;
            while (position + 1u < size &&
                   !(data[position] == '?' && data[position + 1u] == '>')) ++position;
            if (position + 1u >= size) return 0;
            position += 2u;
            continue;
        }
        break;
    }
    if (position + 9u < size && memcmp(data + position, "<!DOCTYPE", 9u) == 0 &&
        (data[position + 9u] == ' ' || data[position + 9u] == '\t' ||
         data[position + 9u] == '\r' || data[position + 9u] == '\n')) return 1;
    if (position >= size || data[position] != '<' || position + 1u >= size) return 0;
    if (data[position + 1u] == '!' || data[position + 1u] == '/') return 0;
    return data[position + 1u] == ':' || data[position + 1u] == '_' ||
           (data[position + 1u] >= 'A' && data[position + 1u] <= 'Z') ||
           (data[position + 1u] >= 'a' && data[position + 1u] <= 'z') ||
           data[position + 1u] >= 0x80u;
}

static int xml_extension_equal(const char *extension, size_t size,
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

static int xml_path_probe(const char *path)
{
    const char *base;
    const char *cursor;
    const char *dot;
    if (!path || !*path) return 0;
    base = path;
    for (cursor = path; *cursor; ++cursor)
        if (*cursor == '/' || *cursor == '\\') base = cursor + 1u;
    dot = strrchr(base, '.');
    return dot && xml_extension_equal(dot, strlen(dot), ".xml");
}

static int xml_open_data(const void *input, size_t size,
                         QwInputFormat input_format,
                         const QwConvertOptions *options,
                         QwDataDocument *out_document,
                         QwConvertReport *report)
{
    int rc;
    (void)options;
    rc = xml_parse_document(input, size, input_format, out_document);
    if (rc == QW_STATUS_OK && report) {
        report->format = input_format;
        report->output_format = QW_FORMAT_UNKNOWN;
        report->output_size = 0u;
        (void)snprintf(report->note, sizeof(report->note),
            "XML elements preserve ordered mixed content and string attributes in the shared data tree");
    }
    return rc;
}

const QwDataWriterDescriptor qw_data_xml_writer = {
    QW_FORMAT_XML, "XML data writer", xml_write_document,
    QW_XML_MAX_OUTPUT
};

const QwFormatDescriptor qw_format_xml = {
    .id = QW_FORMAT_XML,
    .name = "XML",
    .capabilities = 0u,
    .probe = xml_probe,
    .open_memory = NULL,
    .open_layout = NULL,
    .open_audio = NULL,
    .max_input_bytes = QW_XML_MAX_INPUT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .convert_to_pdf = NULL,
    .open_data = xml_open_data
};

const QwPathFormatDescriptor qw_path_format_xml = {
    .id = QW_FORMAT_XML,
    .probe_path = xml_path_probe,
    .decode_document_path = NULL,
    .resource_profile = NULL
};
#endif
