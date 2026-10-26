#include "data_document.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void qw_data_value_dispose(QwDataValue *value)
{
    size_t i;
    if (!value) return;
    if (value->kind == QW_DATA_VALUE_STRING ||
        value->kind == QW_DATA_VALUE_NUMBER ||
        value->kind == QW_DATA_VALUE_TEMPORAL) {
        free(value->value.text.data);
    } else if (value->kind == QW_DATA_VALUE_ARRAY) {
        for (i = 0u; i < value->value.array.count; ++i)
            qw_data_value_dispose(&value->value.array.items[i]);
        free(value->value.array.items);
    } else if (value->kind == QW_DATA_VALUE_OBJECT) {
        for (i = 0u; i < value->value.object.count; ++i) {
            free(value->value.object.items[i].key);
            qw_data_value_dispose(&value->value.object.items[i].value);
        }
        free(value->value.object.items);
    }
    memset(value, 0, sizeof(*value));
}

void qw_data_document_init(QwDataDocument *document,
                           QwInputFormat input_format)
{
    if (!document) return;
    memset(document, 0, sizeof(*document));
    document->input_format = input_format;
}

void qw_data_document_dispose(QwDataDocument *document)
{
    if (!document) return;
    qw_data_value_dispose(&document->root);
    memset(document, 0, sizeof(*document));
}

static int data_utf8_valid(const char *data, size_t size)
{
    size_t i = 0u;
    if (!data && size) return 0;
    while (i < size) {
        const unsigned char first = (unsigned char)data[i];
        size_t count;
        if (first < 0x80u) count = 1u;
        else if (first >= 0xc2u && first <= 0xdfu) count = 2u;
        else if (first >= 0xe0u && first <= 0xefu) count = 3u;
        else if (first >= 0xf0u && first <= 0xf4u) count = 4u;
        else return 0;
        if (count > size - i) return 0;
        if (count > 1u &&
            (((unsigned char)data[i + 1u] & 0xc0u) != 0x80u ||
             (count > 2u &&
              ((unsigned char)data[i + 2u] & 0xc0u) != 0x80u) ||
             (count > 3u &&
              ((unsigned char)data[i + 3u] & 0xc0u) != 0x80u))) return 0;
        if ((first == 0xe0u && (unsigned char)data[i + 1u] < 0xa0u) ||
            (first == 0xedu && (unsigned char)data[i + 1u] >= 0xa0u) ||
            (first == 0xf0u && (unsigned char)data[i + 1u] < 0x90u) ||
            (first == 0xf4u && (unsigned char)data[i + 1u] >= 0x90u)) return 0;
        i += count;
    }
    return 1;
}

static int data_number_valid(const char *text, size_t size)
{
    size_t i = 0u;
    if (!text || !size) return 0;
    if (text[i] == '-') {
        if (++i == size) return 0;
    }
    if (text[i] == '0') {
        ++i;
        if (i < size && text[i] >= '0' && text[i] <= '9') return 0;
    } else {
        if (text[i] < '1' || text[i] > '9') return 0;
        while (i < size && text[i] >= '0' && text[i] <= '9') ++i;
    }
    if (i < size && text[i] == '.') {
        ++i;
        if (i == size || text[i] < '0' || text[i] > '9') return 0;
        while (i < size && text[i] >= '0' && text[i] <= '9') ++i;
    }
    if (i < size && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < size && (text[i] == '+' || text[i] == '-')) ++i;
        if (i == size || text[i] < '0' || text[i] > '9') return 0;
        while (i < size && text[i] >= '0' && text[i] <= '9') ++i;
    }
    return i == size;
}

static int data_value_validate(const QwDataValue *value, size_t depth,
                               size_t *node_count)
{
    size_t i;
    if (!value || !node_count || depth > QW_DATA_DOCUMENT_MAX_DEPTH ||
        *node_count >= QW_DATA_DOCUMENT_MAX_NODES) return 0;
    ++*node_count;
    switch (value->kind) {
    case QW_DATA_VALUE_NULL:
        return 1;
    case QW_DATA_VALUE_BOOLEAN:
        return value->value.boolean == 0 || value->value.boolean == 1;
    case QW_DATA_VALUE_NUMBER:
        return data_number_valid(value->value.text.data,
                                 value->value.text.size);
    case QW_DATA_VALUE_STRING:
        return value->value.text.data &&
               data_utf8_valid(value->value.text.data,
                               value->value.text.size);
    case QW_DATA_VALUE_TEMPORAL:
        return value->value.text.data && value->value.text.size &&
               data_utf8_valid(value->value.text.data,
                               value->value.text.size);
    case QW_DATA_VALUE_ARRAY:
        if (value->value.array.count > value->value.array.capacity ||
            (value->value.array.count && !value->value.array.items) ||
            value->value.array.count >
                QW_DATA_DOCUMENT_MAX_NODES - *node_count) return 0;
        for (i = 0u; i < value->value.array.count; ++i) {
            if (!data_value_validate(&value->value.array.items[i], depth + 1u,
                                     node_count)) return 0;
        }
        return 1;
    case QW_DATA_VALUE_OBJECT:
        if (value->value.object.count > value->value.object.capacity ||
            (value->value.object.count && !value->value.object.items) ||
            value->value.object.count >
                QW_DATA_DOCUMENT_MAX_NODES - *node_count) return 0;
        for (i = 0u; i < value->value.object.count; ++i) {
            const QwDataMember *member = &value->value.object.items[i];
            if (!member->key || !data_utf8_valid(member->key,
                                                 member->key_size) ||
                !data_value_validate(&member->value, depth + 1u,
                                     node_count)) return 0;
        }
        return 1;
    default:
        return 0;
    }
}

int qw_data_document_validate(const QwDataDocument *document)
{
    size_t node_count = 0u;
    if (!document || document->input_format <= QW_FORMAT_UNKNOWN ||
        document->input_format >= QW_FORMAT_COUNT ||
        document->allocation_bytes > QW_DATA_DOCUMENT_MAX_MEMORY ||
        !data_value_validate(&document->root, 0u, &node_count)) return 0;
    return node_count == document->node_count;
}
