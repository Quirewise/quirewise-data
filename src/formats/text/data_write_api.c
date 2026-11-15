/* Public data writer entry points.
 *
 * The serializers live in the format modules behind QwDataWriterDescriptor;
 * this unit maps the shared status values and owns the buffer release helper.
 */

#include "quirewise/data.h"

#include "../../core/writer_registry.h"

#include <stdlib.h>

static QwDataStatus data_write_status(int status)
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

QwDataStatus qw_data_write_document(const QwDataDocument *document,
                                    QwOutputFormat output_format,
                                    const QwConvertOptions *options,
                                    uint8_t **out_data, size_t *out_size,
                                    QwConvertReport *report)
{
    if (!document || !out_data || !out_size)
        return QW_DATA_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    return data_write_status(qw_write_data_document_to_format(
        document, output_format, options, out_data, out_size, report));
}

void qw_data_buffer_free(void *buffer)
{
    free(buffer);
}
