#include <quirewise/data.h>
#include <quirewise/docbook.h>
#include <quirewise/opml.h>
#include <quirewise/xml.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FUZZ_SEED UINT32_C(0x51f02257)
#define FUZZ_RUNS 300u

typedef struct Seed {
    QwDataFormat format;
    const char *text;
} Seed;

typedef struct Stats {
    size_t ok;
    size_t error;
} Stats;

static uint32_t rng_state = FUZZ_SEED;

static uint32_t next_random(void)
{
    uint32_t value = rng_state;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    rng_state = value;
    return value;
}

static void mutate(const char *seed, char *buffer, size_t capacity,
                   size_t *out_size)
{
    size_t length = strlen(seed);
    size_t index;
    unsigned int flips = 1u + next_random() % 4u;
    if (length + 1u > capacity) length = capacity - 1u;
    memcpy(buffer, seed, length);
    buffer[length] = '\0';
    if (next_random() % 8u == 0u && length > 1u) {
        length = 1u + next_random() % length;
    }
    for (index = 0u; index < flips && length; ++index) {
        size_t position = next_random() % length;
        buffer[position] = (char)(buffer[position] ^
                                  (uint8_t)(1u << (next_random() % 8u)));
    }
    *out_size = length;
}

static void exercise(Seed seed, Stats *stats)
{
    char buffer[2048];
    size_t size = 0u;
    QwDataDocument *document = NULL;
    size_t run;
    QwXmlError xml_error;
    QwXmlParseOptions options = {0};
    for (run = 0u; run < FUZZ_RUNS; ++run) {
        mutate(seed.text, buffer, sizeof(buffer), &size);
        if (qw_data_parse(seed.format, buffer, size, &document) == QW_DATA_OK &&
            document) {
            ++stats->ok;
            qw_data_document_free(document);
        } else {
            ++stats->error;
        }
        document = NULL;
        if (run % 3u == 0u) {
            QwXmlNode *node = qw_xml_parse(buffer, size, &xml_error);
            if (node) {
                qw_xml_drop(node);
                ++stats->ok;
            } else {
                ++stats->error;
            }
            memset(&options, 0, sizeof(options));
            node = qw_xml_parse_with_options(buffer, size, &options, &xml_error);
            if (node) qw_xml_drop(node);
        }
    }
}

int main(void)
{
    static const Seed seeds[] = {
        {QW_DATA_FORMAT_JSON,
         "{\"enabled\":true,\"list\":[1,2,3],\"name\":\"quirewise\"}"},
        {QW_DATA_FORMAT_JSONL,
         "{\"a\":1}\n{\"b\":[true,null,\"x\"]}\n"},
        {QW_DATA_FORMAT_YAML, "title: Quirewise\nitems:\n  - one\n  - two\n"},
        {QW_DATA_FORMAT_TOML, "[table]\nname = \"quirewise\"\ncount = 3\n"},
        {QW_DATA_FORMAT_VCARD,
         "BEGIN:VCARD\nVERSION:4.0\nFN:Consumer\nEND:VCARD\n"},
        {QW_DATA_FORMAT_XML,
         "<?xml version=\"1.0\"?><root id=\"r\"><child>text</child></root>"},
        {QW_DATA_FORMAT_OPML,
         "<?xml version=\"1.0\"?><opml version=\"2.0\"><body>"
         "<outline text=\"Feed\"/></body></opml>"},
        {QW_DATA_FORMAT_IPYNB,
         "{\"cells\":[{\"cell_type\":\"markdown\",\"source\":[\"# hi\"]}],"
         "\"metadata\":{},\"nbformat\":4}"},
    };
    Stats stats = {0u, 0u};
    size_t index;
    for (index = 0u; index < sizeof(seeds) / sizeof(seeds[0]); ++index) {
        exercise(seeds[index], &stats);
    }
    {
        static const char docbook[] =
            "<book><title>T</title><para>DocBook</para></book>";
        QwDocBookDocument document = {0};
        if (qw_docbook_parse_html(docbook, sizeof(docbook) - 1u, &document) ==
            QW_DOCBOOK_OK) {
            qw_docbook_document_free(&document);
        }
    }
    {
        static const char opml[] =
            "<opml version=\"2.0\"><body><outline text=\"x\"/></body></opml>";
        QwOpmlDocument document = {0};
        if (qw_opml_parse_html(opml, sizeof(opml) - 1u, &document) == QW_OPML_OK) {
            qw_opml_document_free(&document);
        }
    }
    if (stats.ok + stats.error != FUZZ_RUNS * sizeof(seeds) / sizeof(seeds[0]) +
                                  (FUZZ_RUNS / 3u) * sizeof(seeds) /
                                      sizeof(seeds[0])) {
        fprintf(stderr, "unexpected Data fuzz accounting: %zu/%zu\n", stats.ok,
                stats.error);
        return 1;
    }
    printf("Data fuzz smoke: %zu ok, %zu rejected, seed=0x%08x runs=%u\n",
           stats.ok, stats.error, FUZZ_SEED, FUZZ_RUNS);
    return 0;
}
