#include "../../core/data_document.h"
#include "../../core/layout_document.h"
#ifndef QUIREWISE_STANDALONE_DOCBOOK_PARSER
#include "../../core/format_registry.h"
#endif
#include "../../core/text_buffer.h"
#include "xml_format_internal.h"
#include "quirewise/docbook.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DOCBOOK_MAX_INPUT_BYTES = 64 * 1024 * 1024,
    DOCBOOK_MAX_HTML_BYTES = 128 * 1024 * 1024,
    DOCBOOK_PROBE_BYTES = 65536
};

typedef struct DocBookRenderer {
    QwTextBuffer html;
    int error;
} DocBookRenderer;

static const QwDataValue *docbook_member(const QwDataValue *value,
                                         const char *name)
{
    size_t i;
    size_t name_size = strlen(name);
    if (!value || value->kind != QW_DATA_VALUE_OBJECT) return NULL;
    for (i = 0u; i < value->value.object.count; ++i) {
        const QwDataMember *member = &value->value.object.items[i];
        if (member->key_size == name_size &&
            memcmp(member->key, name, name_size) == 0) return &member->value;
    }
    return NULL;
}

static const char *docbook_string(const QwDataValue *value, size_t *size)
{
    if (!value || value->kind != QW_DATA_VALUE_STRING) return NULL;
    if (size) *size = value->value.text.size;
    return value->value.text.data;
}

static const char *docbook_element_name(const QwDataValue *element,
                                        size_t *name_size)
{
    return docbook_string(docbook_member(element, "$name"), name_size);
}

static int docbook_name_is(const QwDataValue *element, const char *expected)
{
    size_t size = 0u;
    size_t expected_size = strlen(expected);
    const char *name = docbook_element_name(element, &size);
    const char *local = name;
    size_t local_size = size;
    size_t i;
    if (!name) return 0;
    for (i = 0u; i < size; ++i) {
        if (name[i] == ':') {
            local = name + i + 1u;
            local_size = size - i - 1u;
            break;
        }
    }
    return local_size == expected_size &&
           memcmp(local, expected, expected_size) == 0;
}

static const QwDataValue *docbook_attributes(const QwDataValue *element)
{
    return docbook_member(element, "$attributes");
}

static const char *docbook_attribute(const QwDataValue *element,
                                     const char *name, size_t *size)
{
    return docbook_string(docbook_member(docbook_attributes(element), name), size);
}

static const QwDataValue *docbook_content(const QwDataValue *element)
{
    const QwDataValue *content = docbook_member(element, "$content");
    return content && content->kind == QW_DATA_VALUE_ARRAY ? content : NULL;
}

static const QwDataValue *docbook_child(const QwDataValue *element,
                                        const char *name)
{
    const QwDataValue *content = docbook_content(element);
    size_t i;
    if (!content) return NULL;
    for (i = 0u; i < content->value.array.count; ++i) {
        const QwDataValue *child = &content->value.array.items[i];
        if (docbook_name_is(child, name)) return child;
    }
    return NULL;
}

static int docbook_ascii_equal(const char *text, size_t size,
                               const char *expected)
{
    size_t i;
    size_t expected_size = strlen(expected);
    if (!text || size != expected_size) return 0;
    for (i = 0u; i < size; ++i) {
        unsigned char actual = (unsigned char)text[i];
        unsigned char wanted = (unsigned char)expected[i];
        if (actual >= 'A' && actual <= 'Z') actual = (unsigned char)(actual + 32u);
        if (wanted >= 'A' && wanted <= 'Z') wanted = (unsigned char)(wanted + 32u);
        if (actual != wanted) return 0;
    }
    return 1;
}

static int docbook_root_name(const char *name, size_t size)
{
    static const char *const names[] = {
        "acknowledgements", "appendix", "article", "bibliography", "book",
        "chapter", "dedication", "glossary", "index", "part", "preface",
        "refentry", "reference", "refsect1", "refsect2", "refsect3",
        "refsect4", "refsect5", "sect1", "sect2", "sect3", "sect4",
        "sect5", "section", "set"
    };
    const char *local = name;
    size_t local_size = size;
    size_t i;
    size_t n;
    if (!name) return 0;
    for (i = 0u; i < size; ++i) {
        if (name[i] == ':') {
            local = name + i + 1u;
            local_size = size - i - 1u;
            break;
        }
    }
    for (n = 0u; n < sizeof(names) / sizeof(names[0]); ++n)
        if (docbook_ascii_equal(local, local_size, names[n])) return 1;
    return 0;
}

static int docbook_skip_markup(const uint8_t *data, size_t size,
                               size_t start, const char *terminator,
                               size_t *out_end)
{
    size_t terminator_size = strlen(terminator);
    size_t i = start;
    while (i <= size && terminator_size <= size - i) {
        if (memcmp(data + i, terminator, terminator_size) == 0) {
            *out_end = i + terminator_size;
            return 1;
        }
        ++i;
    }
    return 0;
}

static int docbook_skip_doctype(const uint8_t *data, size_t size,
                                size_t start, size_t *out_end,
                                const char **out_root, size_t *out_root_size)
{
    size_t position = start + 9u;
    size_t root_start;
    size_t bracket_depth = 0u;
    unsigned char quote = 0u;
    if (start + 9u > size || memcmp(data + start, "<!DOCTYPE", 9u) != 0)
        return 0;
    while (position < size && (data[position] == ' ' || data[position] == '\t' ||
           data[position] == '\r' || data[position] == '\n')) ++position;
    root_start = position;
    while (position < size && data[position] != ' ' && data[position] != '\t' &&
           data[position] != '\r' && data[position] != '\n' &&
           data[position] != '[' && data[position] != '>') ++position;
    if (position == root_start) return 0;
    *out_root = (const char *)data + root_start;
    *out_root_size = position - root_start;
    while (position < size) {
        if (!quote && position + 4u <= size &&
            memcmp(data + position, "<!--", 4u) == 0) {
            if (!docbook_skip_markup(data, size, position + 4u, "-->", &position))
                return 0;
            continue;
        }
        if (quote) {
            if (data[position] == quote) quote = 0u;
        } else if (data[position] == '\'' || data[position] == '"') {
            quote = data[position];
        } else if (data[position] == '[') {
            ++bracket_depth;
        } else if (data[position] == ']') {
            if (!bracket_depth) return 0;
            --bracket_depth;
        } else if (data[position] == '>' && bracket_depth == 0u) {
            *out_end = position + 1u;
            return 1;
        }
        ++position;
    }
    return 0;
}

static int docbook_scan_root(const uint8_t *data, size_t size,
                             int accept_doctype, size_t *out_start,
                             size_t *out_name_size)
{
    size_t position = 0u;
    size_t limit = size < DOCBOOK_PROBE_BYTES ? size : DOCBOOK_PROBE_BYTES;
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu)
        position = 3u;
    while (position < limit) {
        size_t end;
        while (position < limit && (data[position] == ' ' || data[position] == '\t' ||
               data[position] == '\r' || data[position] == '\n')) ++position;
        if (position + 4u <= limit && memcmp(data + position, "<!--", 4u) == 0) {
            if (!docbook_skip_markup(data, limit, position + 4u, "-->", &position))
                return 0;
            continue;
        }
        if (position + 2u <= limit && data[position] == '<' && data[position + 1u] == '?') {
            if (!docbook_skip_markup(data, limit, position + 2u, "?>", &position)) return 0;
            continue;
        }
        if (position + 9u <= limit && memcmp(data + position, "<!DOCTYPE", 9u) == 0) {
            const char *root_name;
            size_t root_size;
            if (!docbook_skip_doctype(data, limit, position, &end,
                                      &root_name, &root_size)) return 0;
            if (accept_doctype && docbook_root_name(root_name, root_size)) return 1;
            position = end;
            continue;
        }
        if (position + 1u >= limit || data[position] != '<' ||
            data[position + 1u] == '/' || data[position + 1u] == '!' ||
            data[position + 1u] == '?') return 0;
        *out_start = position + 1u;
        end = *out_start;
        while (end < limit && data[end] != ' ' && data[end] != '\t' &&
               data[end] != '\r' && data[end] != '\n' && data[end] != '/' &&
               data[end] != '>') ++end;
        if (end == *out_start) return 0;
        *out_name_size = end - *out_start;
        return docbook_root_name((const char *)data + *out_start, *out_name_size);
    }
    return 0;
}

#ifndef QUIREWISE_STANDALONE_DOCBOOK_PARSER
static int docbook_probe(const void *input, size_t size)
{
    size_t root_start = 0u;
    size_t root_size = 0u;
    if (!input || !size || size > DOCBOOK_MAX_INPUT_BYTES) return 0;
    return docbook_scan_root((const uint8_t *)input, size, 1,
                             &root_start, &root_size);
}

static int docbook_extension_equal(const char *extension, const char *expected)
{
    size_t i;
    size_t size = strlen(extension);
    if (size != strlen(expected)) return 0;
    for (i = 0u; i < size; ++i) {
        char actual = extension[i];
        char wanted = expected[i];
        if (actual >= 'A' && actual <= 'Z') actual = (char)(actual + ('a' - 'A'));
        if (wanted >= 'A' && wanted <= 'Z') wanted = (char)(wanted + ('a' - 'A'));
        if (actual != wanted) return 0;
    }
    return 1;
}

static int docbook_path_probe(const char *path)
{
    const char *base;
    const char *cursor;
    const char *dot;
    if (!path || !*path) return 0;
    base = path;
    for (cursor = path; *cursor; ++cursor)
        if (*cursor == '/' || *cursor == '\\') base = cursor + 1u;
    dot = strrchr(base, '.');
    return dot && (docbook_extension_equal(dot, ".dbk") ||
                   docbook_extension_equal(dot, ".docbook"));
}
#endif

static int docbook_prepare_xml(const uint8_t *input, size_t size,
                               uint8_t **out_data, size_t *out_size)
{
    size_t position = 0u;
    size_t doctype_start = SIZE_MAX;
    size_t doctype_end = 0u;
    uint8_t *copy;
    if (!input || !size || !out_data || !out_size) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    if (size > DOCBOOK_MAX_INPUT_BYTES) return QW_STATUS_ERR_RESOURCE_LIMIT;
    if (size >= 3u && input[0] == 0xefu && input[1] == 0xbbu && input[2] == 0xbfu)
        position = 3u;
    for (;;) {
        while (position < size && (input[position] == ' ' || input[position] == '\t' ||
               input[position] == '\r' || input[position] == '\n')) ++position;
        if (position + 4u <= size && memcmp(input + position, "<!--", 4u) == 0) {
            if (!docbook_skip_markup(input, size, position + 4u, "-->", &position))
                return QW_STATUS_ERR_FORMAT;
            continue;
        }
        if (position + 2u <= size && input[position] == '<' && input[position + 1u] == '?') {
            if (!docbook_skip_markup(input, size, position + 2u, "?>", &position))
                return QW_STATUS_ERR_FORMAT;
            continue;
        }
        if (position + 9u <= size && memcmp(input + position, "<!DOCTYPE", 9u) == 0) {
            const char *root_name;
            size_t root_size;
            doctype_start = position;
            if (!docbook_skip_doctype(input, size, position, &doctype_end,
                                      &root_name, &root_size)) return QW_STATUS_ERR_FORMAT;
        }
        break;
    }
    copy = (uint8_t *)malloc(size + 1u);
    if (!copy) return QW_STATUS_ERR_OUT_OF_MEMORY;
    if (doctype_start == SIZE_MAX) memcpy(copy, input, size);
    else {
        memcpy(copy, input, doctype_start);
        memcpy(copy + doctype_start, input + doctype_end, size - doctype_end);
        size -= doctype_end - doctype_start;
    }
    copy[size] = 0u;
    *out_data = copy;
    *out_size = size;
    return QW_STATUS_OK;
}

static int docbook_append(DocBookRenderer *renderer, const char *text, size_t size)
{
    if (!renderer || (!text && size)) return 0;
    if (renderer->html.size > DOCBOOK_MAX_HTML_BYTES ||
        size > DOCBOOK_MAX_HTML_BYTES - renderer->html.size) {
        renderer->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!qw_text_buffer_append(&renderer->html, text, size)) {
        renderer->error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

static int docbook_literal(DocBookRenderer *renderer, const char *text)
{
    return docbook_append(renderer, text, strlen(text));
}

static int docbook_html_escape(DocBookRenderer *renderer,
                               const char *text, size_t size)
{
    size_t i;
    size_t span = 0u;
    for (i = 0u; i < size; ++i) {
        const char *replacement = NULL;
        size_t replacement_size = 0u;
        switch ((unsigned char)text[i]) {
        case '&': replacement = "&amp;"; replacement_size = 5u; break;
        case '<': replacement = "&lt;"; replacement_size = 4u; break;
        case '>': replacement = "&gt;"; replacement_size = 4u; break;
        case '"': replacement = "&quot;"; replacement_size = 6u; break;
        case '\'': replacement = "&#39;"; replacement_size = 5u; break;
        default: break;
        }
        if (!replacement) continue;
        if (i > span && !docbook_append(renderer, text + span, i - span)) return 0;
        if (!docbook_append(renderer, replacement, replacement_size)) return 0;
        span = i + 1u;
    }
    return span == size || docbook_append(renderer, text + span, size - span);
}

static int docbook_url_safe(const char *url, size_t size)
{
    size_t i;
    static const char *const prefixes[] = { "http://", "https://", "mailto:" };
    size_t n;
    if (!url || !size) return 0;
    for (i = 0u; i < size; ++i)
        if ((unsigned char)url[i] <= 0x20u || url[i] == '"' || url[i] == '\'' ||
            url[i] == '<' || url[i] == '>') return 0;
    for (n = 0u; n < sizeof(prefixes) / sizeof(prefixes[0]); ++n) {
        size_t prefix_size = strlen(prefixes[n]);
        if (size < prefix_size) continue;
        for (i = 0u; i < prefix_size; ++i) {
            char actual = url[i];
            char wanted = prefixes[n][i];
            if (actual >= 'A' && actual <= 'Z') actual = (char)(actual + ('a' - 'A'));
            if (actual != wanted) break;
        }
        if (i == prefix_size) return 1;
    }
    return 0;
}

static int docbook_render_inline(DocBookRenderer *renderer,
                                 const QwDataValue *value, size_t depth);

static int docbook_render_inline_children(DocBookRenderer *renderer,
                                          const QwDataValue *element,
                                          size_t depth)
{
    const QwDataValue *content = docbook_content(element);
    size_t i;
    if (!content) return 1;
    for (i = 0u; i < content->value.array.count; ++i)
        if (!docbook_render_inline(renderer, &content->value.array.items[i], depth + 1u))
            return 0;
    return 1;
}

static int docbook_inline_tag(DocBookRenderer *renderer,
                              const QwDataValue *element,
                              const char *open, const char *close,
                              size_t depth)
{
    return docbook_literal(renderer, open) &&
           docbook_render_inline_children(renderer, element, depth) &&
           docbook_literal(renderer, close);
}

static int docbook_render_inline(DocBookRenderer *renderer,
                                 const QwDataValue *value, size_t depth)
{
    size_t text_size = 0u;
    const char *text = docbook_string(value, &text_size);
    if (text) return docbook_html_escape(renderer, text, text_size);
    if (!value || value->kind != QW_DATA_VALUE_OBJECT) return 1;
    if (depth > QW_DATA_DOCUMENT_MAX_DEPTH) {
        renderer->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (docbook_name_is(value, "emphasis")) {
        size_t role_size = 0u;
        const char *role = docbook_attribute(value, "role", &role_size);
        return role && (docbook_ascii_equal(role, role_size, "strong") ||
                        docbook_ascii_equal(role, role_size, "bold"))
            ? docbook_inline_tag(renderer, value, "<strong>", "</strong>", depth)
            : docbook_inline_tag(renderer, value, "<em>", "</em>", depth);
    }
    if (docbook_name_is(value, "literal") || docbook_name_is(value, "code") ||
        docbook_name_is(value, "filename") || docbook_name_is(value, "command") ||
        docbook_name_is(value, "function") || docbook_name_is(value, "varname") ||
        docbook_name_is(value, "replaceable") || docbook_name_is(value, "userinput") ||
        docbook_name_is(value, "computeroutput"))
        return docbook_inline_tag(renderer, value, "<code>", "</code>", depth);
    if (docbook_name_is(value, "subscript"))
        return docbook_inline_tag(renderer, value, "<sub>", "</sub>", depth);
    if (docbook_name_is(value, "superscript"))
        return docbook_inline_tag(renderer, value, "<sup>", "</sup>", depth);
    if (docbook_name_is(value, "quote") || docbook_name_is(value, "firstterm") ||
        docbook_name_is(value, "citetitle"))
        return docbook_inline_tag(renderer, value, "<i>", "</i>", depth);
    if (docbook_name_is(value, "link") || docbook_name_is(value, "ulink")) {
        size_t href_size = 0u;
        const char *href = docbook_attribute(value, "xlink:href", &href_size);
        if (!href) href = docbook_attribute(value, "href", &href_size);
        if (!href) href = docbook_attribute(value, "url", &href_size);
        if (href && docbook_url_safe(href, href_size)) {
            if (!docbook_literal(renderer, "<a href=\"") ||
                !docbook_html_escape(renderer, href, href_size) ||
                !docbook_literal(renderer, "\">")) return 0;
            if (!docbook_render_inline_children(renderer, value, depth) ||
                !docbook_literal(renderer, "</a>")) return 0;
            return 1;
        }
        return docbook_render_inline_children(renderer, value, depth);
    }
    if (docbook_name_is(value, "xref")) {
        size_t linkend_size = 0u;
        const char *linkend = docbook_attribute(value, "linkend", &linkend_size);
        if (linkend && linkend_size)
            return docbook_html_escape(renderer, linkend, linkend_size);
    }
    if (docbook_name_is(value, "inlinemediaobject"))
        return docbook_literal(renderer, "[Image]");
    return docbook_render_inline_children(renderer, value, depth);
}

static int docbook_render_plain_tree(DocBookRenderer *renderer,
                                     const QwDataValue *value, size_t depth)
{
    size_t text_size = 0u;
    const char *text = docbook_string(value, &text_size);
    const QwDataValue *content;
    size_t i;
    if (text) return docbook_html_escape(renderer, text, text_size);
    if (!value || value->kind != QW_DATA_VALUE_OBJECT) return 1;
    if (depth > QW_DATA_DOCUMENT_MAX_DEPTH) {
        renderer->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    content = docbook_content(value);
    if (!content) return 1;
    for (i = 0u; i < content->value.array.count; ++i)
        if (!docbook_render_plain_tree(renderer, &content->value.array.items[i], depth + 1u))
            return 0;
    return 1;
}

static int docbook_is_info(const QwDataValue *value)
{
    return docbook_name_is(value, "info") || docbook_name_is(value, "bookinfo") ||
           docbook_name_is(value, "articleinfo") || docbook_name_is(value, "chapterinfo") ||
           docbook_name_is(value, "sectioninfo");
}

static const QwDataValue *docbook_container_title(const QwDataValue *element)
{
    const QwDataValue *title = docbook_child(element, "title");
    const QwDataValue *info;
    if (title) return title;
    info = docbook_child(element, "info");
    if (!info) info = docbook_child(element, "bookinfo");
    if (!info) info = docbook_child(element, "articleinfo");
    if (!info) info = docbook_child(element, "chapterinfo");
    if (!info) info = docbook_child(element, "sectioninfo");
    return info ? docbook_child(info, "title") : NULL;
}

static int docbook_is_container(const QwDataValue *value)
{
    static const char *const names[] = {
        "book", "set", "article", "part", "chapter", "appendix", "preface",
        "section", "sect1", "sect2", "sect3", "sect4", "sect5", "reference",
        "refentry", "refsect1", "refsect2", "refsect3", "refsect4", "refsect5",
        "dedication", "acknowledgements", "colophon", "glossary", "bibliography",
        "bibliodiv", "index", "indexdiv"
    };
    size_t i;
    for (i = 0u; i < sizeof(names) / sizeof(names[0]); ++i)
        if (docbook_name_is(value, names[i])) return 1;
    return 0;
}

static int docbook_render_block(DocBookRenderer *renderer,
                                const QwDataValue *value,
                                unsigned heading_level, size_t depth);

static int docbook_render_blocks(DocBookRenderer *renderer,
                                 const QwDataValue *element,
                                 unsigned heading_level, size_t depth)
{
    const QwDataValue *content = docbook_content(element);
    size_t i;
    if (!content) return 1;
    for (i = 0u; i < content->value.array.count; ++i)
        if (!docbook_render_block(renderer, &content->value.array.items[i],
                                  heading_level, depth + 1u)) return 0;
    return 1;
}

static int docbook_heading(DocBookRenderer *renderer,
                           const QwDataValue *title, unsigned level,
                           const char *tag)
{
    char opening[16];
    char closing[16];
    int open_size;
    int close_size;
    if (!level) level = 1u;
    if (level > 6u) level = 6u;
    if (tag) {
        open_size = snprintf(opening, sizeof(opening), "<%s>", tag);
        close_size = snprintf(closing, sizeof(closing), "</%s>", tag);
    } else {
        open_size = snprintf(opening, sizeof(opening), "<h%u>", level);
        close_size = snprintf(closing, sizeof(closing), "</h%u>", level);
    }
    return open_size > 0 && close_size > 0 &&
           (size_t)open_size < sizeof(opening) && (size_t)close_size < sizeof(closing) &&
           docbook_append(renderer, opening, (size_t)open_size) &&
           docbook_render_inline_children(renderer, title, 0u) &&
           docbook_append(renderer, closing, (size_t)close_size);
}

static int docbook_render_container(DocBookRenderer *renderer,
                                    const QwDataValue *element,
                                    unsigned heading_level, size_t depth)
{
    const QwDataValue *content = docbook_content(element);
    const QwDataValue *title = docbook_container_title(element);
    size_t i;
    unsigned child_level = heading_level + 1u;
    if (!content) return 1;
    if (title && !docbook_heading(renderer, title, heading_level, NULL)) return 0;
    if (docbook_name_is(element, "book") || docbook_name_is(element, "set"))
        child_level = heading_level;
    for (i = 0u; i < content->value.array.count; ++i) {
        const QwDataValue *child = &content->value.array.items[i];
        if (docbook_name_is(child, "title") || docbook_name_is(child, "titleabbrev") ||
            docbook_is_info(child)) continue;
        if (!docbook_render_block(renderer, child, child_level, depth + 1u)) return 0;
    }
    return 1;
}

static int docbook_render_list(DocBookRenderer *renderer,
                               const QwDataValue *element,
                               int ordered, unsigned heading_level,
                               size_t depth)
{
    const QwDataValue *content = docbook_content(element);
    size_t i;
    if (!docbook_literal(renderer, ordered ? "<ol>" : "<ul>")) return 0;
    if (content) {
        for (i = 0u; i < content->value.array.count; ++i) {
            const QwDataValue *child = &content->value.array.items[i];
            if (!docbook_name_is(child, "listitem")) continue;
            if (!docbook_literal(renderer, "<li>") ||
                !docbook_render_blocks(renderer, child, heading_level, depth + 1u) ||
                !docbook_literal(renderer, "</li>")) return 0;
        }
    }
    return docbook_literal(renderer, ordered ? "</ol>" : "</ul>");
}

static int docbook_render_variable_list(DocBookRenderer *renderer,
                                        const QwDataValue *element,
                                        unsigned heading_level, size_t depth)
{
    const QwDataValue *content = docbook_content(element);
    size_t i;
    if (!docbook_literal(renderer, "<dl>")) return 0;
    if (content) {
        for (i = 0u; i < content->value.array.count; ++i) {
            const QwDataValue *entry = &content->value.array.items[i];
            const QwDataValue *term;
            const QwDataValue *listitem;
            if (!docbook_name_is(entry, "varlistentry")) continue;
            term = docbook_child(entry, "term");
            listitem = docbook_child(entry, "listitem");
            if (term && (!docbook_literal(renderer, "<dt>") ||
                         !docbook_render_inline_children(renderer, term, depth + 1u) ||
                         !docbook_literal(renderer, "</dt>"))) return 0;
            if (listitem && (!docbook_literal(renderer, "<dd>") ||
                             !docbook_render_blocks(renderer, listitem, heading_level,
                                                    depth + 1u) ||
                             !docbook_literal(renderer, "</dd>"))) return 0;
        }
    }
    return docbook_literal(renderer, "</dl>");
}

static int docbook_render_table(DocBookRenderer *renderer,
                               const QwDataValue *element,
                               unsigned heading_level, size_t depth)
{
    const QwDataValue *content = docbook_content(element);
    size_t i;
    if (!docbook_literal(renderer, "<table><tbody>")) return 0;
    if (content) {
        for (i = 0u; i < content->value.array.count; ++i) {
            const QwDataValue *child = &content->value.array.items[i];
            if (docbook_name_is(child, "title")) {
                if (!docbook_heading(renderer, child, heading_level, "caption")) return 0;
                continue;
            }
            if (docbook_name_is(child, "tgroup")) {
                const QwDataValue *group = docbook_content(child);
                size_t j;
                if (!group) continue;
                for (j = 0u; j < group->value.array.count; ++j) {
                    const QwDataValue *part = &group->value.array.items[j];
                    const QwDataValue *rows;
                    size_t k;
                    if (docbook_name_is(part, "colspec")) continue;
                    if (docbook_name_is(part, "thead") || docbook_name_is(part, "tbody") ||
                        docbook_name_is(part, "tfoot")) {
                        const char *tag = docbook_name_is(part, "thead") ? "thead" :
                            (docbook_name_is(part, "tfoot") ? "tfoot" : "tbody");
                        rows = docbook_content(part);
                        if (!docbook_literal(renderer, "</tbody><") ||
                            !docbook_literal(renderer, tag) || !docbook_literal(renderer, ">"))
                            return 0;
                        for (k = 0u; rows && k < rows->value.array.count; ++k) {
                            const QwDataValue *row = &rows->value.array.items[k];
                            const QwDataValue *cells;
                            size_t m;
                            if (!docbook_name_is(row, "row")) continue;
                            if (!docbook_literal(renderer, "<tr>")) return 0;
                            cells = docbook_content(row);
                            for (m = 0u; cells && m < cells->value.array.count; ++m) {
                                const QwDataValue *cell = &cells->value.array.items[m];
                                const char *cell_tag = docbook_name_is(part, "thead") ? "th" : "td";
                                if (!docbook_name_is(cell, "entry")) continue;
                                if (!docbook_literal(renderer, "<") ||
                                    !docbook_literal(renderer, cell_tag) ||
                                    !docbook_literal(renderer, ">") ||
                                    !docbook_render_blocks(renderer, cell, heading_level,
                                                           depth + 1u) ||
                                    !docbook_literal(renderer, "</") ||
                                    !docbook_literal(renderer, cell_tag) ||
                                    !docbook_literal(renderer, ">")) return 0;
                            }
                            if (!docbook_literal(renderer, "</tr>")) return 0;
                        }
                        if (!docbook_literal(renderer, "</") || !docbook_literal(renderer, tag) ||
                            !docbook_literal(renderer, "><tbody>")) return 0;
                    }
                }
            }
        }
    }
    return docbook_literal(renderer, "</tbody></table>");
}

static int docbook_render_media(DocBookRenderer *renderer,
                                const QwDataValue *element)
{
    const QwDataValue *content = docbook_content(element);
    size_t i;
    if (docbook_name_is(element, "mediaobject") ||
        docbook_name_is(element, "inlinemediaobject")) {
        const char *tag = docbook_name_is(element, "inlinemediaobject") ? "span" : "p";
        if (!docbook_literal(renderer, "<") || !docbook_literal(renderer, tag) ||
            !docbook_literal(renderer, ">[Image: ")) return 0;
        if (content) {
            for (i = 0u; i < content->value.array.count; ++i) {
                const QwDataValue *imageobject = &content->value.array.items[i];
                const QwDataValue *image_content = docbook_content(imageobject);
                size_t j;
                if (!docbook_name_is(imageobject, "imageobject") || !image_content) continue;
                for (j = 0u; j < image_content->value.array.count; ++j) {
                    const QwDataValue *imagedata = &image_content->value.array.items[j];
                    size_t path_size = 0u;
                    const char *path = docbook_attribute(imagedata, "fileref", &path_size);
                    if (docbook_name_is(imagedata, "imagedata") && path && path_size &&
                        !docbook_html_escape(renderer, path, path_size)) return 0;
                }
            }
        }
        return docbook_literal(renderer, "]</") && docbook_literal(renderer, tag) &&
               docbook_literal(renderer, ">");
    }
    return 1;
}

static int docbook_render_block(DocBookRenderer *renderer,
                                const QwDataValue *value,
                                unsigned heading_level, size_t depth)
{
    const QwDataValue *content;
    size_t text_size = 0u;
    const char *text = docbook_string(value, &text_size);
    if (text) {
        size_t i;
        for (i = 0u; i < text_size; ++i)
            if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n')
                return docbook_literal(renderer, "<p>") &&
                       docbook_html_escape(renderer, text, text_size) &&
                       docbook_literal(renderer, "</p>");
        return 1;
    }
    if (!value || value->kind != QW_DATA_VALUE_OBJECT) return 1;
    if (depth > QW_DATA_DOCUMENT_MAX_DEPTH) {
        renderer->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (docbook_is_container(value))
        return docbook_render_container(renderer, value,
            heading_level < 1u ? 1u : heading_level, depth + 1u);
    if (docbook_name_is(value, "para") || docbook_name_is(value, "simpara"))
        return docbook_literal(renderer, "<p>") &&
               docbook_render_inline_children(renderer, value, depth + 1u) &&
               docbook_literal(renderer, "</p>");
    if (docbook_name_is(value, "formalpara")) {
        const QwDataValue *title = docbook_child(value, "title");
        if (!docbook_literal(renderer, "<p>")) return 0;
        if (title && (!docbook_literal(renderer, "<strong>") ||
                      !docbook_render_inline_children(renderer, title, depth + 1u) ||
                      !docbook_literal(renderer, ".</strong> "))) return 0;
        content = docbook_content(value);
        if (content) {
            size_t i;
            for (i = 0u; i < content->value.array.count; ++i) {
                const QwDataValue *child = &content->value.array.items[i];
                if (!docbook_name_is(child, "title") &&
                    !docbook_render_inline(renderer, child, depth + 1u)) return 0;
            }
        }
        return docbook_literal(renderer, "</p>");
    }
    if (docbook_name_is(value, "itemizedlist"))
        return docbook_render_list(renderer, value, 0, heading_level, depth + 1u);
    if (docbook_name_is(value, "orderedlist"))
        return docbook_render_list(renderer, value, 1, heading_level, depth + 1u);
    if (docbook_name_is(value, "variablelist"))
        return docbook_render_variable_list(renderer, value, heading_level, depth + 1u);
    if (docbook_name_is(value, "programlisting") || docbook_name_is(value, "screen") ||
        docbook_name_is(value, "literallayout") || docbook_name_is(value, "synopsis"))
        return docbook_literal(renderer, "<pre><code>") &&
               docbook_render_plain_tree(renderer, value, depth + 1u) &&
               docbook_literal(renderer, "</code></pre>");
    if (docbook_name_is(value, "blockquote") || docbook_name_is(value, "epigraph"))
        return docbook_literal(renderer, "<blockquote>") &&
               docbook_render_blocks(renderer, value, heading_level, depth + 1u) &&
               docbook_literal(renderer, "</blockquote>");
    if (docbook_name_is(value, "note") || docbook_name_is(value, "warning") ||
        docbook_name_is(value, "tip") || docbook_name_is(value, "important") ||
        docbook_name_is(value, "caution")) {
        const QwDataValue *title = docbook_child(value, "title");
        if (!docbook_literal(renderer, "<aside>")) return 0;
        if (title && !docbook_heading(renderer, title, heading_level, "strong")) return 0;
        content = docbook_content(value);
        if (content) {
            size_t i;
            for (i = 0u; i < content->value.array.count; ++i) {
                const QwDataValue *child = &content->value.array.items[i];
                if (!docbook_name_is(child, "title") &&
                    !docbook_render_block(renderer, child, heading_level, depth + 1u))
                    return 0;
            }
        }
        if (!docbook_literal(renderer, "</aside>")) return 0;
        return 1;
    }
    if (docbook_name_is(value, "table") || docbook_name_is(value, "informaltable"))
        return docbook_render_table(renderer, value, heading_level, depth + 1u);
    if (docbook_name_is(value, "mediaobject") ||
        docbook_name_is(value, "inlinemediaobject"))
        return docbook_render_media(renderer, value);
    if (docbook_name_is(value, "figure") || docbook_name_is(value, "informalfigure")) {
        const QwDataValue *title = docbook_child(value, "title");
        if (!docbook_literal(renderer, "<figure>")) return 0;
        if (title && (!docbook_literal(renderer, "<figcaption>") ||
                      !docbook_render_inline_children(renderer, title, depth + 1u) ||
                      !docbook_literal(renderer, "</figcaption>"))) return 0;
        content = docbook_content(value);
        if (content) {
            size_t i;
            for (i = 0u; i < content->value.array.count; ++i) {
                const QwDataValue *child = &content->value.array.items[i];
                if (!docbook_name_is(child, "title") &&
                    !docbook_render_block(renderer, child, heading_level, depth + 1u))
                    return 0;
            }
        }
        if (!docbook_literal(renderer, "</figure>")) return 0;
        return 1;
    }
    if (docbook_name_is(value, "bridgehead"))
        return docbook_heading(renderer, value, heading_level + 1u, NULL);
    if (docbook_name_is(value, "abstract"))
        return docbook_literal(renderer, "<blockquote>") &&
               docbook_render_blocks(renderer, value, heading_level, depth + 1u) &&
               docbook_literal(renderer, "</blockquote>");
    return docbook_render_blocks(renderer, value, heading_level, depth + 1u);
}

static int docbook_open_layout(const void *input, size_t size,
                               QwInputFormat input_format,
                               const QwConvertOptions *options,
                               QwLayoutDocument *out_document,
                               QwConvertReport *report)
{
    static const char prefix[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
        "@page{size:A4;margin:18mm}body{font-family:serif;font-size:11pt;"
        "line-height:1.45;color:#202124}pre{white-space:pre-wrap;overflow-wrap:anywhere}"
        "table{border-collapse:collapse;width:100%}td,th{border:1px solid #777;"
        "padding:4pt;vertical-align:top}blockquote,aside{margin:1em 2em}"
        "</style></head><body>";
    static const char suffix[] = "</body></html>";
    QwDataDocument xml;
    DocBookRenderer renderer;
    const QwDataValue *root;
    uint8_t *prepared = NULL;
    size_t prepared_size = 0u;
    size_t root_start = 0u;
    size_t root_size = 0u;
    int rc;
    (void)options;
    memset(&xml, 0, sizeof(xml));
    memset(&renderer, 0, sizeof(renderer));
    if (!input || !size || !out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (input_format != QW_FORMAT_DOCBOOK) return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (size > DOCBOOK_MAX_INPUT_BYTES) return QW_STATUS_ERR_RESOURCE_LIMIT;
    if (!docbook_scan_root((const uint8_t *)input, size, 1,
                           &root_start, &root_size)) return QW_STATUS_ERR_FORMAT;
    rc = docbook_prepare_xml((const uint8_t *)input, size, &prepared, &prepared_size);
    if (rc != QW_STATUS_OK) return rc;
    rc = qw_xml_parse_data(prepared, prepared_size, &xml);
    free(prepared);
    if (rc != QW_STATUS_OK) return rc;
    root = docbook_member(&xml.root, "$xml");
    if (root) {
        const char *root_name = docbook_element_name(root, &root_size);
        if (docbook_root_name(root_name, root_size)) goto root_valid;
    }
    {
        rc = QW_STATUS_ERR_FORMAT;
        goto cleanup;
    }
root_valid:
    renderer.html.limit = (size_t)DOCBOOK_MAX_HTML_BYTES + 1u;
    if (!docbook_literal(&renderer, prefix) ||
        !docbook_render_container(&renderer, root, 1u, 0u) ||
        !docbook_literal(&renderer, suffix)) {
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
            "DocBook XML rendered to shared layout; DTD declarations are discarded, "
            "external resources are not loaded, and unsupported elements are flattened");
    }
    rc = QW_STATUS_OK;
cleanup:
    free(renderer.html.data);
    qw_data_document_dispose(&xml);
    return rc;
}

#ifndef QUIREWISE_STANDALONE_DOCBOOK_PARSER
const QwFormatDescriptor qw_format_docbook = {
    .id = QW_FORMAT_DOCBOOK,
    .name = "DocBook",
    .probe = docbook_probe,
    .open_layout = docbook_open_layout,
    .max_input_bytes = DOCBOOK_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT
};

const QwPathFormatDescriptor qw_path_format_docbook = {
    .id = QW_FORMAT_DOCBOOK,
    .probe_path = docbook_path_probe,
    .decode_document_path = NULL,
    .resource_profile = NULL
};
#endif

int qw_docbook_abi_version(void)
{
    return QW_DOCBOOK_ABI_VERSION;
}

static QwDocBookStatus docbook_public_status(int status)
{
    switch (status) {
    case QW_STATUS_OK: return QW_DOCBOOK_OK;
    case QW_STATUS_ERR_INVALID_ARGUMENT: return QW_DOCBOOK_ERR_INVALID_ARGUMENT;
    case QW_STATUS_ERR_OUT_OF_MEMORY: return QW_DOCBOOK_ERR_OUT_OF_MEMORY;
    case QW_STATUS_ERR_RESOURCE_LIMIT: return QW_DOCBOOK_ERR_RESOURCE_LIMIT;
    case QW_STATUS_ERR_UNSUPPORTED: return QW_DOCBOOK_ERR_UNSUPPORTED;
    case QW_STATUS_ERR_DECODE: return QW_DOCBOOK_ERR_DECODE;
    default: return QW_DOCBOOK_ERR_FORMAT;
    }
}

QwDocBookStatus qw_docbook_parse_html(const void *data, size_t size,
                                     QwDocBookDocument *out_document)
{
    QwLayoutDocument parsed = {0};
    int status;
    if (!out_document) return QW_DOCBOOK_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    status = docbook_open_layout(data, size, QW_FORMAT_DOCBOOK, NULL,
                                 &parsed, NULL);
    if (status != QW_STATUS_OK) return docbook_public_status(status);
    out_document->html_utf8 = (char *)parsed.context;
    out_document->html_size = parsed.html_size;
    return QW_DOCBOOK_OK;
}

void qw_docbook_document_free(QwDocBookDocument *document)
{
    if (!document) return;
    free(document->html_utf8);
    memset(document, 0, sizeof(*document));
}

const char *qw_docbook_status_string(QwDocBookStatus status)
{
    switch (status) {
    case QW_DOCBOOK_OK: return "ok";
    case QW_DOCBOOK_ERR_INVALID_ARGUMENT: return "invalid argument";
    case QW_DOCBOOK_ERR_OUT_OF_MEMORY: return "out of memory";
    case QW_DOCBOOK_ERR_FORMAT: return "invalid or unsupported DocBook XML";
    case QW_DOCBOOK_ERR_RESOURCE_LIMIT: return "resource limit exceeded";
    case QW_DOCBOOK_ERR_UNSUPPORTED: return "unsupported DocBook feature";
    case QW_DOCBOOK_ERR_DECODE: return "invalid XML encoding";
    default: return "unknown status";
    }
}
