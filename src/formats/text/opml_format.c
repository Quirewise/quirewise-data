#ifndef QUIREWISE_STANDALONE_OPML_PARSER
#include "../../core/format_registry.h"
#endif
#include "../../core/layout_document.h"
#include "../../core/text_buffer.h"
#include "xml_format_internal.h"
#include "quirewise/opml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    OPML_MAX_INPUT_BYTES = 64u * 1024u * 1024u,
    OPML_MAX_OUTPUT_BYTES = 128u * 1024u * 1024u,
    OPML_MAX_OUTLINE_DEPTH = 64u,
    OPML_MAX_OUTLINES = 100000u
};

typedef struct OpmlRenderer {
    QwTextBuffer html;
    size_t outline_count;
    int error;
    int skipped_unsafe_url;
} OpmlRenderer;

static const QwDataValue *opml_member(const QwDataValue *object,
                                      const char *key)
{
    size_t key_size = strlen(key);
    size_t i;
    if (!object || object->kind != QW_DATA_VALUE_OBJECT) return NULL;
    for (i = 0u; i < object->value.object.count; ++i) {
        const QwDataMember *member = &object->value.object.items[i];
        if (member->key_size == key_size &&
            memcmp(member->key, key, key_size) == 0) return &member->value;
    }
    return NULL;
}

static const char *opml_string(const QwDataValue *value, size_t *size)
{
    if (!value || value->kind != QW_DATA_VALUE_STRING) return NULL;
    if (size) *size = value->value.text.size;
    return value->value.text.data;
}

static const char *opml_element_name(const QwDataValue *element,
                                     size_t *name_size)
{
    return opml_string(opml_member(element, "$name"), name_size);
}

static int opml_name_is(const QwDataValue *element, const char *expected)
{
    size_t size = 0u;
    const char *name = opml_element_name(element, &size);
    size_t expected_size = strlen(expected);
    return name && size == expected_size &&
           memcmp(name, expected, expected_size) == 0;
}

static const QwDataValue *opml_attributes(const QwDataValue *element)
{
    return opml_member(element, "$attributes");
}

static const char *opml_attribute(const QwDataValue *element,
                                  const char *name, size_t *size)
{
    return opml_string(opml_member(opml_attributes(element), name), size);
}

static const QwDataValue *opml_content(const QwDataValue *element)
{
    const QwDataValue *content = opml_member(element, "$content");
    return content && content->kind == QW_DATA_VALUE_ARRAY ? content : NULL;
}

static const QwDataValue *opml_child(const QwDataValue *element,
                                     const char *name)
{
    const QwDataValue *content = opml_content(element);
    size_t i;
    if (!content) return NULL;
    for (i = 0u; i < content->value.array.count; ++i) {
        const QwDataValue *child = &content->value.array.items[i];
        if (opml_name_is(child, name)) return child;
    }
    return NULL;
}

static const char *opml_trimmed_text(const QwDataValue *element, size_t *size)
{
    const QwDataValue *content = opml_content(element);
    size_t i;
    if (!content) return NULL;
    for (i = 0u; i < content->value.array.count; ++i) {
        const QwDataValue *value = &content->value.array.items[i];
        const char *text;
        size_t text_size;
        text = opml_string(value, &text_size);
        if (!text) continue;
        while (text_size && (*text == ' ' || *text == '\t' ||
               *text == '\r' || *text == '\n')) {
            ++text;
            --text_size;
        }
        while (text_size && (text[text_size - 1u] == ' ' ||
               text[text_size - 1u] == '\t' || text[text_size - 1u] == '\r' ||
               text[text_size - 1u] == '\n')) --text_size;
        if (text_size) {
            *size = text_size;
            return text;
        }
    }
    return NULL;
}

static int opml_append(OpmlRenderer *renderer, const char *text, size_t size)
{
    if (renderer->html.size > OPML_MAX_OUTPUT_BYTES ||
        size > OPML_MAX_OUTPUT_BYTES - renderer->html.size) {
        renderer->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!qw_text_buffer_append(&renderer->html, text, size)) {
        renderer->error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

static int opml_literal(OpmlRenderer *renderer, const char *text)
{
    return opml_append(renderer, text, strlen(text));
}

static int opml_html_escape(OpmlRenderer *renderer, const char *text, size_t size)
{
    size_t span = 0u;
    size_t i;
    for (i = 0u; i < size; ++i) {
        const char *replacement = NULL;
        size_t replacement_size = 0u;
        switch (text[i]) {
        case '&': replacement = "&amp;"; replacement_size = 5u; break;
        case '<': replacement = "&lt;"; replacement_size = 4u; break;
        case '>': replacement = "&gt;"; replacement_size = 4u; break;
        case '"': replacement = "&quot;"; replacement_size = 6u; break;
        case '\'': replacement = "&#39;"; replacement_size = 5u; break;
        default: break;
        }
        if (!replacement) continue;
        if (i > span && !opml_append(renderer, text + span, i - span)) return 0;
        if (!opml_append(renderer, replacement, replacement_size)) return 0;
        span = i + 1u;
    }
    return span == size || opml_append(renderer, text + span, size - span);
}

static int opml_url_has_prefix(const char *url, size_t size,
                               const char *prefix, size_t prefix_size)
{
    size_t i;
    if (size <= prefix_size) return 0;
    for (i = 0u; i < prefix_size; ++i) {
        char left = url[i];
        char right = prefix[i];
        if (left >= 'A' && left <= 'Z') left = (char)(left + ('a' - 'A'));
        if (left != right) break;
    }
    return i == prefix_size;
}

static int opml_http_url(const char *url, size_t size)
{
    return opml_url_has_prefix(url, size, "http://", 7u) ||
           opml_url_has_prefix(url, size, "https://", 8u);
}

static int opml_outline_has_children(const QwDataValue *outline)
{
    const QwDataValue *content = opml_content(outline);
    size_t i;
    if (!content) return 0;
    for (i = 0u; i < content->value.array.count; ++i) {
        if (opml_name_is(&content->value.array.items[i], "outline")) return 1;
    }
    return 0;
}

static int opml_render_outline(OpmlRenderer *renderer,
                              const QwDataValue *outline, size_t depth);

static int opml_render_children(OpmlRenderer *renderer,
                                const QwDataValue *parent, size_t depth)
{
    const QwDataValue *content = opml_content(parent);
    size_t i;
    if (!content || !opml_literal(renderer, "<ul>")) return 0;
    for (i = 0u; i < content->value.array.count; ++i) {
        const QwDataValue *child = &content->value.array.items[i];
        if (opml_name_is(child, "outline") &&
            !opml_render_outline(renderer, child, depth)) return 0;
    }
    return opml_literal(renderer, "</ul>");
}

static int opml_render_outline(OpmlRenderer *renderer,
                              const QwDataValue *outline, size_t depth)
{
    const char *label;
    const char *url;
    const char *description;
    size_t label_size = 0u;
    size_t url_size = 0u;
    size_t description_size = 0u;
    int has_children;
    if (depth > OPML_MAX_OUTLINE_DEPTH ||
        renderer->outline_count >= OPML_MAX_OUTLINES) {
        renderer->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    ++renderer->outline_count;
    label = opml_attribute(outline, "text", &label_size);
    if (!label || !label_size) label = opml_attribute(outline, "title", &label_size);
    if (!label || !label_size) {
        label = "Untitled outline item";
        label_size = sizeof("Untitled outline item") - 1u;
    }
    url = opml_attribute(outline, "htmlUrl", &url_size);
    if (!url || !url_size) url = opml_attribute(outline, "xmlUrl", &url_size);
    description = opml_attribute(outline, "description", &description_size);
    has_children = opml_outline_has_children(outline);
    if (!opml_literal(renderer, "<li>")) return 0;
    if (url && url_size && opml_http_url(url, url_size)) {
        if (!opml_literal(renderer, "<a href=\"") ||
            !opml_html_escape(renderer, url, url_size) ||
            !opml_literal(renderer, "\">") ||
            !opml_html_escape(renderer, label, label_size) ||
            !opml_literal(renderer, "</a>")) return 0;
    } else {
        if (url && url_size) renderer->skipped_unsafe_url = 1;
        if (!opml_html_escape(renderer, label, label_size)) return 0;
    }
    if (description && description_size &&
        (!opml_literal(renderer, " <small>") ||
         !opml_html_escape(renderer, description, description_size) ||
         !opml_literal(renderer, "</small>"))) return 0;
    if (has_children &&
        (!opml_render_children(renderer, outline, depth + 1u))) return 0;
    return opml_literal(renderer, "</li>");
}

#ifndef QUIREWISE_STANDALONE_OPML_PARSER
static int opml_skip_prolog(const uint8_t *data, size_t size, size_t *position)
{
    size_t i = *position;
    for (;;) {
        while (i < size && (data[i] == ' ' || data[i] == '\t' ||
               data[i] == '\r' || data[i] == '\n')) ++i;
        if (i + 4u <= size && memcmp(data + i, "<!--", 4u) == 0) {
            i += 4u;
            while (i + 2u < size && memcmp(data + i, "-->", 3u) != 0) ++i;
            if (i + 2u >= size) return 0;
            i += 3u;
            continue;
        }
        if (i + 2u <= size && memcmp(data + i, "<?", 2u) == 0) {
            i += 2u;
            while (i + 1u < size && !(data[i] == '?' && data[i + 1u] == '>')) ++i;
            if (i + 1u >= size) return 0;
            i += 2u;
            continue;
        }
        *position = i;
        return 1;
    }
}

static int opml_probe(const void *input, size_t size)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t position = 0u;
    static const char root[] = "<opml";
    if (!input || !size || size > OPML_MAX_INPUT_BYTES) return 0;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    if (!opml_skip_prolog(data, size, &position) ||
        sizeof(root) - 1u > size - position ||
        memcmp(data + position, root, sizeof(root) - 1u) != 0) return 0;
    position += sizeof(root) - 1u;
    return position < size && (data[position] == '>' || data[position] == '/' ||
        data[position] == ' ' || data[position] == '\t' ||
        data[position] == '\r' || data[position] == '\n');
}
#endif

static int opml_parse_document(const void *input, size_t input_size,
                               QwLayoutDocument *out_document,
                               QwConvertReport *report,
                               int *out_skipped_unsafe_urls)
{
    static const char prefix[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
        "@page{size:A4;margin:18mm}body{font-family:sans-serif;font-size:10pt;"
        "line-height:1.45;color:#202124}ul{padding-left:1.6em}li{margin:.2em 0}"
        "small{color:#5f6368}</style></head><body>";
    static const char suffix[] = "</body></html>";
    QwDataDocument xml;
    OpmlRenderer renderer;
    const QwDataValue *root;
    const QwDataValue *head;
    const QwDataValue *body;
    const QwDataValue *title_element;
    const char *title;
    size_t title_size = 0u;
    int rc;
    memset(&xml, 0, sizeof(xml));
    memset(&renderer, 0, sizeof(renderer));
    if (out_skipped_unsafe_urls) *out_skipped_unsafe_urls = 0;
    if (!input || !input_size || !out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (input_size > OPML_MAX_INPUT_BYTES) return QW_STATUS_ERR_RESOURCE_LIMIT;
    rc = qw_xml_parse_data(input, input_size, &xml);
    if (rc != QW_STATUS_OK) return rc;
    root = opml_member(&xml.root, "$xml");
    if (!opml_name_is(root, "opml")) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    head = opml_child(root, "head");
    body = opml_child(root, "body");
    if (!body) {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
    title_element = head ? opml_child(head, "title") : NULL;
    title = title_element ? opml_trimmed_text(title_element, &title_size) : NULL;
    if (!title || !title_size) {
        title = "OPML Outline";
        title_size = sizeof("OPML Outline") - 1u;
    }
    renderer.html.limit = (size_t)OPML_MAX_OUTPUT_BYTES + 1u;
    if (!opml_literal(&renderer, prefix) ||
        !opml_literal(&renderer, "<h1>") ||
        !opml_html_escape(&renderer, title, title_size) ||
        !opml_literal(&renderer, "</h1>")) {
        rc = renderer.error ? renderer.error : QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    if (opml_content(body) && opml_outline_has_children(body)) {
        if (!opml_render_children(&renderer, body, 0u)) {
            rc = renderer.error ? renderer.error : QW_STATUS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
    } else if (!opml_literal(&renderer, "<p>No outline entries.</p>")) {
        rc = renderer.error ? renderer.error : QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    if (!opml_literal(&renderer, suffix)) {
        rc = renderer.error ? renderer.error : QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    out_document->html_utf8 = (const uint8_t *)renderer.html.data;
    out_document->html_size = renderer.html.size;
    out_document->context = renderer.html.data;
    out_document->dispose = free;
    renderer.html.data = NULL;
    if (report) {
        (void)snprintf(report->note, sizeof(report->note),
            "OPML outline rendered as nested lists%s; non-HTTP(S) outline URLs are suppressed",
            renderer.skipped_unsafe_url ? "; one or more unsafe links were omitted" : "");
    }
    if (out_skipped_unsafe_urls)
        *out_skipped_unsafe_urls = renderer.skipped_unsafe_url;
    rc = QW_STATUS_OK;

cleanup:
    free(renderer.html.data);
    qw_data_document_dispose(&xml);
    return rc;
}

#ifndef QUIREWISE_STANDALONE_OPML_PARSER
static int opml_open_layout(const void *input, size_t input_size,
                            QwInputFormat input_format,
                            const QwConvertOptions *options,
                            QwLayoutDocument *out_document,
                            QwConvertReport *report)
{
    (void)options;
    if (input_format != QW_FORMAT_OPML) return QW_STATUS_ERR_INVALID_ARGUMENT;
    return opml_parse_document(input, input_size, out_document, report, NULL);
}

const QwFormatDescriptor qw_format_opml = {
    .id = QW_FORMAT_OPML,
    .name = "OPML",
    .probe = opml_probe,
    .open_layout = opml_open_layout,
    .max_input_bytes = OPML_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT
};
#endif

int qw_opml_abi_version(void)
{
    return QW_OPML_ABI_VERSION;
}

static QwOpmlStatus opml_public_status(int status)
{
    switch (status) {
    case QW_STATUS_OK: return QW_OPML_OK;
    case QW_STATUS_ERR_INVALID_ARGUMENT: return QW_OPML_ERR_INVALID_ARGUMENT;
    case QW_STATUS_ERR_OUT_OF_MEMORY: return QW_OPML_ERR_OUT_OF_MEMORY;
    case QW_STATUS_ERR_RESOURCE_LIMIT: return QW_OPML_ERR_RESOURCE_LIMIT;
    case QW_STATUS_ERR_UNSUPPORTED: return QW_OPML_ERR_UNSUPPORTED;
    case QW_STATUS_ERR_DECODE: return QW_OPML_ERR_DECODE;
    default: return QW_OPML_ERR_FORMAT;
    }
}

QwOpmlStatus qw_opml_parse_html(const void *data, size_t size,
                               QwOpmlDocument *out_document)
{
    QwLayoutDocument parsed = {0};
    int skipped_unsafe_urls = 0;
    int status;
    if (!out_document) return QW_OPML_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    status = opml_parse_document(data, size, &parsed, NULL,
                                 &skipped_unsafe_urls);
    if (status != QW_STATUS_OK) return opml_public_status(status);
    out_document->html_utf8 = (char *)parsed.context;
    out_document->html_size = parsed.html_size;
    out_document->skipped_unsafe_urls = skipped_unsafe_urls;
    return QW_OPML_OK;
}

void qw_opml_document_free(QwOpmlDocument *document)
{
    if (!document) return;
    free(document->html_utf8);
    memset(document, 0, sizeof(*document));
}

const char *qw_opml_status_string(QwOpmlStatus status)
{
    switch (status) {
    case QW_OPML_OK: return "ok";
    case QW_OPML_ERR_INVALID_ARGUMENT: return "invalid argument";
    case QW_OPML_ERR_OUT_OF_MEMORY: return "out of memory";
    case QW_OPML_ERR_FORMAT: return "invalid OPML document";
    case QW_OPML_ERR_RESOURCE_LIMIT: return "resource limit exceeded";
    case QW_OPML_ERR_UNSUPPORTED: return "unsupported OPML feature";
    case QW_OPML_ERR_DECODE: return "invalid XML encoding";
    default: return "unknown status";
    }
}
