/* XML internal DTD entities expand in the typed data model. */

#include <quirewise/data.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int contains(const char *text, size_t size, const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;
    if (!needle_size || needle_size > size) return 0;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(text + index, needle, needle_size) == 0) return 1;
    }
    return 0;
}

int main(void)
{
    static const char xml[] =
        "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE note  [\n<!ENTITY company \"Quirewise\">\n]>\n"
        "<note><name>&company; Suite</name></note>\n";
    QwDataDocument *document = NULL;
    uint8_t *json = NULL;
    size_t json_size = 0u;
    QwConvertReport report;

    {
        QwDataStatus parse_status =
            qw_data_parse(QW_DATA_FORMAT_XML, xml, strlen(xml), &document);
        if (parse_status != QW_DATA_OK || !document) {
            fprintf(stderr, "FAIL: XML internal entity did not parse (%d)\n",
                    (int)parse_status);
            return 1;
        }
    }
    memset(&report, 0, sizeof(report));
    if (qw_data_write_document(document, QW_FORMAT_JSON, NULL, &json,
                               &json_size, &report) != QW_DATA_OK ||
        !json || !json_size) {
        fprintf(stderr, "FAIL: XML entity to JSON (%s)\n", report.note);
        qw_data_document_free(document);
        return 1;
    }
    if (!contains((const char *)json, json_size, "Quirewise Suite")) {
        fprintf(stderr, "FAIL: XML entity was not expanded:\n%.*s\n",
                (int)json_size, (const char *)json);
        qw_data_buffer_free(json);
        qw_data_document_free(document);
        return 1;
    }
    qw_data_buffer_free(json);
    qw_data_document_free(document);
    puts("Quirewise XML entity tests passed");
    return 0;
}
