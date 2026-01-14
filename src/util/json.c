/*
 * Scribe - A protocol for Verifiable Data Lineage
 * util/json.c - JSON utilities using yyjson
 */

#include "scribe/error.h"
#include "scribe/types.h"
#include "yyjson.h"

#include <stdlib.h>
#include <string.h>

/*
 * Helper to get string value from JSON object
 */
const char *json_get_string(yyjson_val *obj, const char *key)
{
    if (!obj || !key) return NULL;
    yyjson_val *val = yyjson_obj_get(obj, key);
    return yyjson_get_str(val);
}

/*
 * Helper to get integer value from JSON object
 */
int64_t json_get_int(yyjson_val *obj, const char *key, int64_t default_val)
{
    if (!obj || !key) return default_val;
    yyjson_val *val = yyjson_obj_get(obj, key);
    if (!val || !yyjson_is_int(val)) return default_val;
    return yyjson_get_int(val);
}

/*
 * Helper to get boolean value from JSON object
 */
int json_get_bool(yyjson_val *obj, const char *key, int default_val)
{
    if (!obj || !key) return default_val;
    yyjson_val *val = yyjson_obj_get(obj, key);
    if (!val || !yyjson_is_bool(val)) return default_val;
    return yyjson_get_bool(val);
}

/*
 * Duplicate a string (helper for JSON parsing)
 */
char *json_strdup(const char *s)
{
    if (!s) return NULL;
    return strdup(s);
}

/*
 * Add string to mutable JSON object (skips NULL values)
 */
void json_mut_add_str(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                      const char *key, const char *value)
{
    if (!doc || !obj || !key) return;
    if (value) {
        yyjson_mut_obj_add_str(doc, obj, key, value);
    }
}

/*
 * Add integer to mutable JSON object
 */
void json_mut_add_int(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                      const char *key, int64_t value)
{
    if (!doc || !obj || !key) return;
    yyjson_mut_obj_add_int(doc, obj, key, value);
}

/*
 * Add boolean to mutable JSON object
 */
void json_mut_add_bool(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                       const char *key, int value)
{
    if (!doc || !obj || !key) return;
    yyjson_mut_obj_add_bool(doc, obj, key, value);
}

/*
 * Create a new mutable JSON object
 */
yyjson_mut_val *json_mut_obj_new(yyjson_mut_doc *doc)
{
    return yyjson_mut_obj(doc);
}

/*
 * Create a new mutable JSON array
 */
yyjson_mut_val *json_mut_arr_new(yyjson_mut_doc *doc)
{
    return yyjson_mut_arr(doc);
}

/*
 * Write JSON document to string (caller must free)
 */
char *json_write(yyjson_mut_doc *doc, size_t *len)
{
    if (!doc) return NULL;
    return yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, len);
}

/*
 * Write JSON document to compact string (caller must free)
 */
char *json_write_compact(yyjson_mut_doc *doc, size_t *len)
{
    if (!doc) return NULL;
    return yyjson_mut_write(doc, 0, len);
}

/*
 * Parse JSON string
 */
yyjson_doc *json_parse(const char *str, size_t len)
{
    if (!str) return NULL;
    if (len == 0) len = strlen(str);
    return yyjson_read(str, len, 0);
}

/*
 * Free JSON document
 */
void json_doc_free(yyjson_doc *doc)
{
    if (doc) yyjson_doc_free(doc);
}

/*
 * Free mutable JSON document
 */
void json_mut_doc_free(yyjson_mut_doc *doc)
{
    if (doc) yyjson_mut_doc_free(doc);
}
