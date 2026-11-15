/*
 * Data writer tests: parse one document, write it in every supported target
 * and parse the result back where the format round-trips. The vCard and XML
 * shapes are format-specific, so those start from their own parsers.
 */

#include <quirewise/data.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static int contains(const uint8_t *data, size_t size, const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;

    if (needle_size > size) return 0;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(data + index, needle, needle_size) == 0) return 1;
    }
    return 0;
}

static QwDataDocument *parse(QwDataFormat format, const char *text)
{
    QwDataDocument *document = NULL;

    if (qw_data_parse(format, text, strlen(text), &document) != QW_DATA_OK ||
        !document)
        fail("parse failed");
    return document;
}

static uint8_t *write_format(QwDataDocument *document, QwOutputFormat format,
                             size_t *out_size)
{
    QwConvertReport report;
    uint8_t *data = NULL;

    memset(&report, 0, sizeof(report));
    *out_size = 0u;
    if (qw_data_write_document(document, format, NULL, &data, out_size,
                               &report) != QW_DATA_OK ||
        !data || !*out_size)
        fail("write failed");
    return data;
}

static void expect_unsupported(QwDataDocument *document, QwOutputFormat format)
{
    QwConvertReport report;
    uint8_t *data = NULL;
    size_t size = 0u;

    memset(&report, 0, sizeof(report));
    if (qw_data_write_document(document, format, NULL, &data, &size,
                               &report) != QW_DATA_ERR_UNSUPPORTED)
        fail("unsupported target was accepted");
    if (data || size) fail("unsupported target produced output");
}

int main(void)
{
    static const char json_text[] =
        "{\"name\":\"quirewise\",\"tags\":[\"a\",\"b\"],\"count\":3,"
        "\"ok\":true}";
    QwDataDocument *document = parse(QW_DATA_FORMAT_JSON, json_text);
    uint8_t *output;
    size_t output_size;

    /* JSON and JSON Lines keep the tree. */
    output = write_format(document, QW_FORMAT_JSON, &output_size);
    if (!contains(output, output_size, "\"name\"") ||
        !contains(output, output_size, "quirewise"))
        fail("JSON writer lost the object");
    {
        QwDataDocument *round = NULL;
        const QwDataValue *root;
        const QwDataValue *count;

        if (qw_data_parse(QW_DATA_FORMAT_JSON, output, output_size, &round) !=
                QW_DATA_OK ||
            !round)
            fail("JSON writer output did not parse");
        root = qw_data_document_root(round);
        count = qw_data_object_find(root, "count", 5u);
        if (!count) fail("JSON round trip lost a member");
        qw_data_document_free(round);
    }
    qw_data_buffer_free(output);

    output = write_format(document, QW_FORMAT_JSONL, &output_size);
    if (!contains(output, output_size, "quirewise") ||
        !contains(output, output_size, "\n"))
        fail("JSON Lines writer failed");
    qw_data_buffer_free(output);

    /* YAML and TOML parse back through their own readers. */
    {
        QwDataFormat formats[] = { QW_DATA_FORMAT_YAML, QW_DATA_FORMAT_TOML };
        QwOutputFormat outputs[] = { QW_FORMAT_YAML, QW_FORMAT_TOML };
        size_t index;

        for (index = 0u; index < 2u; ++index) {
            QwDataDocument *round = NULL;

            output = write_format(document, outputs[index], &output_size);
            if (!contains(output, output_size, "quirewise"))
                fail("writer lost the string value");
            if (qw_data_parse(formats[index], output, output_size, &round) !=
                    QW_DATA_OK ||
                !round)
                fail("writer output did not parse");
            qw_data_document_free(round);
            qw_data_buffer_free(output);
        }
    }

    /* vCard round-trips through the vCard parser. */
    {
        static const char vcard_text[] =
            "BEGIN:VCARD\nVERSION:3.0\nFN:Quirewise Sample\n"
            "EMAIL:sample@example.com\nEND:VCARD\n";
        QwDataDocument *card = parse(QW_DATA_FORMAT_VCARD, vcard_text);

        output = write_format(card, QW_FORMAT_VCF, &output_size);
        if (!contains(output, output_size, "BEGIN:VCARD") ||
            !contains(output, output_size, "sample@example.com"))
            fail("vCard writer lost a property");
        {
            QwDataDocument *round = NULL;
            if (qw_data_parse(QW_DATA_FORMAT_VCARD, output, output_size,
                              &round) != QW_DATA_OK ||
                !round)
                fail("vCard writer output did not parse");
            qw_data_document_free(round);
        }
        qw_data_buffer_free(output);
        qw_data_document_free(card);
    }

    /* XML only serializes the ordered XML document shape. */
    {
        static const char xml_text[] = "<root><item>one</item></root>";
        QwDataDocument *tree = parse(QW_DATA_FORMAT_XML, xml_text);

        output = write_format(tree, QW_FORMAT_XML, &output_size);
        if (!contains(output, output_size, "<?xml") ||
            !contains(output, output_size, "<root>") ||
            !contains(output, output_size, "one"))
            fail("XML writer lost the element tree");
        {
            QwDataDocument *round = NULL;
            if (qw_data_parse(QW_DATA_FORMAT_XML, output, output_size,
                              &round) != QW_DATA_OK ||
                !round)
                fail("XML writer output did not parse");
            qw_data_document_free(round);
        }
        qw_data_buffer_free(output);
        qw_data_document_free(tree);
    }

    /* Non-data and non-writable targets are refused. */
    expect_unsupported(document, QW_FORMAT_PDF);
    expect_unsupported(document, QW_FORMAT_SRT);
    expect_unsupported(document, QW_FORMAT_OPML);

    if (qw_data_write_document(NULL, QW_FORMAT_JSON, NULL, &output,
                               &output_size, NULL) !=
        QW_DATA_ERR_INVALID_ARGUMENT)
        fail("NULL document was accepted");
    qw_data_buffer_free(NULL);
    qw_data_document_free(document);
    puts("Quirewise data writer tests passed");
    return 0;
}
