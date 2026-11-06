#include <quirewise/data.h>

#include <stdio.h>
#include <string.h>

#define NESTING 300

static int parse_toml(const char *text, size_t size)
{
    QwDataDocument *document = NULL;
    int status = qw_data_parse(QW_DATA_FORMAT_TOML, text, size, &document);
    if (document) qw_data_document_free(document);
    return status;
}

int main(void)
{
    char deep[2u * NESTING + 16u];
    size_t position = 0u;
    size_t index;
    int resource_status;
    int format_status;
    int resource_again;
    memcpy(deep + position, "a = ", 4u);
    position += 4u;
    for (index = 0u; index < NESTING; ++index) deep[position++] = '[';
    deep[position++] = '1';
    for (index = 0u; index < NESTING; ++index) deep[position++] = ']';
    deep[position++] = '\n';
    resource_status = parse_toml(deep, position);
    resource_again = parse_toml(deep, position);
    format_status = parse_toml("a = [1\n", 7u);
    printf("resource=%d repeat=%d format=%d\n", resource_status,
           resource_again, format_status);
    if (resource_status != QW_DATA_ERR_RESOURCE_LIMIT) {
        fprintf(stderr, "deep nesting did not report resource limit\n");
        return 1;
    }
    if (resource_again != resource_status) {
        fprintf(stderr, "resource status is not stable\n");
        return 2;
    }
    if (format_status != QW_DATA_ERR_FORMAT) {
        fprintf(stderr, "malformed TOML did not report format error\n");
        return 3;
    }
    if (resource_status == format_status) {
        fprintf(stderr, "resource and format statuses collide\n");
        return 4;
    }
    puts("Data status classes distinct and stable");
    return 0;
}
