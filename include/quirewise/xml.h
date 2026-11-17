#ifndef QUIREWISE_XML_H
#define QUIREWISE_XML_H

#include <stddef.h>
#include <stdint.h>

#define QW_XML_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct QwXmlNode QwXmlNode;

typedef enum QwXmlNodeKind {
    QW_XML_NODE_INVALID = 0,
    QW_XML_NODE_DOCUMENT,
    QW_XML_NODE_ELEMENT,
    QW_XML_NODE_TEXT,
    QW_XML_NODE_COMMENT,
    QW_XML_NODE_PROCESSING_INSTRUCTION
} QwXmlNodeKind;

typedef enum QwXmlErrorCode {
    QW_XML_ERROR_NONE = 0,
    QW_XML_ERROR_ARGUMENT,
    QW_XML_ERROR_FORMAT,
    QW_XML_ERROR_RESOURCE_LIMIT,
    QW_XML_ERROR_OUT_OF_MEMORY,
    QW_XML_ERROR_ENCODING,
    QW_XML_ERROR_UNSUPPORTED,
    QW_XML_ERROR_NAMESPACE,
    QW_XML_ERROR_VALIDITY
} QwXmlErrorCode;

typedef struct QwXmlError {
    QwXmlErrorCode code;
    size_t byte_offset;
    char message[160];
} QwXmlError;

typedef int (*QwXmlResolveEntityFn)(void *context, const char *public_id,
                                    const char *system_id,
                                    const char *base_uri,
                                    const uint8_t **out_data,
                                    size_t *out_size,
                                    const char **out_resolved_base_uri);
typedef void (*QwXmlReleaseEntityFn)(void *context, const uint8_t *data,
                                     size_t size);

typedef struct QwXmlParseOptions {
    int validate_dtd; /* Validate XML 1.0 DTD constraints in addition to syntax. */
    const char *base_uri; /* Base URI used to resolve relative external IDs. */
    void *resolver_context;
    /* External resources are fetched only through this caller-supplied resolver.
     * On success it returns bytes, their size, and optionally the resolved base
     * URI. Bytes stay valid until release_entity is called, or until parsing
     * returns when no release callback is provided. */
    QwXmlResolveEntityFn resolve_entity;
    QwXmlReleaseEntityFn release_entity;
} QwXmlParseOptions;

/*! Return the ABI version compiled into the installed data package. */
int qw_xml_abi_version(void);

/*! Parse namespace-aware XML 1.0. External resources are accessed only through
 *  the caller-supplied resolver. Returns NULL on failure and fills error. */
QwXmlNode *qw_xml_parse(const void *data, size_t size, QwXmlError *error);
/*! Like qw_xml_parse(), with DTD validation and an external-resource resolver. */
QwXmlNode *qw_xml_parse_with_options(const void *data, size_t size,
                                     const QwXmlParseOptions *options,
                                     QwXmlError *error);
/*! Release a tree returned by qw_xml_parse(); NULL is accepted. */
void qw_xml_drop(QwXmlNode *root);

/*! Serialize a tree to UTF-8. The buffer is malloc-owned and released with
 *  free(); returns 0 on success and fills error on failure. */
int qw_xml_serialize(const QwXmlNode *node, uint8_t **out_data,
                     size_t *out_size, QwXmlError *error);

/*! Return the node kind, or QW_XML_NODE_INVALID for NULL. */
QwXmlNodeKind qw_xml_node_kind(const QwXmlNode *node);
/*! Return the document node that owns this node. */
QwXmlNode *qw_xml_root(QwXmlNode *node);
/*! Return the first child, or NULL. */
QwXmlNode *qw_xml_down(QwXmlNode *node);
/*! Return the next sibling, or NULL. */
QwXmlNode *qw_xml_next(QwXmlNode *node);
/*! Return the previous sibling, or NULL. */
QwXmlNode *qw_xml_prev(QwXmlNode *node);
/*! Return the parent element or document node, or NULL. */
QwXmlNode *qw_xml_up(QwXmlNode *node);
/*! Return 1 when the node is an element whose local name equals name. */
int qw_xml_is_tag(const QwXmlNode *node, const char *name);
/*! Return the element's tag name with any namespace prefix stripped. */
const char *qw_xml_tag(const QwXmlNode *node);
/*! Return the element's qualified name including its prefix. */
const char *qw_xml_qualified_tag(const QwXmlNode *node);
/*! Return the element's local name without a namespace prefix. */
const char *qw_xml_local_name(const QwXmlNode *node);
/*! Return the element's namespace URI, or NULL when it is unbound. */
const char *qw_xml_namespace_uri(const QwXmlNode *node);
/*! Return the text content of a text node. */
const char *qw_xml_text(const QwXmlNode *node);
/*! Return the body of a comment node. */
const char *qw_xml_comment(const QwXmlNode *node);
/*! Return the target of a processing instruction. */
const char *qw_xml_pi_target(const QwXmlNode *node);
/*! Return the data of a processing instruction. */
const char *qw_xml_pi_data(const QwXmlNode *node);
/*! Return the value of a qualified attribute name, or NULL. */
const char *qw_xml_att(const QwXmlNode *node, const char *qualified_name);
/*! Return the number of attributes on an element. */
size_t qw_xml_attribute_count(const QwXmlNode *node);
/*! Return the qualified name of the attribute at index, or NULL. */
const char *qw_xml_attribute_name(const QwXmlNode *node, size_t index);
/*! Return the local name of the attribute at index, or NULL. */
const char *qw_xml_attribute_local_name(const QwXmlNode *node, size_t index);
/*! Return the namespace URI of the attribute at index, or NULL. */
const char *qw_xml_attribute_namespace_uri(const QwXmlNode *node,
                                           size_t index);
/*! Return the value of the attribute at index, or NULL. */
const char *qw_xml_attribute_value(const QwXmlNode *node, size_t index);
/*! Return an attribute value by namespace URI and local name. */
const char *qw_xml_att_ns(const QwXmlNode *node, const char *namespace_uri,
                          const char *local_name);
/*! Return the first present attribute value among two qualified names. */
const char *qw_xml_att_alt(const QwXmlNode *node, const char *first_name,
                           const char *second_name);
/*! Return 1 when the attribute exists and its value equals value. */
int qw_xml_att_eq(const QwXmlNode *node, const char *name,
                  const char *value);

/*! Return the first element matching tag, starting with item itself. */
QwXmlNode *qw_xml_find(QwXmlNode *item, const char *tag);
/*! Return the next sibling element matching tag after item. */
QwXmlNode *qw_xml_find_next(QwXmlNode *item, const char *tag);
/*! Return the first child element matching tag. */
QwXmlNode *qw_xml_find_down(QwXmlNode *item, const char *tag);
/*! Return the first sibling element matching tag and attribute=value. */
QwXmlNode *qw_xml_find_match(QwXmlNode *item, const char *tag,
                             const char *attribute, const char *value);
/*! Return the next matching sibling element after item. */
QwXmlNode *qw_xml_find_next_match(QwXmlNode *item, const char *tag,
                                  const char *attribute, const char *value);
/*! Return the first child element matching tag and attribute=value. */
QwXmlNode *qw_xml_find_down_match(QwXmlNode *item, const char *tag,
                                  const char *attribute, const char *value);
/*! Depth-first search from item for tag and an optional attribute=value. */
QwXmlNode *qw_xml_find_dfs(QwXmlNode *item, const char *tag,
                           const char *attribute, const char *value);
/*! Depth-first search from item bounded by the top node. */
QwXmlNode *qw_xml_find_dfs_top(QwXmlNode *item, const char *tag,
                               const char *attribute, const char *value,
                               QwXmlNode *top);
/*! Continue a depth-first search after item. */
QwXmlNode *qw_xml_find_next_dfs(QwXmlNode *item, const char *tag,
                                const char *attribute, const char *value);
/*! Continue a depth-first search after item, bounded by top. */
QwXmlNode *qw_xml_find_next_dfs_top(QwXmlNode *item, const char *tag,
                                    const char *attribute, const char *value,
                                    QwXmlNode *top);

#ifdef __cplusplus
}
#endif

#endif
