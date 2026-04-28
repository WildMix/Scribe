/*
 * MongoDB BSON and Extended JSON canonicalization.
 *
 * The Mongo adapter stores documents as canonical Extended JSON blobs and uses
 * the canonical Extended JSON representation of `_id` as the leaf tree name.
 * libbson provides Extended JSON rendering; this file adds recursive object-key
 * sorting and whitespace removal so equivalent documents hash deterministically.
 */
#include "adapter_mongo/mongo_internal.h"

#include "util/error.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    JSON_STRING,
    JSON_ATOM,
    JSON_ARRAY,
    JSON_OBJECT,
} json_kind;

typedef struct json_value json_value;

typedef struct {
    char *key_raw;
    char *key_decoded;
    json_value *value;
} json_pair;

struct json_value {
    json_kind kind;
    char *raw;
    json_value **items;
    size_t item_count;
    json_pair *pairs;
    size_t pair_count;
};

typedef struct {
    const char *s;
    size_t pos;
    size_t len;
    scribe_arena arena;
} parser;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} sbuf;

/*
 * Advances the parser past JSON whitespace. The serializer does not preserve
 * insignificant whitespace, so whitespace only matters between parsed tokens.
 */
static void skip_ws(parser *p) {
    while (p->pos < p->len && isspace((unsigned char)p->s[p->pos])) {
        p->pos++;
    }
}

/*
 * Copies a byte range into parser arena memory and appends a NUL terminator.
 * JSON value nodes keep raw spellings as strings owned by the parser arena.
 */
static char *arena_copy(scribe_arena *arena, const char *s, size_t len) {
    char *out = scribe_arena_strdup_len(arena, s, len);
    return out;
}

/*
 * Allocates a JSON AST node in the parser arena and initializes its kind. Nodes
 * are freed all at once when the parser arena is destroyed.
 */
static json_value *new_value(parser *p, json_kind kind) {
    json_value *v = (json_value *)scribe_arena_alloc(&p->arena, sizeof(*v), _Alignof(json_value));
    if (v != NULL) {
        memset(v, 0, sizeof(*v));
        v->kind = kind;
    }
    return v;
}

/*
 * Produces the decoded key bytes used for object-key sorting. This is not a full
 * JSON string unescaper for values; it exists only to compare object member
 * names deterministically.
 */
static scribe_error_t decode_json_string(parser *p, const char *raw, size_t raw_len, char **out) {
    char *decoded;
    size_t off = 1;
    size_t used = 0;

    /*
     * Object-key sorting uses decoded key bytes. libbson emits canonical
     * Extended JSON with escaped non-ASCII when needed; for v1 sorting we keep
     * escaped Unicode sequences as literal escape bytes instead of implementing
     * full Unicode normalization. The important property is deterministic
     * ordering for the JSON libbson actually emits.
     */
    if (raw_len < 2 || raw[0] != '"' || raw[raw_len - 1u] != '"') {
        return scribe_set_error(SCRIBE_EMALFORMED, "invalid JSON string");
    }
    decoded = (char *)scribe_arena_alloc(&p->arena, raw_len, _Alignof(char));
    if (decoded == NULL) {
        return SCRIBE_ENOMEM;
    }
    while (off + 1u < raw_len) {
        unsigned char c = (unsigned char)raw[off++];
        if (c == '\\') {
            if (off + 1u >= raw_len) {
                return scribe_set_error(SCRIBE_EMALFORMED, "truncated JSON escape");
            }
            c = (unsigned char)raw[off++];
            if (c == 'u') {
                if (off + 4u > raw_len) {
                    return scribe_set_error(SCRIBE_EMALFORMED, "truncated JSON unicode escape");
                }
                /* Sorting only needs deterministic key bytes for libbson output.
                 * Preserve escaped non-ASCII as its escape sequence. */
                decoded[used++] = '\\';
                decoded[used++] = 'u';
                memcpy(decoded + used, raw + off, 4u);
                used += 4u;
                off += 4u;
            } else {
                decoded[used++] = (char)c;
            }
        } else {
            decoded[used++] = (char)c;
        }
    }
    decoded[used] = '\0';
    *out = decoded;
    return SCRIBE_OK;
}

static scribe_error_t parse_value(parser *p, json_value **out);

/*
 * Parses a quoted JSON string and preserves its raw spelling. Keeping the raw
 * spelling lets serialization avoid changing string escape choices in values.
 */
static scribe_error_t parse_string_value(parser *p, json_value **out) {
    size_t start = p->pos;
    int escaped = 0;
    json_value *v;

    if (p->s[p->pos] != '"') {
        return scribe_set_error(SCRIBE_EMALFORMED, "expected JSON string");
    }
    p->pos++;
    while (p->pos < p->len) {
        char c = p->s[p->pos++];
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (c == '\\') {
            escaped = 1;
            continue;
        }
        if (c == '"') {
            v = new_value(p, JSON_STRING);
            if (v == NULL) {
                return SCRIBE_ENOMEM;
            }
            v->raw = arena_copy(&p->arena, p->s + start, p->pos - start);
            if (v->raw == NULL) {
                return SCRIBE_ENOMEM;
            }
            *out = v;
            return SCRIBE_OK;
        }
    }
    return scribe_set_error(SCRIBE_EMALFORMED, "unterminated JSON string");
}

/*
 * Grows the arena-owned child pointer array for a JSON array node. Arena growth
 * is copy-on-grow because previous arena allocations are not individually freed.
 */
static scribe_error_t grow_items(parser *p, json_value ***items, size_t *cap, size_t count) {
    json_value **grown;
    size_t new_cap = *cap == 0 ? 8u : *cap * 2u;

    if (count < *cap) {
        return SCRIBE_OK;
    }
    grown = (json_value **)scribe_arena_alloc(&p->arena, sizeof(*grown) * new_cap, _Alignof(json_value *));
    if (grown == NULL) {
        return SCRIBE_ENOMEM;
    }
    if (*items != NULL) {
        memcpy(grown, *items, sizeof(*grown) * count);
    }
    *items = grown;
    *cap = new_cap;
    return SCRIBE_OK;
}

/*
 * Grows the arena-owned key/value pair array for a JSON object node. The object
 * parser later sorts this array in place by decoded key.
 */
static scribe_error_t grow_pairs(parser *p, json_pair **pairs, size_t *cap, size_t count) {
    json_pair *grown;
    size_t new_cap = *cap == 0 ? 8u : *cap * 2u;

    if (count < *cap) {
        return SCRIBE_OK;
    }
    grown = (json_pair *)scribe_arena_alloc(&p->arena, sizeof(*grown) * new_cap, _Alignof(json_pair));
    if (grown == NULL) {
        return SCRIBE_ENOMEM;
    }
    if (*pairs != NULL) {
        memcpy(grown, *pairs, sizeof(*grown) * count);
    }
    *pairs = grown;
    *cap = new_cap;
    return SCRIBE_OK;
}

/*
 * Parses a JSON array, preserving item order exactly. RFC 8785-style
 * canonicalization sorts object keys but never reorders arrays.
 */
static scribe_error_t parse_array(parser *p, json_value **out) {
    json_value *v = new_value(p, JSON_ARRAY);
    size_t cap = 0;
    scribe_error_t err;

    if (v == NULL) {
        return SCRIBE_ENOMEM;
    }
    p->pos++;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') {
        p->pos++;
        *out = v;
        return SCRIBE_OK;
    }
    while (p->pos < p->len) {
        err = grow_items(p, &v->items, &cap, v->item_count);
        if (err != SCRIBE_OK) {
            return err;
        }
        err = parse_value(p, &v->items[v->item_count++]);
        if (err != SCRIBE_OK) {
            return err;
        }
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') {
            p->pos++;
            skip_ws(p);
            continue;
        }
        if (p->pos < p->len && p->s[p->pos] == ']') {
            p->pos++;
            *out = v;
            return SCRIBE_OK;
        }
        return scribe_set_error(SCRIBE_EMALFORMED, "expected ',' or ']'");
    }
    return scribe_set_error(SCRIBE_EMALFORMED, "unterminated JSON array");
}

/*
 * qsort comparator for object members. It compares decoded key bytes so escaped
 * keys sort by their actual key spelling as far as this v1 parser supports.
 */
static int pair_cmp(const void *a, const void *b) {
    const json_pair *pa = (const json_pair *)a;
    const json_pair *pb = (const json_pair *)b;
    return strcmp(pa->key_decoded, pb->key_decoded);
}

/*
 * Parses a JSON object and sorts its members by decoded key before returning.
 * Values are parsed recursively, which gives deterministic key order at every
 * object nesting level.
 */
static scribe_error_t parse_object(parser *p, json_value **out) {
    json_value *v = new_value(p, JSON_OBJECT);
    size_t cap = 0;
    scribe_error_t err;

    if (v == NULL) {
        return SCRIBE_ENOMEM;
    }
    p->pos++;
    skip_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') {
        p->pos++;
        *out = v;
        return SCRIBE_OK;
    }
    while (p->pos < p->len) {
        json_value *key;
        err = grow_pairs(p, &v->pairs, &cap, v->pair_count);
        if (err != SCRIBE_OK) {
            return err;
        }
        err = parse_string_value(p, &key);
        if (err != SCRIBE_OK) {
            return err;
        }
        v->pairs[v->pair_count].key_raw = key->raw;
        err = decode_json_string(p, key->raw, strlen(key->raw), &v->pairs[v->pair_count].key_decoded);
        if (err != SCRIBE_OK) {
            return err;
        }
        skip_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != ':') {
            return scribe_set_error(SCRIBE_EMALFORMED, "expected ':'");
        }
        p->pos++;
        err = parse_value(p, &v->pairs[v->pair_count].value);
        if (err != SCRIBE_OK) {
            return err;
        }
        v->pair_count++;
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') {
            p->pos++;
            skip_ws(p);
            continue;
        }
        if (p->pos < p->len && p->s[p->pos] == '}') {
            p->pos++;
            /*
             * Canonicalization sorts object members by decoded key. Values are
             * otherwise preserved exactly as parsed, so Scribe core never adds
             * MongoDB semantics beyond deterministic key order.
             */
            qsort(v->pairs, v->pair_count, sizeof(v->pairs[0]), pair_cmp);
            *out = v;
            return SCRIBE_OK;
        }
        return scribe_set_error(SCRIBE_EMALFORMED, "expected ',' or '}'");
    }
    return scribe_set_error(SCRIBE_EMALFORMED, "unterminated JSON object");
}

/*
 * Parses a non-string, non-container JSON token such as a number, boolean, or
 * null. The raw token spelling is preserved for serialization.
 */
static scribe_error_t parse_atom(parser *p, json_value **out) {
    size_t start = p->pos;
    json_value *v;

    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ',' || c == ']' || c == '}' || isspace((unsigned char)c)) {
            break;
        }
        p->pos++;
    }
    if (p->pos == start) {
        return scribe_set_error(SCRIBE_EMALFORMED, "expected JSON value");
    }
    v = new_value(p, JSON_ATOM);
    if (v == NULL) {
        return SCRIBE_ENOMEM;
    }
    v->raw = arena_copy(&p->arena, p->s + start, p->pos - start);
    if (v->raw == NULL) {
        return SCRIBE_ENOMEM;
    }
    *out = v;
    return SCRIBE_OK;
}

/*
 * Dispatches to the appropriate value parser after skipping leading whitespace.
 * This is the recursive entry point for arrays and objects.
 */
static scribe_error_t parse_value(parser *p, json_value **out) {
    skip_ws(p);
    if (p->pos >= p->len) {
        return scribe_set_error(SCRIBE_EMALFORMED, "unexpected end of JSON");
    }
    if (p->s[p->pos] == '"') {
        return parse_string_value(p, out);
    }
    if (p->s[p->pos] == '{') {
        return parse_object(p, out);
    }
    if (p->s[p->pos] == '[') {
        return parse_array(p, out);
    }
    return parse_atom(p, out);
}

/*
 * Ensures the serializer buffer has room for extra bytes plus a trailing NUL.
 * The buffer is heap-owned because canonical JSON is returned to callers after
 * the parse arena is destroyed.
 */
static scribe_error_t sb_grow(sbuf *b, size_t extra) {
    char *grown;
    size_t new_cap;

    if (extra <= b->cap - b->len) {
        return SCRIBE_OK;
    }
    new_cap = b->cap == 0 ? 256u : b->cap;
    while (extra > new_cap - b->len) {
        new_cap *= 2u;
    }
    grown = (char *)realloc(b->data, new_cap);
    if (grown == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to grow JSON buffer");
    }
    b->data = grown;
    b->cap = new_cap;
    return SCRIBE_OK;
}

/*
 * Appends raw bytes to the serializer buffer and keeps it NUL-terminated for
 * convenience. out_len still reports the exact byte length.
 */
static scribe_error_t sb_append(sbuf *b, const char *s, size_t len) {
    scribe_error_t err = sb_grow(b, len + 1u);
    if (err != SCRIBE_OK) {
        return err;
    }
    memcpy(b->data + b->len, s, len);
    b->len += len;
    b->data[b->len] = '\0';
    return SCRIBE_OK;
}

/*
 * Serializes a parsed JSON value with no insignificant whitespace. Object pairs
 * are already sorted by parse_object(), while arrays and raw values keep their
 * parsed order/spelling.
 */
static scribe_error_t serialize_value(const json_value *v, sbuf *b) {
    size_t i;
    scribe_error_t err;

    if (v->kind == JSON_STRING || v->kind == JSON_ATOM) {
        return sb_append(b, v->raw, strlen(v->raw));
    }
    if (v->kind == JSON_ARRAY) {
        if ((err = sb_append(b, "[", 1)) != SCRIBE_OK) {
            return err;
        }
        for (i = 0; i < v->item_count; i++) {
            if (i != 0 && (err = sb_append(b, ",", 1)) != SCRIBE_OK) {
                return err;
            }
            if ((err = serialize_value(v->items[i], b)) != SCRIBE_OK) {
                return err;
            }
        }
        return sb_append(b, "]", 1);
    }
    if ((err = sb_append(b, "{", 1)) != SCRIBE_OK) {
        return err;
    }
    for (i = 0; i < v->pair_count; i++) {
        if (i != 0 && (err = sb_append(b, ",", 1)) != SCRIBE_OK) {
            return err;
        }
        if ((err = sb_append(b, v->pairs[i].key_raw, strlen(v->pairs[i].key_raw))) != SCRIBE_OK ||
            (err = sb_append(b, ":", 1)) != SCRIBE_OK || (err = serialize_value(v->pairs[i].value, b)) != SCRIBE_OK) {
            return err;
        }
    }
    return sb_append(b, "}", 1);
}

/*
 * Canonicalizes a libbson-style Extended JSON string. The output buffer is
 * heap-owned, has no insignificant whitespace, and has recursively sorted object keys.
 */
scribe_error_t scribe_mongo_canonicalize_json(const char *json, char **out, size_t *out_len) {
    parser p;
    json_value *root = NULL;
    sbuf b;
    scribe_error_t err;

    if (json == NULL || out == NULL || out_len == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid JSON canonicalization argument");
    }
    /*
     * This is a small canonicalizer for libbson's canonical Extended JSON
     * output. It parses enough JSON to reorder object keys recursively and
     * serializes without insignificant whitespace. Arrays preserve order, and
     * strings/atoms preserve their raw spelling.
     */
    memset(&p, 0, sizeof(p));
    p.s = json;
    p.len = strlen(json);
    err = scribe_arena_init(&p.arena, p.len * 8u + 4096u);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = parse_value(&p, &root);
    if (err == SCRIBE_OK) {
        skip_ws(&p);
        if (p.pos != p.len) {
            err = scribe_set_error(SCRIBE_EMALFORMED, "trailing JSON input");
        }
    }
    memset(&b, 0, sizeof(b));
    if (err == SCRIBE_OK) {
        err = serialize_value(root, &b);
    }
    scribe_arena_destroy(&p.arena);
    if (err != SCRIBE_OK) {
        free(b.data);
        return err;
    }
    *out = b.data;
    *out_len = b.len;
    return SCRIBE_OK;
}

/*
 * Converts a BSON document to libbson canonical Extended JSON and then applies
 * Scribe's deterministic key sorting. The returned bytes become blob payloads.
 */
scribe_error_t scribe_mongo_canonicalize_bson(const bson_t *doc, uint8_t **out, size_t *out_len) {
    char *json;
    size_t canonical_len = 0;
    char *canonical = NULL;
    scribe_error_t err;

    json = bson_as_canonical_extended_json(doc, NULL);
    if (json == NULL) {
        return scribe_set_error(SCRIBE_EADAPTER, "failed to render canonical Extended JSON");
    }
    err = scribe_mongo_canonicalize_json(json, &canonical, &canonical_len);
    bson_free(json);
    if (err != SCRIBE_OK) {
        return err;
    }
    *out = (uint8_t *)canonical;
    *out_len = canonical_len;
    return SCRIBE_OK;
}

/*
 * Extracts `_id`, canonicalizes that BSON value, and returns the canonical JSON
 * value text used as the document leaf name in Scribe's Mongo tree shape.
 */
scribe_error_t scribe_mongo_canonicalize_id(const bson_t *doc, char **out) {
    bson_iter_t iter;
    bson_t tmp;
    uint8_t *canonical = NULL;
    size_t canonical_len = 0;
    scribe_error_t err;
    const char *prefix = "{\"v\":";
    size_t prefix_len = strlen(prefix);

    /*
     * Reuse the document canonicalizer for _id by wrapping the BSON value in a
     * temporary object {"v": <id>}. After canonicalization, strip the wrapper
     * and use only the canonical value as the tree leaf name. That is why string
     * ids appear as quoted JSON strings, while ObjectId values appear as
     * {"$oid":"..."}.
     */
    if (!bson_iter_init_find(&iter, doc, "_id")) {
        return scribe_set_error(SCRIBE_EADAPTER, "MongoDB document is missing _id");
    }
    bson_init(&tmp);
    if (!bson_append_value(&tmp, "v", 1, bson_iter_value(&iter))) {
        bson_destroy(&tmp);
        return scribe_set_error(SCRIBE_EADAPTER, "failed to copy _id value");
    }
    err = scribe_mongo_canonicalize_bson(&tmp, &canonical, &canonical_len);
    bson_destroy(&tmp);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (canonical_len < prefix_len + 1u || memcmp(canonical, prefix, prefix_len) != 0 ||
        canonical[canonical_len - 1u] != '}') {
        free(canonical);
        return scribe_set_error(SCRIBE_EADAPTER, "unexpected canonical _id wrapper");
    }
    canonical[canonical_len - 1u] = '\0';
    *out = strdup((char *)canonical + prefix_len);
    free(canonical);
    if (*out == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate canonical _id");
    }
    return SCRIBE_OK;
}
