# quirewise-data

Structured-data readers (JSON, JSON Lines, YAML, TOML, vCard, XML, OPML,
iPynb), a namespace-aware XML tree and an XML serializer.

`Quirewise::Data` is an independent, MIT-licensed C11 library. It is installed
as a CMake package and can be consumed without the Quirewise application or any
other Quirewise package.

## What it does

- Parses JSON and JSON Lines into an ordered tree of null, boolean, number,
  string, array and object values. Object member order and number lexemes are
  preserved.
- Parses a practical YAML subset: single-document block and flow mappings,
  sequences, quoted/plain scalars, comments, booleans, null and numbers.
- Parses TOML: assignments, dotted and quoted keys, tables, arrays of tables,
  inline tables, arrays and scalars; TOML dates and timestamps become temporal
  values.
- Parses vCard 2.1/3.0/4.0 into ordered cards and properties with groups, raw
  parameters and decoded values.
- Parses XML 1.0 into a navigable tree with namespace-aware lookup, optional DTD
  validation, and external entities available only through a caller-supplied
  resolver; serializes an XML tree back to UTF-8 with `qw_xml_serialize`.
- Renders a supported DocBook XML subset to a complete HTML document, and an
  OPML outline to a complete nested-list HTML document.
- Accepts iPynb notebooks (JSON shape check: `nbformat` 4 plus a `cells` array).

- Writes the parsed value tree as JSON, JSON Lines, YAML, TOML, vCard
  (`QW_FORMAT_VCF`) or XML with `qw_data_write_document`, and frees the
  malloc-owned result with `qw_data_buffer_free`.

Deliberate boundaries: JSON↔JSONL changes the document shape on purpose (a JSON
array is emitted as one record per line, and JSON Lines is collected into an
array). The XML writer only accepts the ordered `$xml`/`$name`/`$attributes`/
`$content` document shape produced by the XML reader, so an arbitrary JSON tree
is refused with `QW_DATA_ERR_UNSUPPORTED` instead of being guessed at. DocBook,
OPML and iPynb remain read-only, as does the `quirewise/xml.h` tree apart from
`qw_xml_serialize`. The vCard property-table bridge that turns a vCard into a
spreadsheet is a separate feature and is compiled out of this package (it would
require the spreadsheet package); only the data vCard writer is exposed here.
YAML anchors, aliases, tags, directives, block scalars and multi-document
streams are unsupported. XML DTD validation is optional, not XSD, and the tree
has no public mutation API.

## Requirements

- C11 compiler and CMake 3.21 or newer.
- The `QuirewiseCharset` and `QuirewiseRuntime` CMake packages must be
  installed; the exported `QuirewiseData` package config finds them.
- CTest is used for the test suite.

## Build, test and install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/quirewise/prefix
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

Shared builds are available with `-DQUIREWISE_BUILD_SHARED=ON`.

## Use it from CMake

```cmake
find_package(QuirewiseData CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::Data)
```

## Quick start

Parse a JSON document, read a boolean and the first array element, then free the
document:

```c
#include <quirewise/data.h>

#include <stdio.h>
#include <string.h>

int main(void)
{
    static const char json[] =
        "{\"enabled\":true,\"ports\":[8080,8443],\"name\":\"gateway\"}";
    QwDataDocument *document = NULL;
    const QwDataValue *root;
    const QwDataValue *ports;
    const QwDataValue *first;
    const char *text = NULL;
    size_t text_size = 0u;
    int enabled = 0;

    if (qw_data_parse(QW_DATA_FORMAT_JSON, json, sizeof(json) - 1u,
                      &document) != QW_DATA_OK) {
        fputs("JSON parse failed\n", stderr);
        return 1;
    }
    root = qw_data_document_root(document);
    if (qw_data_value_boolean(qw_data_object_find(root, "enabled", 7u),
                              &enabled) != QW_DATA_OK) {
        qw_data_document_free(document);
        return 1;
    }
    ports = qw_data_object_find(root, "ports", 5u);
    first = qw_data_array_at(ports, 0u);
    if (first && qw_data_value_text(first, &text, &text_size) == QW_DATA_OK)
        printf("enabled=%d ports=%zu first=%.*s\n", enabled,
               qw_data_value_count(ports), (int)text_size, text);

    qw_data_document_free(document);
    return 0;
}
```

## API overview

| Header | Purpose |
|---|---|
| `quirewise/data.h` | ordered value tree and the JSON/JSONL/YAML/TOML/vCard/XML/OPML/iPynb readers |
| `quirewise/xml.h` | XML 1.0 tree, navigation, lookup, serializer and parse options |
| `quirewise/docbook.h` | DocBook XML subset to an HTML document |
| `quirewise/opml.h` | OPML outline to an HTML document |

| Task | Functions |
|---|---|
| Parse into a value tree | `qw_data_parse` |
| Value-tree lifetime | `qw_data_document_free` |
| Document metadata | `qw_data_document_format`, `qw_data_document_root` |
| Inspect a value | `qw_data_value_kind`, `qw_data_value_count`, `qw_data_value_boolean`, `qw_data_value_text` |
| Walk arrays/objects | `qw_data_array_at`, `qw_data_object_at`, `qw_data_object_find` |
| XML parse/drop | `qw_xml_parse`, `qw_xml_parse_with_options`, `qw_xml_drop`, `qw_xml_abi_version` |
| XML navigation | `qw_xml_node_kind`, `qw_xml_root`, `qw_xml_down`, `qw_xml_next`, `qw_xml_prev`, `qw_xml_up` |
| XML names and text | `qw_xml_is_tag`, `qw_xml_tag`, `qw_xml_qualified_tag`, `qw_xml_local_name`, `qw_xml_namespace_uri`, `qw_xml_text`, `qw_xml_comment`, `qw_xml_pi_target`, `qw_xml_pi_data` |
| XML attributes | `qw_xml_att`, `qw_xml_att_ns`, `qw_xml_att_alt`, `qw_xml_att_eq`, `qw_xml_attribute_count`, `qw_xml_attribute_name`, `qw_xml_attribute_local_name`, `qw_xml_attribute_namespace_uri`, `qw_xml_attribute_value` |
| XML search | `qw_xml_find`, `qw_xml_find_next`, `qw_xml_find_down`, `qw_xml_find_match`, `qw_xml_find_next_match`, `qw_xml_find_down_match`, `qw_xml_find_dfs`, `qw_xml_find_dfs_top`, `qw_xml_find_next_dfs`, `qw_xml_find_next_dfs_top` |
| XML output | `qw_xml_serialize` |
| DocBook | `qw_docbook_parse_html`, `qw_docbook_document_free`, `qw_docbook_status_string`, `qw_docbook_abi_version` |
| OPML | `qw_opml_parse_html`, `qw_opml_document_free`, `qw_opml_status_string`, `qw_opml_abi_version` |

## Behavior and limits

- `qw_data_parse` returns `QW_DATA_OK` (0) or a negative `QwDataStatus`.
  A successful document owns every key and value; free it exactly once with
  `qw_data_document_free`. Input bytes are borrowed only during the call.
- Text returned by `qw_data_value_text`, `qw_data_object_at` and similar points
  into the document and stays valid until the document is freed.
- `qw_data_value_text` accepts number, string and temporal values; it returns
  `QW_DATA_ERR_TYPE` for booleans, null, arrays and objects.
- The generic XML reader keeps elements, attributes and text. Comments,
  processing instructions and the DTD are dropped; use `quirewise/xml.h` when
  you need them.
- XML input encodings declared in the XML prolog are converted to UTF-8.
  External entities are never fetched by the library: without
  `QwXmlParseOptions.resolve_entity` a reference fails with
  `QW_XML_ERROR_UNSUPPORTED`.
- Resource limits: JSON/JSONL input 64 MiB, YAML/TOML/vCard input 256 MiB,
  XML input 256 MiB, DocBook/OPML input 64 MiB; the shared value tree holds at
  most 1,000,000 nodes and nests at most 256 levels while a `quirewise/xml.h`
  tree allows 4,096 levels. Violations report `QW_DATA_ERR_RESOURCE_LIMIT` or
  `QW_XML_ERROR_RESOURCE_LIMIT`.
- `qw_data_write_document` and `qw_xml_serialize` write bytes; both return a
  `malloc` buffer that the caller releases with `free()` or
  `qw_data_buffer_free()`. OPML, iPynb and the `quirewise/xml.h` tree remain
  read-only (apart from `qw_xml_serialize`).

## Testing

`ctest` runs a deterministic fuzz smoke over every reader (mutated seeds,
including DocBook and OPML), a status-class test that verifies deep TOML
nesting reports `QW_DATA_ERR_RESOURCE_LIMIT` while malformed TOML reports
`QW_DATA_ERR_FORMAT`, stably across repeated runs, and a writer test that
round-trips JSON/JSON Lines/YAML/TOML/vCard/XML and checks that unsupported
targets are refused without producing bytes.

## Documentation

See [docs/usage.md](docs/usage.md) for task-oriented recipes, the XML tree
model, ownership rules and per-format limits.

## License

MIT. See [LICENSE](LICENSE).
