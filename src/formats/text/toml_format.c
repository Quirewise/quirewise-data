#include "../../core/data_document.h"
#include "data_parser_internal.h"
#include "quirewise/status.h"

#ifndef QUIREWISE_STANDALONE_DATA_PARSER
#include "../../core/format_registry.h"
#include "../../core/writer_registry.h"
#endif

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QW_TOML_MAX_INPUT ((size_t)256u * 1024u * 1024u)
#define QW_TOML_MAX_OUTPUT QW_DATA_DOCUMENT_MAX_MEMORY
#define QW_TOML_MAX_PATH QW_DATA_DOCUMENT_MAX_DEPTH

typedef struct QwTomlBuffer {
    char *data;
    size_t size;
    size_t capacity;
} QwTomlBuffer;

typedef struct QwTomlKeyPath {
    char *keys[QW_TOML_MAX_PATH];
    size_t sizes[QW_TOML_MAX_PATH];
    size_t allocations[QW_TOML_MAX_PATH];
    size_t count;
} QwTomlKeyPath;

typedef struct QwTomlSeenTable {
    char *text;
    size_t size;
} QwTomlSeenTable;

typedef struct QwTomlParser {
    const uint8_t *input;
    size_t size;
    size_t position;
    QwDataDocument *document;
    QwDataValue *current_table;
    size_t current_depth;
    QwTomlSeenTable *seen_tables;
    size_t seen_count;
    size_t seen_capacity;
    int error;
} QwTomlParser;

typedef struct QwTomlOutput {
    char *data;
    size_t size;
    size_t capacity;
    size_t limit;
    int error;
} QwTomlOutput;

static int toml_utf8_valid(const uint8_t *data, size_t size)
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
        } else return 0;
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

static void *toml_allocate(QwTomlParser *parser, size_t size)
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

static void *toml_reallocate(QwTomlParser *parser, void *memory,
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

static void toml_release(QwTomlParser *parser, void *memory, size_t size)
{
    if (!memory) return;
    free(memory);
    if (parser->document && size <= parser->document->allocation_bytes)
        parser->document->allocation_bytes -= size;
}

static int toml_buffer_reserve(QwTomlParser *parser, QwTomlBuffer *buffer,
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
    grown = (char *)toml_reallocate(parser, buffer->data,
                                    buffer->capacity, capacity);
    if (!grown) return 0;
    buffer->data = grown;
    buffer->capacity = capacity;
    return 1;
}

static int toml_buffer_append(QwTomlParser *parser, QwTomlBuffer *buffer,
                              const char *text, size_t size)
{
    if (size > SIZE_MAX - buffer->size - 1u ||
        !toml_buffer_reserve(parser, buffer, buffer->size + size + 1u)) return 0;
    if (size) memcpy(buffer->data + buffer->size, text, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
    return 1;
}

static int toml_buffer_byte(QwTomlParser *parser, QwTomlBuffer *buffer, char ch)
{
    return toml_buffer_append(parser, buffer, &ch, 1u);
}

static int toml_buffer_finish(QwTomlParser *parser, QwTomlBuffer *buffer)
{
    if (!toml_buffer_reserve(parser, buffer, buffer->size + 1u)) return 0;
    buffer->data[buffer->size] = '\0';
    return 1;
}

static void toml_buffer_dispose(QwTomlParser *parser, QwTomlBuffer *buffer)
{
    toml_release(parser, buffer->data, buffer->capacity);
    memset(buffer, 0, sizeof(*buffer));
}

static int toml_new_value(QwTomlParser *parser, QwDataValue *value,
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

static int toml_set_text(QwTomlParser *parser, QwDataValue *value,
                         QwDataValueKind kind, const char *text, size_t size,
                         size_t depth)
{
    char *copy;
    if (!toml_new_value(parser, value, kind, depth)) return 0;
    if (size == SIZE_MAX) { parser->error = QW_STATUS_ERR_RESOURCE_LIMIT; return 0; }
    copy = (char *)toml_allocate(parser, size + 1u);
    if (!copy) return 0;
    if (size) memcpy(copy, text, size);
    copy[size] = '\0';
    value->value.text.data = copy;
    value->value.text.size = size;
    return 1;
}

static int toml_array_reserve(QwTomlParser *parser, QwDataValue *array,
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
    items = (QwDataValue *)toml_reallocate(parser, array->value.array.items,
                                           old_size, new_size);
    if (!items) return 0;
    array->value.array.items = items;
    array->value.array.capacity = capacity;
    return 1;
}

static int toml_object_reserve(QwTomlParser *parser, QwDataValue *object,
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
    items = (QwDataMember *)toml_reallocate(parser, object->value.object.items,
                                            old_size, new_size);
    if (!items) return 0;
    object->value.object.items = items;
    object->value.object.capacity = capacity;
    return 1;
}

static QwDataMember *toml_object_find(QwDataValue *object,
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

static int toml_object_append_owned(QwTomlParser *parser, QwDataValue *object,
                                    char *key, size_t key_size,
                                    size_t key_allocation,
                                    QwDataValue *value)
{
    QwDataMember *member;
    if (toml_object_find(object, key, key_size)) {
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    if (object->value.object.count >= QW_DATA_DOCUMENT_MAX_NODES ||
        !toml_object_reserve(parser, object, object->value.object.count + 1u))
        return 0;
    member = &object->value.object.items[object->value.object.count++];
    member->key = key;
    member->key_size = key_size;
    member->value = *value;
    (void)key_allocation;
    memset(value, 0, sizeof(*value));
    return 1;
}

static int toml_array_append(QwTomlParser *parser, QwDataValue *array,
                             QwDataValue *value)
{
    if (array->value.array.count >= QW_DATA_DOCUMENT_MAX_NODES ||
        !toml_array_reserve(parser, array, array->value.array.count + 1u))
        return 0;
    array->value.array.items[array->value.array.count++] = *value;
    memset(value, 0, sizeof(*value));
    return 1;
}

static int toml_encode_codepoint(QwTomlParser *parser, QwTomlBuffer *buffer,
                                 uint32_t codepoint)
{
    char bytes[4];
    size_t count;
    if (!codepoint || codepoint > 0x10ffffu ||
        (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    if (codepoint <= 0x7fu) { bytes[0] = (char)codepoint; count = 1u; }
    else if (codepoint <= 0x7ffu) {
        bytes[0] = (char)(0xc0u | (codepoint >> 6));
        bytes[1] = (char)(0x80u | (codepoint & 0x3fu)); count = 2u;
    } else if (codepoint <= 0xffffu) {
        bytes[0] = (char)(0xe0u | (codepoint >> 12));
        bytes[1] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
        bytes[2] = (char)(0x80u | (codepoint & 0x3fu)); count = 3u;
    } else {
        bytes[0] = (char)(0xf0u | (codepoint >> 18));
        bytes[1] = (char)(0x80u | ((codepoint >> 12) & 0x3fu));
        bytes[2] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
        bytes[3] = (char)(0x80u | (codepoint & 0x3fu)); count = 4u;
    }
    return toml_buffer_append(parser, buffer, bytes, count);
}

static int toml_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int toml_parse_hex(QwTomlParser *parser, unsigned digits,
                          uint32_t *out_value)
{
    unsigned i;
    uint32_t value = 0u;
    if ((size_t)digits > parser->size - parser->position) return 0;
    for (i = 0u; i < digits; ++i) {
        int digit = toml_hex_value((char)parser->input[parser->position++]);
        if (digit < 0) return 0;
        value = (value << 4) | (unsigned)digit;
    }
    *out_value = value;
    return 1;
}

static int toml_parse_string_text(QwTomlParser *parser, QwTomlBuffer *buffer)
{
    char quote;
    int multiline = 0;
    int basic;
    if (parser->position >= parser->size ||
        (parser->input[parser->position] != '"' &&
         parser->input[parser->position] != '\'')) return 0;
    quote = (char)parser->input[parser->position++];
    basic = quote == '"';
    if (parser->size - parser->position >= 2u &&
        parser->input[parser->position] == (uint8_t)quote &&
        parser->input[parser->position + 1u] == (uint8_t)quote) {
        multiline = 1;
        parser->position += 2u;
        if (parser->position < parser->size && parser->input[parser->position] == '\r') {
            ++parser->position;
            if (parser->position < parser->size && parser->input[parser->position] == '\n')
                ++parser->position;
        } else if (parser->position < parser->size && parser->input[parser->position] == '\n') {
            ++parser->position;
        }
    }
    while (parser->position < parser->size) {
        uint8_t ch = parser->input[parser->position++];
        if (ch == (uint8_t)quote) {
            if (!multiline) return toml_buffer_finish(parser, buffer);
            {
                size_t quote_count = 1u;
                while (parser->position < parser->size &&
                       parser->input[parser->position] == (uint8_t)quote) {
                    ++quote_count;
                    ++parser->position;
                }
                if (quote_count >= 3u) {
                    size_t content_quotes = quote_count - 3u;
                    size_t i;
                    if (quote_count > 5u) {
                        parser->error = QW_STATUS_ERR_FORMAT;
                        return 0;
                    }
                    for (i = 0u; i < content_quotes; ++i)
                        if (!toml_buffer_byte(parser, buffer, quote)) return 0;
                    return toml_buffer_finish(parser, buffer);
                }
                while (quote_count--)
                    if (!toml_buffer_byte(parser, buffer, quote)) return 0;
            }
            continue;
        }
        if (!multiline && (ch == '\n' || ch == '\r')) {
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        if (basic && ch == '\\') {
            uint32_t codepoint;
            unsigned digits;
            if (parser->position >= parser->size) break;
            ch = parser->input[parser->position++];
            if (multiline && (ch == '\n' || ch == '\r')) {
                if (ch == '\r' && parser->position < parser->size &&
                    parser->input[parser->position] == '\n') ++parser->position;
                while (parser->position < parser->size &&
                       (parser->input[parser->position] == ' ' ||
                        parser->input[parser->position] == '\t' ||
                        parser->input[parser->position] == '\r' ||
                        parser->input[parser->position] == '\n')) ++parser->position;
                continue;
            }
            switch (ch) {
            case 'b': ch = '\b'; break;
            case 't': ch = '\t'; break;
            case 'n': ch = '\n'; break;
            case 'f': ch = '\f'; break;
            case 'r': ch = '\r'; break;
            case '"': ch = '"'; break;
            case '\\': ch = '\\'; break;
            case 'u': digits = 4u; goto parse_unicode;
            case 'U': digits = 8u;
parse_unicode:
                if (!toml_parse_hex(parser, digits, &codepoint) ||
                    !toml_encode_codepoint(parser, buffer, codepoint)) return 0;
                continue;
            default:
                parser->error = QW_STATUS_ERR_FORMAT;
                return 0;
            }
        }
        if ((ch < 0x20u && ch != '\t' && ch != '\n' && ch != '\r') || ch == 0u) {
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        if (!toml_buffer_byte(parser, buffer, (char)ch)) return 0;
    }
    parser->error = QW_STATUS_ERR_FORMAT;
    return 0;
}

static void toml_skip_horizontal(QwTomlParser *parser)
{
    while (parser->position < parser->size &&
           (parser->input[parser->position] == ' ' ||
            parser->input[parser->position] == '\t')) ++parser->position;
}

static void toml_skip_space_comments(QwTomlParser *parser, int newlines)
{
    for (;;) {
        toml_skip_horizontal(parser);
        if (newlines && parser->position < parser->size &&
            (parser->input[parser->position] == '\r' ||
             parser->input[parser->position] == '\n')) {
            if (parser->input[parser->position++] == '\r' &&
                parser->position < parser->size &&
                parser->input[parser->position] == '\n') ++parser->position;
            continue;
        }
        if (parser->position < parser->size && parser->input[parser->position] == '#') {
            while (parser->position < parser->size &&
                   parser->input[parser->position] != '\n' &&
                   parser->input[parser->position] != '\r') ++parser->position;
            continue;
        }
        break;
    }
}

static int toml_is_bare_key_char(uint8_t ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
}

static void toml_key_path_dispose(QwTomlParser *parser, QwTomlKeyPath *path)
{
    size_t i;
    for (i = 0u; i < path->count; ++i)
        toml_release(parser, path->keys[i], path->allocations[i]);
    memset(path, 0, sizeof(*path));
}

static int toml_key_path_add(QwTomlParser *parser, QwTomlKeyPath *path,
                             QwTomlBuffer *key)
{
    if (!key->size || path->count >= QW_TOML_MAX_PATH) {
        parser->error = key->size ? QW_STATUS_ERR_RESOURCE_LIMIT : QW_STATUS_ERR_FORMAT;
        return 0;
    }
    path->keys[path->count] = key->data;
    path->sizes[path->count] = key->size;
    path->allocations[path->count] = key->capacity;
    ++path->count;
    memset(key, 0, sizeof(*key));
    return 1;
}

static int toml_parse_key_path(QwTomlParser *parser, QwTomlKeyPath *path)
{
    memset(path, 0, sizeof(*path));
    for (;;) {
        QwTomlBuffer key;
        size_t start;
        memset(&key, 0, sizeof(key));
        toml_skip_horizontal(parser);
        if (parser->position >= parser->size) goto failure;
        if (parser->input[parser->position] == '"' ||
            parser->input[parser->position] == '\'') {
            uint8_t quote = parser->input[parser->position];
            if (parser->size - parser->position >= 3u &&
                parser->input[parser->position + 1u] == quote &&
                parser->input[parser->position + 2u] == quote) {
                parser->error = QW_STATUS_ERR_FORMAT;
                goto failure;
            }
            if (!toml_parse_string_text(parser, &key)) {
                toml_buffer_dispose(parser, &key);
                goto failure;
            }
        } else {
            start = parser->position;
            while (parser->position < parser->size &&
                   toml_is_bare_key_char(parser->input[parser->position]))
                ++parser->position;
            if (parser->position == start ||
                !toml_buffer_append(parser, &key,
                    (const char *)parser->input + start, parser->position - start) ||
                !toml_buffer_finish(parser, &key)) {
                toml_buffer_dispose(parser, &key);
                goto failure;
            }
        }
        if (!toml_key_path_add(parser, path, &key)) {
            toml_buffer_dispose(parser, &key);
            goto failure;
        }
        toml_skip_horizontal(parser);
        if (parser->position < parser->size && parser->input[parser->position] == '.') {
            ++parser->position;
            continue;
        }
        return 1;
    }
failure:
    if (!parser->error) parser->error = QW_STATUS_ERR_FORMAT;
    toml_key_path_dispose(parser, path);
    return 0;
}

static int toml_value_depth(QwTomlParser *parser, QwDataValue *value,
                            QwDataValueKind kind, size_t depth)
{
    return toml_new_value(parser, value, kind, depth);
}

static int toml_json_number_valid(const char *text, size_t size)
{
    size_t i = 0u;
    size_t digits;
    if (!size) return 0;
    if (text[i] == '-') ++i;
    if (i >= size) return 0;
    if (text[i] == '0') {
        ++i;
        if (i < size && text[i] >= '0' && text[i] <= '9') return 0;
    } else {
        if (text[i] < '1' || text[i] > '9') return 0;
        while (i < size && text[i] >= '0' && text[i] <= '9') ++i;
    }
    if (i < size && text[i] == '.') {
        ++i; digits = 0u;
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

static int toml_digits(const char *text, size_t start, size_t count,
                       unsigned *out)
{
    size_t i;
    unsigned value = 0u;
    for (i = 0u; i < count; ++i) {
        char ch = text[start + i];
        if (ch < '0' || ch > '9') return 0;
        value = value * 10u + (unsigned)(ch - '0');
    }
    *out = value;
    return 1;
}

static int toml_time_tail_valid(const char *text, size_t size, size_t start)
{
    unsigned hour, minute, second;
    size_t position = start;
    if (size - position < 8u || !toml_digits(text, position, 2u, &hour) ||
        text[position + 2u] != ':' ||
        !toml_digits(text, position + 3u, 2u, &minute) ||
        text[position + 5u] != ':' ||
        !toml_digits(text, position + 6u, 2u, &second) ||
        hour > 23u || minute > 59u || second > 60u) return 0;
    position += 8u;
    if (position < size && text[position] == '.') {
        ++position;
        if (position >= size || text[position] < '0' || text[position] > '9') return 0;
        while (position < size && text[position] >= '0' && text[position] <= '9')
            ++position;
    }
    if (position < size && (text[position] == 'Z' || text[position] == 'z'))
        ++position;
    else if (position < size && (text[position] == '+' || text[position] == '-')) {
        unsigned offset_hour, offset_minute;
        ++position;
        if (size - position < 5u || !toml_digits(text, position, 2u, &offset_hour) ||
            text[position + 2u] != ':' ||
            !toml_digits(text, position + 3u, 2u, &offset_minute) ||
            offset_hour > 23u || offset_minute > 59u) return 0;
        position += 5u;
    }
    return position == size;
}

static int toml_temporal_valid(const char *text, size_t size)
{
    unsigned year, month, day;
    unsigned days_in_month;
    size_t position;
    if (size == 8u || (size > 8u && text[8] == '.' && text[0] >= '0' &&
                      text[0] <= '9'))
        return toml_time_tail_valid(text, size, 0u);
    if (size < 10u || !toml_digits(text, 0u, 4u, &year) || text[4] != '-' ||
        !toml_digits(text, 5u, 2u, &month) || text[7] != '-' ||
        !toml_digits(text, 8u, 2u, &day) || month < 1u || month > 12u ||
        day < 1u) return 0;
    switch (month) {
    case 2u:
        days_in_month = (year % 4u == 0u &&
                         (year % 100u != 0u || year % 400u == 0u)) ? 29u : 28u;
        break;
    case 4u: case 6u: case 9u: case 11u:
        days_in_month = 30u;
        break;
    default:
        days_in_month = 31u;
        break;
    }
    if (day > days_in_month) return 0;
    if (size == 10u) return 1;
    if (text[10] != 'T' && text[10] != 't' && text[10] != ' ') return 0;
    position = 11u;
    return toml_time_tail_valid(text, size, position);
}

static int toml_number_underscores_valid(const char *text, size_t size,
                                         unsigned base)
{
    size_t i;
    for (i = 0u; i < size; ++i) {
        if (text[i] == '_') {
            int previous;
            int next;
            if (!i || i + 1u >= size) return 0;
            previous = toml_hex_value(text[i - 1u]);
            next = toml_hex_value(text[i + 1u]);
            if (previous < 0 || next < 0 || (unsigned)previous >= base ||
                (unsigned)next >= base) return 0;
        }
    }
    return 1;
}

static int toml_normalize_number(QwTomlParser *parser, const char *text,
                                 size_t size, char *output, size_t capacity,
                                 size_t *out_size)
{
    size_t i;
    size_t used = 0u;
    int has_fraction = 0;
    int had_sign = 0;
    unsigned base = 10u;
    const char *digits;
    size_t digits_size;
    if (!size || size >= capacity) {
        parser->error = size ? QW_STATUS_ERR_RESOURCE_LIMIT : QW_STATUS_ERR_FORMAT;
        return 0;
    }
    if (text[0] == '+' || text[0] == '-') {
        had_sign = 1;
        if (text[0] == '-') output[used++] = '-';
        ++text; --size;
        if (!size) { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
    }
    if (size >= 2u && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'o' || text[1] == 'b')) {
        unsigned long long value = 0u;
        if (had_sign) { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
        base = text[1] == 'x' ? 16u : text[1] == 'o' ? 8u : 2u;
        digits = text + 2u;
        digits_size = size - 2u;
        if (!digits_size || !toml_number_underscores_valid(digits, digits_size, base)) {
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        for (i = 0u; i < digits_size; ++i) if (digits[i] != '_') {
            int digit = toml_hex_value(digits[i]);
            if (digit < 0 || (unsigned)digit >= base) {
                parser->error = QW_STATUS_ERR_FORMAT;
                return 0;
            }
        }
        for (i = 0u; i < digits_size; ++i) {
            int digit;
            if (digits[i] == '_') continue;
            digit = toml_hex_value(digits[i]);
            if (digit < 0 || (unsigned)digit >= base ||
                value > ((unsigned long long)LLONG_MAX - (unsigned)digit) / base) {
                parser->error = QW_STATUS_ERR_FORMAT;
                return 0;
            }
            value = value * base + (unsigned)digit;
        }
        i = (size_t)snprintf(output + used, capacity - used, "%llu", value);
        if (i >= capacity - used) { parser->error = QW_STATUS_ERR_RESOURCE_LIMIT; return 0; }
        used += i;
    } else {
        if (!toml_number_underscores_valid(text, size, 10u)) {
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        for (i = 0u; i < size; ++i) {
            char ch = text[i];
            if (ch == '_') continue;
            if (ch == '.' || ch == 'e' || ch == 'E') has_fraction = 1;
            if (used + 1u >= capacity) { parser->error = QW_STATUS_ERR_RESOURCE_LIMIT; return 0; }
            output[used++] = ch;
        }
        if (!toml_json_number_valid(output, used)) {
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        if (!has_fraction) {
            errno = 0;
            {
                char *end;
                long long value = strtoll(output, &end, 10);
                if (errno == ERANGE || end != output + used) {
                    parser->error = QW_STATUS_ERR_FORMAT;
                    return 0;
                }
                i = (size_t)snprintf(output, capacity, "%lld", value);
                if (i >= capacity) { parser->error = QW_STATUS_ERR_RESOURCE_LIMIT; return 0; }
                used = i;
            }
        }
    }
    *out_size = used;
    return 1;
}

static int toml_parse_value(QwTomlParser *parser, QwDataValue *out_value,
                            size_t depth);

static int toml_assign_path(QwTomlParser *parser, QwDataValue *object,
                            size_t object_depth, QwTomlKeyPath *path,
                            QwDataValue *value)
{
    size_t i;
    QwDataValue *container = object;
    if (!path->count || !object || object->kind != QW_DATA_VALUE_OBJECT) {
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    for (i = 0u; i + 1u < path->count; ++i) {
        QwDataMember *member = toml_object_find(container, path->keys[i], path->sizes[i]);
        if (!member) {
            QwDataValue table;
            if (!toml_new_value(parser, &table, QW_DATA_VALUE_OBJECT,
                                object_depth + i + 1u) ||
                !toml_object_append_owned(parser, container, path->keys[i],
                    path->sizes[i], path->allocations[i], &table)) {
                qw_data_value_dispose(&table);
                return 0;
            }
            path->keys[i] = NULL;
            path->allocations[i] = 0u;
            member = &container->value.object.items[
                container->value.object.count - 1u];
        }
        if (member->value.kind == QW_DATA_VALUE_OBJECT) {
            container = &member->value;
        } else if (member->value.kind == QW_DATA_VALUE_ARRAY &&
                   member->value.value.array.count &&
                   member->value.value.array.items[
                       member->value.value.array.count - 1u].kind == QW_DATA_VALUE_OBJECT) {
            container = &member->value.value.array.items[
                member->value.value.array.count - 1u];
        } else {
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
    }
    if (!toml_object_append_owned(parser, container,
            path->keys[path->count - 1u], path->sizes[path->count - 1u],
            path->allocations[path->count - 1u], value)) return 0;
    path->keys[path->count - 1u] = NULL;
    path->allocations[path->count - 1u] = 0u;
    return 1;
}

static int toml_get_or_create_table(QwTomlParser *parser, QwDataValue *parent,
                                    const char *key, size_t key_size,
                                    size_t parent_depth, QwDataValue **out_table)
{
    QwDataMember *member = toml_object_find(parent, key, key_size);
    if (!member) {
        char *key_copy = (char *)toml_allocate(parser, key_size + 1u);
        QwDataValue table;
        if (!key_copy) return 0;
        memcpy(key_copy, key, key_size);
        key_copy[key_size] = '\0';
        if (!toml_new_value(parser, &table, QW_DATA_VALUE_OBJECT, parent_depth + 1u) ||
            !toml_object_append_owned(parser, parent, key_copy, key_size,
                                      key_size + 1u, &table)) {
            toml_release(parser, key_copy, key_size + 1u);
            qw_data_value_dispose(&table);
            return 0;
        }
        member = toml_object_find(parent, key, key_size);
    }
    if (member->value.kind == QW_DATA_VALUE_OBJECT) {
        *out_table = &member->value;
        return 1;
    }
    if (member->value.kind == QW_DATA_VALUE_ARRAY &&
        member->value.value.array.count &&
        member->value.value.array.items[
            member->value.value.array.count - 1u].kind == QW_DATA_VALUE_OBJECT) {
        *out_table = &member->value.value.array.items[
            member->value.value.array.count - 1u];
        return 1;
    }
    parser->error = QW_STATUS_ERR_FORMAT;
    return 0;
}

static int toml_seen_table(QwTomlParser *parser, const QwTomlKeyPath *path)
{
    size_t i;
    size_t encoded_size = 0u;
    size_t offset = 0u;
    char *encoded;
    QwTomlSeenTable *grown;
    size_t capacity;
    for (i = 0u; i < path->count; ++i) {
        if (path->sizes[i] > SIZE_MAX - sizeof(uint64_t) - encoded_size) {
            parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
            return 0;
        }
        encoded_size += sizeof(uint64_t) + path->sizes[i];
    }
    encoded = (char *)toml_allocate(parser, encoded_size);
    if (!encoded) return 0;
    for (i = 0u; i < path->count; ++i) {
        uint64_t key_size = (uint64_t)path->sizes[i];
        memcpy(encoded + offset, &key_size, sizeof(key_size));
        offset += sizeof(key_size);
        memcpy(encoded + offset, path->keys[i], path->sizes[i]);
        offset += path->sizes[i];
    }
    for (i = 0u; i < parser->seen_count; ++i) {
        if (parser->seen_tables[i].size == encoded_size &&
            memcmp(parser->seen_tables[i].text, encoded, encoded_size) == 0) {
            toml_release(parser, encoded, encoded_size);
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
    }
    if (parser->seen_count >= QW_DATA_DOCUMENT_MAX_NODES) {
        toml_release(parser, encoded, encoded_size);
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (parser->seen_count == parser->seen_capacity) {
        capacity = parser->seen_capacity ? parser->seen_capacity * 2u : 16u;
        if (capacity > QW_DATA_DOCUMENT_MAX_NODES) capacity = QW_DATA_DOCUMENT_MAX_NODES;
        grown = (QwTomlSeenTable *)realloc(parser->seen_tables,
                                           capacity * sizeof(*grown));
        if (!grown) {
            toml_release(parser, encoded, encoded_size);
            parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
            return 0;
        }
        parser->seen_tables = grown;
        parser->seen_capacity = capacity;
    }
    parser->seen_tables[parser->seen_count].text = encoded;
    parser->seen_tables[parser->seen_count].size = encoded_size;
    ++parser->seen_count;
    return 1;
}

static void toml_seen_tables_dispose(QwTomlParser *parser)
{
    size_t i;
    for (i = 0u; i < parser->seen_count; ++i)
        toml_release(parser, parser->seen_tables[i].text,
                     parser->seen_tables[i].size);
    free(parser->seen_tables);
    parser->seen_tables = NULL;
    parser->seen_count = 0u;
    parser->seen_capacity = 0u;
}

static int toml_resolve_table(QwTomlParser *parser, QwTomlKeyPath *path,
                              int array_of_tables)
{
    size_t i;
    QwDataValue *container = &parser->document->root;
    size_t depth = 0u;
    if (!path->count) { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
    for (i = 0u; i + 1u < path->count; ++i) {
        if (!toml_get_or_create_table(parser, container, path->keys[i],
                                      path->sizes[i], depth, &container)) return 0;
        ++depth;
    }
    {
        const size_t last = path->count - 1u;
        QwDataMember *member = toml_object_find(container, path->keys[last],
                                                path->sizes[last]);
        if (array_of_tables) {
            QwDataValue array;
            QwDataValue item;
            if (!member) {
                char *key_copy = (char *)toml_allocate(parser, path->sizes[last] + 1u);
                if (!key_copy) return 0;
                memcpy(key_copy, path->keys[last], path->sizes[last]);
                key_copy[path->sizes[last]] = '\0';
                if (!toml_new_value(parser, &array, QW_DATA_VALUE_ARRAY,
                                    depth + 1u) ||
                    !toml_object_append_owned(parser, container, key_copy,
                        path->sizes[last], path->sizes[last] + 1u, &array)) {
                    toml_release(parser, key_copy, path->sizes[last] + 1u);
                    qw_data_value_dispose(&array);
                    return 0;
                }
                member = toml_object_find(container, path->keys[last],
                                          path->sizes[last]);
            }
            if (!member || member->value.kind != QW_DATA_VALUE_ARRAY ||
                !toml_new_value(parser, &item, QW_DATA_VALUE_OBJECT, depth + 2u) ||
                !toml_array_append(parser, &member->value, &item)) {
                qw_data_value_dispose(&item);
                if (!parser->error) parser->error = QW_STATUS_ERR_FORMAT;
                return 0;
            }
            path->keys[last] = NULL;
            path->allocations[last] = 0u;
            parser->current_table = &member->value.value.array.items[
                member->value.value.array.count - 1u];
            parser->current_depth = depth + 2u;
        } else {
            if (!member) {
                char *key_copy = (char *)toml_allocate(parser, path->sizes[last] + 1u);
                QwDataValue table;
                if (!key_copy) return 0;
                memcpy(key_copy, path->keys[last], path->sizes[last]);
                key_copy[path->sizes[last]] = '\0';
                if (!toml_new_value(parser, &table, QW_DATA_VALUE_OBJECT, depth + 1u) ||
                    !toml_object_append_owned(parser, container, key_copy,
                        path->sizes[last], path->sizes[last] + 1u, &table)) {
                    toml_release(parser, key_copy, path->sizes[last] + 1u);
                    qw_data_value_dispose(&table);
                    return 0;
                }
                member = toml_object_find(container, path->keys[last],
                                          path->sizes[last]);
            }
            if (member->value.kind == QW_DATA_VALUE_OBJECT) {
                parser->current_table = &member->value;
                parser->current_depth = depth + 1u;
            } else {
                parser->error = QW_STATUS_ERR_FORMAT;
                return 0;
            }
            path->keys[last] = NULL;
            path->allocations[last] = 0u;
        }
    }
    return 1;
}

static int toml_parse_table_header(QwTomlParser *parser)
{
    size_t close_count;
    int array_of_tables = 0;
    QwTomlKeyPath path;
    if (parser->input[parser->position++] != '[') return 0;
    if (parser->position < parser->size && parser->input[parser->position] == '[') {
        array_of_tables = 1;
        ++parser->position;
    }
    close_count = array_of_tables ? 2u : 1u;
    toml_skip_horizontal(parser);
    if (!toml_parse_key_path(parser, &path)) return 0;
    toml_skip_horizontal(parser);
    if (parser->size - parser->position < close_count) goto failure;
    if (parser->input[parser->position++] != ']') goto failure;
    if (array_of_tables && parser->input[parser->position++] != ']') goto failure;
    toml_skip_horizontal(parser);
    if (parser->position < parser->size && parser->input[parser->position] == '#') {
        while (parser->position < parser->size &&
               parser->input[parser->position] != '\n' &&
               parser->input[parser->position] != '\r') ++parser->position;
    }
    if (parser->position < parser->size && parser->input[parser->position] == '\r') {
        ++parser->position;
        if (parser->position < parser->size && parser->input[parser->position] == '\n')
            ++parser->position;
    } else if (parser->position < parser->size && parser->input[parser->position] == '\n') {
        ++parser->position;
    } else if (parser->position < parser->size) {
        goto failure;
    }
    if (!array_of_tables && !toml_seen_table(parser, &path)) goto failure;
    if (!toml_resolve_table(parser, &path, array_of_tables)) goto failure;
    toml_key_path_dispose(parser, &path);
    return 1;
failure:
    toml_key_path_dispose(parser, &path);
    if (!parser->error) parser->error = QW_STATUS_ERR_FORMAT;
    return 0;
}

static void toml_skip_inline_space(QwTomlParser *parser)
{
    while (parser->position < parser->size &&
           (parser->input[parser->position] == ' ' ||
            parser->input[parser->position] == '\t')) ++parser->position;
}

static int toml_parse_value(QwTomlParser *parser, QwDataValue *out_value,
                            size_t depth);

static int toml_parse_array(QwTomlParser *parser, QwDataValue *out_value,
                            size_t depth)
{
    if (!toml_new_value(parser, out_value, QW_DATA_VALUE_ARRAY, depth)) return 0;
    ++parser->position;
    toml_skip_space_comments(parser, 1);
    if (parser->position < parser->size && parser->input[parser->position] == ']') {
        ++parser->position;
        return 1;
    }
    for (;;) {
        QwDataValue item;
        memset(&item, 0, sizeof(item));
        if (!toml_parse_value(parser, &item, depth + 1u)) {
            qw_data_value_dispose(&item);
            return 0;
        }
        if (!toml_array_append(parser, out_value, &item)) {
            qw_data_value_dispose(&item);
            return 0;
        }
        toml_skip_space_comments(parser, 1);
        if (parser->position >= parser->size) break;
        if (parser->input[parser->position] == ']') {
            ++parser->position;
            return 1;
        }
        if (parser->input[parser->position++] != ',') break;
        toml_skip_space_comments(parser, 1);
        if (parser->position < parser->size && parser->input[parser->position] == ']') {
            ++parser->position;
            return 1;
        }
    }
    parser->error = QW_STATUS_ERR_FORMAT;
    return 0;
}

static int toml_parse_inline_table(QwTomlParser *parser, QwDataValue *out_value,
                                   size_t depth)
{
    if (!toml_new_value(parser, out_value, QW_DATA_VALUE_OBJECT, depth)) return 0;
    ++parser->position;
    toml_skip_inline_space(parser);
    if (parser->position < parser->size && parser->input[parser->position] == '}') {
        ++parser->position;
        return 1;
    }
    for (;;) {
        QwTomlKeyPath path;
        QwDataValue value;
        memset(&value, 0, sizeof(value));
        if (!toml_parse_key_path(parser, &path)) return 0;
        toml_skip_inline_space(parser);
        if (parser->position >= parser->size || parser->input[parser->position++] != '=') {
            toml_key_path_dispose(parser, &path);
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        toml_skip_inline_space(parser);
        if (!toml_parse_value(parser, &value, depth + path.count)) {
            toml_key_path_dispose(parser, &path);
            qw_data_value_dispose(&value);
            return 0;
        }
        if (!toml_assign_path(parser, out_value, depth, &path, &value)) {
            toml_key_path_dispose(parser, &path);
            qw_data_value_dispose(&value);
            return 0;
        }
        toml_key_path_dispose(parser, &path);
        toml_skip_inline_space(parser);
        if (parser->position < parser->size && parser->input[parser->position] == '}') {
            ++parser->position;
            return 1;
        }
        if (parser->position >= parser->size || parser->input[parser->position++] != ',') {
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        toml_skip_inline_space(parser);
    }
}

static int toml_parse_bare_value(QwTomlParser *parser, QwDataValue *out_value,
                                 size_t depth)
{
    size_t start = parser->position;
    size_t end;
    size_t used;
    char normalized[512];
    while (parser->position < parser->size &&
           parser->input[parser->position] != ',' &&
           parser->input[parser->position] != ']' &&
           parser->input[parser->position] != '}' &&
           parser->input[parser->position] != '#' &&
           parser->input[parser->position] != '\n' &&
           parser->input[parser->position] != '\r') ++parser->position;
    end = parser->position;
    while (end > start && (parser->input[end - 1u] == ' ' ||
                           parser->input[end - 1u] == '\t')) --end;
    while (start < end && (parser->input[start] == ' ' ||
                           parser->input[start] == '\t')) ++start;
    if (start == end) { parser->error = QW_STATUS_ERR_FORMAT; return 0; }
    if ((end - start == 4u && memcmp(parser->input + start, "true", 4u) == 0) ||
        (end - start == 5u && memcmp(parser->input + start, "false", 5u) == 0)) {
        if (!toml_new_value(parser, out_value, QW_DATA_VALUE_BOOLEAN, depth)) return 0;
        out_value->value.boolean = parser->input[start] == 't';
        return 1;
    }
    if (toml_temporal_valid((const char *)parser->input + start, end - start))
        return toml_set_text(parser, out_value, QW_DATA_VALUE_TEMPORAL,
                             (const char *)parser->input + start, end - start, depth);
    if (end - start >= sizeof(normalized)) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!toml_normalize_number(parser, (const char *)parser->input + start,
                               end - start, normalized, sizeof(normalized), &used)) {
        if (!parser->error) parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    return toml_set_text(parser, out_value, QW_DATA_VALUE_NUMBER,
                         normalized, used, depth);
}

static int toml_parse_value(QwTomlParser *parser, QwDataValue *out_value,
                            size_t depth)
{
    if (depth > QW_DATA_DOCUMENT_MAX_DEPTH || parser->position >= parser->size) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (parser->input[parser->position] == '"' ||
        parser->input[parser->position] == '\'') {
        QwTomlBuffer buffer;
        int rc;
        memset(&buffer, 0, sizeof(buffer));
        if (!toml_parse_string_text(parser, &buffer)) {
            toml_buffer_dispose(parser, &buffer);
            return 0;
        }
        if (!toml_new_value(parser, out_value, QW_DATA_VALUE_STRING, depth)) {
            toml_buffer_dispose(parser, &buffer);
            return 0;
        }
        out_value->value.text.data = buffer.data;
        out_value->value.text.size = buffer.size;
        rc = buffer.data != NULL;
        memset(&buffer, 0, sizeof(buffer));
        return rc;
    }
    if (parser->input[parser->position] == '[')
        return toml_parse_array(parser, out_value, depth);
    if (parser->input[parser->position] == '{')
        return toml_parse_inline_table(parser, out_value, depth);
    return toml_parse_bare_value(parser, out_value, depth);
}

static int toml_finish_statement(QwTomlParser *parser)
{
    toml_skip_horizontal(parser);
    if (parser->position < parser->size && parser->input[parser->position] == '#') {
        while (parser->position < parser->size &&
               parser->input[parser->position] != '\n' &&
               parser->input[parser->position] != '\r') ++parser->position;
    }
    if (parser->position < parser->size && parser->input[parser->position] == '\r') {
        ++parser->position;
        if (parser->position < parser->size && parser->input[parser->position] == '\n')
            ++parser->position;
    } else if (parser->position < parser->size && parser->input[parser->position] == '\n') {
        ++parser->position;
    } else if (parser->position < parser->size) {
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    return 1;
}

static int toml_parse_assignment(QwTomlParser *parser)
{
    QwTomlKeyPath path;
    QwDataValue value;
    size_t value_depth;
    memset(&value, 0, sizeof(value));
    if (!toml_parse_key_path(parser, &path)) return 0;
    toml_skip_horizontal(parser);
    if (parser->position >= parser->size || parser->input[parser->position++] != '=') {
        toml_key_path_dispose(parser, &path);
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    toml_skip_horizontal(parser);
    value_depth = parser->current_depth + path.count;
    if (!toml_parse_value(parser, &value, value_depth)) {
        toml_key_path_dispose(parser, &path);
        qw_data_value_dispose(&value);
        return 0;
    }
    if (!toml_assign_path(parser, parser->current_table, parser->current_depth,
                          &path, &value) || !toml_finish_statement(parser)) {
        toml_key_path_dispose(parser, &path);
        qw_data_value_dispose(&value);
        return 0;
    }
    toml_key_path_dispose(parser, &path);
    return 1;
}

static int toml_parse_document(const void *input, size_t size,
                              QwInputFormat input_format,
                              QwDataDocument *out_document)
{
    QwTomlParser parser;
    int rc = QW_STATUS_ERR_FORMAT;
    memset(&parser, 0, sizeof(parser));
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    qw_data_document_init(out_document, input_format);
    if (!input || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (size > QW_TOML_MAX_INPUT) return QW_STATUS_ERR_RESOURCE_LIMIT;
    if (!toml_utf8_valid((const uint8_t *)input, size)) return QW_STATUS_ERR_FORMAT;
    parser.input = (const uint8_t *)input;
    parser.size = size;
    parser.document = out_document;
    if (size >= 3u && parser.input[0] == 0xefu && parser.input[1] == 0xbbu &&
        parser.input[2] == 0xbfu) parser.position = 3u;
    if (!toml_new_value(&parser, &out_document->root,
                        QW_DATA_VALUE_OBJECT, 0u)) goto failure;
    parser.current_table = &out_document->root;
    while (parser.position < parser.size) {
        toml_skip_space_comments(&parser, 1);
        if (parser.position >= parser.size) break;
        if (parser.input[parser.position] == '[') {
            if (!toml_parse_table_header(&parser)) goto failure;
        } else if (!toml_parse_assignment(&parser)) {
            goto failure;
        }
    }
    if (!qw_data_document_validate(out_document)) goto failure;
    rc = QW_STATUS_OK;
    toml_seen_tables_dispose(&parser);
    return rc;
failure:
    toml_seen_tables_dispose(&parser);
    if (parser.error) rc = parser.error;
    qw_data_document_dispose(out_document);
    return rc;
}

static int toml_probe_value(const uint8_t *data, size_t size, size_t position)
{
    uint8_t first;
    while (position < size && (data[position] == ' ' || data[position] == '\t'))
        ++position;
    if (position >= size || data[position] == '\r' || data[position] == '\n') return 0;
    first = data[position];
    if (first == 't')
        return size - position >= 4u && memcmp(data + position, "true", 4u) == 0 &&
               (position + 4u == size || data[position + 4u] == ' ' ||
                data[position + 4u] == '\t' || data[position + 4u] == ',' ||
                data[position + 4u] == ']' || data[position + 4u] == '}' ||
                data[position + 4u] == '#' || data[position + 4u] == '\r' ||
                data[position + 4u] == '\n');
    if (first == 'f')
        return size - position >= 5u && memcmp(data + position, "false", 5u) == 0 &&
               (position + 5u == size || data[position + 5u] == ' ' ||
                data[position + 5u] == '\t' || data[position + 5u] == ',' ||
                data[position + 5u] == ']' || data[position + 5u] == '}' ||
                data[position + 5u] == '#' || data[position + 5u] == '\r' ||
                data[position + 5u] == '\n');
    return first == '"' || first == '\'' || first == '[' || first == '{' ||
           first == '+' || first == '-' || (first >= '0' && first <= '9');
}

static int toml_probe_has_assignment(const uint8_t *data, size_t size,
                                     size_t position)
{
    while (position < size) {
        size_t line_start;
        int quote = 0;
        while (position < size && (data[position] == ' ' || data[position] == '\t' ||
               data[position] == '\r' || data[position] == '\n')) ++position;
        if (position >= size) return 0;
        if (data[position] == '#') {
            while (position < size && data[position] != '\r' &&
                   data[position] != '\n') ++position;
            continue;
        }
        if (data[position] == '[') {
            while (position < size && data[position] != '\r' &&
                   data[position] != '\n') ++position;
            continue;
        }
        line_start = position;
        while (position < size && data[position] != '\r' && data[position] != '\n') {
            uint8_t ch = data[position];
            if (!quote && ch == '#') return 0;
            if (quote && ch == '\\') {
                if (position + 1u < size) position += 2u;
                else ++position;
                continue;
            }
            if (quote) {
                if (ch == (uint8_t)quote) quote = 0;
            } else if (ch == '"' || ch == '\'') {
                quote = ch;
            } else if (ch == '=') {
                return position > line_start &&
                       toml_probe_value(data, size, position + 1u);
            }
            ++position;
        }
        return 0;
    }
    return 0;
}

static int toml_probe_table_header(const uint8_t *data, size_t size,
                                   size_t position)
{
    size_t cursor = position + 1u;
    int array_of_tables = 0;
    if (cursor < size && data[cursor] == '[') {
        array_of_tables = 1;
        ++cursor;
    }
    for (;;) {
        size_t key_start;
        while (cursor < size && (data[cursor] == ' ' || data[cursor] == '\t')) ++cursor;
        if (cursor >= size || data[cursor] == '\r' || data[cursor] == '\n') return 0;
        if (data[cursor] == '"' || data[cursor] == '\'') {
            uint8_t quote = data[cursor++];
            int basic = quote == '"';
            int closed = 0;
            while (cursor < size && data[cursor] != '\r' && data[cursor] != '\n') {
                if (basic && data[cursor] == '\\') {
                    if (cursor + 1u >= size) return 0;
                    cursor += 2u;
                } else if (data[cursor++] == quote) {
                    closed = 1;
                    break;
                }
            }
            if (!closed) return 0;
        } else {
            key_start = cursor;
            while (cursor < size && toml_is_bare_key_char(data[cursor])) ++cursor;
            if (cursor == key_start) return 0;
        }
        while (cursor < size && (data[cursor] == ' ' || data[cursor] == '\t')) ++cursor;
        if (cursor < size && data[cursor] == '.') {
            ++cursor;
            continue;
        }
        break;
    }
    while (cursor < size && (data[cursor] == ' ' || data[cursor] == '\t')) ++cursor;
    if (cursor >= size || data[cursor++] != ']') return 0;
    if (array_of_tables && (cursor >= size || data[cursor++] != ']')) return 0;
    while (cursor < size && (data[cursor] == ' ' || data[cursor] == '\t')) ++cursor;
    if (cursor < size && data[cursor] == '#')
        while (cursor < size && data[cursor] != '\r' && data[cursor] != '\n') ++cursor;
    if (cursor < size && data[cursor] != '\r' && data[cursor] != '\n') return 0;
    return toml_probe_has_assignment(data, size, cursor);
}

static int toml_probe(const void *input, size_t size)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t position = size >= 3u && data && data[0] == 0xefu &&
        data[1] == 0xbbu && data[2] == 0xbfu ? 3u : 0u;
    if (!data || !size || size > QW_TOML_MAX_INPUT) return 0;
    while (position < size) {
        size_t start;
        int quote = 0;
        while (position < size && (data[position] == ' ' || data[position] == '\t' ||
               data[position] == '\r' || data[position] == '\n')) ++position;
        if (position >= size) return 0;
        if (data[position] == '#') {
            while (position < size && data[position] != '\n' && data[position] != '\r')
                ++position;
            if (position < size && data[position] == '\r') {
                ++position;
                if (position < size && data[position] == '\n') ++position;
            } else if (position < size && data[position] == '\n') {
                ++position;
            }
            continue;
        }
        start = position;
        if (data[position] == '[')
            return toml_probe_table_header(data, size, position);
        while (position < size && data[position] != '\r' && data[position] != '\n') {
            uint8_t ch = data[position];
            if (!quote && ch == '#') break;
            if (quote && ch == '\\') { position += 2u; continue; }
            if (quote) { if (ch == (uint8_t)quote) quote = 0; }
            else if (ch == '"' || ch == '\'') quote = ch;
            else if (ch == '=')
                return position > start && toml_probe_value(data, size, position + 1u);
            ++position;
        }
        return 0;
    }
    return 0;
}

static int toml_extension_equal(const char *extension, size_t size,
                                const char *expected)
{
    size_t i;
    if (size != strlen(expected)) return 0;
    for (i = 0u; i < size; ++i) {
        char actual = extension[i];
        if (actual >= 'A' && actual <= 'Z') actual = (char)(actual + ('a' - 'A'));
        if (actual != expected[i]) return 0;
    }
    return 1;
}

static int toml_path_probe(const char *path)
{
    const char *base;
    const char *cursor;
    const char *dot;
    if (!path || !*path) return 0;
    base = path;
    for (cursor = path; *cursor; ++cursor)
        if (*cursor == '/' || *cursor == '\\') base = cursor + 1u;
    dot = strrchr(base, '.');
    return dot && toml_extension_equal(dot, strlen(dot), ".toml");
}

static int toml_output_append(QwTomlOutput *output, const char *text, size_t size)
{
    size_t needed;
    size_t capacity;
    char *grown;
    if (size > output->limit - output->size ||
        output->size > SIZE_MAX - size - 1u) {
        output->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    needed = output->size + size + 1u;
    if (needed > output->capacity) {
        capacity = output->capacity ? output->capacity : 256u;
        while (capacity < needed) {
            if (capacity > output->limit / 2u) { capacity = output->limit + 1u; break; }
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

static int toml_write_string(QwTomlOutput *output, const char *text, size_t size)
{
    size_t i;
    if (!toml_output_append(output, "\"", 1u)) return 0;
    for (i = 0u; i < size; ++i) {
        unsigned char ch = (unsigned char)text[i];
        const char *escape = NULL;
        size_t escape_size = 0u;
        char hex_escape[6];
        static const char hex[] = "0123456789ABCDEF";
        if (ch == '"') { escape = "\\\""; escape_size = 2u; }
        else if (ch == '\\') { escape = "\\\\"; escape_size = 2u; }
        else if (ch == '\b') { escape = "\\b"; escape_size = 2u; }
        else if (ch == '\t') { escape = "\\t"; escape_size = 2u; }
        else if (ch == '\n') { escape = "\\n"; escape_size = 2u; }
        else if (ch == '\f') { escape = "\\f"; escape_size = 2u; }
        else if (ch == '\r') { escape = "\\r"; escape_size = 2u; }
        if (escape) {
            if (!toml_output_append(output, escape, escape_size)) return 0;
        } else if (ch < 0x20u || ch == 0x7fu) {
            hex_escape[0] = '\\'; hex_escape[1] = 'u';
            hex_escape[2] = '0'; hex_escape[3] = '0';
            hex_escape[4] = hex[ch >> 4]; hex_escape[5] = hex[ch & 0x0fu];
            if (!toml_output_append(output, hex_escape, sizeof(hex_escape))) return 0;
        } else if (!toml_output_append(output, text + i, 1u)) return 0;
    }
    return toml_output_append(output, "\"", 1u);
}

static int toml_write_value(QwTomlOutput *output, const QwDataValue *value,
                            size_t depth)
{
    size_t i;
    if (!value || depth > QW_DATA_DOCUMENT_MAX_DEPTH) return 0;
    switch (value->kind) {
    case QW_DATA_VALUE_NULL:
        output->error = QW_STATUS_ERR_UNSUPPORTED;
        return 0;
    case QW_DATA_VALUE_TEMPORAL:
        return toml_output_append(output, value->value.text.data,
                                  value->value.text.size);
    case QW_DATA_VALUE_BOOLEAN:
        return value->value.boolean
            ? toml_output_append(output, "true", 4u)
            : toml_output_append(output, "false", 5u);
    case QW_DATA_VALUE_NUMBER:
        return toml_output_append(output, value->value.text.data,
                                  value->value.text.size);
    case QW_DATA_VALUE_STRING:
        return toml_write_string(output, value->value.text.data,
                                 value->value.text.size);
    case QW_DATA_VALUE_ARRAY:
        if (!toml_output_append(output, "[", 1u)) return 0;
        for (i = 0u; i < value->value.array.count; ++i) {
            if (i && !toml_output_append(output, ", ", 2u)) return 0;
            if (!toml_write_value(output, &value->value.array.items[i], depth + 1u))
                return 0;
        }
        return toml_output_append(output, "]", 1u);
    case QW_DATA_VALUE_OBJECT:
        if (!toml_output_append(output, "{", 1u)) return 0;
        for (i = 0u; i < value->value.object.count; ++i) {
            const QwDataMember *member = &value->value.object.items[i];
            if (i && !toml_output_append(output, ", ", 2u)) return 0;
            if (!toml_write_string(output, member->key, member->key_size) ||
                !toml_output_append(output, " = ", 3u) ||
                !toml_write_value(output, &member->value, depth + 1u)) return 0;
        }
        return toml_output_append(output, "}", 1u);
    default:
        return 0;
    }
}

static int toml_write_document(const QwDataDocument *document,
                               QwOutputFormat output_format,
                               const QwConvertOptions *options,
                               uint8_t **out_data, size_t *out_size,
                               QwConvertReport *report)
{
    QwTomlOutput output;
    size_t i;
    (void)options;
    if (!document || !out_data || !out_size || output_format != QW_FORMAT_TOML)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    if (document->root.kind != QW_DATA_VALUE_OBJECT) return QW_STATUS_ERR_UNSUPPORTED;
    memset(&output, 0, sizeof(output));
    output.limit = QW_TOML_MAX_OUTPUT;
    for (i = 0u; i < document->root.value.object.count; ++i) {
        const QwDataMember *member = &document->root.value.object.items[i];
        if (!toml_write_string(&output, member->key, member->key_size) ||
            !toml_output_append(&output, " = ", 3u) ||
            !toml_write_value(&output, &member->value, 0u) ||
            !toml_output_append(&output, "\n", 1u)) {
            int rc = output.error ? output.error : QW_STATUS_ERR_OUT_OF_MEMORY;
            free(output.data);
            return rc;
        }
    }
    *out_data = (uint8_t *)output.data;
    *out_size = output.size;
    if (report) {
        report->format = document->input_format;
        report->output_format = output_format;
        report->output_size = output.size;
        (void)snprintf(report->note, sizeof(report->note),
            "TOML inline tables preserve nested values; temporal scalars retain their TOML type");
    }
    return QW_STATUS_OK;
}

static int toml_open_data(const void *input, size_t size,
                          QwInputFormat input_format,
                          const QwConvertOptions *options,
                          QwDataDocument *out_document,
                          QwConvertReport *report)
{
    int rc;
    (void)options;
    rc = toml_parse_document(input, size, input_format, out_document);
    if (rc == QW_STATUS_OK && report) {
        report->format = input_format;
        report->output_format = QW_FORMAT_UNKNOWN;
        report->output_size = 0u;
        (void)snprintf(report->note, sizeof(report->note),
            "TOML tables and arrays opened through the shared typed value model");
    }
    return rc;
}

int qw_toml_parse_data_document(const void *data, size_t size,
                                QwDataDocument *out_document)
{
    return toml_parse_document(data, size, QW_FORMAT_TOML, out_document);
}

#ifndef QUIREWISE_STANDALONE_DATA_PARSER
const QwDataWriterDescriptor qw_data_toml_writer = {
    QW_FORMAT_TOML, "TOML data writer", toml_write_document,
    QW_TOML_MAX_OUTPUT
};

const QwFormatDescriptor qw_format_toml = {
    .id = QW_FORMAT_TOML,
    .name = "TOML",
    .capabilities = 0u,
    .probe = toml_probe,
    .open_memory = NULL,
    .open_layout = NULL,
    .open_audio = NULL,
    .max_input_bytes = QW_TOML_MAX_INPUT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .convert_to_pdf = NULL,
    .open_data = toml_open_data
};

const QwPathFormatDescriptor qw_path_format_toml = {
    .id = QW_FORMAT_TOML,
    .probe_path = toml_path_probe,
    .decode_document_path = NULL,
    .resource_profile = NULL
};
#endif
