#ifndef QUIREWISE_DATA_PARSER_INTERNAL_H
#define QUIREWISE_DATA_PARSER_INTERNAL_H

#include "../../core/data_document.h"

int qw_json_parse_data_document(const void *data, size_t size, int jsonl,
                                QwDataDocument *out_document);
int qw_json_parse_data_document_format(const void *data, size_t size,
                                      QwInputFormat input_format, int jsonl,
                                      QwDataDocument *out_document);
int qw_yaml_parse_data_document(const void *data, size_t size,
                                QwDataDocument *out_document);
int qw_toml_parse_data_document(const void *data, size_t size,
                                QwDataDocument *out_document);
int qw_vcard_parse_data_document(const void *data, size_t size,
                                 QwDataDocument *out_document);
int qw_xml_parse_data_format(const void *data, size_t size,
                             QwInputFormat input_format,
                             QwDataDocument *out_document);

#endif
