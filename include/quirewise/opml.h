#ifndef QUIREWISE_OPML_H
#define QUIREWISE_OPML_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QW_OPML_ABI_VERSION 1

typedef enum QwOpmlStatus {
    QW_OPML_OK = 0,
    QW_OPML_ERR_INVALID_ARGUMENT = 1,
    QW_OPML_ERR_OUT_OF_MEMORY = 2,
    QW_OPML_ERR_FORMAT = 3,
    QW_OPML_ERR_RESOURCE_LIMIT = 4,
    QW_OPML_ERR_UNSUPPORTED = 5,
    QW_OPML_ERR_DECODE = 6
} QwOpmlStatus;

typedef struct QwOpmlDocument {
    char *html_utf8; /* NUL-terminated UTF-8 HTML, owned by this structure */
    size_t html_size; /* Byte count excluding the trailing NUL */
    int skipped_unsafe_urls; /* Nonzero if non-HTTP(S) outline URLs were suppressed */
} QwOpmlDocument;

int qw_opml_abi_version(void);

/* Parse an OPML XML outline to nested-list HTML. */
QwOpmlStatus qw_opml_parse_html(const void *data, size_t size,
                               QwOpmlDocument *out_document);
void qw_opml_document_free(QwOpmlDocument *document);
const char *qw_opml_status_string(QwOpmlStatus status);

#ifdef __cplusplus
}
#endif

#endif
