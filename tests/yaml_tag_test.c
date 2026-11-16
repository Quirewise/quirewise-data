/* YAML core tags force scalar types. */

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
    static const char yaml[] =
        "s: !!str 123\n"
        "i: !!int \"42\"\n"
        "f: !!float 3.5\n"
        "b: !!bool \"true\"\n"
        "n: !!null\n";
    QwDataDocument *document = NULL;
    uint8_t *json = NULL;
    size_t json_size = 0u;
    QwConvertReport report;

    if (qw_data_parse(QW_DATA_FORMAT_YAML, yaml, strlen(yaml), &document) !=
            QW_DATA_OK ||
        !document) {
        fputs("FAIL: YAML tags did not parse\n", stderr);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_data_write_document(document, QW_FORMAT_JSON, NULL, &json,
                               &json_size, &report) != QW_DATA_OK ||
        !json || !json_size) {
        fprintf(stderr, "FAIL: tagged YAML to JSON (%s)\n", report.note);
        qw_data_document_free(document);
        return 1;
    }
    if (!contains((const char *)json, json_size, "\"s\": \"123\"") ||
        !contains((const char *)json, json_size, "\"i\": 42") ||
        !contains((const char *)json, json_size, "\"f\": 3.5") ||
        !contains((const char *)json, json_size, "\"b\": true") ||
        !contains((const char *)json, json_size, "\"n\": null")) {
        fprintf(stderr, "FAIL: YAML tagged values were not converted:\n%.*s\n",
                (int)json_size, (const char *)json);
        qw_data_buffer_free(json);
        qw_data_document_free(document);
        return 1;
    }
    qw_data_buffer_free(json);
    qw_data_document_free(document);
    puts("Quirewise YAML tag tests passed");
    return 0;
}
