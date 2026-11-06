#include "../../core/data_document.h"
#include "data_parser_internal.h"

#ifndef QUIREWISE_STANDALONE_DATA_PARSER
#include "../../core/format_registry.h"
#include "../../core/writer_registry.h"
#include "../../backends/mupdf_backend.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    QW_JSON_MAX_INPUT = 64 * 1024 * 1024,
    QW_JSON_HTML_LIMIT = 320 * 1024 * 1024,
    QW_JSON_MAX_DEPTH = QW_DATA_DOCUMENT_MAX_DEPTH,
    QW_JSON_MAX_NODES = QW_DATA_DOCUMENT_MAX_NODES
};

#define QW_JSON_DATA_MEMORY_LIMIT QW_DATA_DOCUMENT_MAX_MEMORY
#define QW_JSON_DATA_OUTPUT_LIMIT ((size_t)128u * 1024u * 1024u)

typedef struct QwJsonBuffer {
    char *data;
    size_t size;
    size_t capacity;
    size_t limit;
    int error;
} QwJsonBuffer;

typedef struct QwJsonParser {
    const uint8_t *data;
    size_t size;
    size_t position;
    QwJsonBuffer html;
    int emit;
    QwDataDocument *capture_document;
} QwJsonParser;

#define QW_JSONL_MAX_RECORDS 1000000u

static int json_buffer_append(QwJsonBuffer *buffer, const char *data, size_t size)
{
    size_t required;
    size_t capacity;
    char *grown;

    if (size > SIZE_MAX - buffer->size - 1u) {
        buffer->error = QW_STATUS_ERR_UNSUPPORTED;
        return 0;
    }
    required = buffer->size + size + 1u;
    if (required > buffer->limit) {
        buffer->error = QW_STATUS_ERR_UNSUPPORTED;
        return 0;
    }
    if (required > buffer->capacity) {
        capacity = buffer->capacity ? buffer->capacity : 4096u;
        while (capacity < required) {
            if (capacity > buffer->limit / 2u) {
                capacity = buffer->limit;
                break;
            }
            capacity *= 2u;
        }
        if (capacity < required) {
            buffer->error = QW_STATUS_ERR_UNSUPPORTED;
            return 0;
        }
        grown = (char *)realloc(buffer->data, capacity);
        if (!grown) {
            buffer->error = QW_STATUS_ERR_OUT_OF_MEMORY;
            return 0;
        }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
    return 1;
}

static int json_append_literal(QwJsonParser *parser, const char *literal)
{
    if (!parser->emit) return 1;
    return json_buffer_append(&parser->html, literal, strlen(literal));
}

static int json_append_range(QwJsonParser *parser, size_t start, size_t end)
{
    size_t run = start;
    size_t i;

    if (!parser->emit) return 1;
    for (i = start; i < end; ++i) {
        const char *escaped = NULL;
        size_t escaped_size = 0u;
        if (parser->data[i] == '&') {
            escaped = "&amp;";
            escaped_size = 5u;
        } else if (parser->data[i] == '<') {
            escaped = "&lt;";
            escaped_size = 4u;
        } else if (parser->data[i] == '>') {
            escaped = "&gt;";
            escaped_size = 4u;
        }
        if (escaped) {
            if (i > run && !json_buffer_append(&parser->html,
                                                (const char *)parser->data + run,
                                                i - run)) return 0;
            if (!json_buffer_append(&parser->html, escaped, escaped_size)) return 0;
            run = i + 1u;
        }
    }
    return run == end || json_buffer_append(&parser->html,
                                             (const char *)parser->data + run,
                                             end - run);
}

static int json_append_indent(QwJsonParser *parser, size_t depth)
{
    static const char spaces[] =
        "                                                                ";
    size_t count = depth * 2u;
    if (!parser->emit) return 1;
    while (count >= sizeof(spaces) - 1u) {
        if (!json_append_literal(parser, spaces)) return 0;
        count -= sizeof(spaces) - 1u;
    }
    return !count || json_buffer_append(&parser->html, spaces, count);
}

static void json_skip_whitespace(QwJsonParser *parser)
{
    while (parser->position < parser->size) {
        uint8_t ch = parser->data[parser->position];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
        ++parser->position;
    }
}

static int json_hex_value(uint8_t ch)
{
    if (ch >= '0' && ch <= '9') return (int)(ch - '0');
    if (ch >= 'a' && ch <= 'f') return (int)(ch - 'a') + 10;
    if (ch >= 'A' && ch <= 'F') return (int)(ch - 'A') + 10;
    return -1;
}

static int json_read_u16_escape(QwJsonParser *parser, unsigned *value)
{
    unsigned result = 0u;
    size_t i;
    if (parser->size - parser->position < 4u) return 0;
    for (i = 0u; i < 4u; ++i) {
        int digit = json_hex_value(parser->data[parser->position + i]);
        if (digit < 0) return 0;
        result = (result << 4) | (unsigned)digit;
    }
    parser->position += 4u;
    *value = result;
    return 1;
}

static void *json_capture_realloc(QwJsonParser *parser, void *memory,
                                  size_t old_size, size_t new_size)
{
    void *grown;
    QwDataDocument *document = parser->capture_document;
    if (!document || new_size < old_size ||
        new_size - old_size > QW_JSON_DATA_MEMORY_LIMIT -
                                  document->allocation_bytes) {
        parser->html.error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return NULL;
    }
    grown = realloc(memory, new_size);
    if (!grown) {
        parser->html.error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    document->allocation_bytes += new_size - old_size;
    return grown;
}

static int json_capture_node(QwJsonParser *parser, QwDataValue *out_value)
{
    if (!out_value) return 1;
    if (!parser->capture_document ||
        parser->capture_document->node_count >= QW_JSON_MAX_NODES) {
        parser->html.error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    ++parser->capture_document->node_count;
    memset(out_value, 0, sizeof(*out_value));
    return 1;
}

static int json_decoded_append_codepoint(QwJsonBuffer *buffer,
                                         unsigned codepoint)
{
    char bytes[4];
    size_t count;
    if (codepoint <= 0x7fu) {
        bytes[0] = (char)codepoint;
        count = 1u;
    } else if (codepoint <= 0x7ffu) {
        bytes[0] = (char)(0xc0u | (codepoint >> 6));
        bytes[1] = (char)(0x80u | (codepoint & 0x3fu));
        count = 2u;
    } else if (codepoint <= 0xffffu) {
        bytes[0] = (char)(0xe0u | (codepoint >> 12));
        bytes[1] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
        bytes[2] = (char)(0x80u | (codepoint & 0x3fu));
        count = 3u;
    } else if (codepoint <= 0x10ffffu) {
        bytes[0] = (char)(0xf0u | (codepoint >> 18));
        bytes[1] = (char)(0x80u | ((codepoint >> 12) & 0x3fu));
        bytes[2] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
        bytes[3] = (char)(0x80u | (codepoint & 0x3fu));
        count = 4u;
    } else {
        return 0;
    }
    return json_buffer_append(buffer, bytes, count);
}

static int json_parse_string(QwJsonParser *parser, QwDataValue *out_value)
{
    size_t start = parser->position;
    QwJsonBuffer decoded;
    int capture = out_value != NULL;
    memset(&decoded, 0, sizeof(decoded));
    decoded.limit = (size_t)QW_JSON_MAX_INPUT + 1u;
    if (parser->position >= parser->size || parser->data[parser->position] != '"')
        return 0;
    ++parser->position;
    while (parser->position < parser->size) {
        uint8_t ch = parser->data[parser->position++];
        if (ch == '"') {
            char *copy;
            size_t copy_size;
            if (!json_append_range(parser, start, parser->position)) goto failure;
            if (!capture) return 1;
            if (decoded.size == SIZE_MAX) goto failure;
            copy_size = decoded.size + 1u;
            copy = (char *)json_capture_realloc(parser, NULL, 0u, copy_size);
            if (!copy) goto failure;
            if (decoded.size) memcpy(copy, decoded.data, decoded.size);
            copy[decoded.size] = '\0';
            out_value->kind = QW_DATA_VALUE_STRING;
            out_value->value.text.data = copy;
            out_value->value.text.size = decoded.size;
            free(decoded.data);
            return 1;
        }
        if (ch < 0x20u) goto failure;
        if (ch != '\\') {
            if (capture && !json_buffer_append(&decoded, (const char *)&ch, 1u))
                goto failure;
            continue;
        }
        if (parser->position >= parser->size) goto failure;
        ch = parser->data[parser->position++];
        if (ch == '"' || ch == '\\' || ch == '/') {
            if (capture && !json_buffer_append(&decoded, (const char *)&ch, 1u))
                goto failure;
        } else if (ch == 'b' || ch == 'f' || ch == 'n' || ch == 'r' || ch == 't') {
            char decoded_ch = ch == 'b' ? '\b' : ch == 'f' ? '\f' :
                              ch == 'n' ? '\n' : ch == 'r' ? '\r' : '\t';
            if (capture &&
                !json_buffer_append(&decoded, &decoded_ch, 1u)) goto failure;
        } else if (ch == 'u') {
            unsigned codepoint;
            if (!json_read_u16_escape(parser, &codepoint)) goto failure;
            if (codepoint >= 0xd800u && codepoint <= 0xdbffu) {
                unsigned low;
                if (parser->size - parser->position < 6u ||
                    parser->data[parser->position] != '\\' ||
                    parser->data[parser->position + 1u] != 'u') goto failure;
                parser->position += 2u;
                if (!json_read_u16_escape(parser, &low) ||
                    low < 0xdc00u || low > 0xdfffu) goto failure;
                codepoint = 0x10000u + ((codepoint - 0xd800u) << 10) +
                            (low - 0xdc00u);
            } else if (codepoint >= 0xdc00u && codepoint <= 0xdfffu) {
                goto failure;
            }
            if (capture &&
                !json_decoded_append_codepoint(&decoded, codepoint)) goto failure;
        } else {
            goto failure;
        }
    }
failure:
    free(decoded.data);
    return 0;
}

static int json_parse_number(QwJsonParser *parser, QwDataValue *out_value)
{
    size_t start = parser->position;
    if (parser->data[parser->position] == '-') ++parser->position;
    if (parser->position >= parser->size) return 0;
    if (parser->data[parser->position] == '0') {
        ++parser->position;
        if (parser->position < parser->size &&
            parser->data[parser->position] >= '0' &&
            parser->data[parser->position] <= '9') return 0;
    } else {
        if (parser->data[parser->position] < '1' ||
            parser->data[parser->position] > '9') return 0;
        while (parser->position < parser->size &&
               parser->data[parser->position] >= '0' &&
               parser->data[parser->position] <= '9') ++parser->position;
    }
    if (parser->position < parser->size && parser->data[parser->position] == '.') {
        ++parser->position;
        if (parser->position >= parser->size ||
            parser->data[parser->position] < '0' ||
            parser->data[parser->position] > '9') return 0;
        while (parser->position < parser->size &&
               parser->data[parser->position] >= '0' &&
               parser->data[parser->position] <= '9') ++parser->position;
    }
    if (parser->position < parser->size &&
        (parser->data[parser->position] == 'e' ||
         parser->data[parser->position] == 'E')) {
        ++parser->position;
        if (parser->position < parser->size &&
            (parser->data[parser->position] == '+' ||
             parser->data[parser->position] == '-')) ++parser->position;
        if (parser->position >= parser->size ||
            parser->data[parser->position] < '0' ||
            parser->data[parser->position] > '9') return 0;
        while (parser->position < parser->size &&
               parser->data[parser->position] >= '0' &&
               parser->data[parser->position] <= '9') ++parser->position;
    }
    if (out_value) {
        size_t number_size = parser->position - start;
        char *number = (char *)json_capture_realloc(parser, NULL, 0u,
                                                     number_size + 1u);
        if (!number) return 0;
        memcpy(number, parser->data + start, number_size);
        number[number_size] = '\0';
        out_value->kind = QW_DATA_VALUE_NUMBER;
        out_value->value.text.data = number;
        out_value->value.text.size = number_size;
    }
    return json_append_range(parser, start, parser->position);
}

static int json_parse_value(QwJsonParser *parser, size_t depth,
                            QwDataValue *out_value);

static int json_parse_object(QwJsonParser *parser, size_t depth,
                             QwDataValue *out_value)
{
    QwDataValue object;
    memset(&object, 0, sizeof(object));
    object.kind = QW_DATA_VALUE_OBJECT;
    ++parser->position;
    if (!json_append_literal(parser, "{")) goto failure;
    json_skip_whitespace(parser);
    if (parser->position < parser->size && parser->data[parser->position] == '}') {
        ++parser->position;
        if (!json_append_literal(parser, "}")) goto failure;
        if (out_value) *out_value = object;
        return 1;
    }
    if (!json_append_literal(parser, "\n")) goto failure;
    for (;;) {
        QwDataValue key;
        QwDataValue member_value;
        char *key_data = NULL;
        size_t key_size = 0u;
        memset(&key, 0, sizeof(key));
        memset(&member_value, 0, sizeof(member_value));
        json_skip_whitespace(parser);
        if (!json_append_indent(parser, depth + 1u) ||
            !json_parse_string(parser, out_value ? &key : NULL)) goto member_failure;
        json_skip_whitespace(parser);
        if (parser->position >= parser->size || parser->data[parser->position] != ':')
            goto member_failure;
        ++parser->position;
        if (!json_append_literal(parser, ": ")) goto member_failure;
        json_skip_whitespace(parser);
        if (!json_parse_value(parser, depth + 1u,
                             out_value ? &member_value : NULL)) goto member_failure;
        if (out_value) {
            QwDataMember *grown;
            size_t new_capacity;
            size_t old_bytes;
            size_t new_bytes;
            if (object.value.object.count == object.value.object.capacity) {
                new_capacity = object.value.object.capacity
                                   ? object.value.object.capacity * 2u : 8u;
                if (new_capacity < object.value.object.capacity ||
                    new_capacity > SIZE_MAX / sizeof(*grown))
                    goto member_failure;
                old_bytes = object.value.object.capacity * sizeof(*grown);
                new_bytes = new_capacity * sizeof(*grown);
                grown = (QwDataMember *)json_capture_realloc(
                    parser, object.value.object.items, old_bytes, new_bytes);
                if (!grown) goto member_failure;
                object.value.object.items = grown;
                object.value.object.capacity = new_capacity;
            }
            key_data = key.value.text.data;
            key_size = key.value.text.size;
            object.value.object.items[object.value.object.count].key = key_data;
            object.value.object.items[object.value.object.count].key_size = key_size;
            object.value.object.items[object.value.object.count].value = member_value;
            ++object.value.object.count;
            memset(&key, 0, sizeof(key));
            memset(&member_value, 0, sizeof(member_value));
        }
        json_skip_whitespace(parser);
        if (parser->position >= parser->size) goto member_failure;
        if (parser->data[parser->position] == '}') {
            ++parser->position;
            if (!json_append_literal(parser, "\n") ||
                !json_append_indent(parser, depth) ||
                !json_append_literal(parser, "}")) goto member_failure;
            if (out_value) *out_value = object;
            return 1;
        }
        if (parser->data[parser->position] != ',') goto member_failure;
        ++parser->position;
        if (!json_append_literal(parser, ",\n")) goto member_failure;
        continue;
member_failure:
        qw_data_value_dispose(&key);
        qw_data_value_dispose(&member_value);
        goto failure;
    }
failure:
    qw_data_value_dispose(&object);
    return 0;
}

static int json_parse_array(QwJsonParser *parser, size_t depth,
                            QwDataValue *out_value)
{
    QwDataValue array;
    memset(&array, 0, sizeof(array));
    array.kind = QW_DATA_VALUE_ARRAY;
    ++parser->position;
    if (!json_append_literal(parser, "[")) goto failure;
    json_skip_whitespace(parser);
    if (parser->position < parser->size && parser->data[parser->position] == ']') {
        ++parser->position;
        if (!json_append_literal(parser, "]")) goto failure;
        if (out_value) *out_value = array;
        return 1;
    }
    if (!json_append_literal(parser, "\n")) goto failure;
    for (;;) {
        QwDataValue item;
        memset(&item, 0, sizeof(item));
        json_skip_whitespace(parser);
        if (!json_append_indent(parser, depth + 1u) ||
            !json_parse_value(parser, depth + 1u, out_value ? &item : NULL)) {
            qw_data_value_dispose(&item);
            goto failure;
        }
        if (out_value) {
            QwDataValue *grown;
            size_t new_capacity;
            size_t old_bytes;
            size_t new_bytes;
            if (array.value.array.count == array.value.array.capacity) {
                new_capacity = array.value.array.capacity
                                   ? array.value.array.capacity * 2u : 8u;
                if (new_capacity < array.value.array.capacity ||
                    new_capacity > SIZE_MAX / sizeof(*grown)) {
                    qw_data_value_dispose(&item);
                    goto failure;
                }
                old_bytes = array.value.array.capacity * sizeof(*grown);
                new_bytes = new_capacity * sizeof(*grown);
                grown = (QwDataValue *)json_capture_realloc(
                    parser, array.value.array.items, old_bytes, new_bytes);
                if (!grown) {
                    qw_data_value_dispose(&item);
                    goto failure;
                }
                array.value.array.items = grown;
                array.value.array.capacity = new_capacity;
            }
            array.value.array.items[array.value.array.count++] = item;
            memset(&item, 0, sizeof(item));
        }
        json_skip_whitespace(parser);
        if (parser->position >= parser->size) goto failure;
        if (parser->data[parser->position] == ']') {
            ++parser->position;
            if (!json_append_literal(parser, "\n") ||
                !json_append_indent(parser, depth) ||
                !json_append_literal(parser, "]")) goto failure;
            if (out_value) *out_value = array;
            return 1;
        }
        if (parser->data[parser->position] != ',') goto failure;
        ++parser->position;
        if (!json_append_literal(parser, ",\n")) goto failure;
    }
failure:
    qw_data_value_dispose(&array);
    return 0;
}

static int json_parse_value(QwJsonParser *parser, size_t depth,
                            QwDataValue *out_value)
{
    size_t start;
    if (depth > QW_JSON_MAX_DEPTH || parser->position >= parser->size ||
        !json_capture_node(parser, out_value)) return 0;
    switch (parser->data[parser->position]) {
    case '{': return json_parse_object(parser, depth, out_value);
    case '[': return json_parse_array(parser, depth, out_value);
    case '"': return json_parse_string(parser, out_value);
    case 't':
        start = parser->position;
        if (parser->size - start < 4u || memcmp(parser->data + start, "true", 4u))
            return 0;
        parser->position += 4u;
        if (out_value) {
            out_value->kind = QW_DATA_VALUE_BOOLEAN;
            out_value->value.boolean = 1;
        }
        return json_append_range(parser, start, parser->position);
    case 'f':
        start = parser->position;
        if (parser->size - start < 5u || memcmp(parser->data + start, "false", 5u))
            return 0;
        parser->position += 5u;
        if (out_value) {
            out_value->kind = QW_DATA_VALUE_BOOLEAN;
            out_value->value.boolean = 0;
        }
        return json_append_range(parser, start, parser->position);
    case 'n':
        start = parser->position;
        if (parser->size - start < 4u || memcmp(parser->data + start, "null", 4u))
            return 0;
        parser->position += 4u;
        if (out_value) out_value->kind = QW_DATA_VALUE_NULL;
        return json_append_range(parser, start, parser->position);
    default:
        if (parser->data[parser->position] == '-' ||
            (parser->data[parser->position] >= '0' &&
             parser->data[parser->position] <= '9'))
            return json_parse_number(parser, out_value);
        return 0;
    }
}

static int json_utf8_valid(const uint8_t *data, size_t size)
{
    size_t i = 0u;
    while (i < size) {
        uint8_t first = data[i];
        size_t count;
        if (first < 0x80u) count = 1u;
        else if (first >= 0xc2u && first <= 0xdfu) count = 2u;
        else if (first >= 0xe0u && first <= 0xefu) count = 3u;
        else if (first >= 0xf0u && first <= 0xf4u) count = 4u;
        else return 0;
        if (count > size - i) return 0;
        for (size_t j = 1u; j < count; ++j) {
            if ((data[i + j] & 0xc0u) != 0x80u) return 0;
        }
        if ((first == 0xe0u && data[i + 1u] < 0xa0u) ||
            (first == 0xedu && data[i + 1u] >= 0xa0u) ||
            (first == 0xf0u && data[i + 1u] < 0x90u) ||
            (first == 0xf4u && data[i + 1u] >= 0x90u)) return 0;
        i += count;
    }
    return 1;
}

static int json_parse_document(QwJsonParser *parser, QwDataValue *out_value)
{
    json_skip_whitespace(parser);
    if (parser->position >= parser->size ||
        !json_parse_value(parser, 0u, out_value)) return 0;
    json_skip_whitespace(parser);
    if (parser->position != parser->size) {
        if (out_value) qw_data_value_dispose(out_value);
        return 0;
    }
    return 1;
}

static int json_prepare_parser(QwJsonParser *parser, const void *input, size_t size,
                               int emit)
{
    size_t offset = 0u;
    if (!input || !size || size > QW_JSON_MAX_INPUT) return 0;
    parser->data = (const uint8_t *)input;
    parser->size = size;
    parser->position = 0u;
    parser->emit = emit;
    if (size >= 3u && parser->data[0] == 0xefu && parser->data[1] == 0xbbu &&
        parser->data[2] == 0xbfu) offset = 3u;
    parser->data += offset;
    parser->size -= offset;
    if (!parser->size || !json_utf8_valid(parser->data, parser->size)) return 0;
    if (emit) {
        memset(&parser->html, 0, sizeof(parser->html));
        parser->html.limit = QW_JSON_HTML_LIMIT;
    }
    return 1;
}

static int json_probe(const void *input, size_t size)
{
    QwJsonParser parser;
    memset(&parser, 0, sizeof(parser));
    return json_prepare_parser(&parser, input, size, 0) &&
           json_parse_document(&parser, NULL);
}

#ifndef QUIREWISE_STANDALONE_DATA_PARSER
static int json_convert_to_pdf(const void *input,
                               size_t size,
                               const QwConvertOptions *options,
                               uint8_t **out_pdf,
                               size_t *out_size,
                               QwConvertReport *report)
{
    static const char prefix[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
        "@page{size:A4;margin:16mm}body{font-family:monospace;font-size:9pt}"
        "pre{white-space:pre-wrap;overflow-wrap:anywhere}"
        "</style></head><body><pre>";
    static const char suffix[] = "</pre></body></html>";
    QwJsonParser parser;
    int rc;

    (void)options;
    if (!out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(&parser, 0, sizeof(parser));
    if (!json_prepare_parser(&parser, input, size, 1))
        return size > QW_JSON_MAX_INPUT ? QW_STATUS_ERR_UNSUPPORTED : QW_STATUS_ERR_FORMAT;
    if (!json_buffer_append(&parser.html, prefix, sizeof(prefix) - 1u))
        return parser.html.error;
    if (!json_parse_document(&parser, NULL)) {
        rc = parser.html.error ? parser.html.error : QW_STATUS_ERR_FORMAT;
        free(parser.html.data);
        return rc;
    }
    if (!json_buffer_append(&parser.html, suffix, sizeof(suffix) - 1u)) {
        rc = parser.html.error;
        free(parser.html.data);
        return rc;
    }
    /* JSON is parsed and pretty-printed to HTML above; PDF emission belongs to
     * the internal Quirewise layout/PDF renderer, which this package does not
     * provide. Return a stable unsupported status with no output. */
    rc = QW_STATUS_ERR_UNSUPPORTED;
    free(parser.html.data);
    if (report)
        snprintf(report->note, sizeof(report->note),
                 "JSON value parsed; PDF rendering requires the internal Quirewise layout/PDF renderer");
    return rc;
}
#endif










static int jsonl_parse_records(QwJsonParser *parser, size_t *out_count,
                               QwDataValue *out_root)
{
    const uint8_t *document;
    size_t document_size;
    size_t line_start = 0u;
    size_t record_count = 0u;
    QwDataValue records;

    if (!parser || !out_count) return 0;
    memset(&records, 0, sizeof(records));
    *out_count = 0u;
    if (out_root) {
        if (!json_capture_node(parser, &records)) return 0;
        records.kind = QW_DATA_VALUE_ARRAY;
    }
    document = parser->data;
    document_size = parser->size;
    while (line_start < document_size) {
        size_t line_end = line_start;
        size_t value_end;
        char heading[64];
        int heading_size;
        QwDataValue record;
        memset(&record, 0, sizeof(record));

        while (line_end < document_size && document[line_end] != '\n')
            ++line_end;
        value_end = line_end;
        if (value_end > line_start && document[value_end - 1u] == '\r')
            --value_end;
        if (value_end == line_start) goto failure;
        if (record_count >= QW_JSONL_MAX_RECORDS) {
            parser->html.error = QW_STATUS_ERR_RESOURCE_LIMIT;
            goto failure;
        }

        parser->data = document + line_start;
        parser->size = value_end - line_start;
        parser->position = 0u;
        json_skip_whitespace(parser);
        if (parser->position >= parser->size) return 0;
        if (parser->emit) {
            heading_size = snprintf(heading, sizeof(heading),
                                    "%s<h2>Record %zu</h2><pre>",
                                    record_count ? "<hr/>" : "",
                                    record_count + 1u);
            if (heading_size < 0 || (size_t)heading_size >= sizeof(heading) ||
                !json_buffer_append(&parser->html, heading,
                                   (size_t)heading_size)) goto failure;
        }
        if (!json_parse_value(parser, 0u, out_root ? &record : NULL)) {
            qw_data_value_dispose(&record);
            goto failure;
        }
        json_skip_whitespace(parser);
        if (parser->position != parser->size) {
            qw_data_value_dispose(&record);
            goto failure;
        }
        if (out_root) {
            QwDataValue *grown;
            size_t new_capacity;
            size_t old_bytes;
            size_t new_bytes;
            if (records.value.array.count == records.value.array.capacity) {
                new_capacity = records.value.array.capacity
                                   ? records.value.array.capacity * 2u : 8u;
                if (new_capacity < records.value.array.capacity ||
                    new_capacity > SIZE_MAX / sizeof(*grown)) {
                    qw_data_value_dispose(&record);
                    goto failure;
                }
                old_bytes = records.value.array.capacity * sizeof(*grown);
                new_bytes = new_capacity * sizeof(*grown);
                grown = (QwDataValue *)json_capture_realloc(
                    parser, records.value.array.items, old_bytes, new_bytes);
                if (!grown) {
                    qw_data_value_dispose(&record);
                    goto failure;
                }
                records.value.array.items = grown;
                records.value.array.capacity = new_capacity;
            }
            records.value.array.items[records.value.array.count++] = record;
            memset(&record, 0, sizeof(record));
        }
        if (parser->emit && !json_append_literal(parser, "</pre>"))
            goto failure;

        ++record_count;
        if (line_end == document_size) break;
        line_start = line_end + 1u;
    }
    parser->data = document;
    parser->size = document_size;
    parser->position = document_size;
    *out_count = record_count;
    if (record_count == 0u) goto failure;
    if (out_root) *out_root = records;
    return 1;
failure:
    qw_data_value_dispose(&records);
    return 0;
}

static int jsonl_probe(const void *input, size_t size)
{
    QwJsonParser parser;
    size_t record_count;
    if (size > QW_JSON_MAX_INPUT) return 0;
    memset(&parser, 0, sizeof(parser));
    return json_prepare_parser(&parser, input, size, 0) &&
           jsonl_parse_records(&parser, &record_count, NULL);
}

#ifndef QUIREWISE_STANDALONE_DATA_PARSER
static int jsonl_convert_to_pdf(const void *input,
                                size_t size,
                                const QwConvertOptions *options,
                                uint8_t **out_pdf,
                                size_t *out_size,
                                QwConvertReport *report)
{
    static const char prefix[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
        "@page{size:A4;margin:16mm}body{font-family:monospace;font-size:9pt}"
        "pre{white-space:pre-wrap;overflow-wrap:anywhere}"
        "h2{font-family:sans-serif;font-size:11pt}"
        "hr{border:0;border-top:1px solid #bbb}"
        "</style></head><body>";
    static const char suffix[] = "</body></html>";
    QwJsonParser parser;
    size_t record_count;
    int rc;

    (void)options;
    if (!out_pdf || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(&parser, 0, sizeof(parser));
    if (!json_prepare_parser(&parser, input, size, 1))
        return size > QW_JSON_MAX_INPUT ? QW_STATUS_ERR_UNSUPPORTED : QW_STATUS_ERR_FORMAT;
    if (!json_buffer_append(&parser.html, prefix, sizeof(prefix) - 1u) ||
        !jsonl_parse_records(&parser, &record_count, NULL)) {
        rc = parser.html.error ? parser.html.error : QW_STATUS_ERR_FORMAT;
        free(parser.html.data);
        return rc;
    }
    if (!json_buffer_append(&parser.html, suffix, sizeof(suffix) - 1u)) {
        rc = parser.html.error;
        free(parser.html.data);
        return rc;
    }
    /* JSON Lines records are parsed and converted to HTML above; PDF emission
     * belongs to the internal Quirewise layout/PDF renderer. */
    rc = QW_STATUS_ERR_UNSUPPORTED;
    free(parser.html.data);
    if (report)
        snprintf(report->note, sizeof(report->note),
                 "JSON Lines records parsed (%zu records); PDF rendering requires the internal Quirewise layout/PDF renderer",
                 record_count);
    return rc;
}
#endif

static int json_open_data_common(const void *input, size_t size,
                                 QwInputFormat input_format,
                                 QwDataDocument *out_document,
                                 QwConvertReport *report, int jsonl)
{
    QwJsonParser parser;
    size_t record_count = 0u;
    int parsed;
    int rc;
    if (!out_document || !input || !size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (size > QW_JSON_MAX_INPUT) return QW_STATUS_ERR_RESOURCE_LIMIT;
    qw_data_document_init(out_document, input_format);
    memset(&parser, 0, sizeof(parser));
    parser.capture_document = out_document;
    if (!json_prepare_parser(&parser, input, size, 0)) {
        rc = QW_STATUS_ERR_FORMAT;
        goto failure;
    }
    parsed = jsonl
        ? jsonl_parse_records(&parser, &record_count, &out_document->root)
        : json_parse_document(&parser, &out_document->root);
    if (!parsed || !qw_data_document_validate(out_document)) {
        rc = parser.html.error ? parser.html.error : QW_STATUS_ERR_FORMAT;
        goto failure;
    }
    if (report) {
        report->format = input_format;
        report->output_format = QW_FORMAT_UNKNOWN;
        report->output_size = 0u;
        if (jsonl) {
            (void)snprintf(report->note, sizeof(report->note),
                           "JSON Lines records opened as an ordered array (%zu records)",
                           record_count);
        }
    }
    return QW_STATUS_OK;
failure:
    qw_data_document_dispose(out_document);
    return rc;
}

static int json_open_data(const void *input, size_t size,
                          QwInputFormat input_format,
                          const QwConvertOptions *options,
                          QwDataDocument *out_document,
                          QwConvertReport *report)
{
    (void)options;
    return json_open_data_common(input, size, input_format, out_document,
                                 report, 0);
}

static int jsonl_open_data(const void *input, size_t size,
                           QwInputFormat input_format,
                           const QwConvertOptions *options,
                           QwDataDocument *out_document,
                           QwConvertReport *report)
{
    (void)options;
    return json_open_data_common(input, size, input_format, out_document,
                                 report, 1);
}

int qw_json_parse_data_document(const void *data, size_t size, int jsonl,
                                QwDataDocument *out_document)
{
    QwInputFormat format = jsonl ? QW_FORMAT_JSONL : QW_FORMAT_JSON;
    return json_open_data_common(data, size, format, out_document, NULL, jsonl);
}

int qw_json_parse_data_document_format(const void *data, size_t size,
                                      QwInputFormat input_format, int jsonl,
                                      QwDataDocument *out_document)
{
    if ((!jsonl && input_format != QW_FORMAT_JSON &&
         input_format != QW_FORMAT_IPYNB) ||
        (jsonl && input_format != QW_FORMAT_JSONL)) {
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    return json_open_data_common(data, size, input_format,
                                 out_document, NULL, jsonl);
}

#ifndef QUIREWISE_STANDALONE_DATA_PARSER
static int json_write_string(QwJsonBuffer *buffer, const char *data,
                             size_t size)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    if (!json_buffer_append(buffer, "\"", 1u)) return 0;
    for (i = 0u; i < size; ++i) {
        unsigned char ch = (unsigned char)data[i];
        const char *escape = NULL;
        size_t escape_size = 0u;
        char unicode_escape[6];
        if (ch == '"') { escape = "\\\""; escape_size = 2u; }
        else if (ch == '\\') { escape = "\\\\"; escape_size = 2u; }
        else if (ch == '\b') { escape = "\\b"; escape_size = 2u; }
        else if (ch == '\f') { escape = "\\f"; escape_size = 2u; }
        else if (ch == '\n') { escape = "\\n"; escape_size = 2u; }
        else if (ch == '\r') { escape = "\\r"; escape_size = 2u; }
        else if (ch == '\t') { escape = "\\t"; escape_size = 2u; }
        if (escape) {
            if (!json_buffer_append(buffer, escape, escape_size)) return 0;
        } else if (ch < 0x20u) {
            unicode_escape[0] = '\\';
            unicode_escape[1] = 'u';
            unicode_escape[2] = '0';
            unicode_escape[3] = '0';
            unicode_escape[4] = hex[ch >> 4];
            unicode_escape[5] = hex[ch & 0x0fu];
            if (!json_buffer_append(buffer, unicode_escape,
                                    sizeof(unicode_escape))) return 0;
        } else if (!json_buffer_append(buffer, (const char *)&data[i], 1u)) {
            return 0;
        }
    }
    return json_buffer_append(buffer, "\"", 1u);
}

static int json_write_indent(QwJsonBuffer *buffer, size_t depth)
{
    static const char spaces[] = "                                                                ";
    size_t count = depth * 2u;
    while (count >= sizeof(spaces) - 1u) {
        if (!json_buffer_append(buffer, spaces, sizeof(spaces) - 1u)) return 0;
        count -= sizeof(spaces) - 1u;
    }
    return !count || json_buffer_append(buffer, spaces, count);
}

static int json_write_value(QwJsonBuffer *buffer, const QwDataValue *value,
                            size_t depth, int pretty)
{
    size_t i;
    if (!value || depth > QW_JSON_MAX_DEPTH) return 0;
    switch (value->kind) {
    case QW_DATA_VALUE_NULL:
        return json_buffer_append(buffer, "null", 4u);
    case QW_DATA_VALUE_BOOLEAN:
        return value->value.boolean
            ? json_buffer_append(buffer, "true", 4u)
            : json_buffer_append(buffer, "false", 5u);
    case QW_DATA_VALUE_NUMBER:
        return json_buffer_append(buffer, value->value.text.data,
                                  value->value.text.size);
    case QW_DATA_VALUE_STRING:
        return json_write_string(buffer, value->value.text.data,
                                 value->value.text.size);
    case QW_DATA_VALUE_TEMPORAL:
        return json_write_string(buffer, value->value.text.data,
                                 value->value.text.size);
    case QW_DATA_VALUE_ARRAY:
        if (!value->value.array.count)
            return json_buffer_append(buffer, "[]", 2u);
        if (!json_buffer_append(buffer, "[", 1u)) return 0;
        for (i = 0u; i < value->value.array.count; ++i) {
            if (i && !json_buffer_append(buffer, ",", 1u)) return 0;
            if (pretty && (!json_buffer_append(buffer, "\n", 1u) ||
                           !json_write_indent(buffer, depth + 1u))) return 0;
            if (!json_write_value(buffer, &value->value.array.items[i],
                                 depth + 1u, pretty)) return 0;
        }
        return (!pretty || (json_buffer_append(buffer, "\n", 1u) &&
                            json_write_indent(buffer, depth))) &&
               json_buffer_append(buffer, "]", 1u);
    case QW_DATA_VALUE_OBJECT:
        if (!value->value.object.count)
            return json_buffer_append(buffer, "{}", 2u);
        if (!json_buffer_append(buffer, "{", 1u)) return 0;
        for (i = 0u; i < value->value.object.count; ++i) {
            const QwDataMember *member = &value->value.object.items[i];
            if (i && !json_buffer_append(buffer, ",", 1u)) return 0;
            if (pretty && (!json_buffer_append(buffer, "\n", 1u) ||
                           !json_write_indent(buffer, depth + 1u))) return 0;
            if (!json_write_string(buffer, member->key, member->key_size) ||
                !json_buffer_append(buffer, pretty ? ": " : ":", pretty ? 2u : 1u) ||
                !json_write_value(buffer, &member->value, depth + 1u,
                                  pretty)) return 0;
        }
        return (!pretty || (json_buffer_append(buffer, "\n", 1u) &&
                            json_write_indent(buffer, depth))) &&
               json_buffer_append(buffer, "}", 1u);
    default:
        return 0;
    }
}

static int json_data_write_document(const QwDataDocument *document,
                                    QwOutputFormat output_format,
                                    const QwConvertOptions *options,
                                    uint8_t **out_data, size_t *out_size,
                                    QwConvertReport *report)
{
    QwJsonBuffer buffer;
    size_t i;
    int rc;
    (void)options;
    if (!out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    memset(&buffer, 0, sizeof(buffer));
    buffer.limit = QW_JSON_DATA_OUTPUT_LIMIT;
    if (output_format == QW_FORMAT_JSON) {
        if (!json_write_value(&buffer, &document->root, 0u, 1)) goto failure;
    } else if (output_format == QW_FORMAT_JSONL) {
        size_t count = document->root.kind == QW_DATA_VALUE_ARRAY
                           ? document->root.value.array.count : 1u;
        if (!count) {
            if (report)
                (void)snprintf(report->note, sizeof(report->note),
                    "an empty JSON array cannot be represented as JSON Lines");
            return QW_STATUS_ERR_UNSUPPORTED;
        }
        for (i = 0u; i < count; ++i) {
            const QwDataValue *record = document->root.kind == QW_DATA_VALUE_ARRAY
                ? &document->root.value.array.items[i] : &document->root;
            if (!json_write_value(&buffer, record, 0u, 0) ||
                !json_buffer_append(&buffer, "\n", 1u)) goto failure;
        }
    } else {
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    *out_data = (uint8_t *)buffer.data;
    *out_size = buffer.size;
    if (report && document->input_format != output_format) {
        report->format = document->input_format;
        report->output_format = output_format;
        report->output_size = buffer.size;
        (void)snprintf(report->note, sizeof(report->note),
            output_format == QW_FORMAT_JSONL
                ? "JSON array elements are emitted as individual JSON Lines records"
                : "JSON Lines records are collected into a JSON array");
    }
    return QW_STATUS_OK;
failure:
    rc = buffer.error ? buffer.error : QW_STATUS_ERR_OUT_OF_MEMORY;
    free(buffer.data);
    return rc;
}

const QwDataWriterDescriptor qw_data_json_writer = {
    QW_FORMAT_JSON, "JSON data writer", json_data_write_document,
    QW_JSON_DATA_OUTPUT_LIMIT
};

const QwDataWriterDescriptor qw_data_jsonl_writer = {
    QW_FORMAT_JSONL, "JSON Lines data writer", json_data_write_document,
    QW_JSON_DATA_OUTPUT_LIMIT
};

const QwFormatDescriptor qw_format_json = {
    .id = QW_FORMAT_JSON,
    .name = "JSON",
    .capabilities = (QW_FORMAT_CAP_MULTI_PAGE) | (0u),
    .probe = json_probe,
    .open_memory = NULL,
    .open_layout = NULL,
    .open_audio = NULL,
    .max_input_bytes = QW_JSON_MAX_INPUT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .convert_to_pdf = json_convert_to_pdf,
    .open_data = json_open_data
};

const QwFormatDescriptor qw_format_jsonl = {
    .id = QW_FORMAT_JSONL,
    .name = "JSON Lines",
    .capabilities = QW_FORMAT_CAP_MULTI_PAGE,
    .probe = jsonl_probe,
    .open_memory = NULL,
    .open_layout = NULL,
    .open_audio = NULL,
    .max_input_bytes = QW_JSON_MAX_INPUT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .convert_to_pdf = jsonl_convert_to_pdf,
    .open_data = jsonl_open_data
};
#endif
