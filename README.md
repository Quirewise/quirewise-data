# quirewise-data

Standalone C11 structured-data library: ordered value trees, JSON/JSONL/YAML/
TOML/vCard readers, an XML 1.0 tree with optional DTD validation, and XML
serialisation.

CMake target: `Quirewise::Data`. Public headers are under `include/quirewise/`.

## Capabilities

- JSON and JSON Lines: object/array order and numeric lexemes preserved.
- YAML: single-document block/flow mapping, sequence, common scalars, comments
  and quoted strings. Anchors/aliases, tags, directives, block scalars and
  multi-document streams are unsupported.
- TOML: assignments, dotted/quoted keys, tables, arrays of tables, inline
  tables, scalars and date/time values. Writing requires an object root; null
  and non-finite numbers are unsupported.
- vCard 2.1/3.0/4.0: ordered properties, groups, raw parameters, quoted-
  printable soft line breaks.
- XML: bounded UTF-8 XML tree, namespace-aware lookup, optional DTD validation
  with an entity resolver, and `qw_xml_serialize` for writing XML bytes.
- OPML and iPynb use the shared XML/JSON readers; DocBook has a dedicated
  HTML-fragment renderer.

## Limits

Input/output size, node count, tree depth and allocation budget are bounded and
report `QW_DATA_ERR_RESOURCE_LIMIT`. Only XML has a writer: JSON/YAML/TOML/
vCard have no serialiser, and there is no general cross-format conversion API.
Comments, whitespace and raw formatting are not preserved.

## Build

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build
```
