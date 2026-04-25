#ifndef SCRIBE_ADAPTER_MONGO_INTERNAL_H
#define SCRIBE_ADAPTER_MONGO_INTERNAL_H

#include "core/internal.h"

#include <bson/bson.h>

typedef struct {
    char *bytes;
    size_t len;
} mongo_string;

scribe_error_t scribe_mongo_canonicalize_json(const char *json, char **out, size_t *out_len);
scribe_error_t scribe_mongo_canonicalize_bson(const bson_t *doc, uint8_t **out, size_t *out_len);
scribe_error_t scribe_mongo_canonicalize_id(const bson_t *doc, char **out);

#endif
