# quirewise-data usage guide

Task-level guide to `Quirewise::Data`; its API is declared in
`quirewise/data.h`, `quirewise/xml.h`, `quirewise/docbook.h`, `quirewise/opml.h`.

## 1. Build and integrate

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/quirewise/prefix
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

```cmake
find_package(QuirewiseData CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::Data)
```

`QuirewiseDataConfig.cmake` calls `find_dependency` for `QuirewiseRuntime` and
`QuirewiseCharset`, so install those packages in a visible prefix. Add
`-DQUIREWISE_BUILD_SHARED=ON` for shared libraries.

## 2. Core concepts

### 2.1 Documents and values

`qw_data_parse(format, data, size, &document)` is the only value-tree entry
point. It returns `QW_DATA_OK` or a negative `QwDataStatus`; on failure
`document` is `NULL` and there is nothing to free.

The document owns the tree. `qw_data_document_root` returns a borrowed root
pointer, and every accessor result points into the document. Free the document
once with `qw_data_document_free`. Input bytes and all text spans are borrowed;
text becomes invalid when the document is freed, while the input can be reused
as soon as the parse call returns.

### 2.2 Ordered values

A value has one `QwDataKind`: `QW_DATA_KIND_NULL`, `BOOLEAN`, `NUMBER`, `STRING`,
`ARRAY`, `OBJECT` or `TEMPORAL`. Objects preserve member order; use
`qw_data_object_at` for ordered iteration and `qw_data_object_find` for a linear
name lookup. `qw_data_value_count` returns the child count for arrays and
objects and `0` for scalars. `qw_data_value_boolean` succeeds only for booleans.
`qw_data_value_text` returns a byte span for number, string and temporal values
and fails with `QW_DATA_ERR_TYPE` for every other kind. Numbers keep their
source lexeme; the library does not convert them to `double`.

### 2.3 Root shapes

| Format | Root value |
|---|---|
| `JSON` | whatever the document contains |
| `JSONL` | array with one entry per non-empty line |
| `YAML` | mapping, sequence or scalar (single document) |
| `TOML` | object |
| `VCARD` | object with key `$vcard` holding an array of cards |
| `XML` | object with key `$xml` holding one element object |
| `OPML` | object with key `$xml`; root must be `opml` with a `body` child |
| `IPYNB` | JSON object containing `nbformat` 4 and a `cells` array |

vCard cards contain `$version` and `$properties`; each property has `$group`,
`$name`, `$params` (array of raw parameter strings) and `$value`. Generic XML
elements contain `$name`, `$attributes` (object) and `$content` (array of text
strings and child elements). Use `quirewise/xml.h` for comments, processing
instructions, namespaces and serialization.

DocBook and OPML also have one-shot HTML helpers (`qw_docbook_parse_html`,
`qw_opml_parse_html`); free their output with the matching `*_document_free`.

## 3. Workflows
Recipes are excerpts; place them inside a function and supply the inputs.

### 3.1 Read a JSON configuration

```c
#include <quirewise/data.h>
#include <stdio.h>

QwDataDocument *document = NULL;
const QwDataValue *timeout;
const char *text = NULL;
size_t text_size = 0u;

if (qw_data_parse(QW_DATA_FORMAT_JSON, json, size, &document) != QW_DATA_OK)
    return 1;
timeout = qw_data_object_find(qw_data_document_root(document),
                              "timeout_ms", 10u);
if (timeout && qw_data_value_kind(timeout) == QW_DATA_KIND_NUMBER &&
    qw_data_value_text(timeout, &text, &text_size) == QW_DATA_OK)
    printf("timeout_ms: %.*s\n", (int)text_size, text);
qw_data_document_free(document);
```

### 3.2 Iterate JSON Lines records

The root is an array, so no second container is needed:

```c
QwDataDocument *document = NULL;
if (qw_data_parse(QW_DATA_FORMAT_JSONL, bytes, size, &document) == QW_DATA_OK) {
    const QwDataValue *records = qw_data_document_root(document);
    size_t count = qw_data_value_count(records);
    size_t index;
    for (index = 0u; index < count; ++index) {
        const QwDataValue *record = qw_data_array_at(records, index);
        const QwDataValue *id = qw_data_object_find(record, "id", 2u);
        const char *text = NULL;
        size_t text_size = 0u;
        if (id && qw_data_value_text(id, &text, &text_size) == QW_DATA_OK)
            printf("record %zu: %.*s\n", index, (int)text_size, text);
    }
    qw_data_document_free(document);
}
```

Each line must hold one complete JSON value; blank lines are rejected with
`QW_DATA_ERR_FORMAT`. A trailing newline is fine.

### 3.3 Read TOML tables and timestamps

TOML dates and timestamps are `QW_DATA_KIND_TEMPORAL` and keep their text:

```c
static const char toml[] =
    "[service]\n"
    "name = \"gateway\"\n"
    "created = 2024-05-01T12:00:00Z\n";
QwDataDocument *document = NULL;
if (qw_data_parse(QW_DATA_FORMAT_TOML, toml, sizeof(toml) - 1u,
                  &document) == QW_DATA_OK) {
    const QwDataValue *service =
        qw_data_object_find(qw_data_document_root(document), "service", 7u);
    const QwDataValue *created = qw_data_object_find(service, "created", 7u);
    if (qw_data_value_kind(created) == QW_DATA_KIND_TEMPORAL) {
        const char *text = NULL;
        size_t text_size = 0u;
        if (qw_data_value_text(created, &text, &text_size) == QW_DATA_OK)
            printf("created: %.*s\n", (int)text_size, text);
    }
    qw_data_document_free(document);
}
```

### 3.4 Extract names and phone numbers from vCard

```c
QwDataDocument *document = NULL;
if (qw_data_parse(QW_DATA_FORMAT_VCARD, input, size, &document) != QW_DATA_OK)
    return;
{
    const QwDataValue *cards =
        qw_data_object_find(qw_data_document_root(document), "$vcard", 6u);
    size_t card_count = qw_data_value_count(cards), card_index;
    for (card_index = 0u; card_index < card_count; ++card_index) {
        const QwDataValue *properties = qw_data_object_find(
            qw_data_array_at(cards, card_index), "$properties", 11u);
        size_t property_count = qw_data_value_count(properties), property_index;
        for (property_index = 0u; property_index < property_count;
             ++property_index) {
            const QwDataValue *property =
                qw_data_array_at(properties, property_index);
            const char *name = NULL, *value = NULL;
            size_t name_size = 0u, value_size = 0u;
            if (qw_data_value_text(qw_data_object_find(property, "$name", 5u),
                                   &name, &name_size) != QW_DATA_OK)
                continue;
            if (!((name_size == 2u && memcmp(name, "FN", 2u) == 0) ||
                  (name_size == 3u && memcmp(name, "TEL", 3u) == 0)))
                continue;
            if (qw_data_value_text(
                    qw_data_object_find(property, "$value", 6u),
                    &value, &value_size) == QW_DATA_OK)
                printf("%.*s: %.*s\n", (int)name_size, name,
                       (int)value_size, value);
        }
    }
}
qw_data_document_free(document);
```

Property order is preserved. Read `$params` the same way when `TYPE=HOME` or
`PREF=1` matters. `VERSION` must be the first line inside each card and every
card needs an `FN` property, otherwise the parse fails with
`QW_DATA_ERR_FORMAT`.

### 3.5 Parse, search and re-serialize XML

```c
QwXmlError error;
QwXmlNode *document = qw_xml_parse(xml, size, &error);
if (!document) {
    fprintf(stderr, "XML error %d at byte %zu: %s\n", (int)error.code,
            error.byte_offset, error.message);
} else {
    QwXmlNode *root = qw_xml_root(document);
    QwXmlNode *item;
    for (item = qw_xml_find_down(root, "item"); item;
         item = qw_xml_find_next(item, "item")) {
        QwXmlNode *title = qw_xml_find_down(item, "title");
        QwXmlNode *child = title ? qw_xml_down(title) : NULL;
        while (child && qw_xml_node_kind(child) != QW_XML_NODE_TEXT)
            child = qw_xml_next(child);
        printf("%s\n", child ? qw_xml_text(child) : "(untitled)");
    }
    uint8_t *serialized = NULL;
    size_t serialized_size = 0u;
    if (qw_xml_serialize(document, &serialized, &serialized_size, &error)) {
        fwrite(serialized, 1u, serialized_size, stdout);
        free(serialized); /* malloc-owned */
    }
    qw_xml_drop(document);
}
```

`qw_xml_parse` returns the document node and `qw_xml_root` the root element.
Element text is stored as child text nodes, hence the scan above.
`qw_xml_find` searches a node and its siblings, `qw_xml_find_down` searches only
children, `qw_xml_find_dfs` searches the whole subtree, and
`qw_xml_find_next` continues with following siblings. `qw_xml_drop` accepts any
node and frees the whole tree. `qw_xml_serialize` also accepts an element and
then writes that subtree.

For DTD validation, fill `QwXmlParseOptions` and call
`qw_xml_parse_with_options`. External resources are fetched only through
`resolve_entity`; with a `NULL` callback an external entity reference fails with
`QW_XML_ERROR_UNSUPPORTED`. The library never opens files or sockets itself.

## 4. Error handling

| Value | Meaning |
|---|---|
| `QW_DATA_OK` (0) | success |
| `QW_DATA_ERR_INVALID_ARGUMENT` (-1) | null pointer or empty input |
| `QW_DATA_ERR_OUT_OF_MEMORY` (-2) | allocation failure |
| `QW_DATA_ERR_FORMAT` (-3) | malformed input for the selected format |
| `QW_DATA_ERR_UNSUPPORTED` (-4) | valid syntax outside the supported subset |
| `QW_DATA_ERR_RESOURCE_LIMIT` (-5) | input, depth or node budget exceeded |
| `QW_DATA_ERR_TYPE` (-6) | wrong value kind for the accessor |

DocBook and OPML use small positive enums (`QW_DOCBOOK_ERR_*`,
`QW_OPML_ERR_*`, values 1-6) plus `qw_docbook_status_string` and
`qw_opml_status_string`. Always check the status before reading an
out-parameter.

XML failures are reported through `QwXmlError`: `code`, `byte_offset` and a
160-byte NUL-terminated `message`. The error argument may be `NULL`; pass a
struct when you log failures. It is zeroed at the start of every parse and
serialize call. Codes include `QW_XML_ERROR_ARGUMENT`, `FORMAT`,
`RESOURCE_LIMIT`, `OUT_OF_MEMORY`, `ENCODING`, `UNSUPPORTED`, `NAMESPACE` and
`VALIDITY`.

## 5. Memory, lifetime and threading

- Input is borrowed for the call; documents and trees are caller-owned after
  success and freed once. `qw_data_document_free` and `qw_xml_drop` accept
  `NULL`; `qw_docbook_document_free` and `qw_opml_document_free` release the
  HTML buffer and reset the struct.
- Serializer output is `malloc`-owned; release it with `free()`.
- There is no global mutable state and no runtime initialization, so separate
  documents may be parsed on different threads. Do not free a document while
  another thread reads it.

## 6. Limits and format notes

| Input | Maximum input | Notes |
|---|---|---|
| JSON / JSONL | 64 MiB | JSONL rejects blank lines and empty input |
| YAML | 256 MiB | single document; no anchors, aliases, tags or block scalars |
| TOML | 256 MiB | object root; date/time values become temporal |
| vCard | 256 MiB | 2.1/3.0/4.0; legacy CHARSET and quoted-printable decoded |
| XML | 256 MiB | depth 4,096 in `xml.h` (generic reader 256), nodes 1,000,000 |
| DocBook / OPML | 64 MiB | rendered to a complete HTML document |

The shared value tree is capped at 1,000,000 nodes and 256 nesting levels;
violations return `QW_DATA_ERR_RESOURCE_LIMIT` deterministically.

- YAML supports block/flow collections, quoted/plain scalars, comments,
  booleans, null and numbers. Anchors, aliases, tags, directives and block
  scalars report `QW_DATA_ERR_UNSUPPORTED`; a second `---` is rejected.
- TOML inline tables and arrays of tables become ordinary objects and arrays;
  dotted keys become nested objects. TOML has no null; only XML can be written.
- iPynb requires `nbformat` 4 and a `cells` array; the JSON reader otherwise
  reports `QW_DATA_ERR_FORMAT`.
- Generic XML parsing drops comments, processing instructions and the DTD and
  does not preserve namespace prefixes; use `quirewise/xml.h` for full fidelity.

## 7. Related packages

- `Quirewise::Charset` performs encoding conversion for the XML and vCard
  readers; `Quirewise::Runtime` supplies shared status/utility definitions. Both
  are public dependencies.
- `Quirewise::Spreadsheet` uses the XML reader for XLSX and ODS parts.
- `Quirewise::Document`, `Quirewise::Ebook`, `Quirewise::Vector` and
  `Quirewise::Pipeline` consume `Quirewise::Data`.

## 8. License

MIT. See `LICENSE`.
