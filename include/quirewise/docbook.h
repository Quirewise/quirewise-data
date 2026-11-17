#ifndef QUIREWISE_DOCBOOK_H
#define QUIREWISE_DOCBOOK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QW_DOCBOOK_ABI_VERSION 1

typedef enum QwDocBookStatus {
    QW_DOCBOOK_OK = 0,
    QW_DOCBOOK_ERR_INVALID_ARGUMENT = 1,
    QW_DOCBOOK_ERR_OUT_OF_MEMORY = 2,
    QW_DOCBOOK_ERR_FORMAT = 3,
    QW_DOCBOOK_ERR_RESOURCE_LIMIT = 4,
    QW_DOCBOOK_ERR_UNSUPPORTED = 5,
    QW_DOCBOOK_ERR_DECODE = 6
} QwDocBookStatus;

typedef struct QwDocBookDocument {
    char *html_utf8; /* NUL-terminated UTF-8 HTML, owned by this structure */
    size_t html_size; /* Byte count excluding the trailing NUL */
} QwDocBookDocument;

/*! Return the ABI version compiled into the installed data package. */
int qw_docbook_abi_version(void);

/* Parse the supported DocBook XML subset to HTML. */
QwDocBookStatus qw_docbook_parse_html(const void *data, size_t size,
                                     QwDocBookDocument *out_document);
/*! Release a document returned by the parser; NULL is accepted. */
void qw_docbook_document_free(QwDocBookDocument *document);
/*! Return a static human-readable name for a status code. */
const char *qw_docbook_status_string(QwDocBookStatus status);

#ifdef __cplusplus
}
#endif

#endif
