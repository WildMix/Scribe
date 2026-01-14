/*
 * Scribe - A protocol for Verifiable Data Lineage
 * core/object.c - Object abstraction
 */

#include "scribe/types.h"
#include "scribe/error.h"
#include "scribe/core/hash.h"

#include <stdlib.h>
#include <string.h>

const char *scribe_object_type_string(scribe_object_type_t type)
{
    switch (type) {
        case SCRIBE_OBJ_BLOB:   return "blob";
        case SCRIBE_OBJ_TREE:   return "tree";
        case SCRIBE_OBJ_COMMIT: return "commit";
        default:               return "unknown";
    }
}

scribe_object_type_t scribe_object_type_from_string(const char *str)
{
    if (!str) return SCRIBE_OBJ_BLOB;

    if (strcmp(str, "blob") == 0) return SCRIBE_OBJ_BLOB;
    if (strcmp(str, "tree") == 0) return SCRIBE_OBJ_TREE;
    if (strcmp(str, "commit") == 0) return SCRIBE_OBJ_COMMIT;

    return SCRIBE_OBJ_BLOB;
}

scribe_object_t *scribe_object_new(scribe_object_type_t type)
{
    scribe_object_t *obj = calloc(1, sizeof(scribe_object_t));
    if (!obj) return NULL;

    obj->type = type;
    return obj;
}

void scribe_object_free(scribe_object_t *obj)
{
    free(obj);
}

scribe_error_t scribe_object_compute_hash(const void *content,
                                          size_t size,
                                          scribe_object_type_t type,
                                          scribe_hash_t *out_hash)
{
    if (!out_hash) return SCRIBE_ERR_INVALID_ARG;

    /* Git-style object hashing: type + space + size + null + content */
    const char *type_str = scribe_object_type_string(type);
    size_t type_len = strlen(type_str);

    /* Calculate header size */
    char size_str[32];
    int size_len = snprintf(size_str, sizeof(size_str), "%zu", size);

    /* Header: "type size\0" */
    size_t header_len = type_len + 1 + size_len + 1;
    size_t total_len = header_len + size;

    char *buffer = malloc(total_len);
    if (!buffer) return SCRIBE_ERR_NOMEM;

    /* Build header */
    memcpy(buffer, type_str, type_len);
    buffer[type_len] = ' ';
    memcpy(buffer + type_len + 1, size_str, size_len);
    buffer[type_len + 1 + size_len] = '\0';

    /* Copy content */
    if (content && size > 0) {
        memcpy(buffer + header_len, content, size);
    }

    /* Hash */
    scribe_error_t err = scribe_hash_bytes(buffer, total_len, out_hash);
    free(buffer);

    return err;
}
