#ifndef QUIREWISE_DATA_H
#define QUIREWISE_DATA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum QwDataStatus {
    QW_DATA_OK = 0,
    QW_DATA_ERR_INVALID_ARGUMENT = -1,
    QW_DATA_ERR_OUT_OF_MEMORY = -2,
    QW_DATA_ERR_FORMAT = -3,
    QW_DATA_ERR_UNSUPPORTED = -4,
    QW_DATA_ERR_RESOURCE_LIMIT = -5,
    QW_DATA_ERR_TYPE = -6
} QwDataStatus;

typedef enum QwDataFormat {
    QW_DATA_FORMAT_INVALID = -1,
    QW_DATA_FORMAT_JSON = 0,
    QW_DATA_FORMAT_JSONL,
    QW_DATA_FORMAT_YAML,
    QW_DATA_FORMAT_TOML,
    QW_DATA_FORMAT_VCARD,
    QW_DATA_FORMAT_XML,
    QW_DATA_FORMAT_OPML,
    QW_DATA_FORMAT_IPYNB
} QwDataFormat;

typedef enum QwDataKind {
    QW_DATA_KIND_NULL = 0,
    QW_DATA_KIND_BOOLEAN,
    QW_DATA_KIND_NUMBER,
    QW_DATA_KIND_STRING,
    QW_DATA_KIND_ARRAY,
    QW_DATA_KIND_OBJECT,
    QW_DATA_KIND_TEMPORAL
} QwDataKind;

typedef struct QwDataDocument QwDataDocument;
typedef struct QwDataValue QwDataValue;

/* Input bytes are borrowed only for the duration of this call. */
QwDataStatus qw_data_parse(QwDataFormat format, const void *data, size_t size,
                           QwDataDocument **out_document);
void qw_data_document_free(QwDataDocument *document);
QwDataFormat qw_data_document_format(const QwDataDocument *document);
const QwDataValue *qw_data_document_root(const QwDataDocument *document);

QwDataKind qw_data_value_kind(const QwDataValue *value);
size_t qw_data_value_count(const QwDataValue *value);
QwDataStatus qw_data_value_boolean(const QwDataValue *value, int *out_boolean);
/* Number, string, and temporal text is borrowed until the document is freed. */
QwDataStatus qw_data_value_text(const QwDataValue *value,
                                const char **out_text,
                                size_t *out_size);
const QwDataValue *qw_data_array_at(const QwDataValue *value, size_t index);
const QwDataValue *qw_data_object_at(const QwDataValue *value, size_t index,
                                     const char **out_key,
                                     size_t *out_key_size);
const QwDataValue *qw_data_object_find(const QwDataValue *value,
                                       const char *key, size_t key_size);

#ifdef __cplusplus
}
#endif

#endif
