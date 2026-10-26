#include "quirewise/data.h"

#include "data_parser_internal.h"
#include "quirewise/status.h"

#include <stdlib.h>
#include <string.h>

static const QwDataValue *data_object_member(const QwDataValue *object,
                                             const char *name)
{
    size_t i;
    size_t name_size = strlen(name);
    if (!object || object->kind != QW_DATA_VALUE_OBJECT) return NULL;
    for (i = 0u; i < object->value.object.count; ++i) {
        const QwDataMember *member = &object->value.object.items[i];
        if (member->key_size == name_size &&
            memcmp(member->key, name, name_size) == 0) return &member->value;
    }
    return NULL;
}

static int data_text_is(const QwDataValue *value, const char *expected)
{
    size_t expected_size = strlen(expected);
    return value && value->kind == QW_DATA_VALUE_STRING &&
           value->value.text.size == expected_size &&
           memcmp(value->value.text.data, expected, expected_size) == 0;
}

static int data_xml_root_is(const QwDataDocument *document, const char *name)
{
    const QwDataValue *element = data_object_member(&document->root, "$xml");
    return data_text_is(data_object_member(element, "$name"), name);
}

static int data_xml_root_has_child(const QwDataDocument *document,
                                   const char *name)
{
    const QwDataValue *element = data_object_member(&document->root, "$xml");
    const QwDataValue *content = data_object_member(element, "$content");
    size_t i;
    if (!content || content->kind != QW_DATA_VALUE_ARRAY) return 0;
    for (i = 0u; i < content->value.array.count; ++i) {
        const QwDataValue *child = &content->value.array.items[i];
        if (data_text_is(data_object_member(child, "$name"), name)) return 1;
    }
    return 0;
}

static int data_ipynb_is_notebook(const QwDataDocument *document)
{
    const QwDataValue *version;
    const QwDataValue *cells;
    if (!document || document->root.kind != QW_DATA_VALUE_OBJECT) return 0;
    version = data_object_member(&document->root, "nbformat");
    cells = data_object_member(&document->root, "cells");
    return version && version->kind == QW_DATA_VALUE_NUMBER &&
           version->value.text.size == 1u && version->value.text.data[0] == '4' &&
           cells && cells->kind == QW_DATA_VALUE_ARRAY;
}

static QwDataStatus data_status_from_parser(int status)
{
    switch (status) {
    case QW_STATUS_OK: return QW_DATA_OK;
    case QW_STATUS_ERR_INVALID_ARGUMENT: return QW_DATA_ERR_INVALID_ARGUMENT;
    case QW_STATUS_ERR_OUT_OF_MEMORY: return QW_DATA_ERR_OUT_OF_MEMORY;
    case QW_STATUS_ERR_RESOURCE_LIMIT: return QW_DATA_ERR_RESOURCE_LIMIT;
    case QW_STATUS_ERR_UNSUPPORTED: return QW_DATA_ERR_UNSUPPORTED;
    default: return QW_DATA_ERR_FORMAT;
    }
}

QwDataStatus qw_data_parse(QwDataFormat format, const void *data, size_t size,
                           QwDataDocument **out_document)
{
    QwDataDocument *document;
    int status;
    if (!out_document) return QW_DATA_ERR_INVALID_ARGUMENT;
    *out_document = NULL;
    if (!data || !size) return QW_DATA_ERR_INVALID_ARGUMENT;
    document = (QwDataDocument *)calloc(1u, sizeof(*document));
    if (!document) return QW_DATA_ERR_OUT_OF_MEMORY;
    switch (format) {
    case QW_DATA_FORMAT_JSON:
        status = qw_json_parse_data_document(data, size, 0, document);
        break;
    case QW_DATA_FORMAT_JSONL:
        status = qw_json_parse_data_document(data, size, 1, document);
        break;
    case QW_DATA_FORMAT_YAML:
        status = qw_yaml_parse_data_document(data, size, document);
        break;
    case QW_DATA_FORMAT_TOML:
        status = qw_toml_parse_data_document(data, size, document);
        break;
    case QW_DATA_FORMAT_VCARD:
        status = qw_vcard_parse_data_document(data, size, document);
        break;
    case QW_DATA_FORMAT_XML:
        status = qw_xml_parse_data_format(data, size, QW_FORMAT_XML, document);
        break;
    case QW_DATA_FORMAT_OPML:
        status = qw_xml_parse_data_format(data, size, QW_FORMAT_OPML, document);
        if (status == QW_STATUS_OK &&
            (!data_xml_root_is(document, "opml") ||
             !data_xml_root_has_child(document, "body"))) {
            status = QW_STATUS_ERR_FORMAT;
        }
        break;
    case QW_DATA_FORMAT_IPYNB:
        status = qw_json_parse_data_document_format(
            data, size, QW_FORMAT_IPYNB, 0, document);
        if (status == QW_STATUS_OK && !data_ipynb_is_notebook(document))
            status = QW_STATUS_ERR_FORMAT;
        break;
    default:
        free(document);
        return QW_DATA_ERR_UNSUPPORTED;
    }
    if (status != QW_STATUS_OK) {
        qw_data_document_dispose(document);
        free(document);
        return data_status_from_parser(status);
    }
    *out_document = document;
    return QW_DATA_OK;
}

void qw_data_document_free(QwDataDocument *document)
{
    if (!document) return;
    qw_data_document_dispose(document);
    free(document);
}

QwDataFormat qw_data_document_format(const QwDataDocument *document)
{
    if (!document) return QW_DATA_FORMAT_INVALID;
    switch (document->input_format) {
    case QW_FORMAT_JSON: return QW_DATA_FORMAT_JSON;
    case QW_FORMAT_JSONL: return QW_DATA_FORMAT_JSONL;
    case QW_FORMAT_YAML: return QW_DATA_FORMAT_YAML;
    case QW_FORMAT_TOML: return QW_DATA_FORMAT_TOML;
    case QW_FORMAT_VCF: return QW_DATA_FORMAT_VCARD;
    case QW_FORMAT_XML: return QW_DATA_FORMAT_XML;
    case QW_FORMAT_OPML: return QW_DATA_FORMAT_OPML;
    case QW_FORMAT_IPYNB: return QW_DATA_FORMAT_IPYNB;
    default: return QW_DATA_FORMAT_INVALID;
    }
}

const QwDataValue *qw_data_document_root(const QwDataDocument *document)
{
    return document ? &document->root : NULL;
}

QwDataKind qw_data_value_kind(const QwDataValue *value)
{
    if (!value) return (QwDataKind)-1;
    switch (value->kind) {
    case QW_DATA_VALUE_NULL: return QW_DATA_KIND_NULL;
    case QW_DATA_VALUE_BOOLEAN: return QW_DATA_KIND_BOOLEAN;
    case QW_DATA_VALUE_NUMBER: return QW_DATA_KIND_NUMBER;
    case QW_DATA_VALUE_STRING: return QW_DATA_KIND_STRING;
    case QW_DATA_VALUE_ARRAY: return QW_DATA_KIND_ARRAY;
    case QW_DATA_VALUE_OBJECT: return QW_DATA_KIND_OBJECT;
    case QW_DATA_VALUE_TEMPORAL: return QW_DATA_KIND_TEMPORAL;
    default: return (QwDataKind)-1;
    }
}

size_t qw_data_value_count(const QwDataValue *value)
{
    if (!value) return 0u;
    if (value->kind == QW_DATA_VALUE_ARRAY) return value->value.array.count;
    if (value->kind == QW_DATA_VALUE_OBJECT) return value->value.object.count;
    return 0u;
}

QwDataStatus qw_data_value_boolean(const QwDataValue *value, int *out_boolean)
{
    if (!value || !out_boolean) return QW_DATA_ERR_INVALID_ARGUMENT;
    if (value->kind != QW_DATA_VALUE_BOOLEAN) return QW_DATA_ERR_TYPE;
    *out_boolean = value->value.boolean;
    return QW_DATA_OK;
}

QwDataStatus qw_data_value_text(const QwDataValue *value,
                                const char **out_text,
                                size_t *out_size)
{
    if (!value || !out_text || !out_size) return QW_DATA_ERR_INVALID_ARGUMENT;
    *out_text = NULL;
    *out_size = 0u;
    if (value->kind != QW_DATA_VALUE_NUMBER &&
        value->kind != QW_DATA_VALUE_STRING &&
        value->kind != QW_DATA_VALUE_TEMPORAL) return QW_DATA_ERR_TYPE;
    *out_text = value->value.text.data;
    *out_size = value->value.text.size;
    return QW_DATA_OK;
}

const QwDataValue *qw_data_array_at(const QwDataValue *value, size_t index)
{
    if (!value || value->kind != QW_DATA_VALUE_ARRAY ||
        index >= value->value.array.count) return NULL;
    return &value->value.array.items[index];
}

const QwDataValue *qw_data_object_at(const QwDataValue *value, size_t index,
                                     const char **out_key,
                                     size_t *out_key_size)
{
    const QwDataMember *member;
    if (out_key) *out_key = NULL;
    if (out_key_size) *out_key_size = 0u;
    if (!value || value->kind != QW_DATA_VALUE_OBJECT ||
        index >= value->value.object.count) return NULL;
    member = &value->value.object.items[index];
    if (out_key) *out_key = member->key;
    if (out_key_size) *out_key_size = member->key_size;
    return &member->value;
}

const QwDataValue *qw_data_object_find(const QwDataValue *value,
                                       const char *key, size_t key_size)
{
    size_t i;
    if (!value || value->kind != QW_DATA_VALUE_OBJECT || (!key && key_size))
        return NULL;
    for (i = 0u; i < value->value.object.count; ++i) {
        const QwDataMember *member = &value->value.object.items[i];
        if (member->key_size == key_size &&
            (!key_size || memcmp(member->key, key, key_size) == 0))
            return &member->value;
    }
    return NULL;
}
