#ifndef QUIREWISE_XML_FORMAT_INTERNAL_H
#define QUIREWISE_XML_FORMAT_INTERNAL_H

#include "../../core/data_document.h"

int qw_xml_parse_data(const void *input, size_t size,
                      QwDataDocument *out_document);

#endif
