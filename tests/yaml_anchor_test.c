/* YAML anchors and aliases expand through the typed data model. */

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
        "base: &base\n"
        "  name: alpha\n"
        "  value: 1\n"
        "copy: *base\n"
        "scalar: &s hello\n"
        "scalar_copy: *s\n"
        "list:\n"
        "  - &item 42\n"
        "  - *item\n"
        "defaults: &defaults\n"
        "  name: base\n"
        "  enabled: true\n"
        "merged:\n"
        "  <<: *defaults\n"
        "  name: override\n";
    QwDataDocument *document = NULL;
    uint8_t *json = NULL;
    size_t json_size = 0u;
    QwConvertReport report;

    {
        QwDataStatus parse_status =
            qw_data_parse(QW_DATA_FORMAT_YAML, yaml, strlen(yaml), &document);
        if (parse_status != QW_DATA_OK || !document) {
            fprintf(stderr, "FAIL: YAML anchors did not parse (%d)\n",
                    (int)parse_status);
            return 1;
        }
    }
    memset(&report, 0, sizeof(report));
    if (qw_data_write_document(document, QW_FORMAT_JSON, NULL, &json,
                               &json_size, &report) != QW_DATA_OK ||
        !json || !json_size) {
        fprintf(stderr, "FAIL: YAML to JSON (%s)\n", report.note);
        qw_data_document_free(document);
        return 1;
    }
    if (!contains((const char *)json, json_size, "\"copy\"") ||
        !contains((const char *)json, json_size, "\"name\": \"alpha\"") ||
        !contains((const char *)json, json_size, "\"scalar_copy\": \"hello\"") ||
        !contains((const char *)json, json_size, "\"list\": [") ||
        !contains((const char *)json, json_size, "42") ||
        !contains((const char *)json, json_size, "\"name\": \"override\"") ||
        !contains((const char *)json, json_size, "\"enabled\": true")) {
        fprintf(stderr, "FAIL: YAML anchors were not expanded:\n%.*s\n",
                (int)json_size, (const char *)json);
        qw_data_buffer_free(json);
        qw_data_document_free(document);
        return 1;
    }
    qw_data_buffer_free(json);
    qw_data_document_free(document);
    puts("Quirewise YAML anchor tests passed");
    return 0;
}
