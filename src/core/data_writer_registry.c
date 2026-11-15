/* Data writer dispatch.
 *
 * The data format modules own the serializers; this unit keeps the ordered
 * writer table, maps an output format to its writer and exposes the internal
 * entry point the document composition layer calls.
 */

#include "writer_registry.h"

#include <stddef.h>
#include <stdio.h>

static const QwDataWriterDescriptor *const qw_data_writer_table[] = {
    &qw_data_json_writer,
    &qw_data_jsonl_writer,
    &qw_data_yaml_writer,
    &qw_data_toml_writer,
    &qw_data_xml_writer,
    &qw_data_vcard_writer
};

#define QW_DATA_WRITER_COUNT \
    (sizeof(qw_data_writer_table) / sizeof(qw_data_writer_table[0]))

const QwDataWriterDescriptor *qw_data_writer_find(QwOutputFormat output_format)
{
    size_t index;

    for (index = 0u; index < QW_DATA_WRITER_COUNT; ++index) {
        if (qw_data_writer_table[index]->output_format == output_format)
            return qw_data_writer_table[index];
    }
    return NULL;
}

int qw_data_writer_registry_validate(char *error, size_t error_capacity)
{
    size_t outer;

    if (error && error_capacity) error[0] = '\0';
    for (outer = 0u; outer < QW_DATA_WRITER_COUNT; ++outer) {
        const QwDataWriterDescriptor *writer = qw_data_writer_table[outer];
        size_t inner;

        if (!writer->name || !writer->name[0]) {
            if (error && error_capacity)
                snprintf(error, error_capacity, "data writer %zu has no name",
                         outer);
            return 0;
        }
        if (!writer->write_document) {
            if (error && error_capacity)
                snprintf(error, error_capacity,
                         "%s has no write function", writer->name);
            return 0;
        }
        if (!writer->max_output_bytes) {
            if (error && error_capacity)
                snprintf(error, error_capacity,
                         "%s has no output limit", writer->name);
            return 0;
        }
        for (inner = outer + 1u; inner < QW_DATA_WRITER_COUNT; ++inner) {
            if (qw_data_writer_table[inner]->output_format ==
                writer->output_format) {
                if (error && error_capacity)
                    snprintf(error, error_capacity, "%s is registered twice",
                             writer->name);
                return 0;
            }
        }
    }
    return 1;
}

int qw_write_data_document_to_format(const QwDataDocument *document,
                                     QwOutputFormat output_format,
                                     const QwConvertOptions *options,
                                     uint8_t **out_data, size_t *out_size,
                                     QwConvertReport *report)
{
    const QwDataWriterDescriptor *writer;

    if (!document || !out_data || !out_size)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    writer = qw_data_writer_find(output_format);
    if (!writer || !writer->write_document)
        return QW_STATUS_ERR_UNSUPPORTED;
    return writer->write_document(document, output_format, options, out_data,
                                  out_size, report);
}
