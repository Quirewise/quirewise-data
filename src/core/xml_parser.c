#include "xml_parser.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <iconv.h>
#endif

enum {
    QW_XML_MAX_DEPTH = 4096,
    QW_XML_MAX_NODES = 1000000,
    QW_XML_MAX_ATTRIBUTES = 4096,
    QW_XML_MAX_ATTRIBUTE_DECLS = 65536,
    QW_XML_MAX_ELEMENT_DECLS = 65536,
    QW_XML_MAX_NOTATION_DECLS = 16384,
    QW_XML_MAX_ENTITIES = 4096,
    QW_XML_MAX_ENTITY_DEPTH = 16
};

typedef enum QwXmlAttributeType {
    QW_XML_ATTR_CDATA = 0,
    QW_XML_ATTR_ID,
    QW_XML_ATTR_IDREF,
    QW_XML_ATTR_IDREFS,
    QW_XML_ATTR_ENTITY,
    QW_XML_ATTR_ENTITIES,
    QW_XML_ATTR_NMTOKEN,
    QW_XML_ATTR_NMTOKENS,
    QW_XML_ATTR_ENUMERATION,
    QW_XML_ATTR_NOTATION
} QwXmlAttributeType;

typedef enum QwXmlContentKind {
    QW_XML_CONTENT_EMPTY = 0,
    QW_XML_CONTENT_ANY,
    QW_XML_CONTENT_MIXED,
    QW_XML_CONTENT_CHILDREN
} QwXmlContentKind;

typedef enum QwXmlDefaultKind {
    QW_XML_DEFAULT_VALUE = 0,
    QW_XML_DEFAULT_REQUIRED,
    QW_XML_DEFAULT_IMPLIED,
    QW_XML_DEFAULT_FIXED
} QwXmlDefaultKind;

#define QW_XML_MAX_INPUT_BYTES ((size_t)256u * 1024u * 1024u)
#define QW_XML_MAX_ALLOC_BYTES ((size_t)256u * 1024u * 1024u)
#define QW_XML_MAX_ENTITY_BYTES ((size_t)256u * 1024u * 1024u)
#define QW_XML_MAX_ENTITY_TABLE_BYTES ((size_t)16u * 1024u * 1024u)

#define QW_XML_NS_XML "http://www.w3.org/XML/1998/namespace"
#define QW_XML_NS_XMLNS "http://www.w3.org/2000/xmlns/"

typedef struct QwXmlAttribute {
    char *name;
    char *value;
    char *local_name;
    char *namespace_uri;
    int specified;
} QwXmlAttribute;

struct QwXmlNode {
    QwXmlNodeKind kind;
    char *tag;
    char *qualified_tag;
    char *namespace_uri;
    char *text;
    char *processing_instruction_target;
    QwXmlAttribute *attributes;
    size_t attribute_count;
    size_t attribute_capacity;
    struct QwXmlNode *parent;
    struct QwXmlNode *previous;
    struct QwXmlNode *first_child;
    struct QwXmlNode *last_child;
    struct QwXmlNode *next;
    int is_document;
    int is_whitespace;
    int is_cdata;
};

typedef struct QwXmlBuffer {
    char *data;
    size_t size;
    size_t capacity;
} QwXmlBuffer;

typedef struct QwXmlOriginMap {
    uint32_t *ids;
    size_t size;
    size_t capacity;
} QwXmlOriginMap;

typedef struct QwXmlEntity {
    char *name;
    char *value;
    size_t value_size;
    char *public_id;
    char *system_id;
    char *base_uri;
    char *notation_name;
    int parameter;
    int external;
    int unparsed;
    int external_markup;
    int referenced;
} QwXmlEntity;

typedef struct QwXmlAttributeDecl {
    char *element_name;
    char *name;
    char *default_value;
    char *enumeration;
    int tokenized;
    QwXmlAttributeType type;
    QwXmlDefaultKind default_kind;
    int external_markup;
} QwXmlAttributeDecl;

typedef struct QwXmlElementDecl {
    char *name;
    char *content_model;
    QwXmlContentKind content_kind;
    int external_markup;
} QwXmlElementDecl;

typedef struct QwXmlNotationDecl {
    char *name;
} QwXmlNotationDecl;

typedef struct QwXmlParser {
    const uint8_t *input;
    size_t size;
    size_t position;
    size_t allocated_bytes;
    size_t node_count;
    size_t text_bytes;
    size_t entity_value_bytes;
    size_t entity_expanded_bytes;
    size_t external_loaded_bytes;
    unsigned entity_expansion_depth;
    uint32_t input_origin_id;
    uint32_t next_origin_id;
    const uint32_t *input_origin_ids;
    size_t conditional_origin_depth;
    uint32_t conditional_origin_stack[QW_XML_MAX_DEPTH];
    size_t depth;
    size_t document_element_count;
    int document_type_seen;
    int external_subset_unread;
    int external_subset_present;
    int in_external_subset;
    int in_parameter_entity;
    int in_document_content;
    int in_document_attribute_value;
    int xml_declaration_seen;
    int standalone_declared;
    int standalone_yes;
    int validate_dtd;
    unsigned int source_code_page;
    const QwXmlParseOptions *options;
    const char *current_base_uri;
    char *doctype_name;
    QwXmlEntity *entities;
    size_t entity_count;
    size_t entity_capacity;
    QwXmlAttributeDecl *attribute_decls;
    size_t attribute_decl_count;
    size_t attribute_decl_capacity;
    QwXmlElementDecl *element_decls;
    size_t element_decl_count;
    size_t element_decl_capacity;
    QwXmlNotationDecl *notation_decls;
    size_t notation_decl_count;
    size_t notation_decl_capacity;
    QwXmlNode *root;
    QwXmlNode *current;
    QwXmlError *error;
    uint8_t *normalized_input;
} QwXmlParser;

enum {
    QW_XML_EXTERNAL_DTD = 0,
    QW_XML_EXTERNAL_CONTENT = 1
};

static int qwx_prepare_input(QwXmlParser *parser, const void *data,
                             size_t size);
static int qwx_qname_parts(const char *name, size_t *prefix_size,
                           size_t *local_start);
static int qwx_parse_content(QwXmlParser *parser);
static int qwx_parse_dtd_subset(QwXmlParser *parser, int close_mode);
static int qwx_parse_external_resource(QwXmlParser *parser,
                                       const char *public_id,
                                       const char *system_id,
                                       const char *base_uri,
                                       int content_entity);
static int qwx_get_external_text(QwXmlParser *parser,
                                const QwXmlEntity *entity,
                                char **out_text, size_t *out_size,
                                const char **out_base_uri);

static int qwx_fail(QwXmlParser *parser, QwXmlErrorCode code,
                    const char *message)
{
    if (parser && parser->error && parser->error->code == QW_XML_ERROR_NONE) {
        parser->error->code = code;
        parser->error->byte_offset = parser->position;
        (void)snprintf(parser->error->message,
                       sizeof(parser->error->message), "%s", message);
    }
    return 0;
}

static void *qwx_allocate(QwXmlParser *parser, size_t size)
{
    void *memory;
    if (!size) size = 1u;
    if (size > QW_XML_MAX_ALLOC_BYTES - parser->allocated_bytes) {
        qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                 "XML parser allocation limit exceeded");
        return NULL;
    }
    memory = malloc(size);
    if (!memory) {
        qwx_fail(parser, QW_XML_ERROR_OUT_OF_MEMORY,
                 "Could not allocate XML parser memory");
        return NULL;
    }
    parser->allocated_bytes += size;
    return memory;
}

static void *qwx_reallocate(QwXmlParser *parser, void *memory,
                            size_t old_size, size_t new_size)
{
    void *grown;
    size_t retained;
    if (old_size > parser->allocated_bytes) {
        qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                 "XML parser allocation accounting failed");
        return NULL;
    }
    retained = parser->allocated_bytes - old_size;
    if (new_size > QW_XML_MAX_ALLOC_BYTES - retained) {
        qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                 "XML parser allocation limit exceeded");
        return NULL;
    }
    grown = realloc(memory, new_size ? new_size : 1u);
    if (!grown) {
        qwx_fail(parser, QW_XML_ERROR_OUT_OF_MEMORY,
                 "Could not grow XML parser memory");
        return NULL;
    }
    parser->allocated_bytes = retained + (new_size ? new_size : 1u);
    return grown;
}

static void qwx_release(QwXmlParser *parser, void *memory, size_t size)
{
    if (!memory) return;
    free(memory);
    if (size <= parser->allocated_bytes) parser->allocated_bytes -= size;
}

static int qwx_buffer_reserve(QwXmlParser *parser, QwXmlBuffer *buffer,
                              size_t needed)
{
    size_t capacity;
    char *grown;
    if (needed <= buffer->capacity) return 1;
    capacity = buffer->capacity ? buffer->capacity : 64u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    grown = (char *)qwx_reallocate(parser, buffer->data, buffer->capacity,
                                   capacity);
    if (!grown) return 0;
    buffer->data = grown;
    buffer->capacity = capacity;
    return 1;
}

static int qwx_buffer_append(QwXmlParser *parser, QwXmlBuffer *buffer,
                             const char *data, size_t size)
{
    if (size > SIZE_MAX - buffer->size - 1u ||
        !qwx_buffer_reserve(parser, buffer, buffer->size + size + 1u)) {
        return 0;
    }
    if (size) memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    buffer->data[buffer->size] = '\0';
    return 1;
}

static uint32_t qwx_input_origin(const QwXmlParser *parser, size_t position)
{
    if (parser->input_origin_ids && position < parser->size)
        return parser->input_origin_ids[position];
    return parser->input_origin_id;
}

static uint32_t qwx_next_parameter_entity_origin(QwXmlParser *parser)
{
    if (parser->next_origin_id == UINT32_MAX) {
        qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                 "XML parameter-entity origin limit exceeded");
        return 0u;
    }
    return ++parser->next_origin_id;
}

static int qwx_require_origin(QwXmlParser *parser, uint32_t origin,
                              size_t position, size_t size,
                              const char *message)
{
    size_t i;
    if (!parser->validate_dtd) return 1;
    for (i = 0u; i < size; ++i) {
        if (qwx_input_origin(parser, position + i) != origin)
            return qwx_fail(parser, QW_XML_ERROR_VALIDITY, message);
    }
    return 1;
}

static int qwx_origin_map_reserve(QwXmlParser *parser,
                                  QwXmlOriginMap *origins, size_t needed)
{
    size_t capacity;
    uint32_t *grown;
    if (needed <= origins->capacity) return 1;
    if (needed > SIZE_MAX / sizeof(*origins->ids))
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML DTD origin map is too large");
    capacity = origins->capacity ? origins->capacity : 64u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    if (capacity > SIZE_MAX / sizeof(*origins->ids)) capacity = needed;
    grown = (uint32_t *)qwx_reallocate(parser, origins->ids,
        origins->capacity * sizeof(*origins->ids),
        capacity * sizeof(*origins->ids));
    if (!grown) return 0;
    origins->ids = grown;
    origins->capacity = capacity;
    return 1;
}

static int qwx_dtd_fragment_append(QwXmlParser *parser, QwXmlBuffer *output,
                                  QwXmlOriginMap *origins,
                                  const char *data, size_t size,
                                  const uint32_t *source_origins,
                                  uint32_t source_origin)
{
    size_t i;
    if (!qwx_buffer_append(parser, output, data, size)) return 0;
    if (!origins) return 1;
    if (size > SIZE_MAX - origins->size ||
        !qwx_origin_map_reserve(parser, origins, origins->size + size))
        return 0;
    if (source_origins) {
        for (i = 0u; i < size; ++i)
            origins->ids[origins->size + i] = source_origins[i];
    } else {
        for (i = 0u; i < size; ++i)
            origins->ids[origins->size + i] = source_origin;
    }
    origins->size += size;
    return 1;
}

static char *qwx_duplicate(QwXmlParser *parser, const char *data, size_t size)
{
    char *copy;
    if (size == SIZE_MAX) {
        qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                 "XML string is too large");
        return NULL;
    }
    copy = (char *)qwx_allocate(parser, size + 1u);
    if (!copy) return NULL;
    if (size) memcpy(copy, data, size);
    copy[size] = '\0';
    return copy;
}

static int qwx_decode_utf8(const uint8_t *data, size_t size,
                           size_t *position, uint32_t *codepoint)
{
    uint8_t first;
    size_t needed;
    uint32_t value;
    uint32_t minimum;
    size_t i;
    if (!data || !position || !codepoint || *position >= size) return 0;
    first = data[(*position)++];
    if (first < 0x80u) {
        *codepoint = first;
        return 1;
    }
    if ((first & 0xe0u) == 0xc0u) {
        needed = 1u; value = first & 0x1fu; minimum = 0x80u;
    } else if ((first & 0xf0u) == 0xe0u) {
        needed = 2u; value = first & 0x0fu; minimum = 0x800u;
    } else if ((first & 0xf8u) == 0xf0u) {
        needed = 3u; value = first & 0x07u; minimum = 0x10000u;
    } else {
        return 0;
    }
    if (needed > size - *position) return 0;
    for (i = 0u; i < needed; ++i) {
        uint8_t next = data[(*position)++];
        if ((next & 0xc0u) != 0x80u) return 0;
        value = (value << 6) | (uint32_t)(next & 0x3fu);
    }
    if (value < minimum || value > 0x10ffffu ||
        (value >= 0xd800u && value <= 0xdfffu)) return 0;
    *codepoint = value;
    return 1;
}

static int qwx_xml_character(uint32_t cp)
{
    return cp == 0x09u || cp == 0x0au || cp == 0x0du ||
           (cp >= 0x20u && cp <= 0xd7ffu) ||
           (cp >= 0xe000u && cp <= 0xfffdu) ||
           (cp >= 0x10000u && cp <= 0x10ffffu);
}

static int qwx_name_start(uint32_t cp)
{
    return cp == ':' || cp == '_' || (cp >= 'A' && cp <= 'Z') ||
           (cp >= 'a' && cp <= 'z') ||
           (cp >= 0xc0u && cp <= 0xd6u) ||
           (cp >= 0xd8u && cp <= 0xf6u) ||
           (cp >= 0xf8u && cp <= 0x2ffu) ||
           (cp >= 0x370u && cp <= 0x37du) ||
           (cp >= 0x37fu && cp <= 0x1fffu) ||
           (cp >= 0x200cu && cp <= 0x200du) ||
           (cp >= 0x2070u && cp <= 0x218fu) ||
           (cp >= 0x2c00u && cp <= 0x2fefu) ||
           (cp >= 0x3001u && cp <= 0xd7ffu) ||
           (cp >= 0xf900u && cp <= 0xfdcfu) ||
           (cp >= 0xfdf0u && cp <= 0xfffdu) ||
           (cp >= 0x10000u && cp <= 0xeffffu);
}

static int qwx_name_char(uint32_t cp)
{
    return qwx_name_start(cp) || cp == '-' || cp == '.' ||
           (cp >= '0' && cp <= '9') || cp == 0xb7u ||
           (cp >= 0x300u && cp <= 0x36fu) ||
           (cp >= 0x203fu && cp <= 0x2040u);
}

static int qwx_append_codepoint(QwXmlParser *parser, QwXmlBuffer *buffer,
                                uint32_t cp)
{
    char bytes[4];
    size_t count;
    if (!qwx_xml_character(cp)) {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML contains an invalid character reference");
    }
    if (cp < 0x80u) {
        bytes[0] = (char)cp; count = 1u;
    } else if (cp < 0x800u) {
        bytes[0] = (char)(0xc0u | (cp >> 6));
        bytes[1] = (char)(0x80u | (cp & 0x3fu)); count = 2u;
    } else if (cp < 0x10000u) {
        bytes[0] = (char)(0xe0u | (cp >> 12));
        bytes[1] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        bytes[2] = (char)(0x80u | (cp & 0x3fu)); count = 3u;
    } else {
        bytes[0] = (char)(0xf0u | (cp >> 18));
        bytes[1] = (char)(0x80u | ((cp >> 12) & 0x3fu));
        bytes[2] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        bytes[3] = (char)(0x80u | (cp & 0x3fu)); count = 4u;
    }
    return qwx_buffer_append(parser, buffer, bytes, count);
}

static int qwx_named_entity(const char *name, size_t size, uint32_t *cp)
{
    if (size == 3u && memcmp(name, "amp", 3u) == 0) *cp = '&';
    else if (size == 2u && memcmp(name, "lt", 2u) == 0) *cp = '<';
    else if (size == 2u && memcmp(name, "gt", 2u) == 0) *cp = '>';
    else if (size == 4u && memcmp(name, "apos", 4u) == 0) *cp = '\'';
    else if (size == 4u && memcmp(name, "quot", 4u) == 0) *cp = '"';
    else return 0;
    return 1;
}

static void qwx_skip_space(QwXmlParser *parser)
{
    while (parser->position < parser->size) {
        uint8_t c = parser->input[parser->position];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++parser->position;
    }
}

static int qwx_starts(const QwXmlParser *parser, const char *text)
{
    size_t size = strlen(text);
    return size <= parser->size - parser->position &&
           memcmp(parser->input + parser->position, text, size) == 0;
}

static int qwx_parse_name(QwXmlParser *parser, char **out_name);
static int qwx_parse_value(QwXmlParser *parser, uint8_t terminator,
                           int attribute, char **out_value);
static int qwx_parse_comment(QwXmlParser *parser, int preserve);
static int qwx_parse_processing_instruction(QwXmlParser *parser, int preserve);
static int qwx_parse_dtd_subset(QwXmlParser *parser, int allow_closing_bracket);

static int qwx_append_entity_value(QwXmlParser *parser, QwXmlBuffer *buffer,
                                  const char *value, size_t value_size,
                                  unsigned depth, int attribute);

static int qwx_entity_append(QwXmlParser *parser, QwXmlBuffer *buffer,
                             const char *data, size_t size)
{
    if (size > QW_XML_MAX_ENTITY_BYTES - parser->entity_expanded_bytes) {
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML entity expansion limit exceeded");
    }
    if (!qwx_buffer_append(parser, buffer, data, size)) return 0;
    parser->entity_expanded_bytes += size;
    return 1;
}

static int qwx_entity_append_codepoint(QwXmlParser *parser,
                                       QwXmlBuffer *buffer, uint32_t cp)
{
    size_t encoded_size = cp < 0x80u ? 1u :
        (cp < 0x800u ? 2u : (cp < 0x10000u ? 3u : 4u));
    size_t before = buffer->size;
    if (!qwx_append_codepoint(parser, buffer, cp)) return 0;
    if (buffer->size - before >
        QW_XML_MAX_ENTITY_BYTES - parser->entity_expanded_bytes) {
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML entity expansion limit exceeded");
    }
    parser->entity_expanded_bytes += encoded_size;
    return 1;
}

static QwXmlEntity *qwx_find_entity(QwXmlParser *parser,
                                   const char *name, size_t name_size,
                                   int parameter)
{
    size_t i;
    for (i = 0u; i < parser->entity_count; ++i) {
        if (parser->entities[i].parameter == parameter &&
            strlen(parser->entities[i].name) == name_size &&
            memcmp(parser->entities[i].name, name, name_size) == 0)
            return &parser->entities[i];
    }
    return NULL;
}

static int qwx_add_entity(QwXmlParser *parser, const char *name,
                          size_t name_size, const char *value,
                          size_t value_size, int parameter, int external,
                          int unparsed, const char *public_id,
                          const char *system_id, const char *base_uri,
                          const char *notation_name)
{
    QwXmlEntity *grown;
    char *entity_name;
    char *entity_value;
    size_t capacity;
    QwXmlEntity *existing = qwx_find_entity(parser, name, name_size,
                                            parameter);
    if (existing) return 1;
    if (parser->entity_count >= QW_XML_MAX_ENTITIES ||
        value_size > QW_XML_MAX_ENTITY_TABLE_BYTES - parser->entity_value_bytes) {
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML entity declaration limit exceeded");
    }
    if (parser->entity_count == parser->entity_capacity) {
        capacity = parser->entity_capacity
            ? parser->entity_capacity * 2u : 16u;
        if (capacity > QW_XML_MAX_ENTITIES) capacity = QW_XML_MAX_ENTITIES;
        grown = (QwXmlEntity *)qwx_reallocate(parser, parser->entities,
            parser->entity_capacity * sizeof(*parser->entities),
            capacity * sizeof(*parser->entities));
        if (!grown) return 0;
        parser->entities = grown;
        parser->entity_capacity = capacity;
    }
    entity_name = qwx_duplicate(parser, name, name_size);
    if (!entity_name) return 0;
    entity_value = qwx_duplicate(parser, value ? value : "", value_size);
    if (!entity_value) {
        qwx_release(parser, entity_name, name_size + 1u);
        return 0;
    }
    memset(&parser->entities[parser->entity_count], 0,
           sizeof(parser->entities[parser->entity_count]));
    parser->entities[parser->entity_count].name = entity_name;
    parser->entities[parser->entity_count].value = entity_value;
    parser->entities[parser->entity_count].value_size = value_size;
    parser->entities[parser->entity_count].parameter = parameter;
    parser->entities[parser->entity_count].external = external;
    parser->entities[parser->entity_count].unparsed = unparsed;
    parser->entities[parser->entity_count].external_markup =
        parser->in_external_subset || parser->in_parameter_entity;
    if (public_id) {
        parser->entities[parser->entity_count].public_id =
            qwx_duplicate(parser, public_id, strlen(public_id));
        if (!parser->entities[parser->entity_count].public_id) goto entity_failure;
    }
    if (system_id) {
        parser->entities[parser->entity_count].system_id =
            qwx_duplicate(parser, system_id, strlen(system_id));
        if (!parser->entities[parser->entity_count].system_id) goto entity_failure;
    }
    if (base_uri) {
        parser->entities[parser->entity_count].base_uri =
            qwx_duplicate(parser, base_uri, strlen(base_uri));
        if (!parser->entities[parser->entity_count].base_uri) goto entity_failure;
    }
    if (notation_name) {
        parser->entities[parser->entity_count].notation_name =
            qwx_duplicate(parser, notation_name, strlen(notation_name));
        if (!parser->entities[parser->entity_count].notation_name) goto entity_failure;
    }
    parser->entity_value_bytes += value_size;
    ++parser->entity_count;
    return 1;

entity_failure:
    qwx_release(parser, parser->entities[parser->entity_count].name,
                name_size + 1u);
    qwx_release(parser, parser->entities[parser->entity_count].value,
                value_size + 1u);
    qwx_release(parser, parser->entities[parser->entity_count].public_id,
        public_id ? strlen(public_id) + 1u : 0u);
    qwx_release(parser, parser->entities[parser->entity_count].system_id,
        system_id ? strlen(system_id) + 1u : 0u);
    qwx_release(parser, parser->entities[parser->entity_count].base_uri,
        base_uri ? strlen(base_uri) + 1u : 0u);
    qwx_release(parser, parser->entities[parser->entity_count].notation_name,
        notation_name ? strlen(notation_name) + 1u : 0u);
    memset(&parser->entities[parser->entity_count], 0,
           sizeof(parser->entities[parser->entity_count]));
    return 0;
}

static int qwx_append_entity_value(QwXmlParser *parser, QwXmlBuffer *buffer,
                                   const char *value, size_t value_size,
                                   unsigned depth, int attribute)
{
    size_t position = 0u;
    if (depth > QW_XML_MAX_ENTITY_DEPTH) {
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML entity nesting limit exceeded");
    }
    while (position < value_size) {
        uint8_t c = (uint8_t)value[position];
        if (c == '&') {
            size_t start = ++position;
            size_t end;
            uint32_t cp = 0u;
            if (position >= value_size) {
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "XML entity declaration contains an incomplete reference");
            }
            while (position < value_size && value[position] != ';') ++position;
            if (position >= value_size || value[position] != ';' ||
                position == start)
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "XML entity declaration has an invalid reference");
            end = position;
            if (value[start] == '#') {
                size_t cursor = start + 1u;
                unsigned base = 10u;
                if (cursor < end &&
                    value[cursor] == 'x') {
                    base = 16u;
                    ++cursor;
                }
                if (cursor == end) {
                    return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                    "XML entity contains an empty character reference");
                }
                for (; cursor < end; ++cursor) {
                    uint8_t digit_char = (uint8_t)value[cursor];
                    unsigned digit;
                    if (digit_char >= '0' && digit_char <= '9')
                        digit = digit_char - '0';
                    else if (base == 16u && digit_char >= 'a' && digit_char <= 'f')
                        digit = digit_char - 'a' + 10u;
                    else if (base == 16u && digit_char >= 'A' && digit_char <= 'F')
                        digit = digit_char - 'A' + 10u;
                    else return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                         "XML entity has an invalid character reference");
                    if (digit >= base || cp > (UINT32_MAX - digit) / base) {
                        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                        "XML entity character reference is out of range");
                    }
                    cp = cp * base + digit;
                }
                if (!qwx_entity_append_codepoint(parser, buffer, cp)) return 0;
            } else if (qwx_named_entity(value + start, end - start, &cp)) {
                if (!qwx_entity_append_codepoint(parser, buffer, cp)) return 0;
            } else {
                QwXmlEntity *entity = qwx_find_entity(parser,
                    value + start, end - start, 0);
                if (!entity)
                    return qwx_fail(parser,
                                    parser->external_subset_unread
                                        ? QW_XML_ERROR_UNSUPPORTED
                                        : QW_XML_ERROR_FORMAT,
                                    "XML entity reference is undeclared");
                if (entity->unparsed)
                    return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                    "Unparsed XML entity cannot be used in an attribute value");
                if (entity->external)
                    return qwx_fail(parser,
                        attribute ? QW_XML_ERROR_FORMAT : QW_XML_ERROR_UNSUPPORTED,
                        attribute
                            ? "External entity references are forbidden in XML attributes"
                            : "External XML entities require a resolver");
                if (!qwx_append_entity_value(parser, buffer, entity->value,
                        entity->value_size, depth + 1u, attribute)) return 0;
            }
            position = end + 1u;
            continue;
        }
        {
            size_t next = position;
            uint32_t cp;
            size_t count;
            if (!qwx_decode_utf8((const uint8_t *)value, value_size,
                                &next, &cp) || !qwx_xml_character(cp)) {
                return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                                "XML entity declaration contains invalid text");
            }
            if (cp == '<' && attribute) {
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "Internal XML entity inserts '<' into an attribute");
            }
            if (cp == '<') {
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "Markup in internal XML entities is unsupported");
            }
            if (cp == '\r') {
                if (next < value_size && value[next] == '\n') ++next;
                if (!qwx_entity_append_codepoint(parser, buffer,
                        attribute ? ' ' : '\n')) return 0;
                position = next;
                continue;
            } else if (attribute && (cp == '\n' || cp == '\t')) {
                if (!qwx_entity_append_codepoint(parser, buffer, ' ')) return 0;
                position = next;
                continue;
            } else {
                count = next - position;
            }
            if (!qwx_entity_append(parser, buffer, value + position, count))
                return 0;
            position = next;
        }
    }
    return 1;
}

static int qwx_read_dtd_literal(QwXmlParser *parser, int public_identifier,
                                char **out_value)
{
    size_t start;
    uint8_t quote;
    if (out_value) *out_value = NULL;
    if (parser->position >= parser->size ||
        (parser->input[parser->position] != '\'' &&
         parser->input[parser->position] != '"')) {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML external identifier requires a quoted literal");
    }
    quote = parser->input[parser->position++];
    start = parser->position;
    while (parser->position < parser->size &&
           parser->input[parser->position] != quote) {
        uint8_t c = parser->input[parser->position];
        if (public_identifier &&
            !(c == ' ' || c == '\r' || c == '\n' ||
              (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '\'' ||
              c == '(' || c == ')' || c == '+' || c == ',' || c == '.' ||
              c == '/' || c == ':' || c == '=' || c == '?' || c == ';' ||
              c == '!' || c == '*' || c == '#' || c == '@' || c == '$' ||
              c == '_' || c == '%')) {
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML public identifier contains an invalid character");
        }
        if (public_identifier && c >= 0x80u) {
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML public identifier must use PubidChar characters");
        }
        ++parser->position;
    }
    if (parser->position >= parser->size) {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML external identifier is not terminated");
    }
    if (out_value && public_identifier) {
        size_t end = parser->position;
        size_t cursor;
        size_t normalized_size = 0u;
        size_t written = 0u;
        int pending_space = 0;
        char *normalized;
        while (start < end && (parser->input[start] == ' ' ||
               parser->input[start] == '\r' || parser->input[start] == '\n'))
            ++start;
        while (end > start && (parser->input[end - 1u] == ' ' ||
               parser->input[end - 1u] == '\r' || parser->input[end - 1u] == '\n'))
            --end;
        for (cursor = start; cursor < end; ++cursor) {
            uint8_t c = parser->input[cursor];
            if (c == ' ' || c == '\r' || c == '\n') {
                pending_space = normalized_size != 0u;
                continue;
            }
            if (pending_space) ++normalized_size;
            ++normalized_size;
            pending_space = 0;
        }
        normalized = (char *)qwx_allocate(parser, normalized_size + 1u);
        if (!normalized) return 0;
        for (cursor = start; cursor < end; ++cursor) {
            uint8_t c = parser->input[cursor];
            if (c == ' ' || c == '\r' || c == '\n') {
                pending_space = written != 0u;
                continue;
            }
            if (pending_space) normalized[written++] = ' ';
            normalized[written++] = (char)c;
            pending_space = 0;
        }
        normalized[written] = '\0';
        *out_value = normalized;
    } else if (out_value) {
        size_t end = parser->position;
        size_t cursor = start;
        size_t normalized_size = 0u;
        size_t written = 0u;
        char *normalized;
        while (cursor < end) {
            if (parser->input[cursor++] == '\r') {
                if (cursor < end && parser->input[cursor] == '\n') ++cursor;
            }
            ++normalized_size;
        }
        normalized = (char *)qwx_allocate(parser, normalized_size + 1u);
        if (!normalized) return 0;
        for (cursor = start; cursor < end;) {
            uint8_t c = parser->input[cursor++];
            if (c == '\r') {
                if (cursor < end && parser->input[cursor] == '\n') ++cursor;
                normalized[written++] = '\n';
            } else {
                normalized[written++] = (char)c;
            }
        }
        normalized[written] = '\0';
        *out_value = normalized;
    }
    ++parser->position;
    return 1;
}

static int qwx_skip_dtd_literal(QwXmlParser *parser, int public_identifier)
{
    return qwx_read_dtd_literal(parser, public_identifier, NULL);
}

static int qwx_entity_name_valid(const uint8_t *name, size_t size)
{
    size_t position = 0u;
    int first = 1;
    if (!name || !size) return 0;
    while (position < size) {
        uint32_t cp;
        size_t next = position;
        if (!qwx_decode_utf8(name, size, &next, &cp) ||
            (first ? !qwx_name_start(cp) : !qwx_name_char(cp))) return 0;
        position = next;
        first = 0;
    }
    return 1;
}

static int qwx_expand_entity_literal(QwXmlParser *parser,
                                     const uint8_t *value, size_t value_size,
                                     QwXmlBuffer *expanded)
{
    size_t position = 0u;
    while (position < value_size) {
        uint8_t c = value[position];
        if (c == '&') {
            size_t start = ++position;
            size_t end;
            if (position >= value_size) {
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "XML entity declaration has an incomplete reference");
            }
            while (position < value_size && value[position] != ';') ++position;
            if (position >= value_size || value[position] != ';' ||
                position == start) {
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "XML entity declaration has an invalid reference");
            }
            end = position;
            if (value[start] == '#') {
                size_t cursor = start + 1u;
                unsigned base = 10u;
                uint32_t cp = 0u;
                if (cursor < end && value[cursor] == 'x') {
                    base = 16u;
                    ++cursor;
                }
                if (cursor == end) {
                    return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                    "XML entity has an empty character reference");
                }
                for (; cursor < end; ++cursor) {
                    uint8_t digit_char = value[cursor];
                    unsigned digit;
                    if (digit_char >= '0' && digit_char <= '9')
                        digit = digit_char - '0';
                    else if (base == 16u && digit_char >= 'a' && digit_char <= 'f')
                        digit = digit_char - 'a' + 10u;
                    else if (base == 16u && digit_char >= 'A' && digit_char <= 'F')
                        digit = digit_char - 'A' + 10u;
                    else return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                         "XML entity has an invalid character reference");
                    if (digit >= base || cp > (UINT32_MAX - digit) / base) {
                        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                        "XML entity character reference is out of range");
                    }
                    cp = cp * base + digit;
                }
                if (!qwx_append_codepoint(parser, expanded, cp)) return 0;
            } else {
                QwXmlEntity *entity;
                if (!qwx_entity_name_valid(value + start, end - start)) {
                    return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                    "XML entity reference has an invalid name");
                }
                entity = qwx_find_entity(parser,
                    (const char *)value + start, end - start, 0);
                if (entity && entity->unparsed) {
                    return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                    "EntityValue cannot reference an unparsed XML entity");
                }
                if (!qwx_buffer_append(parser, expanded,
                        (const char *)value + start - 1u, end - start + 2u))
                    return 0;
            }
            position = end + 1u;
            continue;
        }
    if (c == '%') {
            size_t start = position + 1u;
            size_t end = start;
            QwXmlEntity *entity;
        if (!parser->in_external_subset) {
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "Parameter entity references are not allowed inside internal-subset declarations");
        }
            while (end < value_size && value[end] != ';') ++end;
            if (end >= value_size || value[end] != ';' || end == start ||
                !qwx_entity_name_valid(value + start, end - start)) {
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "XML parameter entity reference is malformed");
            }
            entity = qwx_find_entity(parser,
                (const char *)value + start, end - start, 1);
            if (!entity) {
                return qwx_fail(parser,
                                parser->external_subset_unread
                                    ? QW_XML_ERROR_UNSUPPORTED
                                    : QW_XML_ERROR_FORMAT,
                                "XML parameter entity reference is undeclared");
            }
            if (entity->external) {
                char *external_text = NULL;
                size_t external_size = 0u;
                int expanded_ok;
                if (parser->entity_expansion_depth >= QW_XML_MAX_ENTITY_DEPTH)
                    return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                                    "XML parameter entity nesting limit exceeded");
                ++parser->entity_expansion_depth;
                if (!qwx_get_external_text(parser, entity,
                        &external_text, &external_size, NULL)) {
                    --parser->entity_expansion_depth;
                    return 0;
                }
                if (external_size > QW_XML_MAX_ENTITY_BYTES -
                                    parser->entity_expanded_bytes) {
                    qwx_release(parser, external_text, external_size + 1u);
                    --parser->entity_expansion_depth;
                    return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                                    "XML parameter entity expansion limit exceeded");
                }
                parser->entity_expanded_bytes += external_size;
                expanded_ok = qwx_expand_entity_literal(parser,
                    (const uint8_t *)external_text, external_size, expanded);
                --parser->entity_expansion_depth;
                qwx_release(parser, external_text, external_size + 1u);
                if (!expanded_ok) return 0;
                position = end + 1u;
                continue;
            }
            if (parser->entity_expansion_depth >= QW_XML_MAX_ENTITY_DEPTH ||
                entity->value_size > QW_XML_MAX_ENTITY_BYTES -
                                     parser->entity_expanded_bytes) {
                return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                                "XML parameter entity expansion limit exceeded");
            }
            parser->entity_expanded_bytes += entity->value_size;
            ++parser->entity_expansion_depth;
            if (!qwx_expand_entity_literal(parser,
                    (const uint8_t *)entity->value, entity->value_size,
                    expanded)) {
                --parser->entity_expansion_depth;
                return 0;
            }
            --parser->entity_expansion_depth;
            position = end + 1u;
            continue;
        }
        if (c == '\r') {
            ++position;
            if (position < value_size && value[position] == '\n') ++position;
            if (!qwx_buffer_append(parser, expanded, "\n", 1u)) return 0;
            continue;
        }
        {
            size_t next = position;
            uint32_t cp;
            if (!qwx_decode_utf8(value, value_size, &next, &cp) ||
                !qwx_xml_character(cp)) {
                return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                                "XML entity declaration contains invalid text");
            }
            if (!qwx_buffer_append(parser, expanded,
                    (const char *)value + position, next - position)) return 0;
            position = next;
        }
    }
    return 1;
}

static int qwx_parse_entity_declaration(QwXmlParser *parser)
{
    char *name = NULL;
    char *public_id = NULL;
    char *system_id = NULL;
    char *notation_name = NULL;
    size_t value_start;
    size_t value_size;
    uint8_t quote;
    int parameter = 0;
    int unparsed = 0;
    int ok;
    size_t before_space;
    QwXmlBuffer expanded = {0};
    parser->position += 8u; /* <!ENTITY */
    if (parser->position >= parser->size ||
        (parser->input[parser->position] != ' ' &&
         parser->input[parser->position] != '\t' &&
         parser->input[parser->position] != '\r' &&
         parser->input[parser->position] != '\n')) {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML entity declaration requires whitespace after ENTITY");
    }
    qwx_skip_space(parser);
    if (parser->position < parser->size &&
        parser->input[parser->position] == '%') {
        parameter = 1;
        ++parser->position;
        if (parser->position >= parser->size ||
            (parser->input[parser->position] != ' ' &&
             parser->input[parser->position] != '\t' &&
             parser->input[parser->position] != '\r' &&
             parser->input[parser->position] != '\n')) {
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML parameter entity name requires whitespace after '%' ");
        }
        qwx_skip_space(parser);
    }
    if (!qwx_parse_name(parser, &name)) return 0;
    before_space = parser->position;
    qwx_skip_space(parser);
    if (parser->position == before_space) {
        qwx_release(parser, name, strlen(name) + 1u);
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML entity declaration requires whitespace before its value");
    }
    if (qwx_starts(parser, "SYSTEM") || qwx_starts(parser, "PUBLIC")) {
        int is_public = qwx_starts(parser, "PUBLIC");
        parser->position += 6u;
        before_space = parser->position;
        qwx_skip_space(parser);
        if (parser->position == before_space) {
            qwx_release(parser, name, strlen(name) + 1u);
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "External XML identifier requires whitespace before its literal");
        }
        if (!qwx_read_dtd_literal(parser, is_public,
                                  is_public ? &public_id : &system_id)) {
            qwx_release(parser, name, strlen(name) + 1u);
            return 0;
        }
        if (is_public) {
            before_space = parser->position;
            qwx_skip_space(parser);
            if (parser->position == before_space) {
                qwx_release(parser, public_id,
                    public_id ? strlen(public_id) + 1u : 0u);
                qwx_release(parser, name, strlen(name) + 1u);
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "PUBLIC XML entity identifier requires a system literal");
            }
            if (!qwx_read_dtd_literal(parser, 0, &system_id)) {
                qwx_release(parser, public_id, public_id ? strlen(public_id) + 1u : 0u);
                qwx_release(parser, name, strlen(name) + 1u);
                return 0;
            }
        }
        before_space = parser->position;
        qwx_skip_space(parser);
        if (!parameter && qwx_starts(parser, "NDATA")) {
            if (parser->position == before_space) {
                qwx_release(parser, public_id, public_id ? strlen(public_id) + 1u : 0u);
                qwx_release(parser, system_id, system_id ? strlen(system_id) + 1u : 0u);
                qwx_release(parser, name, strlen(name) + 1u);
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "NDATA must be separated from the external identifier");
            }
            parser->position += 5u;
            before_space = parser->position;
            qwx_skip_space(parser);
            if (parser->position == before_space ||
                !qwx_parse_name(parser, &notation_name)) {
                qwx_release(parser, public_id, public_id ? strlen(public_id) + 1u : 0u);
                qwx_release(parser, system_id, system_id ? strlen(system_id) + 1u : 0u);
                qwx_release(parser, name, strlen(name) + 1u);
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "XML NDATA declaration requires a notation name");
            }
            unparsed = 1;
            before_space = parser->position;
            qwx_skip_space(parser);
        }
        if (parser->position >= parser->size ||
            parser->input[parser->position++] != '>') {
            qwx_release(parser, public_id, public_id ? strlen(public_id) + 1u : 0u);
            qwx_release(parser, system_id, system_id ? strlen(system_id) + 1u : 0u);
            qwx_release(parser, notation_name, notation_name ? strlen(notation_name) + 1u : 0u);
            qwx_release(parser, name, strlen(name) + 1u);
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "External XML entity declaration is malformed");
        }
        ok = qwx_add_entity(parser, name, strlen(name), "", 0u,
                            parameter, 1, unparsed, public_id, system_id,
                            parser->current_base_uri, notation_name);
        qwx_release(parser, public_id, public_id ? strlen(public_id) + 1u : 0u);
        qwx_release(parser, system_id, system_id ? strlen(system_id) + 1u : 0u);
        qwx_release(parser, notation_name, notation_name ? strlen(notation_name) + 1u : 0u);
        qwx_release(parser, name, strlen(name) + 1u);
        return ok;
    }
    if (parser->position >= parser->size ||
        (parser->input[parser->position] != '\'' &&
         parser->input[parser->position] != '"')) {
        qwx_release(parser, name, strlen(name) + 1u);
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "Internal XML entity declaration has no quoted value");
    }
    quote = parser->input[parser->position++];
    value_start = parser->position;
    while (parser->position < parser->size &&
           parser->input[parser->position] != quote) ++parser->position;
    if (parser->position >= parser->size) {
        qwx_release(parser, name, strlen(name) + 1u);
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "Internal XML entity declaration is not terminated");
    }
    value_size = parser->position - value_start;
    ++parser->position;
    qwx_skip_space(parser);
    if (parser->position >= parser->size ||
        parser->input[parser->position] != '>') {
        qwx_release(parser, name, strlen(name) + 1u);
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "Internal XML entity declaration has trailing syntax");
    }
    ++parser->position;
    if (!qwx_expand_entity_literal(parser,
            parser->input + value_start, value_size, &expanded)) {
        qwx_release(parser, name, strlen(name) + 1u);
        qwx_release(parser, expanded.data, expanded.capacity);
        return 0;
    }
    ok = qwx_add_entity(parser, name, strlen(name), expanded.data,
                        expanded.size, parameter, 0, 0, NULL, NULL,
                        parser->current_base_uri, NULL);
    qwx_release(parser, name, strlen(name) + 1u);
    qwx_release(parser, expanded.data, expanded.capacity);
    return ok;
}

static int qwx_parse_parameter_entity_reference(QwXmlParser *parser)
{
    char *name = NULL;
    QwXmlEntity *entity;
    size_t name_size;
    if (parser->input[parser->position] != '%') return 0;
    ++parser->position;
    if (!qwx_parse_name(parser, &name)) return 0;
    name_size = strlen(name);
    if (parser->position >= parser->size ||
        parser->input[parser->position] != ';') {
        qwx_release(parser, name, name_size + 1u);
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML parameter entity reference is missing ';'");
    }
    entity = qwx_find_entity(parser, name, name_size, 1);
    qwx_release(parser, name, name_size + 1u);
    if (!entity) {
        return qwx_fail(parser,
                        parser->external_subset_unread
                            ? QW_XML_ERROR_UNSUPPORTED
                            : QW_XML_ERROR_FORMAT,
                        "XML parameter entity reference is undeclared");
    }
    if (entity->external) {
        ++parser->position;
        if (parser->options && parser->options->resolve_entity) {
            int saved_in_parameter_entity = parser->in_parameter_entity;
            int ok;
            parser->in_parameter_entity = 1;
            ok = qwx_parse_external_resource(parser, entity->public_id,
                entity->system_id, entity->base_uri,
                QW_XML_EXTERNAL_DTD);
            parser->in_parameter_entity = saved_in_parameter_entity;
            return ok;
        }
        return qwx_fail(parser, QW_XML_ERROR_UNSUPPORTED,
                        "External XML parameter entities require a resolver");
    }
    if (parser->entity_expansion_depth >= QW_XML_MAX_ENTITY_DEPTH ||
        parser->entity_expanded_bytes > QW_XML_MAX_ENTITY_BYTES - 2u ||
        entity->value_size > QW_XML_MAX_ENTITY_BYTES - 2u -
                             parser->entity_expanded_bytes ||
        entity->value_size > SIZE_MAX - 2u) {
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML parameter entity expansion limit exceeded");
    }
    ++parser->position;
    {
        size_t expanded_size = entity->value_size + 2u;
        uint8_t *expanded_input = (uint8_t *)qwx_allocate(parser,
                                                          expanded_size);
        const uint8_t *saved_input = parser->input;
        const uint32_t *saved_input_origins = parser->input_origin_ids;
        size_t saved_size = parser->size;
        size_t saved_position = parser->position;
        uint32_t saved_input_origin_id = parser->input_origin_id;
        int saved_in_parameter_entity = parser->in_parameter_entity;
        int ok;
        if (!expanded_input) return 0;
        expanded_input[0] = ' ';
        if (entity->value_size)
            memcpy(expanded_input + 1u, entity->value, entity->value_size);
        expanded_input[expanded_size - 1u] = ' ';
        parser->entity_expanded_bytes += expanded_size;
        ++parser->entity_expansion_depth;
        parser->input_origin_ids = NULL;
        parser->input_origin_id = qwx_next_parameter_entity_origin(parser);
        if (!parser->input_origin_id) {
            parser->input_origin_id = saved_input_origin_id;
            parser->input_origin_ids = saved_input_origins;
            --parser->entity_expansion_depth;
            qwx_release(parser, expanded_input, expanded_size);
            return 0;
        }
        parser->input = expanded_input;
        parser->size = expanded_size;
        parser->position = 0u;
        parser->in_parameter_entity = 1;
        ok = qwx_parse_dtd_subset(parser, 0);
        parser->input = saved_input;
        parser->input_origin_ids = saved_input_origins;
        parser->input_origin_id = saved_input_origin_id;
        parser->size = saved_size;
        parser->position = saved_position;
        parser->in_parameter_entity = saved_in_parameter_entity;
        --parser->entity_expansion_depth;
        qwx_release(parser, expanded_input, expanded_size);
        if (!ok) return 0;
    }
    return 1;
}

static int qwx_require_dtd_space(QwXmlParser *parser, const char *message)
{
    size_t before = parser->position;
    qwx_skip_space(parser);
    if (parser->position == before)
        return qwx_fail(parser, QW_XML_ERROR_FORMAT, message);
    return 1;
}

static int qwx_parse_dtd_nmtoken(QwXmlParser *parser)
{
    size_t start = parser->position;
    while (parser->position < parser->size) {
        size_t next = parser->position;
        uint32_t cp;
        if (!qwx_decode_utf8(parser->input, parser->size, &next, &cp)) {
            return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                            "DTD Nmtoken contains invalid UTF-8");
        }
        if (!qwx_name_char(cp)) break;
        parser->position = next;
    }
    if (parser->position == start)
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "DTD Nmtoken is missing or invalid");
    return 1;
}

static int qwx_parse_dtd_enumeration(QwXmlParser *parser, int names_only,
                                     char **out_values)
{
    size_t count = 0u;
    QwXmlBuffer values = {0};
    if (out_values) *out_values = NULL;
    if (parser->position >= parser->size ||
        parser->input[parser->position++] != '(') {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "DTD enumeration must begin with '('");
    }
    qwx_skip_space(parser);
    for (;;) {
        size_t item_start = parser->position;
        size_t item_size;
        if (names_only ? !qwx_parse_name(parser, NULL)
                       : !qwx_parse_dtd_nmtoken(parser)) {
            qwx_release(parser, values.data, values.capacity);
            return 0;
        }
        item_size = parser->position - item_start;
        if (out_values) {
            size_t cursor = 0u;
            while (cursor < values.size) {
                size_t start;
                size_t end;
                while (cursor < values.size && values.data[cursor] != '|') ++cursor;
                if (cursor >= values.size) break;
                start = ++cursor;
                while (cursor < values.size && values.data[cursor] != '|') ++cursor;
                end = cursor;
                if (end - start == item_size &&
                    memcmp(values.data + start, parser->input + item_start,
                           item_size) == 0 && parser->validate_dtd) {
                    qwx_release(parser, values.data, values.capacity);
                    return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                                    "DTD enumeration contains a duplicate token");
                }
            }
            if (!qwx_buffer_append(parser, &values, "|", 1u) ||
                !qwx_buffer_append(parser, &values,
                    (const char *)parser->input + item_start, item_size) ||
                !qwx_buffer_append(parser, &values, "|", 1u)) {
                qwx_release(parser, values.data, values.capacity);
                return 0;
            }
        }
        ++count;
        qwx_skip_space(parser);
        if (parser->position >= parser->size)
            goto unterminated;
        if (parser->input[parser->position] == ')') {
            ++parser->position;
            if (!count) {
                qwx_release(parser, values.data, values.capacity);
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "DTD enumeration cannot be empty");
            }
            if (out_values) *out_values = values.data;
            else qwx_release(parser, values.data, values.capacity);
            return 1;
        }
        if (parser->input[parser->position++] != '|') {
            qwx_release(parser, values.data, values.capacity);
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "DTD enumeration items must be separated by '|'");
        }
        qwx_skip_space(parser);
    }

unterminated:
    qwx_release(parser, values.data, values.capacity);
    return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                    "DTD enumeration is not terminated");
}

static int qwx_parse_dtd_attribute_type(QwXmlParser *parser,
                                        QwXmlAttributeType *out_type,
                                        char **out_enumeration)
{
    char *type = NULL;
    size_t start = parser->position;
    if (out_enumeration) *out_enumeration = NULL;
    if (parser->position >= parser->size) {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "DTD attribute type is missing");
    }
    if (parser->input[parser->position] == '(') {
        *out_type = QW_XML_ATTR_ENUMERATION;
        return qwx_parse_dtd_enumeration(parser, 0, out_enumeration);
    }
    if (!qwx_parse_name(parser, &type)) return 0;
    if (strcmp(type, "CDATA") == 0) {
        *out_type = QW_XML_ATTR_CDATA;
    } else if (strcmp(type, "ID") == 0) {
        *out_type = QW_XML_ATTR_ID;
    } else if (strcmp(type, "IDREF") == 0) {
        *out_type = QW_XML_ATTR_IDREF;
    } else if (strcmp(type, "IDREFS") == 0) {
        *out_type = QW_XML_ATTR_IDREFS;
    } else if (strcmp(type, "ENTITY") == 0) {
        *out_type = QW_XML_ATTR_ENTITY;
    } else if (strcmp(type, "ENTITIES") == 0) {
        *out_type = QW_XML_ATTR_ENTITIES;
    } else if (strcmp(type, "NMTOKEN") == 0) {
        *out_type = QW_XML_ATTR_NMTOKEN;
    } else if (strcmp(type, "NMTOKENS") == 0) {
        *out_type = QW_XML_ATTR_NMTOKENS;
    } else if (strcmp(type, "NOTATION") == 0) {
        qwx_release(parser, type, strlen(type) + 1u);
        if (!qwx_require_dtd_space(parser,
                "DTD NOTATION type requires whitespace before its enumeration"))
            return 0;
        *out_type = QW_XML_ATTR_NOTATION;
        return qwx_parse_dtd_enumeration(parser, 1, out_enumeration);
    } else {
        qwx_release(parser, type, strlen(type) + 1u);
        parser->position = start;
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "DTD attribute type is unknown");
    }
    qwx_release(parser, type, strlen(type) + 1u);
    return 1;
}

static int qwx_parse_dtd_qname(QwXmlParser *parser)
{
    char *name = NULL;
    int valid;
    if (!qwx_parse_name(parser, &name)) return 0;
    valid = qwx_qname_parts(name, NULL, NULL);
    qwx_release(parser, name, strlen(name) + 1u);
    if (!valid)
        return qwx_fail(parser, QW_XML_ERROR_NAMESPACE,
                        "DTD content model name is not a namespace QName");
    return 1;
}

static int qwx_parse_dtd_particle(QwXmlParser *parser, unsigned depth);

static int qwx_parse_dtd_group(QwXmlParser *parser, unsigned depth)
{
    uint8_t separator = 0u;
    size_t count = 0u;
    uint32_t opening_origin;
    if (depth >= QW_XML_MAX_DEPTH || parser->position >= parser->size ||
        parser->input[parser->position] != '(') {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "DTD content model group is invalid or too deep");
    }
    opening_origin = qwx_input_origin(parser, parser->position++);
    qwx_skip_space(parser);
    if (!qwx_parse_dtd_particle(parser, depth + 1u)) return 0;
    ++count;
    qwx_skip_space(parser);
    while (parser->position < parser->size &&
           (parser->input[parser->position] == ',' ||
            parser->input[parser->position] == '|')) {
        uint8_t current_separator = parser->input[parser->position++];
        if (separator && current_separator != separator)
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "DTD content model mixes ',' and '|' in one group");
        separator = current_separator;
        qwx_skip_space(parser);
        if (!qwx_parse_dtd_particle(parser, depth + 1u)) return 0;
        ++count;
        qwx_skip_space(parser);
    }
    if (count < 2u || parser->position >= parser->size ||
        parser->input[parser->position] != ')') {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "DTD choice or sequence requires at least two particles and a closing ')'");
    }
    if (parser->validate_dtd && opening_origin !=
        qwx_input_origin(parser, parser->position))
        return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                        "DTD content group crosses a parameter-entity boundary");
    ++parser->position;
    return 1;
}

static int qwx_parse_dtd_particle(QwXmlParser *parser, unsigned depth)
{
    if (depth >= QW_XML_MAX_DEPTH)
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "DTD content model nesting limit exceeded");
    if (parser->position < parser->size &&
        parser->input[parser->position] == '(') {
        if (!qwx_parse_dtd_group(parser, depth + 1u)) return 0;
    } else if (!qwx_parse_dtd_qname(parser)) {
        return 0;
    }
    if (parser->position < parser->size &&
        (parser->input[parser->position] == '?' ||
         parser->input[parser->position] == '*' ||
         parser->input[parser->position] == '+')) ++parser->position;
    return 1;
}

static int qwx_parse_dtd_content_spec(QwXmlParser *parser)
{
    if (qwx_starts(parser, "EMPTY") &&
        (parser->position + 5u == parser->size ||
         parser->input[parser->position + 5u] == ' ' ||
         parser->input[parser->position + 5u] == '\t' ||
         parser->input[parser->position + 5u] == '\r' ||
         parser->input[parser->position + 5u] == '\n' ||
         parser->input[parser->position + 5u] == '>')) {
        parser->position += 5u;
        return 1;
    }
    if (qwx_starts(parser, "ANY") &&
        (parser->position + 3u == parser->size ||
         parser->input[parser->position + 3u] == ' ' ||
         parser->input[parser->position + 3u] == '\t' ||
         parser->input[parser->position + 3u] == '\r' ||
         parser->input[parser->position + 3u] == '\n' ||
         parser->input[parser->position + 3u] == '>')) {
        parser->position += 3u;
        return 1;
    }
    if (parser->position < parser->size &&
        parser->input[parser->position] == '(') {
        size_t start = parser->position;
        uint32_t opening_origin = qwx_input_origin(parser, start);
        ++parser->position;
        qwx_skip_space(parser);
        if (qwx_starts(parser, "#PCDATA")) {
            int has_names = 0;
            parser->position += 7u;
            qwx_skip_space(parser);
            while (parser->position < parser->size &&
                   parser->input[parser->position] == '|') {
                ++parser->position;
                qwx_skip_space(parser);
                if (!qwx_parse_dtd_qname(parser)) return 0;
                has_names = 1;
                qwx_skip_space(parser);
            }
            if (parser->position >= parser->size ||
                parser->input[parser->position] != ')') {
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "DTD mixed content model is not terminated");
            }
            if (parser->validate_dtd && opening_origin !=
                qwx_input_origin(parser, parser->position))
                return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                                "DTD mixed content model crosses a parameter-entity boundary");
            ++parser->position;
            if (has_names) {
                if (parser->position >= parser->size ||
                    parser->input[parser->position++] != '*') {
                    return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                    "DTD mixed content with names must end in '*'");
                }
            } else if (parser->position < parser->size &&
                       (parser->input[parser->position] == '?' ||
                        parser->input[parser->position] == '*' ||
                        parser->input[parser->position] == '+')) {
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "DTD (#PCDATA) content model cannot have an occurrence suffix");
            }
            return 1;
        }
        parser->position = start;
        if (!qwx_parse_dtd_group(parser, 0u)) return 0;
        if (parser->position < parser->size &&
            (parser->input[parser->position] == '?' ||
             parser->input[parser->position] == '*' ||
             parser->input[parser->position] == '+')) ++parser->position;
        return 1;
    }
    return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                    "DTD element declaration has an invalid content specification");
}

static QwXmlElementDecl *qwx_find_element_decl(QwXmlParser *parser,
                                               const char *name)
{
    size_t i;
    for (i = 0u; i < parser->element_decl_count; ++i)
        if (strcmp(parser->element_decls[i].name, name) == 0)
            return &parser->element_decls[i];
    return NULL;
}

static int qwx_add_element_decl(QwXmlParser *parser, const char *name,
                                const char *model, size_t model_size,
                                QwXmlContentKind kind)
{
    QwXmlElementDecl *grown;
    size_t capacity;
    QwXmlElementDecl *existing = qwx_find_element_decl(parser, name);
    if (existing) {
        if (parser->validate_dtd)
            return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                            "DTD declares an element type more than once");
        return 1;
    }
    if (parser->element_decl_count >= QW_XML_MAX_ELEMENT_DECLS ||
        model_size > QW_XML_MAX_ENTITY_TABLE_BYTES) {
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML element declaration limit exceeded");
    }
    if (parser->element_decl_count == parser->element_decl_capacity) {
        capacity = parser->element_decl_capacity
            ? parser->element_decl_capacity * 2u : 16u;
        if (capacity > QW_XML_MAX_ELEMENT_DECLS)
            capacity = QW_XML_MAX_ELEMENT_DECLS;
        grown = (QwXmlElementDecl *)qwx_reallocate(parser,
            parser->element_decls,
            parser->element_decl_capacity * sizeof(*parser->element_decls),
            capacity * sizeof(*parser->element_decls));
        if (!grown) return 0;
        parser->element_decls = grown;
        parser->element_decl_capacity = capacity;
    }
    grown = &parser->element_decls[parser->element_decl_count];
    memset(grown, 0, sizeof(*grown));
    grown->name = qwx_duplicate(parser, name, strlen(name));
    grown->content_model = qwx_duplicate(parser, model, model_size);
    if (!grown->name || !grown->content_model) {
        qwx_release(parser, grown->name,
                    grown->name ? strlen(name) + 1u : 0u);
        qwx_release(parser, grown->content_model,
                    grown->content_model ? model_size + 1u : 0u);
        memset(grown, 0, sizeof(*grown));
        return 0;
    }
    grown->content_kind = kind;
    grown->external_markup =
        parser->in_external_subset || parser->in_parameter_entity;
    ++parser->element_decl_count;
    return 1;
}

static int qwx_parse_element_declaration(QwXmlParser *parser)
{
    char *name = NULL;
    size_t model_start;
    QwXmlContentKind kind;
    parser->position += 9u; /* <!ELEMENT */
    if (!qwx_require_dtd_space(parser,
            "DTD ELEMENT declaration requires whitespace after its keyword") ||
        !qwx_parse_name(parser, &name)) {
        qwx_release(parser, name, name ? strlen(name) + 1u : 0u);
        return 0;
    }
    if (!qwx_qname_parts(name, NULL, NULL)) {
        qwx_release(parser, name, strlen(name) + 1u);
        return qwx_fail(parser, QW_XML_ERROR_NAMESPACE,
                        "DTD element type name is not a namespace QName");
    }
    if (!qwx_require_dtd_space(parser,
            "DTD element name must be separated from its content model")) {
        qwx_release(parser, name, strlen(name) + 1u);
        return 0;
    }
    model_start = parser->position;
    if (qwx_starts(parser, "EMPTY")) kind = QW_XML_CONTENT_EMPTY;
    else if (qwx_starts(parser, "ANY")) kind = QW_XML_CONTENT_ANY;
    else {
        size_t cursor = parser->position;
        if (cursor < parser->size && parser->input[cursor] == '(') {
            ++cursor;
            while (cursor < parser->size &&
                   (parser->input[cursor] == ' ' || parser->input[cursor] == '\t' ||
                    parser->input[cursor] == '\r' || parser->input[cursor] == '\n'))
                ++cursor;
        }
        kind = cursor + 7u <= parser->size &&
               memcmp(parser->input + cursor, "#PCDATA", 7u) == 0
            ? QW_XML_CONTENT_MIXED : QW_XML_CONTENT_CHILDREN;
    }
    if (!qwx_parse_dtd_content_spec(parser)) {
        qwx_release(parser, name, strlen(name) + 1u);
        return 0;
    }
    if (!qwx_add_element_decl(parser, name,
            (const char *)parser->input + model_start,
            parser->position - model_start, kind)) {
        qwx_release(parser, name, strlen(name) + 1u);
        return 0;
    }
    qwx_skip_space(parser);
    if (parser->position >= parser->size ||
        parser->input[parser->position++] != '>') {
        qwx_release(parser, name, strlen(name) + 1u);
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "DTD ELEMENT declaration is malformed");
    }
    qwx_release(parser, name, strlen(name) + 1u);
    return 1;
}

static QwXmlAttributeDecl *qwx_find_attribute_decl(QwXmlParser *parser,
                                                   const char *element_name,
                                                   const char *name)
{
    size_t i;
    for (i = 0u; i < parser->attribute_decl_count; ++i) {
        QwXmlAttributeDecl *decl = &parser->attribute_decls[i];
        if (strcmp(decl->element_name, element_name) == 0 &&
            strcmp(decl->name, name) == 0) return decl;
    }
    return NULL;
}

static int qwx_add_attribute_decl(QwXmlParser *parser,
                                  const char *element_name,
                                  const char *name, char *default_value,
                                  QwXmlAttributeType type,
                                  char *enumeration,
                                  QwXmlDefaultKind default_kind)
{
    QwXmlAttributeDecl *grown;
    QwXmlAttributeDecl *existing = qwx_find_attribute_decl(parser,
        element_name, name);
    size_t capacity;
    size_t element_size = strlen(element_name) + 1u;
    size_t name_size = strlen(name) + 1u;
    if (existing) {
        qwx_release(parser, default_value,
            default_value ? strlen(default_value) + 1u : 0u);
        qwx_release(parser, enumeration,
            enumeration ? strlen(enumeration) + 1u : 0u);
        return 1;
    }
    if (parser->attribute_decl_count >= QW_XML_MAX_ATTRIBUTE_DECLS) {
        qwx_release(parser, default_value,
            default_value ? strlen(default_value) + 1u : 0u);
        qwx_release(parser, enumeration,
            enumeration ? strlen(enumeration) + 1u : 0u);
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML attribute declaration limit exceeded");
    }
    if (parser->attribute_decl_count == parser->attribute_decl_capacity) {
        capacity = parser->attribute_decl_capacity
            ? parser->attribute_decl_capacity * 2u : 16u;
        if (capacity > QW_XML_MAX_ATTRIBUTE_DECLS)
            capacity = QW_XML_MAX_ATTRIBUTE_DECLS;
        grown = (QwXmlAttributeDecl *)qwx_reallocate(parser,
            parser->attribute_decls,
            parser->attribute_decl_capacity * sizeof(*parser->attribute_decls),
            capacity * sizeof(*parser->attribute_decls));
        if (!grown) {
            qwx_release(parser, default_value,
                default_value ? strlen(default_value) + 1u : 0u);
            qwx_release(parser, enumeration,
                enumeration ? strlen(enumeration) + 1u : 0u);
            return 0;
        }
        parser->attribute_decls = grown;
        parser->attribute_decl_capacity = capacity;
    }
    grown = &parser->attribute_decls[parser->attribute_decl_count];
    grown->element_name = qwx_duplicate(parser, element_name, element_size - 1u);
    grown->name = qwx_duplicate(parser, name, name_size - 1u);
    if (!grown->element_name || !grown->name) {
        qwx_release(parser, grown->element_name,
            grown->element_name ? element_size : 0u);
        qwx_release(parser, grown->name, grown->name ? name_size : 0u);
        qwx_release(parser, default_value,
            default_value ? strlen(default_value) + 1u : 0u);
        qwx_release(parser, enumeration,
            enumeration ? strlen(enumeration) + 1u : 0u);
        return 0;
    }
    grown->default_value = default_value;
    grown->enumeration = enumeration;
    grown->type = type;
    grown->tokenized = type != QW_XML_ATTR_CDATA;
    grown->default_kind = default_kind;
    grown->external_markup =
        parser->in_external_subset || parser->in_parameter_entity;
    ++parser->attribute_decl_count;
    return 1;
}

static int qwx_normalize_tokenized_attribute(QwXmlParser *parser,
                                            char **value)
{
    char *original = *value;
    char *read = original;
    char *write = original;
    size_t original_size = strlen(original);
    size_t normalized_size;
    int pending_space = 0;
    int has_content = 0;
    while (*read) {
        unsigned char c = (unsigned char)*read++;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (has_content) pending_space = 1;
            continue;
        }
        if (pending_space) *write++ = ' ';
        pending_space = 0;
        has_content = 1;
        *write++ = (char)c;
    }
    *write = '\0';
    normalized_size = (size_t)(write - original);
    if (normalized_size != original_size) {
        char *exact = (char *)qwx_reallocate(parser, original,
            original_size + 1u, normalized_size + 1u);
        if (!exact) {
            qwx_release(parser, original, original_size + 1u);
            *value = NULL;
            return 0;
        }
        *value = exact;
    }
    return 1;
}

static int qwx_parse_attribute_list_declaration(QwXmlParser *parser)
{
    char *element_name = NULL;
    parser->position += 9u; /* <!ATTLIST */
    if (!qwx_require_dtd_space(parser,
            "DTD ATTLIST declaration requires whitespace after its keyword") ||
        !qwx_parse_name(parser, &element_name)) {
        qwx_release(parser, element_name,
            element_name ? strlen(element_name) + 1u : 0u);
        return 0;
    }
    if (!qwx_qname_parts(element_name, NULL, NULL)) {
        qwx_release(parser, element_name, strlen(element_name) + 1u);
        return qwx_fail(parser, QW_XML_ERROR_NAMESPACE,
                        "DTD attribute-list element name is not a namespace QName");
    }
    for (;;) {
        size_t before_space = parser->position;
        char *name = NULL;
        char *default_value = NULL;
        char *enumeration = NULL;
        QwXmlAttributeType type;
        QwXmlDefaultKind default_kind;
        qwx_skip_space(parser);
        if (parser->position >= parser->size) {
            qwx_release(parser, element_name, strlen(element_name) + 1u);
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "DTD ATTLIST declaration is not terminated");
        }
        if (parser->input[parser->position] == '>') {
            ++parser->position;
            qwx_release(parser, element_name, strlen(element_name) + 1u);
            return 1;
        }
        if (parser->position == before_space) {
            qwx_release(parser, element_name, strlen(element_name) + 1u);
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "DTD attribute definitions must be separated by whitespace");
        }
        if (!qwx_parse_name(parser, &name) ||
            !qwx_qname_parts(name, NULL, NULL) ||
            !qwx_require_dtd_space(parser,
                "DTD attribute name must be separated from its type") ||
            !qwx_parse_dtd_attribute_type(parser, &type, &enumeration) ||
            !qwx_require_dtd_space(parser,
                "DTD attribute type must be separated from its default declaration")) {
            qwx_release(parser, name, name ? strlen(name) + 1u : 0u);
            qwx_release(parser, enumeration,
                enumeration ? strlen(enumeration) + 1u : 0u);
            qwx_release(parser, element_name, strlen(element_name) + 1u);
            return 0;
        }
        if (qwx_starts(parser, "#REQUIRED") &&
            (parser->position + 9u == parser->size ||
             parser->input[parser->position + 9u] == ' ' ||
             parser->input[parser->position + 9u] == '\t' ||
             parser->input[parser->position + 9u] == '\r' ||
             parser->input[parser->position + 9u] == '\n' ||
             parser->input[parser->position + 9u] == '>')) {
            parser->position += 9u;
            default_kind = QW_XML_DEFAULT_REQUIRED;
        } else if (qwx_starts(parser, "#IMPLIED") &&
            (parser->position + 8u == parser->size ||
             parser->input[parser->position + 8u] == ' ' ||
             parser->input[parser->position + 8u] == '\t' ||
             parser->input[parser->position + 8u] == '\r' ||
             parser->input[parser->position + 8u] == '\n' ||
             parser->input[parser->position + 8u] == '>')) {
            parser->position += 8u;
            default_kind = QW_XML_DEFAULT_IMPLIED;
        } else if (qwx_starts(parser, "#FIXED")) {
            default_kind = QW_XML_DEFAULT_FIXED;
            parser->position += 6u;
            if (!qwx_require_dtd_space(parser,
                    "DTD #FIXED requires whitespace before its value") ||
                parser->position >= parser->size ||
                (parser->input[parser->position] != '\'' &&
                 parser->input[parser->position] != '"')) {
                qwx_release(parser, name, strlen(name) + 1u);
                qwx_release(parser, enumeration,
                    enumeration ? strlen(enumeration) + 1u : 0u);
                qwx_release(parser, element_name, strlen(element_name) + 1u);
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "DTD #FIXED declaration requires a quoted value");
            }
            {
                uint8_t quote = parser->input[parser->position++];
                if (!qwx_parse_value(parser, quote, 1, &default_value)) {
                    qwx_release(parser, name, strlen(name) + 1u);
                    qwx_release(parser, enumeration,
                        enumeration ? strlen(enumeration) + 1u : 0u);
                    qwx_release(parser, element_name, strlen(element_name) + 1u);
                    return 0;
                }
            }
        } else if (parser->position < parser->size &&
                   (parser->input[parser->position] == '\'' ||
                    parser->input[parser->position] == '"')) {
            default_kind = QW_XML_DEFAULT_VALUE;
            uint8_t quote = parser->input[parser->position++];
            if (!qwx_parse_value(parser, quote, 1, &default_value)) {
                qwx_release(parser, name, strlen(name) + 1u);
                qwx_release(parser, enumeration,
                    enumeration ? strlen(enumeration) + 1u : 0u);
                qwx_release(parser, element_name, strlen(element_name) + 1u);
                return 0;
            }
        } else {
            qwx_release(parser, name, strlen(name) + 1u);
            qwx_release(parser, enumeration,
                enumeration ? strlen(enumeration) + 1u : 0u);
            qwx_release(parser, element_name, strlen(element_name) + 1u);
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "DTD attribute default declaration is invalid");
        }
        if (default_value && type != QW_XML_ATTR_CDATA &&
            !qwx_normalize_tokenized_attribute(parser, &default_value)) {
            qwx_release(parser, name, strlen(name) + 1u);
            qwx_release(parser, enumeration,
                enumeration ? strlen(enumeration) + 1u : 0u);
            qwx_release(parser, element_name, strlen(element_name) + 1u);
            return 0;
        }
        if (!qwx_add_attribute_decl(parser, element_name, name,
                                    default_value, type, enumeration,
                                    default_kind)) {
            qwx_release(parser, name, strlen(name) + 1u);
            qwx_release(parser, element_name, strlen(element_name) + 1u);
            return 0;
        }
        qwx_release(parser, name, strlen(name) + 1u);
    }
}

static int qwx_add_notation_decl(QwXmlParser *parser, const char *name)
{
    QwXmlNotationDecl *grown;
    size_t i;
    size_t capacity;
    for (i = 0u; i < parser->notation_decl_count; ++i) {
        if (strcmp(parser->notation_decls[i].name, name) == 0) {
            if (parser->validate_dtd)
                return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                                "DTD declares a notation more than once");
            return 1;
        }
    }
    if (parser->notation_decl_count >= QW_XML_MAX_NOTATION_DECLS)
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML notation declaration limit exceeded");
    if (parser->notation_decl_count == parser->notation_decl_capacity) {
        capacity = parser->notation_decl_capacity
            ? parser->notation_decl_capacity * 2u : 8u;
        if (capacity > QW_XML_MAX_NOTATION_DECLS)
            capacity = QW_XML_MAX_NOTATION_DECLS;
        grown = (QwXmlNotationDecl *)qwx_reallocate(parser,
            parser->notation_decls,
            parser->notation_decl_capacity * sizeof(*parser->notation_decls),
            capacity * sizeof(*parser->notation_decls));
        if (!grown) return 0;
        parser->notation_decls = grown;
        parser->notation_decl_capacity = capacity;
    }
    grown = &parser->notation_decls[parser->notation_decl_count];
    grown->name = qwx_duplicate(parser, name, strlen(name));
    if (!grown->name) return 0;
    ++parser->notation_decl_count;
    return 1;
}

static int qwx_parse_notation_declaration(QwXmlParser *parser)
{
    char *notation_name = NULL;
    parser->position += 10u; /* <!NOTATION */
    if (!qwx_require_dtd_space(parser,
            "DTD NOTATION declaration requires whitespace after its keyword") ||
        !qwx_parse_name(parser, &notation_name) ||
        !qwx_require_dtd_space(parser,
            "DTD notation name must be separated from its identifier")) {
        qwx_release(parser, notation_name,
            notation_name ? strlen(notation_name) + 1u : 0u);
        return 0;
    }
    if (qwx_starts(parser, "SYSTEM")) {
        parser->position += 6u;
        if (!qwx_require_dtd_space(parser,
                "SYSTEM identifier requires whitespace before its literal") ||
            !qwx_skip_dtd_literal(parser, 0)) goto failure;
    } else if (qwx_starts(parser, "PUBLIC")) {
        parser->position += 6u;
        if (!qwx_require_dtd_space(parser,
                "PUBLIC identifier requires whitespace before its literal") ||
            !qwx_skip_dtd_literal(parser, 1)) goto failure;
        {
            size_t before_space = parser->position;
            qwx_skip_space(parser);
            if (parser->position != before_space &&
                parser->position < parser->size &&
                (parser->input[parser->position] == '\'' ||
                 parser->input[parser->position] == '"')) {
                if (!qwx_skip_dtd_literal(parser, 0)) goto failure;
            }
        }
    } else {
        qwx_fail(parser, QW_XML_ERROR_FORMAT,
                 "DTD NOTATION declaration requires an external identifier");
        goto failure;
    }
    qwx_skip_space(parser);
    if (parser->position >= parser->size ||
        parser->input[parser->position++] != '>') {
        qwx_fail(parser, QW_XML_ERROR_FORMAT,
                 "DTD NOTATION declaration is malformed");
        goto failure;
    }
    if (!qwx_add_notation_decl(parser, notation_name)) goto failure;
    qwx_release(parser, notation_name, strlen(notation_name) + 1u);
    return 1;

failure:
    qwx_release(parser, notation_name,
                notation_name ? strlen(notation_name) + 1u : 0u);
    return 0;
}

static int qwx_skip_conditional_ignore_body(QwXmlParser *parser,
                                           uint32_t opening_origin)
{
    size_t nesting = 1u;
    uint32_t origins[QW_XML_MAX_DEPTH];
    qwx_skip_space(parser);
    if (parser->position >= parser->size ||
        parser->input[parser->position] != '[')
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "IGNORE conditional section is missing '['");
    if (!qwx_require_origin(parser, opening_origin, parser->position, 1u,
            "Conditional-section delimiters cross a parameter-entity boundary"))
        return 0;
    origins[0] = opening_origin;
    ++parser->position;
    while (parser->position < parser->size) {
        if (qwx_starts(parser, "<![")) {
            uint32_t nested_origin = qwx_input_origin(parser, parser->position);
            if (nesting >= QW_XML_MAX_DEPTH)
                return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                                "XML conditional-section nesting limit exceeded");
            if (!qwx_require_origin(parser, nested_origin, parser->position,
                    3u, "Conditional-section delimiter crosses a parameter-entity boundary"))
                return 0;
            origins[nesting] = nested_origin;
            ++nesting;
            parser->position += 3u;
        } else if (qwx_starts(parser, "]]>")) {
            uint32_t closing_origin = origins[nesting - 1u];
            if (!qwx_require_origin(parser, closing_origin, parser->position,
                    3u, "Conditional-section delimiters cross a parameter-entity boundary"))
                return 0;
            parser->position += 3u;
            if (--nesting == 0u) return 1;
        } else {
            ++parser->position;
        }
    }
    return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                    "IGNORE conditional section is not terminated");
}

static int qwx_skip_conditional_ignore(QwXmlParser *parser,
                                      uint32_t opening_origin)
{
    if (!qwx_starts(parser, "IGNORE") ||
        (parser->position + 6u < parser->size &&
         qwx_name_char(parser->input[parser->position + 6u])))
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "Conditional section keyword is invalid");
    parser->position += 6u;
    return qwx_skip_conditional_ignore_body(parser, opening_origin);
}

static int qwx_expand_dtd_fragment(QwXmlParser *parser,
                                   const uint8_t *data, size_t size,
                                   QwXmlBuffer *output,
                                   QwXmlOriginMap *origins,
                                   const uint32_t *data_origins,
                                   uint32_t data_origin_id, unsigned depth,
                                   const char **out_base_uri);
static int qwx_parse_name(QwXmlParser *parser, char **out_name);

static int qwx_parse_conditional_keyword_pe(QwXmlParser *parser,
                                            int *out_include)
{
    char *name = NULL;
    char *external_text = NULL;
    size_t external_size = 0u;
    QwXmlEntity *entity;
    const char *replacement;
    size_t replacement_size;
    QwXmlBuffer expanded = {0};
    size_t start;
    size_t end;
    int ok = 0;
    if (parser->input[parser->position++] != '%' ||
        !qwx_parse_name(parser, &name)) goto malformed;
    if (parser->position >= parser->size || parser->input[parser->position] != ';')
        goto malformed;
    ++parser->position;
    entity = qwx_find_entity(parser, name, strlen(name), 1);
    if (!entity) {
        qwx_fail(parser,
            parser->external_subset_unread
                ? QW_XML_ERROR_UNSUPPORTED : QW_XML_ERROR_FORMAT,
            "Conditional-section parameter entity is undeclared");
        goto cleanup;
    }
    if (parser->entity_expansion_depth >= QW_XML_MAX_ENTITY_DEPTH) {
        qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                 "DTD parameter entity nesting limit exceeded");
        goto cleanup;
    }
    if (entity->external) {
        if (!qwx_get_external_text(parser, entity, &external_text,
                                  &external_size, NULL)) goto cleanup;
        replacement = external_text;
        replacement_size = external_size;
    } else {
        replacement = entity->value;
        replacement_size = entity->value_size;
    }
    if (replacement_size > QW_XML_MAX_ENTITY_BYTES -
                          parser->entity_expanded_bytes) {
        qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                 "DTD parameter entity expansion limit exceeded");
        goto cleanup;
    }
    ++parser->entity_expansion_depth;
    parser->entity_expanded_bytes += replacement_size;
    if (!qwx_buffer_append(parser, &expanded, " ", 1u) ||
        !qwx_expand_dtd_fragment(parser, (const uint8_t *)replacement,
                                 replacement_size, &expanded, NULL, NULL, 0u,
                                 0u, NULL) ||
        !qwx_buffer_append(parser, &expanded, " ", 1u)) {
        --parser->entity_expansion_depth;
        goto cleanup;
    }
    --parser->entity_expansion_depth;
    start = 0u;
    end = expanded.size;
    while (start < end && (expanded.data[start] == ' ' ||
           expanded.data[start] == '\t' || expanded.data[start] == '\r' ||
           expanded.data[start] == '\n'))
        ++start;
    while (end > start && (expanded.data[end - 1u] == ' ' ||
           expanded.data[end - 1u] == '\t' || expanded.data[end - 1u] == '\r' ||
           expanded.data[end - 1u] == '\n'))
        --end;
    if (end - start == 7u && memcmp(expanded.data + start, "INCLUDE", 7u) == 0) {
        *out_include = 1;
        ok = 1;
    } else if (end - start == 6u &&
               memcmp(expanded.data + start, "IGNORE", 6u) == 0) {
        *out_include = 0;
        ok = 1;
    } else {
        qwx_fail(parser, QW_XML_ERROR_FORMAT,
                 "Conditional-section parameter entity must expand to INCLUDE or IGNORE");
    }
    goto cleanup;

malformed:
    qwx_fail(parser, QW_XML_ERROR_FORMAT,
             "Conditional-section parameter entity reference is malformed");
cleanup:
    qwx_release(parser, name, name ? strlen(name) + 1u : 0u);
    qwx_release(parser, external_text,
                external_text ? external_size + 1u : 0u);
    qwx_release(parser, expanded.data, expanded.capacity);
    return ok;
}

static int qwx_parse_conditional_section(QwXmlParser *parser)
{
    size_t opener_position = parser->position;
    uint32_t opening_origin = qwx_input_origin(parser, opener_position);
    int include;
    int ok;
    if (!qwx_require_origin(parser, opening_origin, opener_position, 3u,
            "Conditional-section delimiter crosses a parameter-entity boundary"))
        return 0;
    parser->position += 3u; /* <![ */
    qwx_skip_space(parser);
    if (parser->position < parser->size &&
        parser->input[parser->position] == '%') {
        if (!qwx_parse_conditional_keyword_pe(parser, &include)) return 0;
        if (include) {
            qwx_skip_space(parser);
            if (parser->position >= parser->size ||
                parser->input[parser->position] != '[')
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "INCLUDE conditional section is missing '['");
            if (!qwx_require_origin(parser, opening_origin, parser->position,
                    1u, "Conditional-section delimiters cross a parameter-entity boundary"))
                return 0;
            ++parser->position;
            if (parser->conditional_origin_depth >= QW_XML_MAX_DEPTH)
                return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                                "XML conditional-section nesting limit exceeded");
            parser->conditional_origin_stack[
                parser->conditional_origin_depth++] = opening_origin;
            ok = qwx_parse_dtd_subset(parser, 2);
            --parser->conditional_origin_depth;
            return ok;
        }
        return qwx_skip_conditional_ignore_body(parser, opening_origin);
    }
    if (qwx_starts(parser, "INCLUDE") &&
        (parser->position + 7u == parser->size ||
         !qwx_name_char(parser->input[parser->position + 7u]))) {
        parser->position += 7u;
        qwx_skip_space(parser);
        if (parser->position >= parser->size ||
            parser->input[parser->position] != '[')
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "INCLUDE conditional section is missing '['");
        if (!qwx_require_origin(parser, opening_origin, parser->position, 1u,
                "Conditional-section delimiters cross a parameter-entity boundary"))
            return 0;
        ++parser->position;
        if (parser->conditional_origin_depth >= QW_XML_MAX_DEPTH)
            return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                            "XML conditional-section nesting limit exceeded");
        parser->conditional_origin_stack[
            parser->conditional_origin_depth++] = opening_origin;
        ok = qwx_parse_dtd_subset(parser, 2);
        --parser->conditional_origin_depth;
        return ok;
    }
    if (qwx_starts(parser, "IGNORE") &&
        (parser->position + 6u == parser->size ||
         !qwx_name_char(parser->input[parser->position + 6u]))) {
        return qwx_skip_conditional_ignore(parser, opening_origin);
    }
    return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                    "Conditional section keyword must be INCLUDE or IGNORE");
}

static int qwx_expand_dtd_fragment(QwXmlParser *parser,
                                   const uint8_t *data, size_t size,
                                   QwXmlBuffer *output,
                                   QwXmlOriginMap *origins,
                                   const uint32_t *data_origins,
                                   uint32_t data_origin_id, unsigned depth,
                                   const char **out_base_uri)
{
    size_t position = 0u;
    uint8_t quote = 0u;
    if (depth > QW_XML_MAX_ENTITY_DEPTH)
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "DTD parameter entity nesting limit exceeded");
    while (position < size) {
        uint8_t c = data[position];
        if (quote) {
            if (!qwx_dtd_fragment_append(parser, output, origins,
                    (const char *)data + position, 1u,
                    data_origins ? data_origins + position : NULL,
                    data_origin_id)) return 0;
            if (c == quote) quote = 0u;
            ++position;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            if (!qwx_dtd_fragment_append(parser, output, origins,
                    (const char *)data + position, 1u,
                    data_origins ? data_origins + position : NULL,
                    data_origin_id)) return 0;
            ++position;
            continue;
        }
        if (c != '%') {
            size_t next = position;
            uint32_t cp;
            if (!qwx_decode_utf8(data, size, &next, &cp) ||
                !qwx_dtd_fragment_append(parser, output, origins,
                    (const char *)data + position, next - position,
                    data_origins ? data_origins + position : NULL,
                    data_origin_id)) return 0;
            position = next;
            continue;
        }
        {
            size_t name_start = position + 1u;
            size_t name_end = name_start;
            size_t cursor = name_start;
            uint32_t cp;
            QwXmlEntity *entity;
            const char *replacement;
            const char *replacement_base = NULL;
            char *external_text = NULL;
            size_t replacement_size;
            uint32_t replacement_origin;
            if (cursor >= size) {
                if (!qwx_dtd_fragment_append(parser, output, origins, "%", 1u,
                        data_origins ? data_origins + position : NULL,
                        data_origin_id)) return 0;
                ++position;
                continue;
            }
            {
                size_t next = cursor;
                if (!qwx_decode_utf8(data, size, &next, &cp))
                    return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                                    "DTD parameter entity name is invalid UTF-8");
                if (!qwx_name_start(cp)) {
                    if (!qwx_dtd_fragment_append(parser, output, origins, "%", 1u,
                            data_origins ? data_origins + position : NULL,
                            data_origin_id)) return 0;
                    ++position;
                    continue;
                }
            }
            while (cursor < size) {
                size_t next = cursor;
                if (!qwx_decode_utf8(data, size, &next, &cp))
                    return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                                    "DTD parameter entity name is invalid UTF-8");
                if (!(cursor == name_start ? qwx_name_start(cp)
                                           : qwx_name_char(cp))) break;
                cursor = next;
            }
            name_end = cursor;
            if (name_end == name_start || cursor >= size || data[cursor] != ';')
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "DTD parameter entity reference is malformed");
            entity = qwx_find_entity(parser,
                (const char *)data + name_start, name_end - name_start, 1);
            if (!entity)
                return qwx_fail(parser,
                    parser->external_subset_unread
                        ? QW_XML_ERROR_UNSUPPORTED : QW_XML_ERROR_FORMAT,
                    "DTD parameter entity reference is undeclared");
            if (parser->entity_expansion_depth >= QW_XML_MAX_ENTITY_DEPTH ||
                entity->value_size > QW_XML_MAX_ENTITY_BYTES -
                                     parser->entity_expanded_bytes)
                return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                                "DTD parameter entity expansion limit exceeded");
            if (entity->external) {
                if (!qwx_get_external_text(parser, entity, &external_text,
                        &replacement_size, &replacement_base)) return 0;
                replacement = external_text;
            } else {
                replacement = entity->value;
                replacement_size = entity->value_size;
            }
            if (replacement_size > QW_XML_MAX_ENTITY_BYTES -
                                   parser->entity_expanded_bytes) {
                qwx_release(parser, external_text,
                    external_text ? replacement_size + 1u : 0u);
                qwx_release(parser, (void *)replacement_base,
                    replacement_base ? strlen(replacement_base) + 1u : 0u);
                return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                                "DTD parameter entity expansion limit exceeded");
            }
            if (!qwx_dtd_fragment_append(parser, output, origins, " ", 1u,
                    NULL, data_origin_id)) {
                qwx_release(parser, external_text,
                    external_text ? replacement_size + 1u : 0u);
                qwx_release(parser, (void *)replacement_base,
                    replacement_base ? strlen(replacement_base) + 1u : 0u);
                return 0;
            }
            replacement_origin = qwx_next_parameter_entity_origin(parser);
            if (!replacement_origin) {
                qwx_release(parser, external_text,
                    external_text ? replacement_size + 1u : 0u);
                qwx_release(parser, (void *)replacement_base,
                    replacement_base ? strlen(replacement_base) + 1u : 0u);
                return 0;
            }
            ++parser->entity_expansion_depth;
            parser->entity_expanded_bytes += replacement_size;
            if (!qwx_expand_dtd_fragment(parser,
                    (const uint8_t *)replacement, replacement_size,
                    output, origins, NULL, replacement_origin, depth + 1u,
                    out_base_uri)) {
                --parser->entity_expansion_depth;
                qwx_release(parser, external_text,
                    external_text ? replacement_size + 1u : 0u);
                qwx_release(parser, (void *)replacement_base,
                    replacement_base ? strlen(replacement_base) + 1u : 0u);
                return 0;
            }
            --parser->entity_expansion_depth;
            if (replacement_base && out_base_uri) {
                qwx_release(parser, (void *)*out_base_uri,
                    *out_base_uri ? strlen(*out_base_uri) + 1u : 0u);
                *out_base_uri = replacement_base;
                replacement_base = NULL;
            }
            qwx_release(parser, external_text,
                external_text ? replacement_size + 1u : 0u);
            qwx_release(parser, (void *)replacement_base,
                replacement_base ? strlen(replacement_base) + 1u : 0u);
            if (!qwx_dtd_fragment_append(parser, output, origins, " ", 1u,
                    NULL, data_origin_id)) return 0;
            position = cursor + 1u;
        }
    }
    return 1;
}

typedef int (*QwXmlDtdDeclarationParser)(QwXmlParser *parser);

static int qwx_parse_dtd_markup_declaration(QwXmlParser *parser,
                                            QwXmlDtdDeclarationParser parse)
{
    size_t start = parser->position;
    size_t end = start;
    uint8_t quote = 0u;
    int has_parameter_reference = 0;
    QwXmlBuffer expanded = {0};
    QwXmlOriginMap origins = {0};
    const char *replacement_base = NULL;
    const uint8_t *saved_input;
    const uint32_t *saved_input_origins;
    size_t saved_size;
    size_t saved_position;
    const char *saved_base_uri;
    uint32_t saved_input_origin_id;
    int ok;
    while (end < parser->size) {
        uint8_t c = parser->input[end];
        if (quote) {
            if (c == quote) quote = 0u;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '%') {
            size_t cursor = end + 1u;
            uint32_t cp;
            if (cursor < parser->size) {
                size_t next = cursor;
                if (qwx_decode_utf8(parser->input, parser->size, &next, &cp) &&
                    qwx_name_start(cp)) {
                    cursor = next;
                    while (cursor < parser->size) {
                        next = cursor;
                        if (!qwx_decode_utf8(parser->input, parser->size,
                                             &next, &cp) || !qwx_name_char(cp))
                            break;
                        cursor = next;
                    }
                    if (cursor < parser->size && parser->input[cursor] == ';')
                        has_parameter_reference = 1;
                }
            }
        } else if (c == '>') {
            ++end;
            break;
        }
        ++end;
    }
    if (end == parser->size &&
        (end == start || parser->input[end - 1u] != '>'))
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "DTD markup declaration is not terminated");
    if (!has_parameter_reference) {
        if (parser->validate_dtd && end > start &&
            qwx_input_origin(parser, start) !=
                qwx_input_origin(parser, end - 1u))
            return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                            "DTD markup declaration crosses a parameter-entity boundary");
        return parse(parser);
    }
    if (!qwx_expand_dtd_fragment(parser, parser->input + start,
            end - start, &expanded, &origins,
            parser->input_origin_ids ? parser->input_origin_ids + start : NULL,
            parser->input_origin_id, 0u, &replacement_base)) {
        qwx_release(parser, expanded.data, expanded.capacity);
        qwx_release(parser, origins.ids,
                    origins.capacity * sizeof(*origins.ids));
        qwx_release(parser, (void *)replacement_base,
            replacement_base ? strlen(replacement_base) + 1u : 0u);
        return 0;
    }
    if (origins.size != expanded.size) {
        qwx_release(parser, expanded.data, expanded.capacity);
        qwx_release(parser, origins.ids,
                    origins.capacity * sizeof(*origins.ids));
        qwx_release(parser, (void *)replacement_base,
            replacement_base ? strlen(replacement_base) + 1u : 0u);
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML DTD origin map is inconsistent");
    }
    saved_input = parser->input;
    saved_input_origins = parser->input_origin_ids;
    saved_size = parser->size;
    saved_position = parser->position;
    saved_base_uri = parser->current_base_uri;
    saved_input_origin_id = parser->input_origin_id;
    parser->input = (const uint8_t *)expanded.data;
    parser->input_origin_ids = origins.ids;
    parser->input_origin_id = 0u;
    parser->size = expanded.size;
    parser->position = 0u;
    if (replacement_base) parser->current_base_uri = replacement_base;
    if (parser->validate_dtd && expanded.size &&
        origins.ids[0] != origins.ids[expanded.size - 1u])
        ok = qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                      "DTD markup declaration crosses a parameter-entity boundary");
    else
        ok = parse(parser);
    if (ok && parser->position != parser->size)
        ok = qwx_fail(parser, QW_XML_ERROR_FORMAT,
                      "Expanded DTD parameter entity leaves trailing declaration text");
    parser->input = saved_input;
    parser->input_origin_ids = saved_input_origins;
    parser->input_origin_id = saved_input_origin_id;
    parser->size = saved_size;
    parser->position = end;
    parser->current_base_uri = saved_base_uri;
    qwx_release(parser, expanded.data, expanded.capacity);
    qwx_release(parser, origins.ids,
                origins.capacity * sizeof(*origins.ids));
    qwx_release(parser, (void *)replacement_base,
        replacement_base ? strlen(replacement_base) + 1u : 0u);
    if (!ok) parser->position = saved_position;
    return ok;
}

static int qwx_parse_dtd_subset(QwXmlParser *parser, int close_mode)
{
    for (;;) {
        qwx_skip_space(parser);
        if (parser->position >= parser->size) {
            if (close_mode) {
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "XML internal subset is not terminated");
            }
            return 1;
        }
        if (parser->input[parser->position] == ']') {
            if (close_mode == 2) {
                if (!qwx_starts(parser, "]]>"))
                    return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                    "INCLUDE conditional section is malformed");
                if (!parser->conditional_origin_depth ||
                    !qwx_require_origin(parser,
                        parser->conditional_origin_stack[
                            parser->conditional_origin_depth - 1u],
                        parser->position, 3u,
                        "Conditional-section delimiters cross a parameter-entity boundary"))
                    return parser->conditional_origin_depth
                        ? 0 : qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                       "Conditional-section stack is unbalanced");
                parser->position += 3u;
                return 1;
            }
            if (close_mode != 1) {
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "XML parameter entity replacement has unexpected ']' ");
            }
            ++parser->position;
            qwx_skip_space(parser);
            if (parser->position >= parser->size ||
                parser->input[parser->position++] != '>') {
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "XML document type declaration is malformed");
            }
            return 1;
        }
        if (qwx_starts(parser, "<![")) {
            if (!parser->in_external_subset)
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "Conditional sections are only allowed in external DTDs");
            if (!qwx_parse_conditional_section(parser)) return 0;
            continue;
        }
        if (qwx_starts(parser, "<!--")) {
            if (!qwx_parse_comment(parser, 0)) return 0;
            continue;
        }
        if (qwx_starts(parser, "<?")) {
            if (!qwx_parse_processing_instruction(parser, 0)) return 0;
            continue;
        }
        if (qwx_starts(parser, "<!ENTITY")) {
            if (!(parser->in_external_subset
                    ? qwx_parse_dtd_markup_declaration(parser,
                                                       qwx_parse_entity_declaration)
                    : qwx_parse_entity_declaration(parser))) return 0;
            continue;
        }
        if (parser->input[parser->position] == '%') {
            if (!qwx_parse_parameter_entity_reference(parser)) return 0;
            continue;
        }
        if (qwx_starts(parser, "<!DOCTYPE")) {
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML DOCTYPE declaration cannot be nested");
        }
        if (qwx_starts(parser, "<!ELEMENT")) {
            if (!(parser->in_external_subset
                    ? qwx_parse_dtd_markup_declaration(parser,
                                                       qwx_parse_element_declaration)
                    : qwx_parse_element_declaration(parser))) return 0;
            continue;
        }
        if (qwx_starts(parser, "<!ATTLIST")) {
            if (!(parser->in_external_subset
                    ? qwx_parse_dtd_markup_declaration(parser,
                                             qwx_parse_attribute_list_declaration)
                    : qwx_parse_attribute_list_declaration(parser))) return 0;
            continue;
        }
        if (qwx_starts(parser, "<!NOTATION")) {
            if (!(parser->in_external_subset
                    ? qwx_parse_dtd_markup_declaration(parser,
                                                       qwx_parse_notation_declaration)
                    : qwx_parse_notation_declaration(parser))) return 0;
            continue;
        }
        if (qwx_starts(parser, "<!")) {
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML internal subset contains an unknown declaration");
        }
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML internal subset contains unexpected text");
    }
}

static int qwx_append_reference(QwXmlParser *parser, QwXmlBuffer *buffer,
                                unsigned depth, int attribute)
{
    size_t start;
    uint32_t cp = 0u;
    QwXmlEntity *entity;
    if (parser->position >= parser->size ||
        parser->input[parser->position] != '&') return 0;
    ++parser->position;
    start = parser->position;
    while (parser->position < parser->size &&
           parser->input[parser->position] != ';') ++parser->position;
    if (parser->position >= parser->size ||
        parser->input[parser->position] != ';' || parser->position == start)
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML entity reference is not terminated");
    if (parser->input[start] == '#') {
        size_t cursor = start + 1u;
        unsigned base = 10u;
        if (cursor < parser->position &&
            parser->input[cursor] == 'x') {
            base = 16u;
            ++cursor;
        }
        if (cursor == parser->position) {
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML character reference has no digits");
        }
        for (; cursor < parser->position; ++cursor) {
            uint8_t c = parser->input[cursor];
            unsigned digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (base == 16u && c >= 'a' && c <= 'f') digit = c - 'a' + 10u;
            else if (base == 16u && c >= 'A' && c <= 'F') digit = c - 'A' + 10u;
            else return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                 "XML character reference is invalid");
            if (digit >= base || cp > (UINT32_MAX - digit) / base) {
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "XML character reference is out of range");
            }
            cp = cp * base + digit;
        }
        if (!qwx_append_codepoint(parser, buffer, cp)) return 0;
        ++parser->position;
        return 1;
    } else if (!qwx_named_entity((const char *)parser->input + start,
                                 parser->position - start, &cp)) {
        entity = qwx_find_entity(parser,
            (const char *)parser->input + start, parser->position - start, 0);
        if (!entity)
            return qwx_fail(parser,
                            parser->external_subset_unread
                                ? QW_XML_ERROR_UNSUPPORTED
                                : QW_XML_ERROR_FORMAT,
                            "XML entity reference is undeclared");
        if (parser->in_document_attribute_value)
            entity->referenced = 1;
        if (entity->unparsed)
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "Unparsed XML entity cannot be referenced here");
        if (entity->external)
            return qwx_fail(parser,
                attribute ? QW_XML_ERROR_FORMAT : QW_XML_ERROR_UNSUPPORTED,
                attribute
                    ? "External entity references are forbidden in XML attributes"
                    : "External XML entities are not resolved");
        ++parser->position;
        return qwx_append_entity_value(parser, buffer, entity->value,
                                       entity->value_size, depth + 1u,
                                       attribute);
    } else {
        ++parser->position;
        return qwx_append_codepoint(parser, buffer, cp);
    }
}

static int qwx_parse_name(QwXmlParser *parser, char **out_name)
{
    size_t start = parser->position;
    size_t cursor = parser->position;
    int first = 1;
    if (out_name) *out_name = NULL;
    while (cursor < parser->size) {
        uint32_t cp;
        size_t next = cursor;
        if (!qwx_decode_utf8(parser->input, parser->size, &next, &cp)) {
            return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                            "XML name contains invalid UTF-8");
        }
        if (first ? !qwx_name_start(cp) : !qwx_name_char(cp)) break;
        cursor = next;
        first = 0;
    }
    if (cursor == start) {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML name is missing or invalid");
    }
    if (out_name) {
        *out_name = qwx_duplicate(parser,
            (const char *)parser->input + start, cursor - start);
        if (!*out_name) return 0;
    }
    parser->position = cursor;
    return 1;
}

static const char *qwx_local_name(const char *qualified_name)
{
    const char *colon;
    if (!qualified_name) return NULL;
    colon = strrchr(qualified_name, ':');
    return colon ? colon + 1u : qualified_name;
}

static int qwx_parse_value(QwXmlParser *parser, uint8_t terminator,
                           int attribute, char **out_value)
{
    QwXmlBuffer buffer = {0};
    if (out_value) *out_value = NULL;
    while (parser->position < parser->size &&
           parser->input[parser->position] != terminator) {
        uint8_t c = parser->input[parser->position];
        if (!attribute && qwx_starts(parser, "]]>"))
        {
            qwx_fail(parser, QW_XML_ERROR_FORMAT,
                     "XML character data contains ']]>'");
            goto failure;
        }
        if (attribute && c == '<') {
            qwx_fail(parser, QW_XML_ERROR_FORMAT,
                     "XML attribute value contains an unescaped '<'");
            goto failure;
        }
        if (c == '&') {
            if (!qwx_append_reference(parser, &buffer, 0u, attribute))
                goto failure;
            continue;
        }
        if (c == '\r') {
            char normalized = attribute ? ' ' : '\n';
            ++parser->position;
            if (parser->position < parser->size &&
                parser->input[parser->position] == '\n') ++parser->position;
            if (!qwx_append_codepoint(parser, &buffer, (uint8_t)normalized))
                goto failure;
            continue;
        }
        if (attribute && (c == '\n' || c == '\t')) {
            ++parser->position;
            if (!qwx_append_codepoint(parser, &buffer, ' ')) goto failure;
            continue;
        }
        {
            size_t cursor = parser->position;
            uint32_t cp;
            if (!qwx_decode_utf8(parser->input, parser->size, &cursor, &cp) ||
                !qwx_xml_character(cp)) {
                qwx_fail(parser, QW_XML_ERROR_ENCODING,
                         "XML text contains an invalid character");
                goto failure;
            }
            {
                size_t byte_count = cursor - parser->position;
                if (!qwx_buffer_append(parser, &buffer,
                        (const char *)parser->input + parser->position,
                        byte_count)) goto failure;
                parser->position = cursor;
            }
        }
    }
    if (parser->position >= parser->size && terminator != '<') {
        qwx_fail(parser, QW_XML_ERROR_FORMAT,
                 "XML quoted value or text is not terminated");
        goto failure;
    }
    if (!buffer.data) {
        buffer.data = (char *)qwx_allocate(parser, 1u);
        if (!buffer.data) goto failure;
        buffer.data[0] = '\0';
        buffer.capacity = 1u;
    }
    if (buffer.size > QW_XML_MAX_ENTITY_BYTES - parser->text_bytes) {
        qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                 "XML decoded text limit exceeded");
        goto failure;
    }
    parser->text_bytes += buffer.size;
    if (parser->position < parser->size && terminator != '<')
        ++parser->position;
    if (buffer.capacity != buffer.size + 1u) {
        char *exact = (char *)qwx_reallocate(parser, buffer.data,
                                             buffer.capacity,
                                             buffer.size + 1u);
        if (!exact) goto failure;
        buffer.data = exact;
        buffer.capacity = buffer.size + 1u;
    }
    *out_value = buffer.data;
    return 1;

failure:
    qwx_release(parser, buffer.data, buffer.capacity);
    return 0;
}

static QwXmlNode *qwx_new_node(QwXmlParser *parser, char *tag, char *text,
                               int is_document)
{
    QwXmlNode *node;
    if (parser->node_count >= QW_XML_MAX_NODES) {
        qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                 "XML node limit exceeded");
        free(tag);
        free(text);
        return NULL;
    }
    node = (QwXmlNode *)qwx_allocate(parser, sizeof(*node));
    if (!node) {
        free(tag);
        free(text);
        return NULL;
    }
    memset(node, 0, sizeof(*node));
    node->kind = is_document ? QW_XML_NODE_DOCUMENT
        : tag ? QW_XML_NODE_ELEMENT : QW_XML_NODE_TEXT;
    node->tag = tag;
    node->text = text;
    node->is_document = is_document;
    ++parser->node_count;
    return node;
}

static void qwx_append_child(QwXmlParser *parser, QwXmlNode *parent,
                             QwXmlNode *child)
{
    child->parent = parent;
    if (parent) {
        if (parent->last_child) {
            parent->last_child->next = child;
            child->previous = parent->last_child;
        }
        else parent->first_child = child;
        parent->last_child = child;
    } else {
        parser->root = child;
    }
}

static int qwx_add_text(QwXmlParser *parser, char *text,
                        int raw_whitespace_only, int is_cdata)
{
    QwXmlNode *node;
    if (!parser->current) {
        if (is_cdata || !raw_whitespace_only) {
            qwx_release(parser, text, strlen(text) + 1u);
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML document has character data outside its root element");
        }
        qwx_release(parser, text, strlen(text) + 1u);
        return 1;
    }
    node = qwx_new_node(parser, NULL, text, 0);
    if (!node) return 0;
    node->is_whitespace = raw_whitespace_only;
    node->is_cdata = is_cdata;
    qwx_append_child(parser, parser->current, node);
    return 1;
}

static int qwx_skip_until(QwXmlParser *parser, const char *terminator,
                          const char *error_message)
{
    size_t length = strlen(terminator);
    while (parser->position + length <= parser->size) {
        if (memcmp(parser->input + parser->position, terminator, length) == 0) {
            parser->position += length;
            return 1;
        }
        ++parser->position;
    }
    return qwx_fail(parser, QW_XML_ERROR_FORMAT, error_message);
}

static int qwx_copy_normalized_range(QwXmlParser *parser, size_t start,
                                    size_t end, char **out_text,
                                    size_t *out_size)
{
    QwXmlBuffer text = {0};
    if (out_text) *out_text = NULL;
    if (out_size) *out_size = 0u;
    while (start < end) {
        if (parser->input[start] == '\r') {
            if (!qwx_buffer_append(parser, &text, "\n", 1u)) goto failure;
            ++start;
            if (start < end && parser->input[start] == '\n') ++start;
            continue;
        }
        {
            size_t next = start;
            uint32_t cp;
            if (!qwx_decode_utf8(parser->input, end, &next, &cp) ||
                !qwx_xml_character(cp) ||
                !qwx_buffer_append(parser, &text,
                    (const char *)parser->input + start, next - start)) {
                qwx_fail(parser, QW_XML_ERROR_ENCODING,
                         "XML miscellaneous content contains invalid characters");
                goto failure;
            }
            start = next;
        }
    }
    if (!text.data) {
        text.data = (char *)qwx_allocate(parser, 1u);
        if (!text.data) goto failure;
        text.data[0] = '\0';
        text.capacity = 1u;
    }
    if (text.capacity != text.size + 1u) {
        char *exact = (char *)qwx_reallocate(parser, text.data,
            text.capacity, text.size + 1u);
        if (!exact) goto failure;
        text.data = exact;
        text.capacity = text.size + 1u;
    }
    if (out_size) *out_size = text.size;
    if (out_text) *out_text = text.data;
    return 1;

failure:
    qwx_release(parser, text.data, text.capacity);
    return 0;
}

static int qwx_add_misc_node(QwXmlParser *parser, QwXmlNodeKind kind,
                             char *target, char *text, size_t text_size)
{
    QwXmlNode *node;
    if (text_size > QW_XML_MAX_ENTITY_BYTES - parser->text_bytes) {
        qwx_release(parser, target,
            target ? strlen(target) + 1u : 0u);
        qwx_release(parser, text, text_size + 1u);
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML miscellaneous content limit exceeded");
    }
    parser->text_bytes += text_size;
    node = qwx_new_node(parser, NULL, text, 0);
    if (!node) {
        qwx_release(parser, target,
            target ? strlen(target) + 1u : 0u);
        return 0;
    }
    node->kind = kind;
    node->processing_instruction_target = target;
    qwx_append_child(parser, parser->current ? parser->current : parser->root,
                     node);
    return 1;
}

static int qwx_parse_comment(QwXmlParser *parser, int preserve)
{
    size_t content_start;
    size_t content_end;
    parser->position += 4u;
    content_start = parser->position;
    while (parser->position + 2u < parser->size) {
        if (parser->input[parser->position] == '-' &&
            parser->input[parser->position + 1u] == '-' &&
            parser->input[parser->position + 2u] == '>') {
            content_end = parser->position;
            parser->position += 3u;
            if (preserve) {
                char *text = NULL;
                size_t text_size = 0u;
                if (!qwx_copy_normalized_range(parser, content_start,
                        content_end, &text, &text_size)) return 0;
                return qwx_add_misc_node(parser, QW_XML_NODE_COMMENT,
                                         NULL, text, text_size);
            }
            return 1;
        }
        if (parser->input[parser->position] == '-' &&
            parser->input[parser->position + 1u] == '-') {
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML comment contains an invalid '--'");
        }
        ++parser->position;
    }
    return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                    "XML comment is not terminated");
}

static int qwx_skip_doctype(QwXmlParser *parser)
{
    char *root_name = NULL;
    char *public_id = NULL;
    char *system_id = NULL;
    size_t before_space;
    int has_space;
    int ok = 0;
    parser->position += 9u;
    before_space = parser->position;
    qwx_skip_space(parser);
    if (parser->position == before_space ||
        !qwx_parse_name(parser, &root_name)) {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML document type declaration requires a root name");
    }
    if (!qwx_qname_parts(root_name, NULL, NULL)) {
        qwx_release(parser, root_name, strlen(root_name) + 1u);
        return qwx_fail(parser, QW_XML_ERROR_NAMESPACE,
                        "DOCTYPE root name is not a namespace QName");
    }
    parser->doctype_name = qwx_duplicate(parser, root_name, strlen(root_name));
    qwx_release(parser, root_name, strlen(root_name) + 1u);
    if (!parser->doctype_name) return 0;

    before_space = parser->position;
    qwx_skip_space(parser);
    has_space = parser->position != before_space;
    if (qwx_starts(parser, "SYSTEM")) {
        if (!has_space) {
            qwx_fail(parser, QW_XML_ERROR_FORMAT,
                     "XML SYSTEM identifier must be separated from the root name");
            goto cleanup;
        }
        parser->position += 6u;
        before_space = parser->position;
        qwx_skip_space(parser);
        if (parser->position == before_space) {
            qwx_fail(parser, QW_XML_ERROR_FORMAT,
                     "XML SYSTEM identifier requires whitespace before its literal");
            goto cleanup;
        }
        if (!qwx_read_dtd_literal(parser, 0, &system_id)) goto cleanup;
        parser->external_subset_present = 1;
    } else if (qwx_starts(parser, "PUBLIC")) {
        if (!has_space) {
            qwx_fail(parser, QW_XML_ERROR_FORMAT,
                     "XML PUBLIC identifier must be separated from the root name");
            goto cleanup;
        }
        parser->position += 6u;
        before_space = parser->position;
        qwx_skip_space(parser);
        if (parser->position == before_space) {
            qwx_fail(parser, QW_XML_ERROR_FORMAT,
                     "XML PUBLIC identifier requires whitespace before its literal");
            goto cleanup;
        }
        if (!qwx_read_dtd_literal(parser, 1, &public_id)) goto cleanup;
        before_space = parser->position;
        qwx_skip_space(parser);
        if (parser->position == before_space) {
            qwx_fail(parser, QW_XML_ERROR_FORMAT,
                     "XML PUBLIC identifier requires whitespace before its system literal");
            goto cleanup;
        }
        if (!qwx_read_dtd_literal(parser, 0, &system_id)) {
            goto cleanup;
        }
        parser->external_subset_present = 1;
    }

    before_space = parser->position;
    qwx_skip_space(parser);
    has_space = parser->position != before_space;
    if (parser->position < parser->size &&
        parser->input[parser->position] == '[') {
        if (!has_space) {
            qwx_fail(parser, QW_XML_ERROR_FORMAT,
                     "XML internal subset must be separated by whitespace");
            goto cleanup;
        }
        ++parser->position;
        if (!qwx_parse_dtd_subset(parser, 1)) goto cleanup;
        ok = 1;
    } else {
        if (parser->position >= parser->size ||
            parser->input[parser->position++] != '>') {
            qwx_fail(parser, QW_XML_ERROR_FORMAT,
                     "XML document type declaration is not terminated");
            goto cleanup;
        }
        ok = 1;
    }

    if (ok && parser->external_subset_present) {
        parser->external_subset_unread = 1;
        if (parser->options && parser->options->resolve_entity) {
            if (!qwx_parse_external_resource(parser, public_id, system_id,
                    parser->current_base_uri, QW_XML_EXTERNAL_DTD))
                ok = 0;
            else
                parser->external_subset_unread = 0;
        } else if (parser->validate_dtd || parser->standalone_yes) {
            qwx_fail(parser, QW_XML_ERROR_UNSUPPORTED,
                     "The external subset is required but no resolver is configured");
            ok = 0;
        }
    }
cleanup:
    qwx_release(parser, public_id, public_id ? strlen(public_id) + 1u : 0u);
    qwx_release(parser, system_id, system_id ? strlen(system_id) + 1u : 0u);
    return ok;
}

static int qwx_append_node_attribute(QwXmlParser *parser, QwXmlNode *node,
                                    char *name, char *value)
{
    if (node->attribute_count >= QW_XML_MAX_ATTRIBUTES) {
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML attribute count limit exceeded");
    }
    if (node->attribute_count == node->attribute_capacity) {
        size_t capacity = node->attribute_capacity
            ? node->attribute_capacity * 2u : 4u;
        QwXmlAttribute *attributes;
        if (capacity > QW_XML_MAX_ATTRIBUTES) capacity = QW_XML_MAX_ATTRIBUTES;
        attributes = (QwXmlAttribute *)qwx_reallocate(
            parser, node->attributes,
            node->attribute_capacity * sizeof(*node->attributes),
            capacity * sizeof(*node->attributes));
        if (!attributes) return 0;
        node->attributes = attributes;
        node->attribute_capacity = capacity;
    }
    node->attributes[node->attribute_count].name = name;
    node->attributes[node->attribute_count].value = value;
    node->attributes[node->attribute_count].local_name = NULL;
    node->attributes[node->attribute_count].namespace_uri = NULL;
    node->attributes[node->attribute_count].specified = 1;
    ++node->attribute_count;
    return 1;
}

static int qwx_tokenized_value_would_change(const char *value)
{
    size_t read = 0u;
    size_t write = 0u;
    size_t size = strlen(value);
    int pending_space = 0;
    int has_content = 0;
    while (read < size) {
        unsigned char c = (unsigned char)value[read++];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!has_content) return 1;
            pending_space = 1;
            continue;
        }
        if (pending_space) {
            if (value[write++] != ' ') return 1;
            pending_space = 0;
        }
        if (value[write++] != (char)c) return 1;
        has_content = 1;
    }
    return pending_space || write != size;
}

static int qwx_apply_declared_attribute_type(QwXmlParser *parser,
                                            QwXmlNode *node,
                                            const char *name, char **value)
{
    QwXmlAttributeDecl *decl = qwx_find_attribute_decl(parser,
        node->qualified_tag, name);
    if (decl && decl->tokenized) {
        if (parser->validate_dtd && parser->standalone_yes &&
            decl->external_markup &&
            qwx_tokenized_value_would_change(*value))
            return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                            "Standalone document uses external tokenized attribute normalization");
        return qwx_normalize_tokenized_attribute(parser, value);
    }
    return 1;
}

static int qwx_apply_default_attributes(QwXmlParser *parser, QwXmlNode *node)
{
    size_t i;
    for (i = 0u; i < parser->attribute_decl_count; ++i) {
        QwXmlAttributeDecl *decl = &parser->attribute_decls[i];
        size_t j;
        char *name;
        char *value;
        if (!decl->default_value ||
            strcmp(decl->element_name, node->qualified_tag) != 0) continue;
        for (j = 0u; j < node->attribute_count; ++j) {
            if (strcmp(node->attributes[j].name, decl->name) == 0) break;
        }
        if (j < node->attribute_count) continue;
        name = qwx_duplicate(parser, decl->name, strlen(decl->name));
        if (!name) return 0;
        value = qwx_duplicate(parser, decl->default_value,
                              strlen(decl->default_value));
        if (!value) {
            qwx_release(parser, name, strlen(name) + 1u);
            return 0;
        }
        if (!qwx_append_node_attribute(parser, node, name, value)) {
            qwx_release(parser, name, strlen(name) + 1u);
            qwx_release(parser, value, strlen(value) + 1u);
            return 0;
        }
        node->attributes[node->attribute_count - 1u].specified = 0;
    }
    return 1;
}

static int qwx_parse_start_element(QwXmlParser *parser)
{
    QwXmlNode *node;
    char *qualified_tag = NULL;
    char *tag = NULL;
    int self_closing = 0;
    ++parser->position;
    if (!qwx_parse_name(parser, &qualified_tag)) return 0;
    if (!parser->current && parser->document_element_count != 0u) {
        qwx_release(parser, qualified_tag, strlen(qualified_tag) + 1u);
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML document contains more than one root element");
    }
    tag = qwx_duplicate(parser, qwx_local_name(qualified_tag),
                        strlen(qwx_local_name(qualified_tag)));
    if (!tag) {
        free(qualified_tag);
        return 0;
    }
    if (parser->depth >= QW_XML_MAX_DEPTH) {
        free(tag);
        free(qualified_tag);
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML nesting depth limit exceeded");
    }
    node = qwx_new_node(parser, tag, NULL, 0);
    if (!node) {
        free(qualified_tag);
        return 0;
    }
    node->qualified_tag = qualified_tag;
    qwx_append_child(parser, parser->current ? parser->current : parser->root,
                     node);
    if (!parser->current) ++parser->document_element_count;
    for (;;) {
        size_t before_space = parser->position;
        size_t i;
        int has_space;
        char *name = NULL;
        char *value = NULL;
        qwx_skip_space(parser);
        has_space = parser->position != before_space;
        if (parser->position >= parser->size) {
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML start tag is not terminated");
        }
        if (qwx_starts(parser, "/>")) {
            parser->position += 2u;
            self_closing = 1;
            break;
        }
        if (parser->input[parser->position] == '>') {
            ++parser->position;
            break;
        }
        if (!has_space) {
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML attributes must be separated by whitespace");
        }
        if (node->attribute_count >= QW_XML_MAX_ATTRIBUTES) {
            return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                            "XML attribute count limit exceeded");
        }
        if (!qwx_parse_name(parser, &name)) return 0;
        for (i = 0u; i < node->attribute_count; ++i) {
            if (strcmp(node->attributes[i].name, name) == 0) {
                qwx_release(parser, name, strlen(name) + 1u);
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "XML start tag contains a duplicate attribute");
            }
        }
        qwx_skip_space(parser);
        if (parser->position >= parser->size ||
            parser->input[parser->position] != '=') {
            qwx_release(parser, name, strlen(name) + 1u);
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML attribute is missing '='");
        }
        ++parser->position;
        qwx_skip_space(parser);
        if (parser->position >= parser->size ||
            (parser->input[parser->position] != '\'' &&
             parser->input[parser->position] != '"')) {
            qwx_release(parser, name, strlen(name) + 1u);
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML attribute values must be quoted");
        }
        {
            uint8_t quote = parser->input[parser->position++];
            int saved_document_attribute_value =
                parser->in_document_attribute_value;
            parser->in_document_attribute_value = parser->in_document_content;
            if (!qwx_parse_value(parser, quote, 1, &value)) {
                parser->in_document_attribute_value =
                    saved_document_attribute_value;
                qwx_release(parser, name, strlen(name) + 1u);
                return 0;
            }
            parser->in_document_attribute_value =
                saved_document_attribute_value;
        }
        if (!qwx_apply_declared_attribute_type(parser, node, name, &value)) {
            qwx_release(parser, name, strlen(name) + 1u);
            qwx_release(parser, value, value ? strlen(value) + 1u : 0u);
            return 0;
        }
        if (!qwx_append_node_attribute(parser, node, name, value)) {
            qwx_release(parser, name, strlen(name) + 1u);
            qwx_release(parser, value, strlen(value) + 1u);
            return 0;
        }
    }
    if (!qwx_apply_default_attributes(parser, node)) return 0;
    if (!self_closing) {
        parser->current = node;
        ++parser->depth;
    }
    return 1;
}

static int qwx_parse_end_element(QwXmlParser *parser)
{
    char *tag = NULL;
    QwXmlNode *current = parser->current;
    ++parser->position;
    ++parser->position;
    if (!current || !qwx_parse_name(parser, &tag)) {
        free(tag);
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML closing tag has no matching start tag");
    }
    qwx_skip_space(parser);
    if (parser->position >= parser->size ||
        parser->input[parser->position] != '>' ||
        strcmp(tag, current->qualified_tag) != 0) {
        free(tag);
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML closing tag does not match its start tag");
    }
    free(tag);
    ++parser->position;
    parser->current = current->parent;
    if (parser->current && parser->current->is_document)
        parser->current = NULL;
    --parser->depth;
    return 1;
}

static int qwx_parse_cdata(QwXmlParser *parser)
{
    size_t start;
    size_t end;
    size_t raw_size;
    int raw_whitespace_only = 1;
    QwXmlBuffer text = {0};
    parser->position += 9u;
    start = parser->position;
    while (parser->position + 2u < parser->size &&
           !qwx_starts(parser, "]]>")) ++parser->position;
    if (parser->position + 2u >= parser->size) {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "CDATA section is not terminated");
    }
    end = parser->position;
    raw_size = end - start;
    {
        size_t i;
        for (i = start; i < end; ++i) {
            uint8_t c = parser->input[i];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                raw_whitespace_only = 0;
                break;
            }
        }
    }
    if (raw_size > QW_XML_MAX_ENTITY_BYTES - parser->text_bytes) {
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML decoded text limit exceeded");
    }
    while (start < end) {
        if (parser->input[start] == '\r') {
            if (!qwx_buffer_append(parser, &text, "\n", 1u)) {
                qwx_release(parser, text.data, text.capacity);
                return 0;
            }
            ++start;
            if (start < end && parser->input[start] == '\n') ++start;
            continue;
        }
        size_t next = start;
        uint32_t cp;
        if (!qwx_decode_utf8(parser->input, end, &next, &cp) ||
            !qwx_xml_character(cp) ||
            !qwx_buffer_append(parser, &text,
                (const char *)parser->input + start, next - start)) {
            qwx_release(parser, text.data, text.capacity);
            return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                            "CDATA contains invalid UTF-8");
        }
        start = next;
    }
    if (!text.data) {
        text.data = (char *)qwx_allocate(parser, 1u);
        if (!text.data) return 0;
        text.data[0] = '\0';
        text.capacity = 1u;
    }
    if (text.capacity != text.size + 1u) {
        char *exact = (char *)qwx_reallocate(parser, text.data,
                                             text.capacity, text.size + 1u);
        if (!exact) {
            qwx_release(parser, text.data, text.capacity);
            return 0;
        }
        text.data = exact;
        text.capacity = text.size + 1u;
    }
    parser->text_bytes += raw_size;
    parser->position += 3u;
    return qwx_add_text(parser, text.data, raw_whitespace_only, 1);
}

static int qwx_parse_markup(QwXmlParser *parser)
{
    if (qwx_starts(parser, "<!--")) return qwx_parse_comment(parser, 1);
    if (qwx_starts(parser, "<![CDATA[")) return qwx_parse_cdata(parser);
    if (qwx_starts(parser, "<?"))
        return qwx_parse_processing_instruction(parser, 1);
    if (qwx_starts(parser, "<!DOCTYPE")) {
        if (parser->current || parser->document_element_count ||
            parser->document_type_seen) {
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML document type declaration is misplaced or duplicated");
        }
        parser->document_type_seen = 1;
        return qwx_skip_doctype(parser);
    }
    if (qwx_starts(parser, "</")) return qwx_parse_end_element(parser);
    if (qwx_starts(parser, "<!")) {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "Unsupported XML declaration inside document content");
    }
    return qwx_parse_start_element(parser);
}

static int qwx_normalize_utf16(QwXmlParser *parser, const uint8_t *data,
                               size_t size, int little_endian, size_t offset)
{
    QwXmlBuffer buffer = {0};
    size_t position = offset;
    while (position + 1u < size) {
        uint32_t cp;
        uint16_t first = little_endian
            ? (uint16_t)(data[position] | ((uint16_t)data[position + 1u] << 8))
            : (uint16_t)(((uint16_t)data[position] << 8) | data[position + 1u]);
        position += 2u;
        cp = first;
        if (first >= 0xd800u && first <= 0xdbffu) {
            uint16_t second;
            if (position + 1u >= size) {
                free(buffer.data);
                return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                                "UTF-16 XML ends inside a surrogate pair");
            }
            second = little_endian
                ? (uint16_t)(data[position] | ((uint16_t)data[position + 1u] << 8))
                : (uint16_t)(((uint16_t)data[position] << 8) | data[position + 1u]);
            position += 2u;
            if (second < 0xdc00u || second > 0xdfffu) {
                free(buffer.data);
                return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                                "UTF-16 XML has an invalid surrogate pair");
            }
            cp = 0x10000u + (((uint32_t)first - 0xd800u) << 10) +
                 ((uint32_t)second - 0xdc00u);
        } else if (first >= 0xdc00u && first <= 0xdfffu) {
            free(buffer.data);
            return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                            "UTF-16 XML has an unpaired low surrogate");
        }
        if (!qwx_append_codepoint(parser, &buffer, cp)) {
            free(buffer.data);
            return 0;
        }
    }
    if (position != size) {
        free(buffer.data);
        return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                        "UTF-16 XML has an incomplete code unit");
    }
    parser->normalized_input = (uint8_t *)buffer.data;
    parser->input = parser->normalized_input;
    parser->size = buffer.size;
    return 1;
}

static int qwx_normalize_utf32(QwXmlParser *parser, const uint8_t *data,
                               size_t size, int little_endian, size_t offset)
{
    QwXmlBuffer buffer = {0};
    size_t position = offset;
    if (offset > size || (size - offset) % 4u != 0u) {
        return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                        "UTF-32 XML has an incomplete code unit");
    }
    while (position < size) {
        uint32_t cp;
        if (little_endian) {
            cp = (uint32_t)data[position] |
                 ((uint32_t)data[position + 1u] << 8) |
                 ((uint32_t)data[position + 2u] << 16) |
                 ((uint32_t)data[position + 3u] << 24);
        } else {
            cp = ((uint32_t)data[position] << 24) |
                 ((uint32_t)data[position + 1u] << 16) |
                 ((uint32_t)data[position + 2u] << 8) |
                 (uint32_t)data[position + 3u];
        }
        position += 4u;
        if (!qwx_append_codepoint(parser, &buffer, cp)) {
            free(buffer.data);
            return 0;
        }
    }
    if (!buffer.data) {
        buffer.data = (char *)qwx_allocate(parser, 1u);
        if (!buffer.data) return 0;
        buffer.data[0] = '\0';
        buffer.capacity = 1u;
    }
    parser->normalized_input = (uint8_t *)buffer.data;
    parser->input = parser->normalized_input;
    parser->size = buffer.size;
    return 1;
}

typedef struct QwXmlEncoding {
    const char *iconv_name;
    unsigned int windows_code_page;
    int generic_utf16;
    int generic_utf32;
} QwXmlEncoding;

static int qwx_encoding_from_name(const char *name, QwXmlEncoding *encoding)
{
    char normalized[64];
    size_t source = 0u;
    size_t target = 0u;
    if (!name || !encoding) return 0;
    encoding->generic_utf16 = 0;
    encoding->generic_utf32 = 0;
    while (name[source] && target + 1u < sizeof(normalized)) {
        unsigned char c = (unsigned char)name[source++];
        if (c == '-' || c == '_' || c == ' ' || c == '.') continue;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        normalized[target++] = (char)c;
    }
    if (name[source] || target == 0u) return 0;
    normalized[target] = '\0';
    if (strcmp(normalized, "utf8") == 0) {
        encoding->iconv_name = "UTF-8";
        encoding->windows_code_page = 65001u;
    } else if (strcmp(normalized, "utf16") == 0) {
        encoding->iconv_name = "UTF-16BE";
        encoding->windows_code_page = 1201u;
        encoding->generic_utf16 = 1;
    } else if (strcmp(normalized, "utf16be") == 0) {
        encoding->iconv_name = "UTF-16BE";
        encoding->windows_code_page = 1201u;
    } else if (strcmp(normalized, "utf16le") == 0) {
        encoding->iconv_name = "UTF-16LE";
        encoding->windows_code_page = 1200u;
    } else if (strcmp(normalized, "utf32") == 0) {
        encoding->iconv_name = "UTF-32LE";
        encoding->windows_code_page = 12000u;
        encoding->generic_utf32 = 1;
    } else if (strcmp(normalized, "utf32be") == 0) {
        encoding->iconv_name = "UTF-32BE";
        encoding->windows_code_page = 12001u;
    } else if (strcmp(normalized, "utf32le") == 0) {
        encoding->iconv_name = "UTF-32LE";
        encoding->windows_code_page = 12000u;
    } else if (strcmp(normalized, "big5") == 0 ||
               strcmp(normalized, "cnbig5") == 0 ||
               strcmp(normalized, "csbig5") == 0 ||
               strcmp(normalized, "xxbig5") == 0 ||
               strcmp(normalized, "big5hkscs") == 0) {
        encoding->iconv_name = "BIG5";
        encoding->windows_code_page = 950u;
    } else if (strcmp(normalized, "euccn") == 0) {
        encoding->iconv_name = "EUC-CN";
        encoding->windows_code_page = 51936u;
    } else if (strcmp(normalized, "eucjp") == 0 ||
               strcmp(normalized, "xeucjp") == 0 ||
               strcmp(normalized, "cseucpkdfmtjapanese") == 0) {
        encoding->iconv_name = "EUC-JP";
        encoding->windows_code_page = 51932u;
    } else if (strcmp(normalized, "euckr") == 0 ||
               strcmp(normalized, "cseuckr") == 0 ||
               strcmp(normalized, "csksc56011987") == 0 ||
               strcmp(normalized, "isoir149") == 0 ||
               strcmp(normalized, "korean") == 0 ||
               strcmp(normalized, "ksc5601") == 0 ||
               strcmp(normalized, "windows949") == 0) {
        encoding->iconv_name = "EUC-KR";
        encoding->windows_code_page = 51949u;
    } else if (strcmp(normalized, "euctw") == 0) {
        encoding->iconv_name = "EUC-TW";
        encoding->windows_code_page = 51950u;
    } else if (strcmp(normalized, "gb18030") == 0 ||
               strcmp(normalized, "gb180302000") == 0 ||
               strcmp(normalized, "gbk") == 0 ||
               strcmp(normalized, "gb2312") == 0 ||
               strcmp(normalized, "chinese") == 0 ||
               strcmp(normalized, "csgb2312") == 0 ||
               strcmp(normalized, "csiso58gb231280") == 0 ||
               strcmp(normalized, "isoir58") == 0 ||
               strcmp(normalized, "xgbk") == 0) {
        encoding->iconv_name = "GB18030";
        encoding->windows_code_page = 54936u;
    } else if (strcmp(normalized, "ascii") == 0 ||
               strcmp(normalized, "usascii") == 0 ||
               strcmp(normalized, "ansix341968") == 0) {
        encoding->iconv_name = "US-ASCII";
        encoding->windows_code_page = 20127u;
    } else if (strcmp(normalized, "iso88591") == 0 ||
               strcmp(normalized, "latin1") == 0 ||
               strcmp(normalized, "latin") == 0 ||
               strcmp(normalized, "l1") == 0) {
        encoding->iconv_name = "ISO-8859-1";
        encoding->windows_code_page = 28591u;
    } else if (strcmp(normalized, "iso88597") == 0 ||
               strcmp(normalized, "greek") == 0 ||
               strcmp(normalized, "greek8") == 0) {
        encoding->iconv_name = "ISO-8859-7";
        encoding->windows_code_page = 28597u;
    } else if (strcmp(normalized, "koi8r") == 0) {
        encoding->iconv_name = "KOI8-R";
        encoding->windows_code_page = 20866u;
    } else if (strcmp(normalized, "koi") == 0 ||
               strcmp(normalized, "koi8") == 0 ||
               strcmp(normalized, "koi8ru") == 0 ||
               strcmp(normalized, "koi8u") == 0) {
        encoding->iconv_name = "KOI8-R";
        encoding->windows_code_page = 20866u;
    } else if (strcmp(normalized, "shiftjis") == 0 ||
               strcmp(normalized, "sjis") == 0 ||
               strcmp(normalized, "mskanji") == 0 ||
               strcmp(normalized, "cp932") == 0 ||
               strcmp(normalized, "ms932") == 0 ||
               strcmp(normalized, "csshiftjis") == 0 ||
               strcmp(normalized, "windows31j") == 0 ||
               strcmp(normalized, "xsjis") == 0) {
        encoding->iconv_name = "SHIFT_JIS";
        encoding->windows_code_page = 932u;
    } else if (strcmp(normalized, "windows1250") == 0 ||
               strcmp(normalized, "cp1250") == 0 ||
               strcmp(normalized, "xcp1250") == 0) {
        encoding->iconv_name = "WINDOWS-1250";
        encoding->windows_code_page = 1250u;
    } else if (strcmp(normalized, "windows1251") == 0 ||
               strcmp(normalized, "cp1251") == 0 ||
               strcmp(normalized, "xcp1251") == 0) {
        encoding->iconv_name = "WINDOWS-1251";
        encoding->windows_code_page = 1251u;
    } else if (strcmp(normalized, "windows1252") == 0 ||
               strcmp(normalized, "cp1252") == 0 ||
               strcmp(normalized, "xcp1252") == 0 ||
               strcmp(normalized, "cp819") == 0) {
        encoding->iconv_name = "WINDOWS-1252";
        encoding->windows_code_page = 1252u;
    } else {
        return 0;
    }
    return 1;
}

#if defined(_WIN32)
static int qwx_convert_to_utf8(QwXmlParser *parser, const uint8_t *data,
                               size_t size, const QwXmlEncoding *encoding)
{
    int wide_count;
    int converted_count;
    DWORD flags = MB_ERR_INVALID_CHARS;
    WCHAR *wide;
    size_t i;
    QwXmlBuffer output = {0};
    if (size > INT_MAX) {
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "Encoded XML input is too large for conversion");
    }
    if (encoding->windows_code_page == 20127u) {
        for (i = 0u; i < size; ++i) {
            if (data[i] >= 0x80u) {
                return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                                "US-ASCII XML contains a non-ASCII byte");
            }
        }
    }
    wide_count = MultiByteToWideChar(encoding->windows_code_page, flags,
        (LPCCH)data, (int)size, NULL, 0);
    if (!wide_count && GetLastError() == ERROR_INVALID_FLAGS) {
        flags = 0u;
        wide_count = MultiByteToWideChar(encoding->windows_code_page, flags,
            (LPCCH)data, (int)size, NULL, 0);
    }
    if (!wide_count || (size_t)wide_count > SIZE_MAX / sizeof(*wide)) {
        return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                        "XML encoding is not supported or contains invalid bytes");
    }
    wide = (WCHAR *)malloc((size_t)wide_count * sizeof(*wide));
    if (!wide) {
        return qwx_fail(parser, QW_XML_ERROR_OUT_OF_MEMORY,
                        "Could not allocate XML conversion buffer");
    }
    converted_count = MultiByteToWideChar(encoding->windows_code_page, flags,
        (LPCCH)data, (int)size, wide, wide_count);
    if (converted_count != wide_count) {
        free(wide);
        return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                        "XML encoding conversion failed");
    }
    for (i = 0u; i < (size_t)wide_count; ++i) {
        uint32_t cp = wide[i];
        if (cp >= 0xd800u && cp <= 0xdbffu) {
            uint32_t low;
            if (++i >= (size_t)wide_count || wide[i] < 0xdc00u ||
                wide[i] > 0xdfffu) {
                free(wide);
                qwx_release(parser, output.data, output.capacity);
                return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                                "XML conversion produced an invalid surrogate pair");
            }
            low = wide[i];
            cp = 0x10000u + ((cp - 0xd800u) << 10) + (low - 0xdc00u);
        } else if (cp >= 0xdc00u && cp <= 0xdfffu) {
            free(wide);
            qwx_release(parser, output.data, output.capacity);
            return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                            "XML conversion produced an unpaired low surrogate");
        }
        if (!qwx_append_codepoint(parser, &output, cp)) {
            free(wide);
            qwx_release(parser, output.data, output.capacity);
            return 0;
        }
    }
    free(wide);
    if (!output.data) {
        output.data = (char *)qwx_allocate(parser, 1u);
        if (!output.data) return 0;
        output.capacity = 1u;
        output.data[0] = '\0';
    }
    if (output.capacity != output.size + 1u) {
        char *exact = (char *)qwx_reallocate(parser, output.data,
            output.capacity, output.size + 1u);
        if (!exact) {
            qwx_release(parser, output.data, output.capacity);
            return 0;
        }
        output.data = exact;
    }
    parser->normalized_input = (uint8_t *)output.data;
    parser->input = parser->normalized_input;
    parser->size = output.size;
    return 1;
}
#else
static int qwx_convert_to_utf8(QwXmlParser *parser, const uint8_t *data,
                               size_t size, const QwXmlEncoding *encoding)
{
    iconv_t converter;
    char *input = (char *)(uintptr_t)data;
    size_t input_left = size;
    QwXmlBuffer output = {0};
    converter = iconv_open("UTF-8", encoding->iconv_name);
    if (converter == (iconv_t)-1) {
        return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                        "XML encoding is not supported by this build");
    }
    for (;;) {
        char *output_at;
        size_t output_left;
        size_t converted;
        if (!qwx_buffer_reserve(parser, &output,
                               output.size + 65u)) goto failure;
        output_at = output.data + output.size;
        output_left = output.capacity - output.size - 1u;
        converted = iconv(converter, input_left ? &input : NULL,
                          input_left ? &input_left : NULL,
                          &output_at, &output_left);
        output.size = (size_t)(output_at - output.data);
        if (converted != (size_t)-1) {
            if (input_left == 0u) {
                char *flush_at;
                size_t flush_left;
                if (!qwx_buffer_reserve(parser, &output,
                                       output.size + 65u)) goto failure;
                flush_at = output.data + output.size;
                flush_left = output.capacity - output.size - 1u;
                converted = iconv(converter, NULL, NULL,
                                  &flush_at, &flush_left);
                output.size = (size_t)(flush_at - output.data);
                if (converted != (size_t)-1) break;
                if (errno != E2BIG) goto invalid_input;
            }
            continue;
        }
        if (errno == E2BIG) continue;
        goto invalid_input;
    }
    iconv_close(converter);
    if (!output.data) {
        output.data = (char *)qwx_allocate(parser, 1u);
        if (!output.data) return 0;
        output.capacity = 1u;
    }
    output.data[output.size] = '\0';
    parser->normalized_input = (uint8_t *)output.data;
    parser->input = parser->normalized_input;
    parser->size = output.size;
    return 1;

invalid_input:
    qwx_fail(parser, QW_XML_ERROR_ENCODING,
             "XML encoding contains invalid or incomplete byte sequences");
failure:
    iconv_close(converter);
    qwx_release(parser, output.data, output.capacity);
    return 0;
}
#endif

static int qwx_extract_encoding(const uint8_t *data, size_t start, size_t end,
                                const char *key,
                                char *name,
                                size_t name_capacity)
{
    size_t i;
    for (i = start; i < end; ++i) {
        size_t cursor;
        size_t count;
        size_t key_size = strlen(key);
        if (i == start || (data[i - 1u] != ' ' && data[i - 1u] != '\t' &&
                           data[i - 1u] != '\r' && data[i - 1u] != '\n'))
            continue;
        if (key_size > end - i || memcmp(data + i, key, key_size) != 0)
            continue;
        cursor = i + key_size;
        while (cursor < end && (data[cursor] == ' ' || data[cursor] == '\t' ||
               data[cursor] == '\r' || data[cursor] == '\n')) ++cursor;
        if (cursor >= end || data[cursor++] != '=') continue;
        while (cursor < end && (data[cursor] == ' ' || data[cursor] == '\t' ||
               data[cursor] == '\r' || data[cursor] == '\n')) ++cursor;
        if (cursor >= end || (data[cursor] != '\'' && data[cursor] != '"'))
            continue;
        {
            uint8_t quote = data[cursor++];
            size_t value_start = cursor;
            while (cursor < end && data[cursor] != quote) ++cursor;
            count = cursor - value_start;
            if (cursor == end || count == 0u || count >= name_capacity)
                return 0;
            memcpy(name, data + value_start, count);
            name[count] = '\0';
            return 1;
        }
    }
    return 0;
}

static int qwx_find_input_encoding(const uint8_t *data, size_t size,
                                   char *name, size_t name_capacity)
{
    size_t end = 6u;
    if (size < 6u || memcmp(data, "<?xml", 5u) != 0 ||
        (data[5] != ' ' && data[5] != '\t' &&
         data[5] != '\r' && data[5] != '\n')) return 0;
    while (end + 1u < size &&
           !(data[end] == '?' && data[end + 1u] == '>')) ++end;
    if (end + 1u >= size) return 0;
    if (qwx_extract_encoding(data, 6u, end, "encoding",
                             name, name_capacity)) return 1;
    return 0;
}

static int qwx_prepare_input(QwXmlParser *parser, const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t position = 0u;
    char declared_name[64];
    QwXmlEncoding encoding;
    int has_declared_encoding;
    if (!data || !size) {
        return qwx_fail(parser, QW_XML_ERROR_ARGUMENT,
                        "XML input is empty");
    }
    if (size > QW_XML_MAX_INPUT_BYTES) {
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML input exceeds the parser limit");
    }
    if (size >= 4u && bytes[0] == 0x00u && bytes[1] == 0x00u &&
        bytes[2] == 0xfeu && bytes[3] == 0xffu) {
        parser->source_code_page = 12001u;
        if (!qwx_normalize_utf32(parser, bytes, size, 0, 4u)) return 0;
    } else if (size >= 4u && bytes[0] == 0xffu && bytes[1] == 0xfeu &&
               bytes[2] == 0x00u && bytes[3] == 0x00u) {
        parser->source_code_page = 12000u;
        if (!qwx_normalize_utf32(parser, bytes, size, 1, 4u)) return 0;
    } else if (size >= 4u && bytes[0] == 0x00u && bytes[1] == 0x00u &&
               bytes[2] == 0x00u && bytes[3] == '<') {
        parser->source_code_page = 12001u;
        if (!qwx_normalize_utf32(parser, bytes, size, 0, 0u)) return 0;
    } else if (size >= 4u && bytes[0] == '<' && bytes[1] == 0x00u &&
               bytes[2] == 0x00u && bytes[3] == 0x00u) {
        parser->source_code_page = 12000u;
        if (!qwx_normalize_utf32(parser, bytes, size, 1, 0u)) return 0;
    } else if (size >= 2u && bytes[0] == 0xffu && bytes[1] == 0xfeu) {
        parser->source_code_page = 1200u;
        if (!qwx_normalize_utf16(parser, bytes, size, 1, 2u)) return 0;
    } else if (size >= 2u && bytes[0] == 0xfeu && bytes[1] == 0xffu) {
        parser->source_code_page = 1201u;
        if (!qwx_normalize_utf16(parser, bytes, size, 0, 2u)) return 0;
    } else if (size >= 4u && bytes[0] == '<' && bytes[1] == 0u &&
               bytes[3] == 0u) {
        parser->source_code_page = 1200u;
        if (!qwx_normalize_utf16(parser, bytes, size, 1, 0u)) return 0;
    } else if (size >= 4u && bytes[0] == 0u && bytes[1] == '<' &&
               bytes[2] == 0u) {
        parser->source_code_page = 1201u;
        if (!qwx_normalize_utf16(parser, bytes, size, 0, 0u)) return 0;
    } else {
        if (size >= 3u && bytes[0] == 0xefu && bytes[1] == 0xbbu &&
            bytes[2] == 0xbfu) position = 3u;
        parser->source_code_page = 65001u;
        parser->input = bytes + position;
        parser->size = size - position;
        has_declared_encoding = position == 0u &&
            qwx_find_input_encoding(parser->input, parser->size,
                                    declared_name, sizeof(declared_name));
        if (has_declared_encoding) {
            if (!qwx_encoding_from_name(declared_name, &encoding)) {
                return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                                "XML encoding declaration is not supported");
            }
            parser->source_code_page = encoding.windows_code_page;
            if (encoding.windows_code_page == 1200u ||
                encoding.windows_code_page == 1201u) {
                if (!qwx_normalize_utf16(parser, parser->input, parser->size,
                        encoding.windows_code_page == 1200u, 0u)) return 0;
            } else if (encoding.windows_code_page == 12000u ||
                       encoding.windows_code_page == 12001u) {
                if (!qwx_normalize_utf32(parser, parser->input, parser->size,
                        encoding.windows_code_page == 12000u, 0u)) return 0;
            } else if (encoding.windows_code_page != 65001u) {
                if (!qwx_convert_to_utf8(parser, parser->input,
                                         parser->size, &encoding)) return 0;
            }
        }
    }
    position = 0u;
    while (position < parser->size) {
        size_t next = position;
        uint32_t cp;
        if (!qwx_decode_utf8(parser->input, parser->size, &next, &cp) ||
            !qwx_xml_character(cp)) {
            return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                            "XML input is not valid UTF-8 or contains invalid characters");
        }
        position = next;
    }
    return 1;
}

static int qwx_encoding_name_valid(const char *name);
static int qwx_xml_version_number_valid(const char *version);
static int qwx_read_xml_pseudo_attribute(QwXmlParser *parser,
                                         char *name, size_t name_capacity,
                                         char *value, size_t value_capacity);

static int qwx_skip_external_text_declaration(QwXmlParser *parser)
{
    char name[16];
    char value[64];
    QwXmlEncoding encoding;
    size_t before_space;
    if (!qwx_starts(parser, "<?xml") || parser->size < 6u ||
        !(parser->input[5] == ' ' || parser->input[5] == '\t' ||
          parser->input[5] == '\r' || parser->input[5] == '\n')) return 1;
    parser->position += 5u;
    before_space = parser->position;
    qwx_skip_space(parser);
    if (parser->position == before_space ||
        !qwx_read_xml_pseudo_attribute(parser, name, sizeof(name),
                                       value, sizeof(value)))
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "External XML text declaration is malformed");
    if (strcmp(name, "version") == 0) {
        if (!qwx_xml_version_number_valid(value))
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "External XML text declaration has an invalid version");
        before_space = parser->position;
        qwx_skip_space(parser);
        if (parser->position == before_space ||
            !qwx_read_xml_pseudo_attribute(parser, name, sizeof(name),
                                           value, sizeof(value)))
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "External XML text declaration requires an encoding");
    }
    if (strcmp(name, "encoding") != 0 ||
        !qwx_encoding_name_valid(value) ||
        !qwx_encoding_from_name(value, &encoding))
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "External XML text declaration has an invalid encoding");
    if (encoding.windows_code_page != parser->source_code_page &&
        !(encoding.generic_utf16 &&
          (parser->source_code_page == 1200u ||
           parser->source_code_page == 1201u)) &&
        !(encoding.generic_utf32 &&
          (parser->source_code_page == 12000u ||
           parser->source_code_page == 12001u)))
        return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                        "External XML text declaration disagrees with its byte encoding");
    qwx_skip_space(parser);
    if (!qwx_starts(parser, "?>"))
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "External XML text declaration has trailing syntax");
    parser->position += 2u;
    return 1;
}

static int qwx_get_external_text(QwXmlParser *parser,
                                const QwXmlEntity *entity,
                                char **out_text, size_t *out_size,
                                const char **out_base_uri)
{
    static const uint8_t empty_resource[1] = {0u};
    const QwXmlParseOptions *options = parser->options;
    const uint8_t *resource = NULL;
    size_t resource_size = 0u;
    const char *resolved_base_uri = NULL;
    char *base_uri_copy = NULL;
    QwXmlParser input_parser;
    size_t text_size;
    int ok = 0;
    *out_text = NULL;
    *out_size = 0u;
    if (out_base_uri) *out_base_uri = NULL;
    if (!options || !options->resolve_entity)
        return qwx_fail(parser, QW_XML_ERROR_UNSUPPORTED,
                        "External XML parameter entities require a resolver");
    if (!options->resolve_entity(options->resolver_context,
            entity->public_id, entity->system_id, entity->base_uri,
            &resource, &resource_size, &resolved_base_uri) ||
        (resource_size && !resource))
        return qwx_fail(parser, QW_XML_ERROR_UNSUPPORTED,
                        "External XML parameter entity could not be resolved");
    if (resource_size > QW_XML_MAX_INPUT_BYTES ||
        resource_size > QW_XML_MAX_INPUT_BYTES - parser->external_loaded_bytes) {
        if (options->release_entity && resource)
            options->release_entity(options->resolver_context, resource,
                                    resource_size);
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML external resource budget exceeded");
    }
    parser->external_loaded_bytes += resource_size;
    memset(&input_parser, 0, sizeof(input_parser));
    input_parser.error = parser->error;
    if (!resource_size) {
        input_parser.input = empty_resource;
        input_parser.size = 0u;
        input_parser.source_code_page = 65001u;
    } else if (!qwx_prepare_input(&input_parser, resource, resource_size)) {
        free(input_parser.normalized_input);
        if (options->release_entity && resource)
            options->release_entity(options->resolver_context, resource,
                                    resource_size);
        return 0;
    }
    if (!qwx_skip_external_text_declaration(&input_parser)) goto cleanup;
    text_size = input_parser.size - input_parser.position;
    *out_text = qwx_duplicate(parser,
        (const char *)input_parser.input + input_parser.position, text_size);
    if (!*out_text) goto cleanup;
    if (out_base_uri && resolved_base_uri) {
        base_uri_copy = qwx_duplicate(parser, resolved_base_uri,
                                      strlen(resolved_base_uri));
        if (!base_uri_copy) {
            qwx_release(parser, *out_text, text_size + 1u);
            *out_text = NULL;
            goto cleanup;
        }
        *out_base_uri = base_uri_copy;
        base_uri_copy = NULL;
    }
    *out_size = text_size;
    ok = 1;

cleanup:
    qwx_release(parser, base_uri_copy,
                base_uri_copy ? strlen(base_uri_copy) + 1u : 0u);
    free(input_parser.normalized_input);
    if (options->release_entity && resource)
        options->release_entity(options->resolver_context, resource,
                                resource_size);
    return ok;
}

static int qwx_parse_external_resource(QwXmlParser *parser,
                                       const char *public_id,
                                       const char *system_id,
                                       const char *base_uri,
                                       int content_entity)
{
    static const uint8_t empty_resource[1] = {0u};
    const QwXmlParseOptions *options = parser->options;
    const uint8_t *resource = NULL;
    size_t resource_size = 0u;
    const char *resolved_base_uri = NULL;
    QwXmlParser input_parser;
    const uint8_t *saved_input = parser->input;
    const uint32_t *saved_input_origins = parser->input_origin_ids;
    size_t saved_size = parser->size;
    size_t saved_position = parser->position;
    const char *saved_base_uri = parser->current_base_uri;
    QwXmlNode *saved_current = parser->current;
    size_t saved_depth = parser->depth;
    unsigned saved_entity_expansion_depth = parser->entity_expansion_depth;
    unsigned saved_source_code_page = parser->source_code_page;
    int saved_external_subset = parser->in_external_subset;
    uint32_t saved_input_origin_id = parser->input_origin_id;
    uint32_t external_origin_id;
    int ok = 0;
    if (!options || !options->resolve_entity)
        return qwx_fail(parser, QW_XML_ERROR_UNSUPPORTED,
                        "External XML resources require a resolver");
    if (parser->entity_expansion_depth >= QW_XML_MAX_ENTITY_DEPTH)
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML external entity nesting limit exceeded");
    if (!system_id)
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "External XML identifier has no system identifier");
    if (!options->resolve_entity(options->resolver_context, public_id,
            system_id, base_uri ? base_uri : options->base_uri,
            &resource, &resource_size, &resolved_base_uri) ||
        (resource_size && !resource)) {
        return qwx_fail(parser, QW_XML_ERROR_UNSUPPORTED,
                        "XML external resource could not be resolved");
    }
    if (resource_size > QW_XML_MAX_INPUT_BYTES ||
        resource_size > QW_XML_MAX_INPUT_BYTES - parser->external_loaded_bytes) {
        if (options->release_entity)
            options->release_entity(options->resolver_context, resource,
                                    resource_size);
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML external resource budget exceeded");
    }
    parser->external_loaded_bytes += resource_size;
    memset(&input_parser, 0, sizeof(input_parser));
    input_parser.error = parser->error;
    if (!resource_size) {
        input_parser.input = empty_resource;
        input_parser.size = 0u;
        input_parser.source_code_page = 65001u;
    } else if (!qwx_prepare_input(&input_parser, resource, resource_size)) {
        free(input_parser.normalized_input);
        if (options->release_entity)
            options->release_entity(options->resolver_context, resource,
                                    resource_size);
        return 0;
    }
    external_origin_id = parser->in_parameter_entity
        ? qwx_next_parameter_entity_origin(parser) : 0u;
    if (parser->in_parameter_entity && !external_origin_id) {
        free(input_parser.normalized_input);
        if (options->release_entity)
            options->release_entity(options->resolver_context, resource,
                                    resource_size);
        return 0;
    }
    parser->input = input_parser.input;
    parser->input_origin_ids = NULL;
    parser->input_origin_id = external_origin_id;
    parser->size = input_parser.size;
    parser->position = 0u;
    parser->source_code_page = input_parser.source_code_page;
    parser->current_base_uri = resolved_base_uri
        ? resolved_base_uri : (base_uri ? base_uri : options->base_uri);
    if (!content_entity) parser->in_external_subset = 1;
    ++parser->entity_expansion_depth;
    if (qwx_skip_external_text_declaration(parser)) {
        if (content_entity) {
            ok = qwx_parse_content(parser);
            if (ok && (parser->current != saved_current ||
                       parser->depth != saved_depth))
                ok = qwx_fail(parser, QW_XML_ERROR_FORMAT,
                              "External XML entity replacement is not balanced");
        } else {
            ok = qwx_parse_dtd_subset(parser, 0);
        }
    }
    parser->input = saved_input;
    parser->input_origin_ids = saved_input_origins;
    parser->input_origin_id = saved_input_origin_id;
    parser->size = saved_size;
    parser->position = saved_position;
    parser->current_base_uri = saved_base_uri;
    parser->current = saved_current;
    parser->depth = saved_depth;
    parser->source_code_page = saved_source_code_page;
    parser->in_external_subset = saved_external_subset;
    parser->entity_expansion_depth = saved_entity_expansion_depth;
    free(input_parser.normalized_input);
    if (options->release_entity)
        options->release_entity(options->resolver_context, resource,
                                resource_size);
    return ok;
}

static int qwx_ascii_case_equal(const char *left, const char *right)
{
    size_t i;
    if (!left || !right || strlen(left) != strlen(right)) return 0;
    for (i = 0u; left[i]; ++i) {
        unsigned char a = (unsigned char)left[i];
        unsigned char b = (unsigned char)right[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return 0;
    }
    return 1;
}

static int qwx_encoding_name_valid(const char *name)
{
    size_t i;
    if (!name || !((name[0] >= 'A' && name[0] <= 'Z') ||
                   (name[0] >= 'a' && name[0] <= 'z'))) return 0;
    for (i = 1u; name[i]; ++i) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

static int qwx_read_xml_pseudo_attribute(QwXmlParser *parser,
                                         char *name, size_t name_capacity,
                                         char *value, size_t value_capacity)
{
    char *parsed_name = NULL;
    size_t name_size;
    size_t value_start;
    size_t value_size;
    uint8_t quote;
    if (!qwx_parse_name(parser, &parsed_name)) return 0;
    name_size = strlen(parsed_name);
    if (!name_size || name_size >= name_capacity) {
        qwx_release(parser, parsed_name, name_size + 1u);
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML declaration attribute name is too long");
    }
    memcpy(name, parsed_name, name_size + 1u);
    qwx_release(parser, parsed_name, name_size + 1u);
    qwx_skip_space(parser);
    if (parser->position >= parser->size ||
        parser->input[parser->position++] != '=') {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML declaration attribute is missing '='");
    }
    qwx_skip_space(parser);
    if (parser->position >= parser->size ||
        (parser->input[parser->position] != '\'' &&
         parser->input[parser->position] != '"')) {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML declaration attribute value must be quoted");
    }
    quote = parser->input[parser->position++];
    value_start = parser->position;
    while (parser->position < parser->size &&
           parser->input[parser->position] != quote) {
        uint8_t c = parser->input[parser->position];
        if (c >= 0x80u || c == '&' || c == '<' ||
            c == '\t' || c == '\r' || c == '\n') {
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML declaration contains an invalid pseudo-attribute value");
        }
        ++parser->position;
    }
    if (parser->position >= parser->size) {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML declaration attribute value is not terminated");
    }
    value_size = parser->position - value_start;
    if (!value_size || value_size >= value_capacity) {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML declaration attribute value is empty or too long");
    }
    memcpy(value, parser->input + value_start, value_size);
    value[value_size] = '\0';
    ++parser->position;
    return 1;
}

static int qwx_xml_version_number_valid(const char *version)
{
    const unsigned char *cursor = (const unsigned char *)version;
    if (!cursor || cursor[0] != '1' || cursor[1] != '.') return 0;
    cursor += 2u;
    if (*cursor < '0' || *cursor > '9') return 0;
    for (; *cursor; ++cursor)
        if (*cursor < '0' || *cursor > '9') return 0;
    return 1;
}

static int qwx_parse_xml_declaration(QwXmlParser *parser)
{
    char name[16];
    char value[64];
    size_t before_space;
    int encoding_seen = 0;
    int standalone_seen = 0;
    QwXmlEncoding encoding;

    before_space = parser->position;
    qwx_skip_space(parser);
    if (parser->position == before_space ||
        !qwx_read_xml_pseudo_attribute(parser, name, sizeof(name),
                                       value, sizeof(value)) ||
        strcmp(name, "version") != 0) {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML declaration must begin with a version attribute");
    }
    if (!qwx_xml_version_number_valid(value)) {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML version number must have the form 1.x");
    }
    for (;;) {
        before_space = parser->position;
        qwx_skip_space(parser);
        if (qwx_starts(parser, "?>")) {
            parser->position += 2u;
            parser->xml_declaration_seen = 1;
            return 1;
        }
        if (parser->position == before_space) {
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML declaration attributes must be separated by whitespace");
        }
        if (!qwx_read_xml_pseudo_attribute(parser, name, sizeof(name),
                                           value, sizeof(value))) return 0;
        if (strcmp(name, "encoding") == 0) {
            int compatible;
            if (encoding_seen || standalone_seen ||
                !qwx_encoding_name_valid(value) ||
                !qwx_encoding_from_name(value, &encoding)) {
                return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                                "XML encoding declaration is invalid or unsupported");
            }
            compatible = encoding.generic_utf16
                ? (parser->source_code_page == 1200u ||
                   parser->source_code_page == 1201u)
                : encoding.generic_utf32
                    ? (parser->source_code_page == 12000u ||
                       parser->source_code_page == 12001u)
                    : encoding.windows_code_page == parser->source_code_page;
            if (!compatible) {
                return qwx_fail(parser, QW_XML_ERROR_ENCODING,
                                "XML encoding declaration does not match the input bytes");
            }
            encoding_seen = 1;
        } else if (strcmp(name, "standalone") == 0) {
            if (standalone_seen ||
                (strcmp(value, "yes") != 0 && strcmp(value, "no") != 0)) {
                return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                                "XML standalone declaration must be 'yes' or 'no'");
            }
            standalone_seen = 1;
            parser->standalone_declared = 1;
            parser->standalone_yes = strcmp(value, "yes") == 0;
        } else {
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML declaration contains an unknown attribute");
        }
    }
}

static int qwx_parse_processing_instruction(QwXmlParser *parser, int preserve)
{
    size_t start = parser->position;
    size_t data_start;
    size_t data_end;
    char *target = NULL;
    int xml_target;
    parser->position += 2u;
    if (!qwx_parse_name(parser, &target)) return 0;
    xml_target = qwx_ascii_case_equal(target, "xml");
    if (xml_target) {
        int exact_xml = strcmp(target, "xml") == 0;
        qwx_release(parser, target, strlen(target) + 1u);
        if (!exact_xml) {
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "Processing instruction target 'xml' is reserved");
        }
        if (start != 0u || parser->xml_declaration_seen ||
            parser->document_type_seen || parser->document_element_count ||
            parser->current) {
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML declaration is not at the start of the document");
        }
        return qwx_parse_xml_declaration(parser);
    }
    if (parser->position < parser->size &&
        parser->input[parser->position] != '?' &&
        parser->input[parser->position] != ' ' &&
        parser->input[parser->position] != '\t' &&
        parser->input[parser->position] != '\r' &&
        parser->input[parser->position] != '\n') {
        qwx_release(parser, target, strlen(target) + 1u);
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "Processing instruction target must be separated from its data");
    }
    if (parser->position < parser->size &&
        parser->input[parser->position] == '?' &&
        !qwx_starts(parser, "?>")) {
        qwx_release(parser, target, strlen(target) + 1u);
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "Processing instruction data must be separated from its target");
    }
    if (!preserve) {
        qwx_release(parser, target, strlen(target) + 1u);
        return qwx_skip_until(parser, "?>",
                              "XML processing instruction is not terminated");
    }
    qwx_skip_space(parser);
    data_start = parser->position;
    while (parser->position + 1u < parser->size &&
           !qwx_starts(parser, "?>")) ++parser->position;
    if (parser->position + 1u >= parser->size) {
        qwx_release(parser, target, strlen(target) + 1u);
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML processing instruction is not terminated");
    }
    data_end = parser->position;
    parser->position += 2u;
    {
        char *data = NULL;
        size_t data_size = 0u;
        if (!qwx_copy_normalized_range(parser, data_start, data_end,
                                       &data, &data_size)) {
            qwx_release(parser, target, strlen(target) + 1u);
            return 0;
        }
        return qwx_add_misc_node(parser,
            QW_XML_NODE_PROCESSING_INSTRUCTION,
            target, data, data_size);
    }
}

static int qwx_parse_content(QwXmlParser *parser);

static int qwx_parse_content_text(QwXmlParser *parser)
{
    QwXmlBuffer text = {0};
    int whitespace_only = 1;
    size_t start = parser->position;
    while (parser->position < parser->size &&
           parser->input[parser->position] != '<' &&
           parser->input[parser->position] != '&') {
        if (qwx_starts(parser, "]]>")) {
            qwx_fail(parser, QW_XML_ERROR_FORMAT,
                     "XML character data contains ']]>'");
            goto failure;
        }
        size_t cursor;
        uint32_t cp;
        if (parser->input[parser->position] == '\r') {
            ++parser->position;
            if (parser->position < parser->size &&
                parser->input[parser->position] == '\n') ++parser->position;
            if (!qwx_buffer_append(parser, &text, "\n", 1u)) goto failure;
            continue;
        }
        cursor = parser->position;
        if (!qwx_decode_utf8(parser->input, parser->size, &cursor, &cp) ||
            !qwx_xml_character(cp)) {
            qwx_fail(parser, QW_XML_ERROR_ENCODING,
                     "XML text contains invalid UTF-8 or a forbidden character");
            goto failure;
        }
        if (cp != ' ' && cp != '\t' && cp != '\n' && cp != '\r')
            whitespace_only = 0;
        if (!qwx_buffer_append(parser, &text,
                (const char *)parser->input + parser->position,
                cursor - parser->position)) goto failure;
        parser->position = cursor;
    }
    if (parser->position == start) {
        qwx_fail(parser, QW_XML_ERROR_FORMAT,
                 "XML text parser did not consume any input");
        goto failure;
    }
    if (text.size > QW_XML_MAX_ENTITY_BYTES - parser->text_bytes) {
        qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                 "XML decoded text limit exceeded");
        goto failure;
    }
    parser->text_bytes += text.size;
    if (!text.data) {
        text.data = (char *)qwx_allocate(parser, 1u);
        if (!text.data) goto failure;
        text.data[0] = '\0';
        text.capacity = 1u;
    }
    if (text.capacity != text.size + 1u) {
        char *exact = (char *)qwx_reallocate(parser, text.data,
            text.capacity, text.size + 1u);
        if (!exact) goto failure;
        text.data = exact;
        text.capacity = text.size + 1u;
    }
    return qwx_add_text(parser, text.data, whitespace_only, 0);

failure:
    qwx_release(parser, text.data, text.capacity);
    return 0;
}

static int qwx_parse_content_reference(QwXmlParser *parser)
{
    size_t reference_start = parser->position;
    size_t name_start;
    size_t name_size;
    QwXmlEntity *entity;
    uint32_t predefined;
    if (parser->input[parser->position] != '&') return 0;
    ++parser->position;
    name_start = parser->position;
    while (parser->position < parser->size &&
           parser->input[parser->position] != ';') ++parser->position;
    if (parser->position >= parser->size ||
        parser->input[parser->position] != ';' ||
        parser->position == name_start) {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML entity reference is not terminated");
    }
    name_size = parser->position - name_start;
    if (parser->input[name_start] == '#' ||
        qwx_named_entity((const char *)parser->input + name_start,
                         name_size, &predefined)) {
        QwXmlBuffer text = {0};
        char *value;
        size_t value_size;
        parser->position = reference_start;
        if (!qwx_append_reference(parser, &text, 0u, 0)) {
            qwx_release(parser, text.data, text.capacity);
            return 0;
        }
        if (text.capacity != text.size + 1u) {
            char *exact = (char *)qwx_reallocate(parser, text.data,
                text.capacity, text.size + 1u);
            if (!exact) {
                qwx_release(parser, text.data, text.capacity);
                return 0;
            }
            text.data = exact;
            text.capacity = text.size + 1u;
        }
        value = text.data;
        value_size = text.size;
        if (value_size > QW_XML_MAX_ENTITY_BYTES - parser->text_bytes) {
            qwx_release(parser, value, text.capacity);
            return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                            "XML decoded text limit exceeded");
        }
        parser->text_bytes += value_size;
        return qwx_add_text(parser, value, 0, 0);
    }
    entity = qwx_find_entity(parser,
        (const char *)parser->input + name_start, name_size, 0);
    if (!entity) {
        return qwx_fail(parser,
                        parser->external_subset_unread
                            ? QW_XML_ERROR_UNSUPPORTED
                            : QW_XML_ERROR_FORMAT,
                        "XML entity reference is undeclared");
    }
    if (entity->unparsed) {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "Unparsed XML entity cannot be referenced here");
    }
    if (parser->in_document_content) entity->referenced = 1;
    if (entity->external) {
        if (!parser->current)
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML entity references are not allowed outside the root element");
        if (!parser->options || !parser->options->resolve_entity)
            return qwx_fail(parser, QW_XML_ERROR_UNSUPPORTED,
                            "External XML entities require a resolver");
        ++parser->position;
        return qwx_parse_external_resource(parser, entity->public_id,
            entity->system_id, entity->base_uri,
            QW_XML_EXTERNAL_CONTENT);
    }
    if (!parser->current) {
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML entity references are not allowed outside the root element");
    }
    if (parser->entity_expansion_depth >= QW_XML_MAX_ENTITY_DEPTH ||
        entity->value_size > QW_XML_MAX_ENTITY_BYTES -
                             parser->entity_expanded_bytes) {
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "XML entity expansion limit exceeded");
    }
    ++parser->position;
    {
        const uint8_t *saved_input = parser->input;
        size_t saved_size = parser->size;
        size_t saved_position = parser->position;
        QwXmlNode *saved_current = parser->current;
        size_t saved_depth = parser->depth;
        QwXmlNode *expanded_current;
        size_t expanded_depth;
        int ok;
        parser->entity_expanded_bytes += entity->value_size;
        ++parser->entity_expansion_depth;
        parser->input = (const uint8_t *)entity->value;
        parser->size = entity->value_size;
        parser->position = 0u;
        ok = qwx_parse_content(parser);
        expanded_current = parser->current;
        expanded_depth = parser->depth;
        parser->input = saved_input;
        parser->size = saved_size;
        parser->position = saved_position;
        parser->current = saved_current;
        parser->depth = saved_depth;
        --parser->entity_expansion_depth;
        if (!ok) return 0;
        if (expanded_current != saved_current || expanded_depth != saved_depth) {
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "Internal XML entity replacement is not balanced");
        }
    }
    return 1;
}

static int qwx_parse_content(QwXmlParser *parser)
{
    while (parser->position < parser->size) {
        if (parser->input[parser->position] == '<') {
            if (!qwx_parse_markup(parser)) return 0;
        } else if (parser->input[parser->position] == '&') {
            if (!qwx_parse_content_reference(parser)) return 0;
        } else if (!qwx_parse_content_text(parser)) {
            return 0;
        }
    }
    return 1;
}

static int qwx_parse_document(QwXmlParser *parser)
{
    parser->in_document_content = 1;
    if (!qwx_parse_content(parser)) return 0;
    if (parser->current || parser->depth != 0u)
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML document has unclosed elements");
    if (parser->document_element_count != 1u)
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML document must contain exactly one root element");
    return 1;
}

typedef struct QwXmlModelCursor {
    const char *data;
    size_t size;
    size_t position;
} QwXmlModelCursor;

typedef struct QwXmlNfaState {
    int first_edge;
} QwXmlNfaState;

typedef struct QwXmlNfaEdge {
    int to;
    int next;
    size_t label_start;
    size_t label_size;
} QwXmlNfaEdge;

typedef struct QwXmlNfa {
    const char *model;
    size_t model_size;
    QwXmlNfaState *states;
    QwXmlNfaEdge *edges;
    size_t state_count;
    size_t state_capacity;
    size_t edge_count;
    size_t edge_capacity;
} QwXmlNfa;

typedef struct QwXmlValueSet {
    const char **values;
    size_t count;
    size_t capacity;
} QwXmlValueSet;

static void qwx_model_skip_space(QwXmlModelCursor *cursor)
{
    while (cursor->position < cursor->size &&
           (cursor->data[cursor->position] == ' ' ||
            cursor->data[cursor->position] == '\t' ||
            cursor->data[cursor->position] == '\r' ||
            cursor->data[cursor->position] == '\n')) ++cursor->position;
}

static int qwx_model_read_name(QwXmlModelCursor *cursor,
                               size_t *start, size_t *size)
{
    size_t position = cursor->position;
    size_t next = position;
    uint32_t cp;
    int first = 1;
    while (position < cursor->size) {
        next = position;
        if (!qwx_decode_utf8((const uint8_t *)cursor->data, cursor->size,
                             &next, &cp)) return 0;
        if (!(first ? qwx_name_start(cp) : qwx_name_char(cp))) break;
        first = 0;
        position = next;
    }
    if (first) return 0;
    *start = cursor->position;
    *size = position - cursor->position;
    cursor->position = position;
    return 1;
}

static int qwx_nfa_new_state(QwXmlParser *parser, QwXmlNfa *nfa)
{
    size_t index = nfa->state_count++;
    if (index >= nfa->state_capacity || index > (size_t)INT_MAX) {
        --nfa->state_count;
        qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                 "DTD content model is too large to validate");
        return -1;
    }
    nfa->states[index].first_edge = -1;
    return (int)index;
}

static int qwx_nfa_add_edge(QwXmlParser *parser, QwXmlNfa *nfa,
                            int from, int to, size_t label_start,
                            size_t label_size)
{
    QwXmlNfaEdge *edge;
    if (nfa->edge_count >= nfa->edge_capacity || from < 0 || to < 0) {
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "DTD content model edge limit exceeded");
    }
    edge = &nfa->edges[nfa->edge_count];
    edge->to = to;
    edge->label_start = label_start;
    edge->label_size = label_size;
    edge->next = nfa->states[from].first_edge;
    nfa->states[from].first_edge = (int)nfa->edge_count++;
    return 1;
}

static int qwx_nfa_parse_particle(QwXmlParser *parser, QwXmlNfa *nfa,
                                  QwXmlModelCursor *cursor,
                                  int *out_start, int *out_end);

static int qwx_nfa_parse_group(QwXmlParser *parser, QwXmlNfa *nfa,
                               QwXmlModelCursor *cursor,
                               int *out_start, int *out_end)
{
    int group_start;
    int group_end;
    int first_start;
    int first_end;
    int previous_end;
    int choice;
    size_t count = 1u;
    uint8_t separator;
    if (cursor->position >= cursor->size ||
        cursor->data[cursor->position++] != '(')
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "Stored DTD content model is malformed");
    group_start = qwx_nfa_new_state(parser, nfa);
    group_end = qwx_nfa_new_state(parser, nfa);
    if (group_start < 0 || group_end < 0) return 0;
    qwx_model_skip_space(cursor);
    if (!qwx_nfa_parse_particle(parser, nfa, cursor,
                                &first_start, &first_end)) return 0;
    qwx_model_skip_space(cursor);
    if (cursor->position >= cursor->size ||
        (cursor->data[cursor->position] != ',' &&
         cursor->data[cursor->position] != '|'))
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "Stored DTD content group has no separator");
    separator = (uint8_t)cursor->data[cursor->position++];
    choice = separator == '|';
    if (!qwx_nfa_add_edge(parser, nfa, group_start, first_start, 0u, 0u))
        return 0;
    if (choice && !qwx_nfa_add_edge(parser, nfa, first_end,
                                    group_end, 0u, 0u)) return 0;
    previous_end = first_end;
    for (;;) {
        int child_start;
        int child_end;
        qwx_model_skip_space(cursor);
        if (!qwx_nfa_parse_particle(parser, nfa, cursor,
                                    &child_start, &child_end)) return 0;
        ++count;
        if (choice) {
            if (!qwx_nfa_add_edge(parser, nfa, group_start,
                                  child_start, 0u, 0u) ||
                !qwx_nfa_add_edge(parser, nfa, child_end,
                                  group_end, 0u, 0u)) return 0;
        } else {
            if (!qwx_nfa_add_edge(parser, nfa, previous_end,
                                  child_start, 0u, 0u)) return 0;
            previous_end = child_end;
        }
        qwx_model_skip_space(cursor);
        if (cursor->position >= cursor->size)
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "Stored DTD content group is unterminated");
        if (cursor->data[cursor->position] == ')') {
            ++cursor->position;
            break;
        }
        if ((uint8_t)cursor->data[cursor->position++] != separator)
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "Stored DTD content group mixes separators");
    }
    if (count < 2u)
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "Stored DTD content group is incomplete");
    if (!choice && !qwx_nfa_add_edge(parser, nfa, previous_end,
                                     group_end, 0u, 0u)) return 0;
    *out_start = group_start;
    *out_end = group_end;
    return 1;
}

static int qwx_nfa_parse_particle(QwXmlParser *parser, QwXmlNfa *nfa,
                                  QwXmlModelCursor *cursor,
                                  int *out_start, int *out_end)
{
    int start;
    int end;
    if (cursor->position < cursor->size &&
        cursor->data[cursor->position] == '(') {
        if (!qwx_nfa_parse_group(parser, nfa, cursor, &start, &end)) return 0;
    } else {
        size_t label_start;
        size_t label_size;
        if (!qwx_model_read_name(cursor, &label_start, &label_size))
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "Stored DTD content particle is not a name");
        start = qwx_nfa_new_state(parser, nfa);
        end = qwx_nfa_new_state(parser, nfa);
        if (start < 0 || end < 0 ||
            !qwx_nfa_add_edge(parser, nfa, start, end,
                              label_start, label_size)) return 0;
    }
    qwx_model_skip_space(cursor);
    if (cursor->position < cursor->size &&
        (cursor->data[cursor->position] == '?' ||
         cursor->data[cursor->position] == '*' ||
         cursor->data[cursor->position] == '+')) {
        char suffix = cursor->data[cursor->position++];
        int wrapped_start = qwx_nfa_new_state(parser, nfa);
        int wrapped_end = qwx_nfa_new_state(parser, nfa);
        if (wrapped_start < 0 || wrapped_end < 0) return 0;
        if (!qwx_nfa_add_edge(parser, nfa, wrapped_start,
                              start, 0u, 0u) ||
            !qwx_nfa_add_edge(parser, nfa, end,
                              wrapped_end, 0u, 0u)) return 0;
        if (suffix == '?' || suffix == '*') {
            if (!qwx_nfa_add_edge(parser, nfa, wrapped_start,
                                  wrapped_end, 0u, 0u)) return 0;
        }
        if (suffix == '*' || suffix == '+') {
            if (!qwx_nfa_add_edge(parser, nfa, end,
                                  start, 0u, 0u)) return 0;
        }
        start = wrapped_start;
        end = wrapped_end;
    }
    *out_start = start;
    *out_end = end;
    return 1;
}

static int qwx_nfa_compile(QwXmlParser *parser, const char *model,
                           QwXmlNfa *nfa, int *start, int *accept)
{
    QwXmlModelCursor cursor;
    size_t size = strlen(model);
    if (size > 262144u || size > (SIZE_MAX - 8u) / 4u)
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "DTD content model exceeds the validation limit");
    memset(nfa, 0, sizeof(*nfa));
    nfa->model = model;
    nfa->model_size = size;
    nfa->state_capacity = size * 2u + 4u;
    nfa->edge_capacity = size * 4u + 8u;
    nfa->states = (QwXmlNfaState *)calloc(nfa->state_capacity,
                                         sizeof(*nfa->states));
    nfa->edges = (QwXmlNfaEdge *)calloc(nfa->edge_capacity,
                                       sizeof(*nfa->edges));
    if (!nfa->states || !nfa->edges) {
        free(nfa->states);
        free(nfa->edges);
        memset(nfa, 0, sizeof(*nfa));
        return qwx_fail(parser, QW_XML_ERROR_OUT_OF_MEMORY,
                        "Could not allocate DTD content model state");
    }
    cursor.data = model;
    cursor.size = size;
    cursor.position = 0u;
    qwx_model_skip_space(&cursor);
    if (!qwx_nfa_parse_particle(parser, nfa, &cursor, start, accept))
        goto failure;
    qwx_model_skip_space(&cursor);
    if (cursor.position != cursor.size) {
        qwx_fail(parser, QW_XML_ERROR_FORMAT,
                 "Stored DTD content model has trailing data");
        goto failure;
    }
    return 1;

failure:
    free(nfa->states);
    free(nfa->edges);
    memset(nfa, 0, sizeof(*nfa));
    return 0;
}

static int qwx_nfa_epsilon_closure(QwXmlParser *parser, const QwXmlNfa *nfa,
                                   uint8_t *active, int *queue,
                                   size_t *steps, size_t *out_count)
{
    size_t head = 0u;
    size_t tail = 0u;
    size_t i;
    for (i = 0u; i < nfa->state_count; ++i)
        if (active[i]) queue[tail++] = (int)i;
    while (head < tail) {
        int state = queue[head++];
        int edge_index;
        for (edge_index = nfa->states[state].first_edge; edge_index >= 0;
             edge_index = nfa->edges[edge_index].next) {
            const QwXmlNfaEdge *edge = &nfa->edges[edge_index];
            if (++*steps > 10000000u)
                return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                                "DTD validation operation limit exceeded");
            if (!edge->label_size && !active[edge->to]) {
                active[edge->to] = 1u;
                queue[tail++] = edge->to;
            }
        }
    }
    if (out_count) *out_count = tail;
    return 1;
}

static int qwx_validate_deterministic_content_model(
    QwXmlParser *parser, const QwXmlElementDecl *declaration)
{
    QwXmlNfa nfa;
    uint8_t *active = NULL;
    int *queue = NULL;
    int start;
    int accept;
    size_t state;
    size_t closure_count;
    size_t steps = 0u;
    int ok = 0;
    if (!qwx_nfa_compile(parser, declaration->content_model,
                         &nfa, &start, &accept)) return 0;
    (void)start;
    (void)accept;
    active = (uint8_t *)calloc(nfa.state_count, 1u);
    queue = (int *)malloc(nfa.state_count * sizeof(*queue));
    if (!active || !queue) {
        qwx_fail(parser, QW_XML_ERROR_OUT_OF_MEMORY,
                 "Could not allocate DTD determinism state");
        goto cleanup;
    }
    for (state = 0u; state < nfa.state_count; ++state) {
        size_t head = 0u;
        int left_state;
        active[state] = 1u;
        queue[0] = (int)state;
        closure_count = 1u;
        while (head < closure_count) {
            int closure_state = queue[head++];
            int edge_index;
            for (edge_index = nfa.states[closure_state].first_edge;
                 edge_index >= 0;
                 edge_index = nfa.edges[edge_index].next) {
                const QwXmlNfaEdge *edge = &nfa.edges[edge_index];
                if (++steps > 10000000u) {
                    qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                             "DTD determinism operation limit exceeded");
                    goto cleanup;
                }
                if (!edge->label_size && !active[edge->to]) {
                    active[edge->to] = 1u;
                    queue[closure_count++] = edge->to;
                }
            }
        }
        for (size_t left_index = 0u; left_index < closure_count; ++left_index) {
            int left_edge_index;
            left_state = queue[left_index];
            if (!active[left_state]) continue;
            for (left_edge_index = nfa.states[left_state].first_edge;
                 left_edge_index >= 0;
                 left_edge_index = nfa.edges[left_edge_index].next) {
                const QwXmlNfaEdge *left = &nfa.edges[left_edge_index];
                size_t right_index;
                if (!left->label_size) continue;
                for (right_index = left_index;
                     right_index < closure_count; ++right_index) {
                    int right_edge_index;
                    int right_state = queue[right_index];
                    if (!active[right_state]) continue;
                    for (right_edge_index = nfa.states[right_state].first_edge;
                         right_edge_index >= 0;
                         right_edge_index = nfa.edges[right_edge_index].next) {
                        const QwXmlNfaEdge *right =
                            &nfa.edges[right_edge_index];
                        if (right_state == left_state &&
                            right_edge_index == left_edge_index) continue;
                        if (++steps > 10000000u) {
                            qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                                     "DTD determinism operation limit exceeded");
                            goto cleanup;
                        }
                        if (right->label_size == left->label_size &&
                            memcmp(nfa.model + right->label_start,
                                   nfa.model + left->label_start,
                                   left->label_size) == 0) {
                            qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                                     "DTD content model is nondeterministic");
                            goto cleanup;
                        }
                    }
                }
            }
        }
        for (head = 0u; head < closure_count; ++head)
            active[queue[head]] = 0u;
    }
    ok = 1;

cleanup:
    free(active);
    free(queue);
    free(nfa.states);
    free(nfa.edges);
    return ok;
}

static int qwx_validate_children_model(QwXmlParser *parser,
                                      const QwXmlElementDecl *declaration,
                                      const QwXmlNode *element)
{
    QwXmlNfa nfa;
    uint8_t *active = NULL;
    uint8_t *next = NULL;
    int *queue = NULL;
    int start;
    int accept;
    size_t steps = 0u;
    const QwXmlNode *child;
    int valid = 0;
    if (!qwx_nfa_compile(parser, declaration->content_model,
                         &nfa, &start, &accept)) return 0;
    active = (uint8_t *)calloc(nfa.state_count, 1u);
    next = (uint8_t *)calloc(nfa.state_count, 1u);
    queue = (int *)malloc(nfa.state_count * sizeof(*queue));
    if (!active || !next || !queue) {
        qwx_fail(parser, QW_XML_ERROR_OUT_OF_MEMORY,
                 "Could not allocate DTD validation state");
        goto cleanup;
    }
    active[start] = 1u;
    if (!qwx_nfa_epsilon_closure(parser, &nfa, active, queue, &steps, NULL))
        goto cleanup;
    for (child = element->first_child; child; child = child->next) {
        int state;
        if (child->kind != QW_XML_NODE_ELEMENT) continue;
        memset(next, 0, nfa.state_count);
        for (state = 0; (size_t)state < nfa.state_count; ++state) {
            int edge_index;
            if (!active[state]) continue;
            for (edge_index = nfa.states[state].first_edge; edge_index >= 0;
                 edge_index = nfa.edges[edge_index].next) {
                const QwXmlNfaEdge *edge = &nfa.edges[edge_index];
                const char *child_name = child->qualified_tag;
                if (++steps > 10000000u) {
                    qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                             "DTD validation operation limit exceeded");
                    goto cleanup;
                }
                if (edge->label_size && child_name &&
                    strlen(child_name) == edge->label_size &&
                    memcmp(nfa.model + edge->label_start, child_name,
                           edge->label_size) == 0)
                    next[edge->to] = 1u;
            }
        }
        {
            uint8_t *swap = active;
            active = next;
            next = swap;
        }
            if (!qwx_nfa_epsilon_closure(parser, &nfa, active,
                                         queue, &steps, NULL)) goto cleanup;
    }
    valid = active[accept] != 0u;

cleanup:
    free(active);
    free(next);
    free(queue);
    free(nfa.states);
    free(nfa.edges);
    if (!valid && parser->error && parser->error->code == QW_XML_ERROR_NONE)
        qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                 "Element children do not match the declared DTD content model");
    return valid;
}

static int qwx_xml_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int qwx_valid_name_slice(const char *value, size_t size, int nmtoken)
{
    size_t position = 0u;
    int first = 1;
    if (!value || !size) return 0;
    while (position < size) {
        size_t next = position;
        uint32_t cp;
        if (!qwx_decode_utf8((const uint8_t *)value, size, &next, &cp) ||
            !(first && !nmtoken ? qwx_name_start(cp) : qwx_name_char(cp)))
            return 0;
        first = 0;
        position = next;
    }
    return 1;
}

static int qwx_enum_contains(const char *enumeration, const char *value,
                             size_t value_size)
{
    const char *cursor = enumeration;
    if (!enumeration) return 0;
    while (*cursor) {
        const char *start;
        const char *end;
        while (*cursor == '|') ++cursor;
        start = cursor;
        while (*cursor && *cursor != '|') ++cursor;
        end = cursor;
        if ((size_t)(end - start) == value_size &&
            memcmp(start, value, value_size) == 0) return 1;
        if (*cursor == '|') ++cursor;
    }
    return 0;
}

static int qwx_has_notation(const QwXmlParser *parser,
                            const char *name, size_t size)
{
    size_t i;
    for (i = 0u; i < parser->notation_decl_count; ++i)
        if (strlen(parser->notation_decls[i].name) == size &&
            memcmp(parser->notation_decls[i].name, name, size) == 0) return 1;
    return 0;
}

static int qwx_value_set_add(QwXmlParser *parser, QwXmlValueSet *set,
                             const char *value, size_t size)
{
    const char **grown;
    size_t capacity;
    char *copy = qwx_duplicate(parser, value, size);
    if (!copy) return 0;
    if (set->count == set->capacity) {
        capacity = set->capacity ? set->capacity * 2u : 16u;
        if (capacity > QW_XML_MAX_NODES) capacity = QW_XML_MAX_NODES;
        if (capacity <= set->capacity) {
            qwx_release(parser, copy, size + 1u);
            return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                            "DTD ID and IDREF value limit exceeded");
        }
        grown = (const char **)realloc(set->values,
                                       capacity * sizeof(*set->values));
        if (!grown) {
            qwx_release(parser, copy, size + 1u);
            return qwx_fail(parser, QW_XML_ERROR_OUT_OF_MEMORY,
                            "Could not grow DTD ID and IDREF table");
        }
        set->values = grown;
        set->capacity = capacity;
    }
    set->values[set->count++] = copy;
    return 1;
}

static int qwx_validate_attribute_value(QwXmlParser *parser,
                                        const QwXmlAttributeDecl *declaration,
                                        const char *value, int instance_value,
                                        int collect_ids,
                                        QwXmlValueSet *ids,
                                        QwXmlValueSet *idrefs)
{
    size_t position = 0u;
    size_t token_count = 0u;
    int plural = declaration->type == QW_XML_ATTR_IDREFS ||
                 declaration->type == QW_XML_ATTR_ENTITIES ||
                 declaration->type == QW_XML_ATTR_NMTOKENS;
    if (declaration->type == QW_XML_ATTR_CDATA) return 1;
    while (value[position]) {
        size_t start;
        size_t size;
        int is_name = declaration->type == QW_XML_ATTR_ID ||
                      declaration->type == QW_XML_ATTR_IDREF ||
                      declaration->type == QW_XML_ATTR_IDREFS ||
                      declaration->type == QW_XML_ATTR_ENTITY ||
                      declaration->type == QW_XML_ATTR_ENTITIES ||
                      declaration->type == QW_XML_ATTR_NOTATION;
        while (qwx_xml_space((unsigned char)value[position])) ++position;
        if (!value[position]) break;
        start = position;
        while (value[position] && !qwx_xml_space((unsigned char)value[position]))
            ++position;
        size = position - start;
        if (!qwx_valid_name_slice(value + start, size, !is_name))
            return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                            "DTD attribute value contains an invalid token");
        ++token_count;
        if (!plural && token_count > 1u)
            return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                            "DTD attribute value requires exactly one token");
        switch (declaration->type) {
        case QW_XML_ATTR_ID:
            if (collect_ids && !qwx_value_set_add(parser, ids,
                    value + start, size)) return 0;
            break;
        case QW_XML_ATTR_IDREF:
        case QW_XML_ATTR_IDREFS:
            if (collect_ids && !qwx_value_set_add(parser, idrefs,
                    value + start, size)) return 0;
            break;
        case QW_XML_ATTR_ENTITY:
        case QW_XML_ATTR_ENTITIES: {
            if (instance_value) {
                QwXmlEntity *entity = qwx_find_entity(parser, value + start,
                                                      size, 0);
                if (!entity || !entity->unparsed)
                    return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                                    "ENTITY attribute does not name an unparsed entity");
            }
            break;
        }
        case QW_XML_ATTR_ENUMERATION:
            if (!qwx_enum_contains(declaration->enumeration,
                                   value + start, size))
                return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                                "Attribute value is outside its DTD enumeration");
            break;
        case QW_XML_ATTR_NOTATION:
            if (!qwx_enum_contains(declaration->enumeration,
                                   value + start, size) ||
                !qwx_has_notation(parser, value + start, size))
                return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                                "NOTATION attribute value is not a declared notation");
            break;
        default:
            break;
        }
    }
    if (!token_count)
        return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                        "DTD tokenized attribute value is empty");
    return 1;
}

static int qwx_mixed_model_has(const char *model, const char *name)
{
    QwXmlModelCursor cursor;
    cursor.data = model;
    cursor.size = strlen(model);
    cursor.position = 0u;
    qwx_model_skip_space(&cursor);
    if (cursor.position >= cursor.size ||
        cursor.data[cursor.position++] != '(') return 0;
    qwx_model_skip_space(&cursor);
    if (cursor.position + 7u > cursor.size ||
        memcmp(cursor.data + cursor.position, "#PCDATA", 7u) != 0) return 0;
    cursor.position += 7u;
    qwx_model_skip_space(&cursor);
    while (cursor.position < cursor.size &&
           cursor.data[cursor.position] == '|') {
        size_t start;
        size_t size;
        ++cursor.position;
        qwx_model_skip_space(&cursor);
        if (!qwx_model_read_name(&cursor, &start, &size)) return 0;
        if (strlen(name) == size && memcmp(cursor.data + start, name, size) == 0)
            return 1;
        qwx_model_skip_space(&cursor);
    }
    return 0;
}

typedef struct QwXmlNameHashEntry {
    const char *name;
    size_t size;
    uint64_t hash;
} QwXmlNameHashEntry;

static int qwx_validate_mixed_model(QwXmlParser *parser, const char *model)
{
    QwXmlModelCursor cursor;
    QwXmlNameHashEntry *entries;
    size_t capacity = 8u;
    size_t model_size = strlen(model);
    cursor.data = model;
    cursor.size = model_size;
    cursor.position = 0u;
    if (model_size > 262144u)
        return qwx_fail(parser, QW_XML_ERROR_RESOURCE_LIMIT,
                        "DTD mixed content model exceeds the validation limit");
    while (capacity < model_size * 2u) capacity *= 2u;
    entries = (QwXmlNameHashEntry *)calloc(capacity, sizeof(*entries));
    if (!entries)
        return qwx_fail(parser, QW_XML_ERROR_OUT_OF_MEMORY,
                        "Could not allocate mixed content validation state");
    qwx_model_skip_space(&cursor);
    if (cursor.position >= cursor.size || cursor.data[cursor.position++] != '(') {
        free(entries);
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "Stored mixed content model is malformed");
    }
    qwx_model_skip_space(&cursor);
    if (cursor.position + 7u > cursor.size ||
        memcmp(cursor.data + cursor.position, "#PCDATA", 7u) != 0) {
        free(entries);
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "Stored mixed content model lacks #PCDATA");
    }
    cursor.position += 7u;
    qwx_model_skip_space(&cursor);
    while (cursor.position < cursor.size &&
           cursor.data[cursor.position] == '|') {
        size_t start;
        size_t size;
        size_t i;
        uint64_t hash = UINT64_C(1469598103934665603);
        ++cursor.position;
        qwx_model_skip_space(&cursor);
        if (!qwx_model_read_name(&cursor, &start, &size)) {
            free(entries);
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "Stored mixed content model has an invalid name");
        }
        for (i = 0u; i < size; ++i) {
            hash ^= (uint8_t)cursor.data[start + i];
            hash *= UINT64_C(1099511628211);
        }
        i = (size_t)hash & (capacity - 1u);
        while (entries[i].name) {
            if (entries[i].hash == hash && entries[i].size == size &&
                memcmp(entries[i].name, cursor.data + start, size) == 0) {
                free(entries);
                return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                                "Mixed DTD content model repeats an element type");
            }
            i = (i + 1u) & (capacity - 1u);
        }
        entries[i].name = cursor.data + start;
        entries[i].size = size;
        entries[i].hash = hash;
        qwx_model_skip_space(&cursor);
    }
    free(entries);
    return 1;
}

static int qwx_validate_dtd_element(QwXmlParser *parser, QwXmlNode *node,
                                    QwXmlValueSet *ids,
                                    QwXmlValueSet *idrefs)
{
    QwXmlElementDecl *declaration = qwx_find_element_decl(parser,
                                                          node->qualified_tag);
    size_t i;
    QwXmlNode *child;
    if (!declaration)
        return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                        "Document contains an element type not declared in the DTD");
    for (i = 0u; i < parser->attribute_decl_count; ++i) {
        QwXmlAttributeDecl *attribute_decl = &parser->attribute_decls[i];
        const char *value = NULL;
        int specified = 0;
        if (strcmp(attribute_decl->element_name, node->qualified_tag) != 0)
            continue;
        for (size_t j = 0u; j < node->attribute_count; ++j) {
            if (strcmp(node->attributes[j].name, attribute_decl->name) == 0) {
                value = node->attributes[j].value;
                specified = node->attributes[j].specified;
                break;
            }
        }
        if (parser->standalone_yes && attribute_decl->external_markup &&
            attribute_decl->default_value && !specified)
            return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                            "Standalone document depends on an external default attribute");
        if (!value && attribute_decl->default_kind == QW_XML_DEFAULT_REQUIRED)
            return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                            "Required DTD attribute is missing");
        if (value && attribute_decl->default_kind == QW_XML_DEFAULT_FIXED &&
            attribute_decl->default_value &&
            strcmp(value, attribute_decl->default_value) != 0)
            return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                            "DTD #FIXED attribute value differs from its default");
        if (value && !qwx_validate_attribute_value(parser, attribute_decl,
                value, 1, 1, ids, idrefs)) return 0;
    }

    if (declaration->content_kind == QW_XML_CONTENT_EMPTY) {
        if (node->first_child)
            return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                            "Element declared EMPTY contains content");
    } else if (declaration->content_kind == QW_XML_CONTENT_MIXED) {
        for (child = node->first_child; child; child = child->next)
            if (child->kind == QW_XML_NODE_ELEMENT &&
                !qwx_mixed_model_has(declaration->content_model,
                                     child->qualified_tag))
                return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                                "Element child is not allowed by the mixed content model");
    } else if (declaration->content_kind == QW_XML_CONTENT_CHILDREN) {
        for (child = node->first_child; child; child = child->next) {
            if (child->kind != QW_XML_NODE_TEXT || !child->text) continue;
            if (parser->standalone_yes && declaration->external_markup &&
                child->is_whitespace)
                return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                                "Standalone document has whitespace governed by an external element declaration");
            if (!child->is_whitespace || child->is_cdata)
                return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                                "Element-only content contains character data outside ignorable whitespace");
        }
        if (!qwx_validate_children_model(parser, declaration, node)) return 0;
    }
    for (child = node->first_child; child; child = child->next)
        if (child->kind == QW_XML_NODE_ELEMENT &&
            !qwx_validate_dtd_element(parser, child, ids, idrefs)) return 0;
    return 1;
}

static int qwx_value_pointer_compare(const void *left, const void *right)
{
    const char *const *a = (const char *const *)left;
    const char *const *b = (const char *const *)right;
    return strcmp(*a, *b);
}

static int qwx_value_set_contains(const QwXmlValueSet *set, const char *value)
{
    size_t low = 0u;
    size_t high = set->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        int order = strcmp(set->values[middle], value);
        if (!order) return 1;
        if (order < 0) low = middle + 1u;
        else high = middle;
    }
    return 0;
}

static int qwx_validate_dtd(QwXmlParser *parser)
{
    QwXmlNode *root;
    QwXmlValueSet ids = {0};
    QwXmlValueSet idrefs = {0};
    size_t i;
    int ok = 0;
    if (!parser->document_type_seen || !parser->doctype_name)
        return qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                        "DTD validation was requested for a document without a DOCTYPE");
    root = qw_xml_root(parser->root);
    if (!root || strcmp(parser->doctype_name, root->qualified_tag) != 0) {
        qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                 "DOCTYPE root name does not match the document element");
        goto cleanup;
    }
    for (i = 0u; i < parser->element_decl_count; ++i) {
        if (parser->element_decls[i].content_kind == QW_XML_CONTENT_CHILDREN &&
            !qwx_validate_deterministic_content_model(parser,
                &parser->element_decls[i])) goto cleanup;
        if (parser->element_decls[i].content_kind == QW_XML_CONTENT_MIXED &&
            !qwx_validate_mixed_model(parser,
                parser->element_decls[i].content_model)) goto cleanup;
    }
    for (i = 0u; i < parser->attribute_decl_count; ++i) {
        QwXmlAttributeDecl *decl = &parser->attribute_decls[i];
        size_t j;
        size_t id_count = 0u;
        size_t notation_count = 0u;
        for (j = 0u; j < parser->attribute_decl_count; ++j) {
            QwXmlAttributeDecl *other = &parser->attribute_decls[j];
            if (strcmp(decl->element_name, other->element_name) != 0) continue;
            if (other->type == QW_XML_ATTR_ID) ++id_count;
            if (other->type == QW_XML_ATTR_NOTATION) ++notation_count;
        }
        if (id_count > 1u) {
            qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                     "An element type has more than one ID attribute");
            goto cleanup;
        }
        if (notation_count > 1u) {
            qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                     "An element type has more than one NOTATION attribute");
            goto cleanup;
        }
        if (decl->type == QW_XML_ATTR_NOTATION) {
            QwXmlElementDecl *element_decl = qwx_find_element_decl(parser,
                                            decl->element_name);
            if (element_decl &&
                element_decl->content_kind == QW_XML_CONTENT_EMPTY) {
                qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                         "NOTATION attribute cannot be declared on an EMPTY element");
                goto cleanup;
            }
        }
        if (decl->type == QW_XML_ATTR_ID &&
            decl->default_kind != QW_XML_DEFAULT_REQUIRED &&
            decl->default_kind != QW_XML_DEFAULT_IMPLIED) {
            qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                     "An ID attribute must be #REQUIRED or #IMPLIED");
            goto cleanup;
        }
        if (decl->type == QW_XML_ATTR_NOTATION && decl->enumeration) {
            const char *cursor = decl->enumeration;
            while (*cursor) {
                const char *start;
                const char *end;
                while (*cursor == '|') ++cursor;
                start = cursor;
                while (*cursor && *cursor != '|') ++cursor;
                end = cursor;
                if (end > start && !qwx_has_notation(parser, start,
                                                     (size_t)(end - start))) {
                    qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                             "NOTATION attribute names an undeclared notation");
                    goto cleanup;
                }
            }
        }
        if (decl->default_value &&
            !qwx_validate_attribute_value(parser, decl,
                decl->default_value, 0, 0, NULL, NULL)) goto cleanup;
    }
    for (i = 0u; i < parser->entity_count; ++i) {
        QwXmlEntity *entity = &parser->entities[i];
        if (parser->standalone_yes && !entity->parameter &&
            entity->external_markup && entity->referenced) {
            qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                     "Standalone document references an externally declared entity");
            goto cleanup;
        }
        if (entity->unparsed && entity->notation_name &&
            !qwx_has_notation(parser, entity->notation_name,
                              strlen(entity->notation_name))) {
            qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                     "Unparsed entity refers to an undeclared notation");
            goto cleanup;
        }
    }
    if (!qwx_validate_dtd_element(parser, root, &ids, &idrefs)) goto cleanup;
    if (ids.count > 1u)
        qsort(ids.values, ids.count, sizeof(*ids.values), qwx_value_pointer_compare);
    if (idrefs.count > 1u)
        qsort(idrefs.values, idrefs.count, sizeof(*idrefs.values), qwx_value_pointer_compare);
    for (i = 1u; i < ids.count; ++i) {
        if (strcmp(ids.values[i - 1u], ids.values[i]) == 0) {
            qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                     "ID attribute values must be unique");
            goto cleanup;
        }
    }
    for (i = 0u; i < idrefs.count; ++i) {
        if (!qwx_value_set_contains(&ids, idrefs.values[i])) {
            qwx_fail(parser, QW_XML_ERROR_VALIDITY,
                     "IDREF value does not match an ID attribute");
            goto cleanup;
        }
    }
    ok = 1;

cleanup:
    for (i = 0u; i < ids.count; ++i)
        qwx_release(parser, (void *)ids.values[i], strlen(ids.values[i]) + 1u);
    for (i = 0u; i < idrefs.count; ++i)
        qwx_release(parser, (void *)idrefs.values[i], strlen(idrefs.values[i]) + 1u);
    free(ids.values);
    free(idrefs.values);
    return ok;
}

static int qwx_ncname_part(const char *name, size_t size)
{
    size_t position = 0u;
    int first = 1;
    if (!name || !size) return 0;
    while (position < size) {
        size_t next = position;
        uint32_t cp;
        if (!qwx_decode_utf8((const uint8_t *)name, size, &next, &cp) ||
            cp == ':' || !(first ? qwx_name_start(cp) : qwx_name_char(cp)))
            return 0;
        first = 0;
        position = next;
    }
    return 1;
}

static int qwx_qname_parts(const char *name, size_t *prefix_size,
                           size_t *local_start)
{
    const char *colon;
    size_t size;
    if (!name) return 0;
    size = strlen(name);
    colon = strchr(name, ':');
    if (!colon) {
        if (!qwx_ncname_part(name, size)) return 0;
        if (prefix_size) *prefix_size = 0u;
        if (local_start) *local_start = 0u;
        return 1;
    }
    if (strchr(colon + 1, ':') || colon == name || !colon[1]) return 0;
    if (!qwx_ncname_part(name, (size_t)(colon - name)) ||
        !qwx_ncname_part(colon + 1, strlen(colon + 1))) return 0;
    if (prefix_size) *prefix_size = (size_t)(colon - name);
    if (local_start) *local_start = (size_t)(colon - name) + 1u;
    return 1;
}

static const char *qwx_namespace_binding(const QwXmlNode *node,
                                         const char *prefix,
                                         size_t prefix_size,
                                         int *found)
{
    const QwXmlNode *scope;
    if (found) *found = 0;
    for (scope = node; scope; scope = scope->parent) {
        size_t i;
        for (i = 0u; i < scope->attribute_count; ++i) {
            const char *name = scope->attributes[i].name;
            if (!prefix_size) {
                if (strcmp(name, "xmlns") == 0) {
                    if (found) *found = 1;
                    return scope->attributes[i].value;
                }
            } else if (strncmp(name, "xmlns:", 6u) == 0 &&
                       strlen(name + 6u) == prefix_size &&
                       memcmp(name + 6u, prefix, prefix_size) == 0) {
                if (found) *found = 1;
                return scope->attributes[i].value;
            }
        }
    }
    return NULL;
}

static int qwx_resolve_node_namespaces(QwXmlParser *parser, QwXmlNode *node)
{
    QwXmlNode *child;
    size_t i;
    if (node->kind == QW_XML_NODE_ELEMENT) {
        size_t prefix_size = 0u;
        size_t local_start = 0u;
        const char *binding;
        int found = 0;
        if (!qwx_qname_parts(node->qualified_tag, &prefix_size, &local_start))
            return qwx_fail(parser, QW_XML_ERROR_NAMESPACE,
                            "XML element name is not a namespace QName");
        if (prefix_size && prefix_size == 5u &&
            memcmp(node->qualified_tag, "xmlns", 5u) == 0)
            return qwx_fail(parser, QW_XML_ERROR_NAMESPACE,
                            "The xmlns prefix cannot be used on an element");
        if (prefix_size && prefix_size == 3u &&
            memcmp(node->qualified_tag, "xml", 3u) == 0) {
            binding = QW_XML_NS_XML;
            found = 1;
        } else {
            binding = qwx_namespace_binding(node,
                prefix_size ? node->qualified_tag : "", prefix_size, &found);
        }
        if (prefix_size && !found)
            return qwx_fail(parser, QW_XML_ERROR_NAMESPACE,
                            "XML element uses an undeclared namespace prefix");
        if (binding && *binding) {
            node->namespace_uri = qwx_duplicate(parser, binding, strlen(binding));
            if (!node->namespace_uri) return 0;
        }
        if (strcmp(node->tag, node->qualified_tag + local_start) != 0) {
            char *local = qwx_duplicate(parser,
                node->qualified_tag + local_start,
                strlen(node->qualified_tag + local_start));
            if (!local) return 0;
            qwx_release(parser, node->tag, strlen(node->tag) + 1u);
            node->tag = local;
        }

        /* Namespace declarations are attributes in the reserved XMLNS URI. */
        for (i = 0u; i < node->attribute_count; ++i) {
            QwXmlAttribute *attribute = &node->attributes[i];
            size_t attr_prefix_size = 0u;
            size_t attr_local_start = 0u;
            const char *uri = NULL;
            if (!qwx_qname_parts(attribute->name, &attr_prefix_size,
                                 &attr_local_start))
                return qwx_fail(parser, QW_XML_ERROR_NAMESPACE,
                                "XML attribute name is not a namespace QName");
            attribute->local_name = qwx_duplicate(parser,
                attribute->name + attr_local_start,
                strlen(attribute->name + attr_local_start));
            if (!attribute->local_name) return 0;
            if (attr_prefix_size == 5u &&
                memcmp(attribute->name, "xmlns", 5u) == 0) {
                const char *declared_prefix = attribute->name + 6u;
                size_t declared_size = strlen(declared_prefix);
                const char *value = attribute->value;
                if (strcmp(declared_prefix, "xmlns") == 0 ||
                    strcmp(value, QW_XML_NS_XMLNS) == 0 ||
                    (strcmp(value, QW_XML_NS_XML) == 0 &&
                     strcmp(declared_prefix, "xml") != 0) ||
                    (strcmp(declared_prefix, "xml") == 0 &&
                     strcmp(value, QW_XML_NS_XML) != 0) ||
                    (declared_size && !*value))
                    return qwx_fail(parser, QW_XML_ERROR_NAMESPACE,
                                    "XML namespace declaration violates a reserved binding");
                uri = QW_XML_NS_XMLNS;
            } else if (strcmp(attribute->name, "xmlns") == 0) {
                const char *value = attribute->value;
                if (strcmp(value, QW_XML_NS_XMLNS) == 0 ||
                    strcmp(value, QW_XML_NS_XML) == 0)
                    return qwx_fail(parser, QW_XML_ERROR_NAMESPACE,
                                    "Default namespace cannot bind a reserved XML URI");
                uri = QW_XML_NS_XMLNS;
            } else if (attr_prefix_size) {
                if (attr_prefix_size == 3u &&
                    memcmp(attribute->name, "xml", 3u) == 0) {
                    uri = QW_XML_NS_XML;
                } else {
                    binding = qwx_namespace_binding(node, attribute->name,
                                                    attr_prefix_size, &found);
                    if (!found || !binding || !*binding)
                        return qwx_fail(parser, QW_XML_ERROR_NAMESPACE,
                                        "XML attribute uses an undeclared namespace prefix");
                    uri = binding;
                }
            }
            if (uri) {
                attribute->namespace_uri = qwx_duplicate(parser, uri,
                                                         strlen(uri));
                if (!attribute->namespace_uri) return 0;
            }
        }
        for (i = 0u; i < node->attribute_count; ++i) {
            size_t j;
            QwXmlAttribute *left = &node->attributes[i];
            for (j = 0u; j < i; ++j) {
                QwXmlAttribute *right = &node->attributes[j];
                if ((!left->namespace_uri && !right->namespace_uri) ||
                    (left->namespace_uri && right->namespace_uri &&
                     strcmp(left->namespace_uri, right->namespace_uri) == 0)) {
                    if (strcmp(left->local_name, right->local_name) == 0)
                        return qwx_fail(parser, QW_XML_ERROR_NAMESPACE,
                                        "XML start tag has duplicate expanded attribute names");
                }
            }
        }
    }
    for (child = node->first_child; child; child = child->next)
        if (!qwx_resolve_node_namespaces(parser, child)) return 0;
    return 1;
}

static void qwx_drop_nodes(QwXmlNode *node)
{
    while (node) {
        QwXmlNode *next = node->next;
        size_t i;
        qwx_drop_nodes(node->first_child);
        free(node->tag);
        free(node->qualified_tag);
        free(node->namespace_uri);
        free(node->text);
        free(node->processing_instruction_target);
        for (i = 0u; i < node->attribute_count; ++i) {
            free(node->attributes[i].name);
            free(node->attributes[i].value);
            free(node->attributes[i].local_name);
            free(node->attributes[i].namespace_uri);
        }
        free(node->attributes);
        free(node);
        node = next;
    }
}

static void qwx_drop_entities(QwXmlParser *parser)
{
    size_t i;
    for (i = 0u; i < parser->entity_count; ++i) {
        qwx_release(parser, parser->entities[i].name,
                    strlen(parser->entities[i].name) + 1u);
        qwx_release(parser, parser->entities[i].value,
                    parser->entities[i].value_size + 1u);
        qwx_release(parser, parser->entities[i].public_id,
            parser->entities[i].public_id
                ? strlen(parser->entities[i].public_id) + 1u : 0u);
        qwx_release(parser, parser->entities[i].system_id,
            parser->entities[i].system_id
                ? strlen(parser->entities[i].system_id) + 1u : 0u);
        qwx_release(parser, parser->entities[i].base_uri,
            parser->entities[i].base_uri
                ? strlen(parser->entities[i].base_uri) + 1u : 0u);
        qwx_release(parser, parser->entities[i].notation_name,
            parser->entities[i].notation_name
                ? strlen(parser->entities[i].notation_name) + 1u : 0u);
    }
    qwx_release(parser, parser->entities,
                parser->entity_capacity * sizeof(*parser->entities));
    parser->entities = NULL;
    parser->entity_count = 0u;
    parser->entity_capacity = 0u;
    for (i = 0u; i < parser->attribute_decl_count; ++i) {
        QwXmlAttributeDecl *decl = &parser->attribute_decls[i];
        qwx_release(parser, decl->element_name,
                    strlen(decl->element_name) + 1u);
        qwx_release(parser, decl->name, strlen(decl->name) + 1u);
        qwx_release(parser, decl->default_value,
            decl->default_value ? strlen(decl->default_value) + 1u : 0u);
        qwx_release(parser, decl->enumeration,
            decl->enumeration ? strlen(decl->enumeration) + 1u : 0u);
    }
    qwx_release(parser, parser->attribute_decls,
        parser->attribute_decl_capacity * sizeof(*parser->attribute_decls));
    parser->attribute_decls = NULL;
    parser->attribute_decl_count = 0u;
    parser->attribute_decl_capacity = 0u;
    for (i = 0u; i < parser->element_decl_count; ++i) {
        qwx_release(parser, parser->element_decls[i].name,
            strlen(parser->element_decls[i].name) + 1u);
        qwx_release(parser, parser->element_decls[i].content_model,
            strlen(parser->element_decls[i].content_model) + 1u);
    }
    qwx_release(parser, parser->element_decls,
        parser->element_decl_capacity * sizeof(*parser->element_decls));
    parser->element_decls = NULL;
    parser->element_decl_count = 0u;
    parser->element_decl_capacity = 0u;
    for (i = 0u; i < parser->notation_decl_count; ++i)
        qwx_release(parser, parser->notation_decls[i].name,
            strlen(parser->notation_decls[i].name) + 1u);
    qwx_release(parser, parser->notation_decls,
        parser->notation_decl_capacity * sizeof(*parser->notation_decls));
    parser->notation_decls = NULL;
    parser->notation_decl_count = 0u;
    parser->notation_decl_capacity = 0u;
}

static int qwx_validate_serialized_chars(QwXmlParser *parser,
                                        const char *text, size_t size)
{
    size_t position = 0u;
    while (position < size) {
        uint32_t cp;
        if (!qwx_decode_utf8((const uint8_t *)text, size, &position, &cp) ||
            !qwx_xml_character(cp))
            return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                            "XML serialization contains invalid UTF-8 or a forbidden character");
    }
    return 1;
}

static int qwx_validate_serialized_name(QwXmlParser *parser,
                                       const char *name)
{
    size_t size;
    if (!name || !(size = strlen(name)) ||
        !qwx_entity_name_valid((const uint8_t *)name, size) ||
        !qwx_qname_parts(name, NULL, NULL))
        return qwx_fail(parser, QW_XML_ERROR_FORMAT,
                        "XML serialization contains an invalid namespace name");
    return 1;
}

static int qwx_serialize_append(QwXmlParser *parser, QwXmlBuffer *buffer,
                               const char *data, size_t size)
{
    parser->position = buffer->size;
    return qwx_buffer_append(parser, buffer, data, size);
}

static int qwx_serialize_escaped(QwXmlParser *parser, QwXmlBuffer *buffer,
                                 const char *text, size_t size, int attribute)
{
    size_t i;
    if (!qwx_validate_serialized_chars(parser, text, size)) return 0;
    for (i = 0u; i < size; ++i) {
        const char *escaped = NULL;
        size_t escaped_size = 0u;
        switch ((uint8_t)text[i]) {
        case '&': escaped = "&amp;"; escaped_size = 5u; break;
        case '<': escaped = "&lt;"; escaped_size = 4u; break;
        case '>': escaped = "&gt;"; escaped_size = 4u; break;
        case '"':
            if (attribute) { escaped = "&quot;"; escaped_size = 6u; }
            break;
        case '\'':
            if (attribute) { escaped = "&apos;"; escaped_size = 6u; }
            break;
        case '\r': escaped = "&#xD;"; escaped_size = 5u; break;
        case '\n':
            if (attribute) { escaped = "&#xA;"; escaped_size = 5u; }
            break;
        case '\t':
            if (attribute) { escaped = "&#x9;"; escaped_size = 5u; }
            break;
        default: break;
        }
        if (escaped) {
            if (!qwx_serialize_append(parser, buffer, escaped, escaped_size))
                return 0;
        } else if (!qwx_serialize_append(parser, buffer, text + i, 1u)) {
            return 0;
        }
    }
    return 1;
}

int qw_xml_serialize(const QwXmlNode *node, uint8_t **out_data,
                     size_t *out_size, QwXmlError *error)
{
    QwXmlError local_error;
    QwXmlParser writer;
    QwXmlBuffer output;
    const QwXmlNode *open_elements[QW_XML_MAX_DEPTH];
    const QwXmlNode *scope;
    const QwXmlNode *current;
    size_t open_count = 0u;
    int done = 0;

    if (!error) error = &local_error;
    memset(error, 0, sizeof(*error));
    memset(&writer, 0, sizeof(writer));
    memset(&output, 0, sizeof(output));
    writer.error = error;
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0u;
    if (!node || !out_data || !out_size) {
        qwx_fail(&writer, QW_XML_ERROR_ARGUMENT,
                 "XML serialization requires a node and output buffers");
        return 0;
    }

    if (node->is_document) {
        const QwXmlNode *child;
        size_t root_count = 0u;
        for (child = node->first_child; child; child = child->next) {
            if (child->kind == QW_XML_NODE_DOCUMENT) {
                qwx_fail(&writer, QW_XML_ERROR_FORMAT,
                         "XML document contains a nested document node");
                return 0;
            }
            if (child->kind == QW_XML_NODE_ELEMENT) {
                ++root_count;
            } else if (child->kind == QW_XML_NODE_TEXT && child->text) {
                const unsigned char *text = (const unsigned char *)child->text;
                while (*text == ' ' || *text == '\t' ||
                       *text == '\r' || *text == '\n') ++text;
                if (*text) {
                    qwx_fail(&writer, QW_XML_ERROR_FORMAT,
                             "XML document has text outside its root element");
                    return 0;
                }
            } else if (child->kind == QW_XML_NODE_COMMENT && child->text) {
                continue;
            } else if (child->kind == QW_XML_NODE_PROCESSING_INSTRUCTION &&
                       child->processing_instruction_target && child->text) {
                continue;
            } else {
                qwx_fail(&writer, QW_XML_ERROR_FORMAT,
                         "XML document contains an invalid top-level node");
                return 0;
            }
        }
        if (root_count != 1u) {
            qwx_fail(&writer, QW_XML_ERROR_FORMAT,
                     "XML document must contain exactly one root element");
            return 0;
        }
    }

    scope = node;
    current = node->is_document ? node->first_child : node;
    while (current && !done) {
        if (current->kind == QW_XML_NODE_DOCUMENT) {
            qwx_fail(&writer, QW_XML_ERROR_FORMAT,
                     "Unexpected document node inside an XML subtree");
            goto failure;
        }
        if (current->kind == QW_XML_NODE_ELEMENT) {
            size_t i;
            const char *name = current->qualified_tag;
            if (!qwx_validate_serialized_name(&writer, name) ||
                !qwx_serialize_append(&writer, &output, "<", 1u) ||
                !qwx_serialize_append(&writer, &output, name,
                                      strlen(name))) {
                if (error->code == QW_XML_ERROR_NONE)
                    qwx_fail(&writer, QW_XML_ERROR_FORMAT,
                             "XML element has no qualified name");
                goto failure;
            }
            for (i = 0u; i < current->attribute_count; ++i) {
                const QwXmlAttribute *attribute = &current->attributes[i];
                size_t j;
                if (!qwx_validate_serialized_name(&writer, attribute->name) ||
                    !attribute->value) {
                    qwx_fail(&writer, QW_XML_ERROR_FORMAT,
                             "XML attribute is incomplete or has an invalid name");
                    goto failure;
                }
                for (j = 0u; j < i; ++j) {
                    if (strcmp(current->attributes[j].name,
                               attribute->name) == 0) {
                        qwx_fail(&writer, QW_XML_ERROR_FORMAT,
                                 "XML serialization contains a duplicate attribute");
                        goto failure;
                    }
                }
                if (!qwx_serialize_append(&writer, &output, " ", 1u) ||
                    !qwx_serialize_append(&writer, &output, attribute->name,
                                          strlen(attribute->name)) ||
                    !qwx_serialize_append(&writer, &output, "=\"", 2u) ||
                    !qwx_serialize_escaped(&writer, &output, attribute->value,
                                           strlen(attribute->value), 1) ||
                    !qwx_serialize_append(&writer, &output, "\"", 1u))
                    goto failure;
            }
            if (current->first_child) {
                if (open_count >= QW_XML_MAX_DEPTH) {
                    qwx_fail(&writer, QW_XML_ERROR_RESOURCE_LIMIT,
                             "XML serialization depth limit exceeded");
                    goto failure;
                }
                if (!qwx_serialize_append(&writer, &output, ">", 1u))
                    goto failure;
                open_elements[open_count++] = current;
                current = current->first_child;
                continue;
            }
            if (!qwx_serialize_append(&writer, &output, "/>", 2u))
                goto failure;
        } else if (current->kind == QW_XML_NODE_TEXT && current->text) {
            if (!qwx_serialize_escaped(&writer, &output, current->text,
                                       strlen(current->text), 0)) goto failure;
        } else if (current->kind == QW_XML_NODE_COMMENT && current->text) {
            size_t comment_size = strlen(current->text);
            if (!qwx_validate_serialized_chars(&writer, current->text,
                                               comment_size) ||
                strstr(current->text, "--") ||
                (comment_size && current->text[comment_size - 1u] == '-')) {
                qwx_fail(&writer, QW_XML_ERROR_FORMAT,
                         "XML comment content cannot contain '--' or end in '-'");
                goto failure;
            }
            if (!qwx_serialize_append(&writer, &output, "<!--", 4u) ||
                !qwx_serialize_append(&writer, &output, current->text,
                                      strlen(current->text)) ||
                !qwx_serialize_append(&writer, &output, "-->", 3u))
                goto failure;
        } else if (current->kind == QW_XML_NODE_PROCESSING_INSTRUCTION &&
                   current->processing_instruction_target && current->text) {
            size_t data_size = strlen(current->text);
            if (!qwx_entity_name_valid(
                    (const uint8_t *)current->processing_instruction_target,
                    strlen(current->processing_instruction_target)) ||
                qwx_ascii_case_equal(current->processing_instruction_target,
                                     "xml") ||
                !qwx_validate_serialized_chars(&writer, current->text,
                                               data_size) ||
                strstr(current->text, "?>")) {
                qwx_fail(&writer, QW_XML_ERROR_FORMAT,
                         "XML processing instruction target or data is invalid");
                goto failure;
            }
            if (!qwx_serialize_append(&writer, &output, "<?", 2u) ||
                !qwx_serialize_append(&writer, &output,
                    current->processing_instruction_target,
                    strlen(current->processing_instruction_target)) ||
                (data_size &&
                 (!qwx_serialize_append(&writer, &output, " ", 1u) ||
                  !qwx_serialize_append(&writer, &output, current->text,
                                        data_size))) ||
                !qwx_serialize_append(&writer, &output, "?>", 2u))
                goto failure;
        } else {
            qwx_fail(&writer, QW_XML_ERROR_FORMAT,
                     "XML node is neither an element, text, nor document");
            goto failure;
        }

        if (current == scope) {
            done = 1;
            break;
        }
        for (;;) {
            if (current->next) {
                current = current->next;
                break;
            }
            current = current->parent;
            if (!current) {
                qwx_fail(&writer, QW_XML_ERROR_FORMAT,
                         "XML node has an incomplete parent chain");
                goto failure;
            }
            if (current->is_document) {
                current = NULL;
                done = 1;
                break;
            }
            if (!open_count || open_elements[open_count - 1u] != current) {
                qwx_fail(&writer, QW_XML_ERROR_FORMAT,
                         "XML element nesting is inconsistent");
                goto failure;
            }
            --open_count;
            if (!qwx_serialize_append(&writer, &output, "</", 2u) ||
                !qwx_serialize_append(&writer, &output,
                                      current->qualified_tag,
                                      strlen(current->qualified_tag)) ||
                !qwx_serialize_append(&writer, &output, ">", 1u))
                goto failure;
            if (current == scope) {
                done = 1;
                break;
            }
        }
    }

    if (!qwx_buffer_reserve(&writer, &output, output.size + 1u))
        goto failure;
    output.data[output.size] = '\0';
    *out_data = (uint8_t *)output.data;
    *out_size = output.size;
    return 1;

failure:
    qwx_release(&writer, output.data, output.capacity);
    return 0;
}

QwXmlNode *qw_xml_parse_with_options(const void *data, size_t size,
                                     const QwXmlParseOptions *options,
                                     QwXmlError *error)
{
    QwXmlParser parser;
    if (error) memset(error, 0, sizeof(*error));
    memset(&parser, 0, sizeof(parser));
    parser.error = error;
    parser.options = options;
    parser.validate_dtd = options ? !!options->validate_dtd : 0;
    parser.current_base_uri = options ? options->base_uri : NULL;
    parser.root = qwx_new_node(&parser, NULL, NULL, 1);
    if (!parser.root || !qwx_prepare_input(&parser, data, size) ||
        !qwx_parse_document(&parser) ||
        !qwx_resolve_node_namespaces(&parser, parser.root) ||
        (parser.validate_dtd && !qwx_validate_dtd(&parser))) {
        qwx_drop_nodes(parser.root);
        qwx_drop_entities(&parser);
        qwx_release(&parser, parser.doctype_name,
            parser.doctype_name ? strlen(parser.doctype_name) + 1u : 0u);
        free(parser.normalized_input);
        return NULL;
    }
    qwx_drop_entities(&parser);
    qwx_release(&parser, parser.doctype_name,
        parser.doctype_name ? strlen(parser.doctype_name) + 1u : 0u);
    free(parser.normalized_input);
    return parser.root;
}

QwXmlNode *qw_xml_parse(const void *data, size_t size, QwXmlError *error)
{
    return qw_xml_parse_with_options(data, size, NULL, error);
}

void qw_xml_drop(QwXmlNode *root)
{
    while (root && root->parent) root = root->parent;
    qwx_drop_nodes(root);
}

QwXmlNodeKind qw_xml_node_kind(const QwXmlNode *node)
{
    return node ? node->kind : QW_XML_NODE_INVALID;
}

QwXmlNode *qw_xml_root(QwXmlNode *node)
{
    QwXmlNode *top = node;
    while (top && top->parent) top = top->parent;
    if (!top || !top->is_document) return top;
    for (node = top->first_child; node; node = node->next) {
        if (node->tag) return node;
    }
    return NULL;
}

QwXmlNode *qw_xml_down(QwXmlNode *node)
{
    return node ? node->first_child : NULL;
}

QwXmlNode *qw_xml_next(QwXmlNode *node)
{
    return node ? node->next : NULL;
}

QwXmlNode *qw_xml_prev(QwXmlNode *node)
{
    return node ? node->previous : NULL;
}

QwXmlNode *qw_xml_up(QwXmlNode *node)
{
    QwXmlNode *parent = node ? node->parent : NULL;
    return parent && !parent->is_document ? parent : NULL;
}

int qw_xml_is_tag(const QwXmlNode *node, const char *name)
{
    return node && node->tag && name && strcmp(node->tag, name) == 0;
}

const char *qw_xml_tag(const QwXmlNode *node)
{
    return node && node->kind == QW_XML_NODE_ELEMENT ? node->tag : NULL;
}

const char *qw_xml_qualified_tag(const QwXmlNode *node)
{
    return node && node->kind == QW_XML_NODE_ELEMENT
        ? node->qualified_tag : NULL;
}

const char *qw_xml_local_name(const QwXmlNode *node)
{
    return qw_xml_tag(node);
}

const char *qw_xml_namespace_uri(const QwXmlNode *node)
{
    return node && node->kind == QW_XML_NODE_ELEMENT
        ? node->namespace_uri : NULL;
}

const char *qw_xml_text(const QwXmlNode *node)
{
    return node && node->kind == QW_XML_NODE_TEXT ? node->text : NULL;
}

const char *qw_xml_comment(const QwXmlNode *node)
{
    return node && node->kind == QW_XML_NODE_COMMENT ? node->text : NULL;
}

const char *qw_xml_pi_target(const QwXmlNode *node)
{
    return node && node->kind == QW_XML_NODE_PROCESSING_INSTRUCTION
        ? node->processing_instruction_target : NULL;
}

const char *qw_xml_pi_data(const QwXmlNode *node)
{
    return node && node->kind == QW_XML_NODE_PROCESSING_INSTRUCTION
        ? node->text : NULL;
}

const char *qw_xml_att(const QwXmlNode *node, const char *qualified_name)
{
    size_t i;
    if (!node || !node->tag || !qualified_name) return NULL;
    for (i = node->attribute_count; i > 0u; --i) {
        if (strcmp(node->attributes[i - 1u].name, qualified_name) == 0)
            return node->attributes[i - 1u].value;
    }
    return NULL;
}

size_t qw_xml_attribute_count(const QwXmlNode *node)
{
    return node && node->kind == QW_XML_NODE_ELEMENT
        ? node->attribute_count : 0u;
}

const char *qw_xml_attribute_name(const QwXmlNode *node, size_t index)
{
    return node && index < node->attribute_count
        ? node->attributes[index].name : NULL;
}

const char *qw_xml_attribute_local_name(const QwXmlNode *node, size_t index)
{
    return node && index < node->attribute_count
        ? node->attributes[index].local_name : NULL;
}

const char *qw_xml_attribute_namespace_uri(const QwXmlNode *node,
                                           size_t index)
{
    return node && index < node->attribute_count
        ? node->attributes[index].namespace_uri : NULL;
}

const char *qw_xml_attribute_value(const QwXmlNode *node, size_t index)
{
    return node && index < node->attribute_count
        ? node->attributes[index].value : NULL;
}

const char *qw_xml_att_ns(const QwXmlNode *node, const char *namespace_uri,
                          const char *local_name)
{
    size_t i;
    if (!node || node->kind != QW_XML_NODE_ELEMENT || !local_name) return NULL;
    for (i = 0u; i < node->attribute_count; ++i) {
        const QwXmlAttribute *attribute = &node->attributes[i];
        if ((!namespace_uri && !attribute->namespace_uri) ||
            (namespace_uri && attribute->namespace_uri &&
             strcmp(namespace_uri, attribute->namespace_uri) == 0)) {
            if (attribute->local_name &&
                strcmp(attribute->local_name, local_name) == 0)
                return attribute->value;
        }
    }
    return NULL;
}

const char *qw_xml_att_alt(const QwXmlNode *node, const char *first_name,
                           const char *second_name)
{
    const char *value = qw_xml_att(node, first_name);
    return value ? value : qw_xml_att(node, second_name);
}

int qw_xml_att_eq(const QwXmlNode *node, const char *name, const char *value)
{
    const char *attribute = qw_xml_att(node, name);
    return attribute && value && strcmp(attribute, value) == 0;
}

static int qwx_node_matches(const QwXmlNode *node, const char *tag,
                            const char *attribute, const char *value)
{
    const char *actual;
    if (!node || !node->tag || (tag && !qw_xml_is_tag(node, tag))) return 0;
    if (!attribute) return 1;
    actual = qw_xml_att(node, attribute);
    return actual && (!value || strcmp(actual, value) == 0);
}

static QwXmlNode *qwx_find_siblings(QwXmlNode *item, const char *tag,
                                    const char *attribute, const char *value)
{
    if (item && item->is_document) item = item->first_child;
    for (; item; item = item->next) {
        if (qwx_node_matches(item, tag, attribute, value)) return item;
    }
    return NULL;
}

QwXmlNode *qw_xml_find(QwXmlNode *item, const char *tag)
{
    return qwx_find_siblings(item, tag, NULL, NULL);
}

QwXmlNode *qw_xml_find_next(QwXmlNode *item, const char *tag)
{
    if (item && item->is_document) item = item->first_child;
    return qwx_find_siblings(item ? item->next : NULL, tag, NULL, NULL);
}

QwXmlNode *qw_xml_find_down(QwXmlNode *item, const char *tag)
{
    return qwx_find_siblings(item ? item->first_child : NULL,
                             tag, NULL, NULL);
}

QwXmlNode *qw_xml_find_match(QwXmlNode *item, const char *tag,
                             const char *attribute, const char *value)
{
    return qwx_find_siblings(item, tag, attribute, value);
}

QwXmlNode *qw_xml_find_next_match(QwXmlNode *item, const char *tag,
                                  const char *attribute, const char *value)
{
    if (item && item->is_document) item = item->first_child;
    return qwx_find_siblings(item ? item->next : NULL, tag,
                             attribute, value);
}

QwXmlNode *qw_xml_find_down_match(QwXmlNode *item, const char *tag,
                                  const char *attribute, const char *value)
{
    return qwx_find_siblings(item ? item->first_child : NULL, tag,
                             attribute, value);
}

static QwXmlNode *qwx_find_dfs_range(QwXmlNode *item, const char *tag,
                                     const char *attribute, const char *value,
                                     QwXmlNode *top)
{
    QwXmlNode *node = item;
    if (node && node->is_document) node = node->first_child;
    while (node) {
        if (qwx_node_matches(node, tag, attribute, value)) return node;
        if (node->tag && node->first_child) {
            node = node->first_child;
            continue;
        }
        if (node->next) {
            node = node->next;
            continue;
        }
        for (;;) {
            node = node->parent;
            if (!node || node == top || node->is_document) return NULL;
            if (node->next) {
                node = node->next;
                break;
            }
        }
    }
    return NULL;
}

QwXmlNode *qw_xml_find_dfs(QwXmlNode *item, const char *tag,
                           const char *attribute, const char *value)
{
    return qwx_find_dfs_range(item, tag, attribute, value, NULL);
}

QwXmlNode *qw_xml_find_dfs_top(QwXmlNode *item, const char *tag,
                               const char *attribute, const char *value,
                               QwXmlNode *top)
{
    return qwx_find_dfs_range(item, tag, attribute, value, top);
}

static QwXmlNode *qwx_next_dfs_node(QwXmlNode *item, QwXmlNode *top)
{
    QwXmlNode *node = item;
    if (node && node->is_document) node = node->first_child;
    if (!node) return NULL;
    if (node->first_child) return node->first_child;
    if (node->next) return node->next;
    while (node && node != top) {
        node = node->parent;
        if (!node || node == top || node->is_document) return NULL;
        if (node->next) return node->next;
    }
    return NULL;
}

QwXmlNode *qw_xml_find_next_dfs_top(QwXmlNode *item, const char *tag,
                                    const char *attribute, const char *value,
                                    QwXmlNode *top)
{
    QwXmlNode *node = item;
    while ((node = qwx_next_dfs_node(node, top)) != NULL) {
        if (qwx_node_matches(node, tag, attribute, value)) return node;
    }
    return NULL;
}

QwXmlNode *qw_xml_find_next_dfs(QwXmlNode *item, const char *tag,
                                const char *attribute, const char *value)
{
    return qw_xml_find_next_dfs_top(item, tag, attribute, value, NULL);
}
