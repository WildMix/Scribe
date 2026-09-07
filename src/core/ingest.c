/*
 * Core JSON ingest endpoint.
 *
 * This is the small HTTP-facing commit path used by `scribe` itself. It accepts
 * a deliberately narrow JSON shape, decodes base64 payloads into normal
 * scribe_change_batch events, and leaves all repository mutation to
 * scribe_commit_batch().
 */
#include "core/internal.h"

#include "util/error.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *p;
    const char *end;
} json_parser;

typedef struct {
    scribe_change_event *events;
    char ***paths;
    uint8_t **payloads;
    size_t count;
    size_t cap;
} ingest_builder;

typedef struct {
    char *author_name;
    char *author_email;
    char *author_source;
    char *committer_name;
    char *committer_email;
    char *committer_source;
    char *process_name;
    char *process_version;
    char *process_params;
    char *process_correlation_id;
    char *message;
    size_t message_len;
    char *checkpoint_adapter;
    char *checkpoint_key;
    uint8_t *checkpoint_value;
    size_t checkpoint_value_len;
    int has_checkpoint;
    int64_t timestamp;
    ingest_builder builder;
} ingest_request;

typedef enum {
    INGEST_EVENT_OP_UNSET = 0,
    INGEST_EVENT_OP_PUT,
    INGEST_EVENT_OP_DELETE,
} ingest_event_op;

static int checkpoint_name_valid(const char *value) {
    size_t i;

    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    for (i = 0u; value[i] != '\0'; i++) {
        char ch = value[i];

        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
              ch == '_' || ch == '.')) {
            return 0;
        }
    }
    return 1;
}
static int path_component_valid(const char *component) {
    const unsigned char *p;

    if (component == NULL || component[0] == '\0') {
        return 0;
    }
    for (p = (const unsigned char *)component; *p != '\0'; p++) {
        if (*p <= 0x1fu || *p == 0x7fu || *p == '\t' || *p == '\n') {
            return 0;
        }
    }
    return 1;
}

static int64_t now_unix_nanos(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return ((int64_t)ts.tv_sec * INT64_C(1000000000)) + (int64_t)ts.tv_nsec;
}

static void builder_init(ingest_builder *builder) {
    if (builder != NULL) {
        memset(builder, 0, sizeof(*builder));
    }
}

static void builder_destroy(ingest_builder *builder) {
    size_t i;

    if (builder == NULL) {
        return;
    }
    for (i = 0; i < builder->count; i++) {
        size_t j;

        if (builder->paths != NULL && builder->paths[i] != NULL) {
            for (j = 0; j < builder->events[i].path_len; j++) {
                free(builder->paths[i][j]);
            }
            free(builder->paths[i]);
        }
        if (builder->payloads != NULL) {
            free(builder->payloads[i]);
        }
    }
    free(builder->events);
    free(builder->paths);
    free(builder->payloads);
    memset(builder, 0, sizeof(*builder));
}

static scribe_error_t builder_reserve(ingest_builder *builder, size_t needed) {
    scribe_change_event *events;
    char ***paths;
    uint8_t **payloads;
    size_t new_cap;

    if (needed <= builder->cap) {
        return SCRIBE_OK;
    }
    new_cap = builder->cap == 0u ? 16u : builder->cap;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2u) {
            return scribe_set_error(SCRIBE_ENOMEM, "ingest batch is too large");
        }
        new_cap *= 2u;
    }
    events = (scribe_change_event *)realloc(builder->events, sizeof(*events) * new_cap);
    if (events == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to grow ingest events");
    }
    builder->events = events;
    paths = (char ***)realloc(builder->paths, sizeof(*paths) * new_cap);
    if (paths == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to grow ingest paths");
    }
    builder->paths = paths;
    payloads = (uint8_t **)realloc(builder->payloads, sizeof(*payloads) * new_cap);
    if (payloads == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to grow ingest payloads");
    }
    builder->payloads = payloads;
    while (builder->cap < new_cap) {
        memset(&builder->events[builder->cap], 0, sizeof(builder->events[builder->cap]));
        builder->paths[builder->cap] = NULL;
        builder->payloads[builder->cap] = NULL;
        builder->cap++;
    }
    return SCRIBE_OK;
}

static scribe_error_t builder_add(ingest_builder *builder, const char *const *path, size_t path_len,
                                  const uint8_t *payload, size_t payload_len) {
    char **path_copy = NULL;
    uint8_t *payload_copy = NULL;
    size_t i;
    size_t idx;
    scribe_error_t err;

    if (path == NULL || path_len == 0u || (payload == NULL && payload_len != 0u)) {
        return scribe_set_error(SCRIBE_EMALFORMED, "invalid ingest event");
    }
    for (i = 0; i < path_len; i++) {
        if (!path_component_valid(path[i])) {
            return scribe_set_error(SCRIBE_EPATH, "invalid ingest path component");
        }
    }
    err = builder_reserve(builder, builder->count + 1u);
    if (err != SCRIBE_OK) {
        return err;
    }
    path_copy = (char **)calloc(path_len, sizeof(*path_copy));
    if (path_copy == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate ingest path");
    }
    for (i = 0; i < path_len; i++) {
        path_copy[i] = strdup(path[i]);
        if (path_copy[i] == NULL) {
            while (i > 0u) {
                free(path_copy[--i]);
            }
            free(path_copy);
            return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate ingest path component");
        }
    }
    if (payload != NULL) {
        payload_copy = (uint8_t *)malloc(payload_len == 0u ? 1u : payload_len);
        if (payload_copy == NULL) {
            for (i = 0; i < path_len; i++) {
                free(path_copy[i]);
            }
            free(path_copy);
            return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate ingest payload");
        }
        if (payload_len != 0u) {
            memcpy(payload_copy, payload, payload_len);
        }
    }
    idx = builder->count++;
    builder->paths[idx] = path_copy;
    builder->payloads[idx] = payload_copy;
    builder->events[idx].path = (const char *const *)path_copy;
    builder->events[idx].path_len = path_len;
    builder->events[idx].payload = payload_copy;
    builder->events[idx].payload_len = payload_len;
    return SCRIBE_OK;
}

static scribe_error_t builder_add_tombstone(ingest_builder *builder, const char *const *path, size_t path_len) {
    return builder_add(builder, path, path_len, NULL, 0u);
}

static int b64_value(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (int)(ch - 'A');
    }
    if (ch >= 'a' && ch <= 'z') {
        return (int)(ch - 'a') + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return (int)(ch - '0') + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

static scribe_error_t base64_decode(const char *text, uint8_t **out, size_t *out_len) {
    size_t len;
    size_t i;
    size_t o = 0u;
    uint8_t *buf;

    if (text == NULL || out == NULL || out_len == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid base64 output");
    }
    len = strlen(text);
    if (len % 4u != 0u) {
        return scribe_set_error(SCRIBE_EMALFORMED, "malformed base64 payload");
    }
    buf = (uint8_t *)malloc((len / 4u) * 3u + 1u);
    if (buf == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate decoded payload");
    }
    for (i = 0u; i < len; i += 4u) {
        int v0 = b64_value((unsigned char)text[i]);
        int v1 = b64_value((unsigned char)text[i + 1u]);
        int v2 = text[i + 2u] == '=' ? -2 : b64_value((unsigned char)text[i + 2u]);
        int v3 = text[i + 3u] == '=' ? -2 : b64_value((unsigned char)text[i + 3u]);
        uint32_t triple;

        if (v0 < 0 || v1 < 0 || (v2 < 0 && v2 != -2) || (v3 < 0 && v3 != -2) || (v2 == -2 && v3 != -2) ||
            (v2 == -2 && i + 4u != len) || (v3 == -2 && i + 4u != len)) {
            free(buf);
            return scribe_set_error(SCRIBE_EMALFORMED, "malformed base64 payload");
        }
        triple = ((uint32_t)v0 << 18u) | ((uint32_t)v1 << 12u) | ((uint32_t)(v2 < 0 ? 0 : v2) << 6u) |
                 (uint32_t)(v3 < 0 ? 0 : v3);
        buf[o++] = (uint8_t)((triple >> 16u) & 0xffu);
        if (v2 != -2) {
            buf[o++] = (uint8_t)((triple >> 8u) & 0xffu);
        }
        if (v3 != -2) {
            buf[o++] = (uint8_t)(triple & 0xffu);
        }
    }
    *out = buf;
    *out_len = o;
    return SCRIBE_OK;
}

static void json_skip_ws(json_parser *jp) {
    while (jp->p < jp->end && isspace((unsigned char)*jp->p)) {
        jp->p++;
    }
}

static int json_consume(json_parser *jp, char ch) {
    json_skip_ws(jp);
    if (jp->p >= jp->end || *jp->p != ch) {
        return 0;
    }
    jp->p++;
    return 1;
}

static char *json_parse_string(json_parser *jp) {
    char *out;
    size_t cap = 32u;
    size_t len = 0u;

    json_skip_ws(jp);
    if (jp->p >= jp->end || *jp->p != '"') {
        return NULL;
    }
    jp->p++;
    out = (char *)malloc(cap);
    if (out == NULL) {
        return NULL;
    }
    while (jp->p < jp->end) {
        unsigned char ch = (unsigned char)*jp->p++;

        if (ch == '"') {
            out[len] = '\0';
            return out;
        }
        if (ch == '\\') {
            if (jp->p >= jp->end) {
                free(out);
                return NULL;
            }
            ch = (unsigned char)*jp->p++;
            switch (ch) {
            case '"':
            case '\\':
            case '/':
                break;
            case 'b':
                ch = '\b';
                break;
            case 'f':
                ch = '\f';
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            default:
                free(out);
                return NULL;
            }
        } else if (ch < 0x20u) {
            free(out);
            return NULL;
        }
        if (len + 1u >= cap) {
            char *grown;

            if (cap > SIZE_MAX / 2u) {
                free(out);
                return NULL;
            }
            cap *= 2u;
            grown = (char *)realloc(out, cap);
            if (grown == NULL) {
                free(out);
                return NULL;
            }
            out = grown;
        }
        out[len++] = (char)ch;
    }
    free(out);
    return NULL;
}

static int json_parse_i64(json_parser *jp, int64_t *out) {
    const char *start;
    int sign = 1;
    uint64_t limit;
    uint64_t value = 0u;

    json_skip_ws(jp);
    if (jp->p >= jp->end) {
        return 0;
    }
    if (*jp->p == '-') {
        sign = -1;
        jp->p++;
    }
    start = jp->p;
    if (jp->p >= jp->end || !isdigit((unsigned char)*jp->p)) {
        return 0;
    }
    limit = sign < 0 ? ((uint64_t)INT64_MAX + 1u) : (uint64_t)INT64_MAX;
    while (jp->p < jp->end && isdigit((unsigned char)*jp->p)) {
        unsigned int digit = (unsigned int)(*jp->p - '0');

        if (value > (limit - digit) / 10u) {
            return 0;
        }
        value = (value * 10u) + digit;
        jp->p++;
    }
    if (jp->p == start) {
        return 0;
    }
    if (sign < 0 && value == ((uint64_t)INT64_MAX + 1u)) {
        *out = INT64_MIN;
    } else if (sign < 0) {
        *out = -((int64_t)value);
    } else {
        *out = (int64_t)value;
    }
    return 1;
}

static int json_skip_value(json_parser *jp);

static int json_skip_array(json_parser *jp) {
    if (!json_consume(jp, '[')) {
        return 0;
    }
    json_skip_ws(jp);
    if (json_consume(jp, ']')) {
        return 1;
    }
    for (;;) {
        if (!json_skip_value(jp)) {
            return 0;
        }
        json_skip_ws(jp);
        if (json_consume(jp, ']')) {
            return 1;
        }
        if (!json_consume(jp, ',')) {
            return 0;
        }
    }
}

static int json_skip_object(json_parser *jp) {
    if (!json_consume(jp, '{')) {
        return 0;
    }
    json_skip_ws(jp);
    if (json_consume(jp, '}')) {
        return 1;
    }
    for (;;) {
        char *key = json_parse_string(jp);

        if (key == NULL) {
            return 0;
        }
        free(key);
        if (!json_consume(jp, ':') || !json_skip_value(jp)) {
            return 0;
        }
        json_skip_ws(jp);
        if (json_consume(jp, '}')) {
            return 1;
        }
        if (!json_consume(jp, ',')) {
            return 0;
        }
    }
}

static int json_skip_value(json_parser *jp) {
    json_skip_ws(jp);
    if (jp->p >= jp->end) {
        return 0;
    }
    if (*jp->p == '"') {
        char *s = json_parse_string(jp);

        free(s);
        return s != NULL;
    }
    if (*jp->p == '{') {
        return json_skip_object(jp);
    }
    if (*jp->p == '[') {
        return json_skip_array(jp);
    }
    if (jp->end - jp->p >= 4 && strncmp(jp->p, "null", 4u) == 0) {
        jp->p += 4;
        return 1;
    }
    if (jp->end - jp->p >= 4 && strncmp(jp->p, "true", 4u) == 0) {
        jp->p += 4;
        return 1;
    }
    if (jp->end - jp->p >= 5 && strncmp(jp->p, "false", 5u) == 0) {
        jp->p += 5;
        return 1;
    }
    while (jp->p < jp->end && strchr(" \t\r\n,]}", *jp->p) == NULL) {
        jp->p++;
    }
    return 1;
}

static scribe_error_t request_init(ingest_request *req) {
    memset(req, 0, sizeof(*req));
    req->author_name = strdup("unknown");
    req->author_email = strdup("");
    req->author_source = strdup("http");
    req->committer_name = strdup("scribe");
    req->committer_email = strdup("");
    req->committer_source = strdup("scribe");
    req->process_name = strdup("scribe-ingest");
    req->process_version = strdup(SCRIBE_VERSION);
    req->process_params = strdup("");
    req->process_correlation_id = strdup("");
    req->message = strdup("http ingest");
    if (req->author_name == NULL || req->author_email == NULL || req->author_source == NULL ||
        req->committer_name == NULL || req->committer_email == NULL || req->committer_source == NULL ||
        req->process_name == NULL || req->process_version == NULL || req->process_params == NULL ||
        req->process_correlation_id == NULL || req->message == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate ingest defaults");
    }
    req->message_len = strlen(req->message);
    req->timestamp = now_unix_nanos();
    builder_init(&req->builder);
    return SCRIBE_OK;
}

static void request_destroy(ingest_request *req) {
    free(req->author_name);
    free(req->author_email);
    free(req->author_source);
    free(req->committer_name);
    free(req->committer_email);
    free(req->committer_source);
    free(req->process_name);
    free(req->process_version);
    free(req->process_params);
    free(req->process_correlation_id);
    free(req->message);
    builder_destroy(&req->builder);
    free(req->checkpoint_adapter);
    free(req->checkpoint_key);
    free(req->checkpoint_value);
    memset(req, 0, sizeof(*req));
}

static void replace_string(char **slot, char *value) {
    if (value != NULL) {
        free(*slot);
        *slot = value;
    }
}

static int parse_string_field_object(json_parser *jp, const char *kind, ingest_request *req) {
    if (!json_consume(jp, '{')) {
        return 0;
    }
    json_skip_ws(jp);
    if (json_consume(jp, '}')) {
        return 1;
    }
    for (;;) {
        char *key = json_parse_string(jp);
        char *value;

        if (key == NULL || !json_consume(jp, ':')) {
            free(key);
            return 0;
        }
        value = json_parse_string(jp);
        if (value == NULL) {
            free(key);
            return 0;
        }
        if (strcmp(kind, "author") == 0) {
            if (strcmp(key, "name") == 0) {
                replace_string(&req->author_name, value);
            } else if (strcmp(key, "email") == 0) {
                replace_string(&req->author_email, value);
            } else if (strcmp(key, "source") == 0) {
                replace_string(&req->author_source, value);
            } else {
                free(value);
            }
        } else if (strcmp(kind, "committer") == 0) {
            if (strcmp(key, "name") == 0) {
                replace_string(&req->committer_name, value);
            } else if (strcmp(key, "email") == 0) {
                replace_string(&req->committer_email, value);
            } else if (strcmp(key, "source") == 0) {
                replace_string(&req->committer_source, value);
            } else {
                free(value);
            }
        } else if (strcmp(kind, "process") == 0) {
            if (strcmp(key, "name") == 0) {
                replace_string(&req->process_name, value);
            } else if (strcmp(key, "version") == 0) {
                replace_string(&req->process_version, value);
            } else if (strcmp(key, "params") == 0) {
                replace_string(&req->process_params, value);
            } else if (strcmp(key, "correlation_id") == 0) {
                replace_string(&req->process_correlation_id, value);
            } else {
                free(value);
            }
        } else {
            free(value);
        }
        free(key);
        json_skip_ws(jp);
        if (json_consume(jp, '}')) {
            return 1;
        }
        if (!json_consume(jp, ',')) {
            return 0;
        }
    }
}

static int parse_path_array(json_parser *jp, char ***out_path, size_t *out_len) {
    char **path = NULL;
    size_t count = 0u;
    size_t cap = 0u;

    if (!json_consume(jp, '[')) {
        return 0;
    }
    json_skip_ws(jp);
    if (json_consume(jp, ']')) {
        return 0;
    }
    for (;;) {
        char *component = json_parse_string(jp);

        if (component == NULL || !path_component_valid(component)) {
            free(component);
            goto fail;
        }
        if (count == cap) {
            size_t new_cap = cap == 0u ? 4u : cap * 2u;
            char **grown = (char **)realloc(path, sizeof(*path) * new_cap);

            if (grown == NULL) {
                free(component);
                goto fail;
            }
            path = grown;
            cap = new_cap;
        }
        path[count++] = component;
        json_skip_ws(jp);
        if (json_consume(jp, ']')) {
            *out_path = path;
            *out_len = count;
            return 1;
        }
        if (!json_consume(jp, ',')) {
            goto fail;
        }
    }

fail:
    while (count > 0u) {
        free(path[--count]);
    }
    free(path);
    return 0;
}

static void free_path(char **path, size_t path_len) {
    size_t i;

    for (i = 0; i < path_len; i++) {
        free(path[i]);
    }
    free(path);
}

static int parse_event(json_parser *jp, ingest_request *req) {
    char **path = NULL;
    size_t path_len = 0u;
    uint8_t *payload = NULL;
    size_t payload_len = 0u;
    int has_path = 0;
    int has_payload = 0;
    int payload_is_null = 0;
    int has_op = 0;
    ingest_event_op op = INGEST_EVENT_OP_UNSET;
    scribe_error_t err;

    if (!json_consume(jp, '{')) {
        return 0;
    }
    json_skip_ws(jp);
    if (json_consume(jp, '}')) {
        return 0;
    }
    for (;;) {
        char *key = json_parse_string(jp);

        if (key == NULL || !json_consume(jp, ':')) {
            free(key);
            goto fail;
        }
        if (strcmp(key, "path") == 0) {
            if (has_path) {
                free(key);
                goto fail;
            }
            if (!parse_path_array(jp, &path, &path_len)) {
                free(key);
                goto fail;
            }
            has_path = 1;
        } else if (strcmp(key, "op") == 0) {
            char *value;

            if (has_op) {
                free(key);
                goto fail;
            }
            value = json_parse_string(jp);
            if (value == NULL) {
                free(key);
                goto fail;
            }
            if (strcmp(value, "put") == 0) {
                op = INGEST_EVENT_OP_PUT;
            } else if (strcmp(value, "delete") == 0) {
                op = INGEST_EVENT_OP_DELETE;
            } else {
                free(value);
                free(key);
                goto fail;
            }
            free(value);
            has_op = 1;
        } else if (strcmp(key, "payload") == 0) {
            if (has_payload) {
                free(key);
                goto fail;
            }
            json_skip_ws(jp);
            if (jp->end - jp->p >= 4 && strncmp(jp->p, "null", 4u) == 0) {
                jp->p += 4;
                payload_is_null = 1;
                has_payload = 1;
            } else {
                char *encoded = json_parse_string(jp);

                if (encoded == NULL) {
                    free(key);
                    goto fail;
                }
                err = base64_decode(encoded, &payload, &payload_len);
                free(encoded);
                if (err != SCRIBE_OK) {
                    free(key);
                    goto fail;
                }
                has_payload = 1;
            }
        } else if (!json_skip_value(jp)) {
            free(key);
            goto fail;
        }
        free(key);
        json_skip_ws(jp);
        if (json_consume(jp, '}')) {
            break;
        }
        if (!json_consume(jp, ',')) {
            goto fail;
        }
    }
    if (!has_path) {
        goto fail;
    }
    if (has_op) {
        if (op == INGEST_EVENT_OP_PUT) {
            if (!has_payload || payload_is_null) {
                goto fail;
            }
            err = builder_add(&req->builder, (const char *const *)path, path_len, payload, payload_len);
        } else {
            if (has_payload && !payload_is_null) {
                goto fail;
            }
            err = builder_add_tombstone(&req->builder, (const char *const *)path, path_len);
        }
    } else if (has_payload) {
        if (payload_is_null) {
            err = builder_add_tombstone(&req->builder, (const char *const *)path, path_len);
        } else {
            err = builder_add(&req->builder, (const char *const *)path, path_len, payload, payload_len);
        }
    } else {
        goto fail;
    }
    free(payload);
    free_path(path, path_len);
    return err == SCRIBE_OK;

fail:
    free(payload);
    if (path != NULL) {
        free_path(path, path_len);
    }
    return 0;
}

static int parse_events(json_parser *jp, ingest_request *req) {
    if (!json_consume(jp, '[')) {
        return 0;
    }
    json_skip_ws(jp);
    if (json_consume(jp, ']')) {
        return 0;
    }
    for (;;) {
        if (!parse_event(jp, req)) {
            return 0;
        }
        json_skip_ws(jp);
        if (json_consume(jp, ']')) {
            return 1;
        }
        if (!json_consume(jp, ',')) {
            return 0;
        }
    }
}

static int parse_checkpoint_object(json_parser *jp, ingest_request *req) {
    char *adapter = NULL;
    char *key = NULL;
    uint8_t *value = NULL;
    size_t value_len = 0u;
    int has_adapter = 0;
    int has_key = 0;
    int has_value = 0;

    if (!json_consume(jp, '{')) {
        return 0;
    }
    json_skip_ws(jp);
    if (json_consume(jp, '}')) {
        return 0;
    }
    for (;;) {
        char *field = json_parse_string(jp);

        if (field == NULL || !json_consume(jp, ':')) {
            free(field);
            goto fail;
        }
        if (strcmp(field, "adapter") == 0) {
            if (has_adapter) {
                free(field);
                goto fail;
            }
            adapter = json_parse_string(jp);
            if (adapter == NULL) {
                free(field);
                goto fail;
            }
            has_adapter = 1;
        } else if (strcmp(field, "key") == 0) {
            if (has_key) {
                free(field);
                goto fail;
            }
            key = json_parse_string(jp);
            if (key == NULL) {
                free(field);
                goto fail;
            }
            has_key = 1;
        } else if (strcmp(field, "value") == 0) {
            char *encoded;
            scribe_error_t err;

            if (has_value) {
                free(field);
                goto fail;
            }
            encoded = json_parse_string(jp);
            if (encoded == NULL) {
                free(field);
                goto fail;
            }
            err = base64_decode(encoded, &value, &value_len);
            free(encoded);
            if (err != SCRIBE_OK) {
                free(field);
                goto fail;
            }
            has_value = 1;
        } else if (!json_skip_value(jp)) {
            free(field);
            goto fail;
        }
        free(field);
        json_skip_ws(jp);
        if (json_consume(jp, '}')) {
            break;
        }
        if (!json_consume(jp, ',')) {
            goto fail;
        }
    }
    if (!has_adapter || !has_key || !has_value || !checkpoint_name_valid(adapter) || !checkpoint_name_valid(key)) {
        goto fail;
    }
    req->checkpoint_adapter = adapter;
    req->checkpoint_key = key;
    req->checkpoint_value = value;
    req->checkpoint_value_len = value_len;
    req->has_checkpoint = 1;
    return 1;

fail:
    free(adapter);
    free(key);
    free(value);
    return 0;
}
static scribe_error_t parse_ingest_body(const uint8_t *body, size_t body_len, ingest_request *req) {
    json_parser jp;
    int has_events = 0;
    scribe_error_t err = request_init(req);

    if (err != SCRIBE_OK) {
        return err;
    }
    jp.p = (const char *)body;
    jp.end = (const char *)body + body_len;
    if (!json_consume(&jp, '{')) {
        return scribe_set_error(SCRIBE_EMALFORMED, "malformed ingest request");
    }
    json_skip_ws(&jp);
    if (json_consume(&jp, '}')) {
        return scribe_set_error(SCRIBE_EMALFORMED, "ingest request must contain events");
    }
    for (;;) {
        char *key = json_parse_string(&jp);

        if (key == NULL || !json_consume(&jp, ':')) {
            free(key);
            return scribe_set_error(SCRIBE_EMALFORMED, "malformed ingest request");
        }
        if (strcmp(key, "events") == 0) {
            if (has_events) {
                free(key);
                return scribe_set_error(SCRIBE_EMALFORMED, "duplicate ingest events field");
            }
            if (!parse_events(&jp, req)) {
                free(key);
                return scribe_set_error(SCRIBE_EMALFORMED, "malformed ingest events");
            }
            has_events = 1;
        } else if (strcmp(key, "checkpoint") == 0) {
            if (req->has_checkpoint) {
                free(key);
                return scribe_set_error(SCRIBE_EMALFORMED, "duplicate ingest checkpoint field");
            }
            if (!parse_checkpoint_object(&jp, req)) {
                free(key);
                return scribe_set_error(SCRIBE_EMALFORMED, "malformed ingest checkpoint");
            }
        } else if (strcmp(key, "message") == 0) {
            char *message = json_parse_string(&jp);

            if (message == NULL) {
                free(key);
                return scribe_set_error(SCRIBE_EMALFORMED, "malformed ingest message");
            }
            replace_string(&req->message, message);
            req->message_len = strlen(req->message);
        } else if (strcmp(key, "timestamp_unix_nanos") == 0) {
            if (!json_parse_i64(&jp, &req->timestamp)) {
                free(key);
                return scribe_set_error(SCRIBE_EMALFORMED, "malformed ingest timestamp");
            }
        } else if (strcmp(key, "author") == 0 || strcmp(key, "committer") == 0 || strcmp(key, "process") == 0) {
            if (!parse_string_field_object(&jp, key, req)) {
                free(key);
                return scribe_set_error(SCRIBE_EMALFORMED, "malformed ingest identity object");
            }
        } else if (!json_skip_value(&jp)) {
            free(key);
            return scribe_set_error(SCRIBE_EMALFORMED, "malformed ingest field");
        }
        free(key);
        json_skip_ws(&jp);
        if (json_consume(&jp, '}')) {
            break;
        }
        if (!json_consume(&jp, ',')) {
            return scribe_set_error(SCRIBE_EMALFORMED, "malformed ingest request");
        }
    }
    json_skip_ws(&jp);
    if (jp.p != jp.end || !has_events || req->builder.count == 0u) {
        return scribe_set_error(SCRIBE_EMALFORMED, "ingest request must contain at least one event");
    }
    return SCRIBE_OK;
}

scribe_error_t scribe_ingest_json_commit(scribe_ctx *ctx, const uint8_t *body, size_t body_len,
                                         uint8_t out_commit_hash[SCRIBE_HASH_SIZE]) {
    ingest_request req;
    scribe_change_batch batch;
    scribe_error_t err;

    if (ctx == NULL || body == NULL || body_len == 0u || out_commit_hash == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid ingest request");
    }
    memset(&req, 0, sizeof(req));
    err = parse_ingest_body(body, body_len, &req);
    if (err == SCRIBE_OK) {
        memset(&batch, 0, sizeof(batch));
        batch.events = req.builder.events;
        batch.event_count = req.builder.count;
        batch.author = (scribe_identity){req.author_name, req.author_email, req.author_source};
        batch.committer = (scribe_identity){req.committer_name, req.committer_email, req.committer_source};
        batch.process = (scribe_process_info){req.process_name, req.process_version, req.process_params,
                                              req.process_correlation_id};
        batch.timestamp_unix_nanos = req.timestamp;
        batch.message = req.message;
        batch.message_len = req.message_len;
        err = scribe_commit_batch(ctx, &batch, out_commit_hash);
        if (err == SCRIBE_OK && req.has_checkpoint) {
            err = scribe_checkpoint_write(ctx, req.checkpoint_adapter, req.checkpoint_key, req.checkpoint_value,
                                          req.checkpoint_value_len, out_commit_hash);
        }
    }
    request_destroy(&req);
    return err;
}

scribe_error_t scribe_ingest_content_commit(scribe_ctx *ctx, const char *const *path, size_t path_len,
                                            scribe_ingest_op op, const uint8_t *payload, size_t payload_len,
                                            const char *message, uint8_t out_commit_hash[SCRIBE_HASH_SIZE]) {
    static const uint8_t empty_payload = 0u;
    const char *commit_message = message == NULL ? "http content ingest" : message;
    scribe_change_event event;
    scribe_change_batch batch;
    size_t i;

    if (ctx == NULL || path == NULL || path_len == 0u || out_commit_hash == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid content ingest arguments");
    }
    for (i = 0u; i < path_len; i++) {
        if (!path_component_valid(path[i])) {
            return scribe_set_error(SCRIBE_EPATH, "invalid content ingest path component");
        }
    }
    if (op == SCRIBE_INGEST_OP_PUT) {
        if (payload == NULL && payload_len != 0u) {
            return scribe_set_error(SCRIBE_EINVAL, "content payload is NULL");
        }
    } else if (op == SCRIBE_INGEST_OP_DELETE) {
        if (payload != NULL || payload_len != 0u) {
            return scribe_set_error(SCRIBE_EINVAL, "delete content ingest cannot include a payload");
        }
    } else {
        return scribe_set_error(SCRIBE_EINVAL, "invalid content ingest operation");
    }

    memset(&event, 0, sizeof(event));
    event.path = path;
    event.path_len = path_len;
    event.payload = op == SCRIBE_INGEST_OP_PUT ? (payload == NULL ? &empty_payload : payload) : NULL;
    event.payload_len = op == SCRIBE_INGEST_OP_PUT ? payload_len : 0u;

    memset(&batch, 0, sizeof(batch));
    batch.events = &event;
    batch.event_count = 1u;
    batch.author = (scribe_identity){"unknown", "", "http"};
    batch.committer = (scribe_identity){"scribe", "", "scribe"};
    batch.process = (scribe_process_info){"scribe-content", SCRIBE_VERSION, "", ""};
    batch.timestamp_unix_nanos = now_unix_nanos();
    batch.message = commit_message;
    batch.message_len = strlen(commit_message);
    return scribe_commit_batch(ctx, &batch, out_commit_hash);
}
