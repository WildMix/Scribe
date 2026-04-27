#include "adapter_mongo/mongo_adapter.h"

#include "adapter_mongo/mongo_internal.h"
#include "util/error.h"
#include "util/hex.h"
#include "util/log.h"

#include "blake3.h"
#include <mongoc/mongoc.h>

#include <signal.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SCRIBE_MONGO_STATE_INVALID "invalid"

typedef enum {
    MONGO_EVENT_IGNORED = 0,
    MONGO_EVENT_DATA = 1,
    MONGO_EVENT_INVALIDATE = 2,
} mongo_event_kind;

typedef struct {
    char *database;
} mongo_watch_scope;

typedef struct mongo_task {
    bson_t *doc;
    char *db;
    char *coll;
    struct mongo_task *next;
} mongo_task;

typedef struct {
    mongo_task *head;
    mongo_task *tail;
    size_t count;
    size_t capacity;
    int closed;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} mongo_task_queue;

typedef struct {
    const char **path;
    uint8_t *payload;
    size_t payload_len;
    uint8_t payload_hash[SCRIBE_HASH_SIZE];
} mongo_result;

typedef struct {
    mongo_result *items;
    size_t count;
    size_t cap;
    pthread_mutex_t mu;
} mongo_results;

typedef struct {
    mongo_task_queue *queue;
    mongo_results *results;
    scribe_error_t err;
} mongo_worker_ctx;

static void mongo_watch_scope_destroy(mongo_watch_scope *scope) {
    if (scope != NULL) {
        free(scope->database);
        scope->database = NULL;
    }
}

static scribe_error_t mongo_watch_scope_from_uri(const char *uri, mongo_watch_scope *scope) {
    mongoc_uri_t *parsed;
    bson_error_t error;
    const char *database;

    if (scope == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid MongoDB watch scope");
    }
    scope->database = NULL;
    parsed = mongoc_uri_new_with_error(uri, &error);
    if (parsed == NULL) {
        return scribe_set_error(SCRIBE_EADAPTER, "invalid MongoDB URI: %s", error.message);
    }
    database = mongoc_uri_get_database(parsed);
    if (database != NULL && database[0] != '\0') {
        scope->database = strdup(database);
        if (scope->database == NULL) {
            mongoc_uri_destroy(parsed);
            return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate MongoDB watch database");
        }
    }
    mongoc_uri_destroy(parsed);
    return SCRIBE_OK;
}

static char *base64_encode(const uint8_t *data, size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = ((len + 2u) / 3u) * 4u;
    char *out = (char *)malloc(out_len + 1u);
    size_t i = 0;
    size_t j = 0;

    if (out == NULL) {
        (void)scribe_set_error(SCRIBE_ENOMEM, "failed to allocate base64 token");
        return NULL;
    }
    while (i < len) {
        uint32_t a = data[i++];
        uint32_t b = i < len ? data[i++] : 0u;
        uint32_t c = i < len ? data[i++] : 0u;
        uint32_t triple = (a << 16u) | (b << 8u) | c;

        out[j++] = table[(triple >> 18u) & 0x3fu];
        out[j++] = table[(triple >> 12u) & 0x3fu];
        out[j++] = (i - 1u) <= len ? table[(triple >> 6u) & 0x3fu] : '=';
        out[j++] = i <= len ? table[triple & 0x3fu] : '=';
    }
    if (len % 3u == 1u) {
        out[out_len - 2u] = '=';
        out[out_len - 1u] = '=';
    } else if (len % 3u == 2u) {
        out[out_len - 1u] = '=';
    }
    out[out_len] = '\0';
    return out;
}

static int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static scribe_error_t base64_decode(const char *s, uint8_t **out, size_t *out_len) {
    size_t len;
    size_t pad = 0;
    size_t expected;
    uint8_t *buf;
    size_t i;
    size_t j = 0;

    if (s == NULL || out == NULL || out_len == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid base64 decode argument");
    }
    len = strlen(s);
    if (len == 0 || (len % 4u) != 0) {
        return scribe_set_error(SCRIBE_EADAPTER, "malformed MongoDB resume token encoding");
    }
    if (s[len - 1u] == '=') {
        pad++;
    }
    if (len > 1u && s[len - 2u] == '=') {
        pad++;
    }
    expected = (len / 4u) * 3u - pad;
    buf = (uint8_t *)malloc(expected == 0 ? 1u : expected);
    if (buf == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate decoded resume token");
    }
    for (i = 0; i < len; i += 4u) {
        int vals[4];
        uint32_t triple;
        size_t k;
        for (k = 0; k < 4u; k++) {
            if (s[i + k] == '=') {
                vals[k] = 0;
            } else {
                vals[k] = base64_value(s[i + k]);
                if (vals[k] < 0) {
                    free(buf);
                    return scribe_set_error(SCRIBE_EADAPTER, "malformed MongoDB resume token encoding");
                }
            }
        }
        triple =
            ((uint32_t)vals[0] << 18u) | ((uint32_t)vals[1] << 12u) | ((uint32_t)vals[2] << 6u) | (uint32_t)vals[3];
        if (j < expected) {
            buf[j++] = (uint8_t)((triple >> 16u) & 0xffu);
        }
        if (j < expected) {
            buf[j++] = (uint8_t)((triple >> 8u) & 0xffu);
        }
        if (j < expected) {
            buf[j++] = (uint8_t)(triple & 0xffu);
        }
    }
    *out = buf;
    *out_len = expected;
    return SCRIBE_OK;
}

static scribe_error_t task_queue_init(mongo_task_queue *q, size_t capacity) {
    memset(q, 0, sizeof(*q));
    q->capacity = capacity == 0 ? 64u : capacity;
    if (pthread_mutex_init(&q->mu, NULL) != 0 || pthread_cond_init(&q->not_empty, NULL) != 0 ||
        pthread_cond_init(&q->not_full, NULL) != 0) {
        return scribe_set_error(SCRIBE_ERR, "failed to initialize Mongo task queue");
    }
    return SCRIBE_OK;
}

static void task_queue_destroy(mongo_task_queue *q) {
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->mu);
}

static scribe_error_t task_queue_push(mongo_task_queue *q, mongo_task *task) {
    pthread_mutex_lock(&q->mu);
    while (q->count == q->capacity) {
        pthread_cond_wait(&q->not_full, &q->mu);
    }
    task->next = NULL;
    if (q->tail == NULL) {
        q->head = task;
        q->tail = task;
    } else {
        q->tail->next = task;
        q->tail = task;
    }
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
    return SCRIBE_OK;
}

static mongo_task *task_queue_pop(mongo_task_queue *q) {
    mongo_task *task;

    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->closed) {
        pthread_cond_wait(&q->not_empty, &q->mu);
    }
    task = q->head;
    if (task != NULL) {
        q->head = task->next;
        if (q->head == NULL) {
            q->tail = NULL;
        }
        q->count--;
        pthread_cond_signal(&q->not_full);
    }
    pthread_mutex_unlock(&q->mu);
    return task;
}

static void task_queue_close(mongo_task_queue *q) {
    pthread_mutex_lock(&q->mu);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
}

static void free_task(mongo_task *task) {
    if (task != NULL) {
        bson_destroy(task->doc);
        free(task->db);
        free(task->coll);
        free(task);
    }
}

static scribe_error_t results_init(mongo_results *results) {
    memset(results, 0, sizeof(*results));
    if (pthread_mutex_init(&results->mu, NULL) != 0) {
        return scribe_set_error(SCRIBE_ERR, "failed to initialize Mongo results");
    }
    return SCRIBE_OK;
}

static void result_free(mongo_result *r) {
    if (r != NULL) {
        if (r->path != NULL) {
            free((void *)r->path[0]);
            free((void *)r->path[1]);
            free((void *)r->path[2]);
            free(r->path);
        }
        free(r->payload);
    }
}

static void results_destroy(mongo_results *results) {
    size_t i;
    for (i = 0; i < results->count; i++) {
        result_free(&results->items[i]);
    }
    free(results->items);
    pthread_mutex_destroy(&results->mu);
}

static scribe_error_t results_add(mongo_results *results, mongo_result *result) {
    mongo_result *grown;

    pthread_mutex_lock(&results->mu);
    if (results->count == results->cap) {
        size_t new_cap = results->cap == 0 ? 128u : results->cap * 2u;
        grown = (mongo_result *)realloc(results->items, sizeof(*grown) * new_cap);
        if (grown == NULL) {
            pthread_mutex_unlock(&results->mu);
            return scribe_set_error(SCRIBE_ENOMEM, "failed to grow Mongo result list");
        }
        results->items = grown;
        results->cap = new_cap;
    }
    results->items[results->count++] = *result;
    pthread_mutex_unlock(&results->mu);
    return SCRIBE_OK;
}

typedef struct mongo_snapshot_node mongo_snapshot_node;

typedef struct {
    char *name;
    uint8_t type;
    uint8_t hash[SCRIBE_HASH_SIZE];
    mongo_snapshot_node *child;
} mongo_snapshot_entry;

struct mongo_snapshot_node {
    mongo_snapshot_entry *entries;
    size_t count;
    size_t cap;
};

static mongo_snapshot_node *snapshot_node_new(void) {
    return (mongo_snapshot_node *)calloc(1, sizeof(mongo_snapshot_node));
}

static void snapshot_node_free(mongo_snapshot_node *node) {
    size_t i;

    if (node == NULL) {
        return;
    }
    for (i = 0; i < node->count; i++) {
        free(node->entries[i].name);
        snapshot_node_free(node->entries[i].child);
    }
    free(node->entries);
    free(node);
}

static ssize_t snapshot_node_find(mongo_snapshot_node *node, const char *name) {
    size_t i;

    for (i = 0; i < node->count; i++) {
        if (strcmp(node->entries[i].name, name) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static scribe_error_t snapshot_node_reserve(mongo_snapshot_node *node) {
    mongo_snapshot_entry *grown;
    size_t new_cap;

    if (node->count < node->cap) {
        return SCRIBE_OK;
    }
    new_cap = node->cap == 0 ? 8u : node->cap * 2u;
    grown = (mongo_snapshot_entry *)realloc(node->entries, sizeof(*grown) * new_cap);
    if (grown == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to grow Mongo snapshot tree");
    }
    memset(grown + node->cap, 0, sizeof(*grown) * (new_cap - node->cap));
    node->entries = grown;
    node->cap = new_cap;
    return SCRIBE_OK;
}

static scribe_error_t snapshot_node_child(mongo_snapshot_node *node, const char *name, mongo_snapshot_node **out) {
    ssize_t idx = snapshot_node_find(node, name);
    mongo_snapshot_node *child;

    if (idx >= 0) {
        if (node->entries[(size_t)idx].type != SCRIBE_OBJECT_TREE) {
            return scribe_set_error(SCRIBE_ECORRUPT, "Mongo snapshot path collides with blob");
        }
        *out = node->entries[(size_t)idx].child;
        return SCRIBE_OK;
    }
    if (snapshot_node_reserve(node) != SCRIBE_OK) {
        return SCRIBE_ENOMEM;
    }
    child = snapshot_node_new();
    if (child == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate Mongo snapshot tree node");
    }
    node->entries[node->count].name = strdup(name);
    if (node->entries[node->count].name == NULL) {
        snapshot_node_free(child);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate Mongo snapshot path component");
    }
    node->entries[node->count].type = SCRIBE_OBJECT_TREE;
    node->entries[node->count].child = child;
    node->count++;
    *out = child;
    return SCRIBE_OK;
}

static scribe_error_t snapshot_node_set_blob(mongo_snapshot_node *node, const char *name,
                                             const uint8_t hash[SCRIBE_HASH_SIZE]) {
    ssize_t idx = snapshot_node_find(node, name);

    if (idx >= 0) {
        mongo_snapshot_entry *entry = &node->entries[(size_t)idx];
        snapshot_node_free(entry->child);
        entry->child = NULL;
        entry->type = SCRIBE_OBJECT_BLOB;
        scribe_hash_copy(entry->hash, hash);
        return SCRIBE_OK;
    }
    if (snapshot_node_reserve(node) != SCRIBE_OK) {
        return SCRIBE_ENOMEM;
    }
    node->entries[node->count].name = strdup(name);
    if (node->entries[node->count].name == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate Mongo snapshot blob name");
    }
    node->entries[node->count].type = SCRIBE_OBJECT_BLOB;
    scribe_hash_copy(node->entries[node->count].hash, hash);
    node->count++;
    return SCRIBE_OK;
}

static scribe_error_t snapshot_add_result(scribe_ctx *ctx, mongo_snapshot_node *root, const mongo_result *result) {
    mongo_snapshot_node *db;
    mongo_snapshot_node *coll;
    uint8_t blob_hash[SCRIBE_HASH_SIZE];
    scribe_error_t err;

    err = scribe_object_write(ctx, SCRIBE_OBJECT_BLOB, result->payload, result->payload_len, blob_hash);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = snapshot_node_child(root, result->path[0], &db);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = snapshot_node_child(db, result->path[1], &coll);
    if (err != SCRIBE_OK) {
        return err;
    }
    return snapshot_node_set_blob(coll, result->path[2], blob_hash);
}

static scribe_error_t checked_add_size(size_t *total, size_t add) {
    if (*total > SIZE_MAX - add) {
        return scribe_set_error(SCRIBE_ENOMEM, "MongoDB snapshot tree arena size is too large");
    }
    *total += add;
    return SCRIBE_OK;
}

static scribe_error_t snapshot_tree_arena_capacity(const mongo_snapshot_node *node, size_t *out) {
    size_t i;
    size_t entry_count;
    size_t total = 4096u;

    if (node == NULL || out == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid MongoDB snapshot tree arena capacity input");
    }
    entry_count = node->count == 0 ? 1u : node->count;
    if (entry_count > SIZE_MAX / sizeof(scribe_tree_entry)) {
        return scribe_set_error(SCRIBE_ENOMEM, "MongoDB snapshot tree has too many entries");
    }
    if (checked_add_size(&total, sizeof(scribe_tree_entry) * entry_count) != SCRIBE_OK ||
        checked_add_size(&total, sizeof(scribe_tree_entry) * entry_count) != SCRIBE_OK) {
        return SCRIBE_ENOMEM;
    }
    for (i = 0; i < node->count; i++) {
        size_t name_len = strlen(node->entries[i].name);
        if (checked_add_size(&total, 1u + SCRIBE_HASH_SIZE + 10u + name_len) != SCRIBE_OK) {
            return SCRIBE_ENOMEM;
        }
    }
    *out = total;
    return SCRIBE_OK;
}

static scribe_error_t snapshot_write_tree(scribe_ctx *ctx, mongo_snapshot_node *node,
                                          uint8_t out_hash[SCRIBE_HASH_SIZE]) {
    scribe_tree_entry *entries;
    scribe_arena arena;
    uint8_t *payload;
    size_t payload_len;
    size_t i;
    scribe_error_t err;
    size_t arena_capacity = 0;

    err = snapshot_tree_arena_capacity(node, &arena_capacity);
    if (err == SCRIBE_OK) {
        err = scribe_arena_init(&arena, arena_capacity);
    }
    if (err != SCRIBE_OK) {
        return err;
    }
    entries = (scribe_tree_entry *)scribe_arena_alloc(&arena, sizeof(*entries) * (node->count == 0 ? 1u : node->count),
                                                      _Alignof(scribe_tree_entry));
    if (entries == NULL) {
        scribe_arena_destroy(&arena);
        return SCRIBE_ENOMEM;
    }
    memset(entries, 0, sizeof(*entries) * (node->count == 0 ? 1u : node->count));
    for (i = 0; i < node->count; i++) {
        entries[i].type = node->entries[i].type;
        entries[i].name = node->entries[i].name;
        entries[i].name_len = strlen(node->entries[i].name);
        if (node->entries[i].type == SCRIBE_OBJECT_TREE) {
            err = snapshot_write_tree(ctx, node->entries[i].child, entries[i].hash);
            if (err != SCRIBE_OK) {
                scribe_arena_destroy(&arena);
                return err;
            }
        } else {
            scribe_hash_copy(entries[i].hash, node->entries[i].hash);
        }
    }
    err = scribe_tree_serialize(entries, node->count, &arena, &payload, &payload_len);
    if (err == SCRIBE_OK) {
        err = scribe_object_write(ctx, SCRIBE_OBJECT_TREE, payload, payload_len, out_hash);
    }
    scribe_arena_destroy(&arena);
    return err;
}

static void hash_payload(const uint8_t *payload, size_t payload_len, uint8_t out[SCRIBE_HASH_SIZE]) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, payload, payload_len);
    blake3_hasher_finalize(&hasher, out, SCRIBE_HASH_SIZE);
}

static scribe_error_t process_task(mongo_task *task, mongo_result *out) {
    char *id = NULL;
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    const char **path;
    scribe_error_t err;

    memset(out, 0, sizeof(*out));
    err = scribe_mongo_canonicalize_id(task->doc, &id);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_mongo_canonicalize_bson(task->doc, &payload, &payload_len);
    if (err != SCRIBE_OK) {
        free(id);
        return err;
    }
    path = (const char **)calloc(3u, sizeof(char *));
    if (path == NULL) {
        free(id);
        free(payload);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate Mongo path");
    }
    path[0] = strdup(task->db);
    path[1] = strdup(task->coll);
    path[2] = id;
    if (path[0] == NULL || path[1] == NULL) {
        free((void *)path[0]);
        free((void *)path[1]);
        free(id);
        free(path);
        free(payload);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate Mongo path component");
    }
    out->path = path;
    out->payload = payload;
    out->payload_len = payload_len;
    hash_payload(payload, payload_len, out->payload_hash);
    return SCRIBE_OK;
}

static void *worker_main(void *arg) {
    mongo_worker_ctx *ctx = (mongo_worker_ctx *)arg;

    for (;;) {
        mongo_task *task = task_queue_pop(ctx->queue);
        mongo_result result;
        scribe_error_t err;
        if (task == NULL) {
            break;
        }
        err = process_task(task, &result);
        free_task(task);
        if (err != SCRIBE_OK) {
            ctx->err = err;
            continue;
        }
        err = results_add(ctx->results, &result);
        if (err != SCRIBE_OK) {
            result_free(&result);
            ctx->err = err;
        }
    }
    return NULL;
}

static int is_excluded_db(scribe_ctx *ctx, const char *db) {
    char excluded[sizeof(ctx->config.adapter_excluded_databases)];
    char *save = NULL;
    char *tok;

    snprintf(excluded, sizeof(excluded), "%s", ctx->config.adapter_excluded_databases);
    for (tok = strtok_r(excluded, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
        if (strcmp(tok, db) == 0) {
            return 1;
        }
    }
    return 0;
}

static scribe_error_t verify_topology(mongoc_client_t *client) {
    bson_t cmd;
    bson_t reply;
    bson_error_t error;
    bson_iter_t iter;
    int ok;

    bson_init(&cmd);
    BSON_APPEND_INT32(&cmd, "hello", 1);
    ok = mongoc_client_command_simple(client, "admin", &cmd, NULL, &reply, &error);
    bson_destroy(&cmd);
    if (!ok) {
        return scribe_set_error(SCRIBE_EADAPTER, "Mongo hello failed: %s", error.message);
    }
    if (!bson_iter_init_find(&iter, &reply, "setName")) {
        bson_iter_t msg;
        if (!bson_iter_init_find(&msg, &reply, "msg") || !BSON_ITER_HOLDS_UTF8(&msg) ||
            strcmp(bson_iter_utf8(&msg, NULL), "isdbgrid") != 0) {
            bson_destroy(&reply);
            return scribe_set_error(SCRIBE_EADAPTER, "MongoDB must be a replica set or sharded cluster");
        }
    }
    bson_destroy(&reply);
    return SCRIBE_OK;
}

static scribe_error_t verify_topology_with_retry(scribe_ctx *ctx, mongoc_client_t *client) {
    scribe_error_t err = SCRIBE_ERR;
    int attempt;

    for (attempt = 0; attempt < 60; attempt++) {
        err = verify_topology(client);
        if (err == SCRIBE_OK) {
            return SCRIBE_OK;
        }
        if (attempt == 0) {
            scribe_log_msg(ctx, SCRIBE_LOG_INFO, "mongo", "waiting for MongoDB replica set topology");
        }
        sleep(1);
    }
    return err;
}

static mongoc_change_stream_t *watch_from_scope(mongoc_client_t *client, const mongo_watch_scope *scope,
                                                const bson_t *pipeline, const bson_t *opts) {
    if (scope != NULL && scope->database != NULL) {
        mongoc_database_t *db = mongoc_client_get_database(client, scope->database);
        mongoc_change_stream_t *stream;
        if (db == NULL) {
            return NULL;
        }
        stream = mongoc_database_watch(db, pipeline, opts);
        mongoc_database_destroy(db);
        return stream;
    }
    return mongoc_client_watch(client, pipeline, opts);
}

static scribe_error_t capture_start_resume_token(mongoc_client_t *client, const mongo_watch_scope *scope,
                                                 char **out_token) {
    bson_t pipeline;
    bson_t opts;
    mongoc_change_stream_t *stream;
    const bson_t *event_doc = NULL;
    const bson_t *token;
    bson_error_t error;
    const bson_t *error_doc = NULL;
    bool has_error;

    bson_init(&pipeline);
    bson_init(&opts);
    BSON_APPEND_INT32(&opts, "maxAwaitTimeMS", 100);
    BSON_APPEND_BOOL(&opts, "showExpandedEvents", true);
    stream = watch_from_scope(client, scope, &pipeline, &opts);
    bson_destroy(&pipeline);
    bson_destroy(&opts);
    if (stream == NULL) {
        return scribe_set_error(SCRIBE_EADAPTER, "failed to open MongoDB change stream");
    }
    (void)mongoc_change_stream_next(stream, &event_doc);
    has_error = mongoc_change_stream_error_document(stream, &error, &error_doc);
    (void)error_doc;
    if (has_error) {
        mongoc_change_stream_destroy(stream);
        if (scope != NULL && scope->database != NULL && strstr(error.message, "not authorized on admin") != NULL) {
            return scribe_set_error(SCRIBE_EADAPTER, "failed to capture MongoDB resume token for database '%s': %s",
                                    scope->database, error.message);
        }
        return scribe_set_error(
            SCRIBE_EADAPTER, "failed to capture MongoDB resume token: %s%s", error.message,
            scope == NULL || scope->database == NULL
                ? "; cluster-wide watch requires admin-level change stream privileges; use a URI with /<database> "
                  "for database-scoped watching when only one database is authorized"
                : "");
    }
    token = mongoc_change_stream_get_resume_token(stream);
    if (token == NULL || bson_empty(token)) {
        mongoc_change_stream_destroy(stream);
        return scribe_set_error(SCRIBE_EADAPTER, "MongoDB change stream did not provide a resume token");
    }
    *out_token = base64_encode(bson_get_data(token), token->len);
    mongoc_change_stream_destroy(stream);
    if (*out_token == NULL) {
        return SCRIBE_ENOMEM;
    }
    return SCRIBE_OK;
}

static long worker_count(scribe_ctx *ctx) {
    long n;

    if (ctx->config.worker_threads > 0) {
        return ctx->config.worker_threads;
    }
    n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) {
        return 1;
    }
    return n;
}

static scribe_error_t enqueue_collection(mongo_task_queue *queue, mongoc_collection_t *collection, const char *db_name,
                                         const char *coll_name) {
    bson_t query;
    bson_t opts;
    mongoc_cursor_t *cursor;
    const bson_t *doc;
    bson_error_t error;

    bson_init(&query);
    bson_init(&opts);
    BSON_APPEND_INT32(&opts, "batchSize", 1000);
    cursor = mongoc_collection_find_with_opts(collection, &query, &opts, NULL);
    bson_destroy(&query);
    bson_destroy(&opts);
    if (cursor == NULL) {
        return scribe_set_error(SCRIBE_EADAPTER, "failed to create Mongo cursor");
    }
    while (mongoc_cursor_next(cursor, &doc)) {
        mongo_task *task = (mongo_task *)calloc(1, sizeof(*task));
        if (task == NULL) {
            mongoc_cursor_destroy(cursor);
            return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate Mongo task");
        }
        task->doc = bson_copy(doc);
        task->db = strdup(db_name);
        task->coll = strdup(coll_name);
        if (task->doc == NULL || task->db == NULL || task->coll == NULL) {
            free_task(task);
            mongoc_cursor_destroy(cursor);
            return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate Mongo task contents");
        }
        if (task_queue_push(queue, task) != SCRIBE_OK) {
            free_task(task);
            mongoc_cursor_destroy(cursor);
            return SCRIBE_ERR;
        }
    }
    if (mongoc_cursor_error(cursor, &error)) {
        mongoc_cursor_destroy(cursor);
        return scribe_set_error(SCRIBE_EADAPTER, "Mongo cursor failed: %s", error.message);
    }
    mongoc_cursor_destroy(cursor);
    return SCRIBE_OK;
}

static scribe_error_t enqueue_database_documents(mongoc_client_t *client, mongo_task_queue *queue,
                                                 const char *db_name) {
    bson_error_t error;
    mongoc_database_t *db;
    char **collections;
    size_t j;

    db = mongoc_client_get_database(client, db_name);
    collections = mongoc_database_get_collection_names_with_opts(db, NULL, &error);
    if (collections == NULL) {
        mongoc_database_destroy(db);
        return scribe_set_error(SCRIBE_EADAPTER, "failed to list Mongo collections for database '%s': %s", db_name,
                                error.message);
    }
    for (j = 0; collections[j] != NULL; j++) {
        mongoc_collection_t *collection = mongoc_database_get_collection(db, collections[j]);
        scribe_error_t err = enqueue_collection(queue, collection, db_name, collections[j]);
        mongoc_collection_destroy(collection);
        if (err != SCRIBE_OK) {
            bson_strfreev(collections);
            mongoc_database_destroy(db);
            return err;
        }
    }
    bson_strfreev(collections);
    mongoc_database_destroy(db);
    return SCRIBE_OK;
}

static scribe_error_t enqueue_all_documents(scribe_ctx *ctx, mongoc_client_t *client, const mongo_watch_scope *scope,
                                            mongo_task_queue *queue) {
    bson_error_t error;
    char **dbs;
    size_t i;

    if (scope != NULL && scope->database != NULL) {
        return enqueue_database_documents(client, queue, scope->database);
    }
    dbs = mongoc_client_get_database_names_with_opts(client, NULL, &error);
    if (dbs == NULL) {
        return scribe_set_error(SCRIBE_EADAPTER, "failed to list Mongo databases: %s", error.message);
    }
    for (i = 0; dbs[i] != NULL; i++) {
        scribe_error_t err;
        if (is_excluded_db(ctx, dbs[i])) {
            continue;
        }
        err = enqueue_database_documents(client, queue, dbs[i]);
        if (err != SCRIBE_OK) {
            bson_strfreev(dbs);
            return err;
        }
    }
    bson_strfreev(dbs);
    return SCRIBE_OK;
}

static int64_t unix_nanos_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * INT64_C(1000000000) + (int64_t)ts.tv_nsec;
}

static void iso8601_now(char out[32]) {
    time_t now = time(NULL);
    struct tm tm_utc;
    if (gmtime_r(&now, &tm_utc) == NULL || strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        strcpy(out, "1970-01-01T00:00:00Z");
    }
}

static scribe_error_t write_adapter_state(scribe_ctx *ctx, const char *resume_token,
                                          const uint8_t commit_hash[SCRIBE_HASH_SIZE]) {
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    char ts[32];
    char *body;
    size_t body_len;
    char *dir;
    char *path;
    int n;
    scribe_error_t err;

    dir = scribe_path_join(ctx->repo_path, "adapter-state");
    if (dir == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = scribe_mkdir_p(dir);
    path = scribe_path_join(dir, "mongodb");
    free(dir);
    if (err != SCRIBE_OK) {
        free(path);
        return err;
    }
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    scribe_hash_to_hex(commit_hash, hex);
    iso8601_now(ts);
    n = snprintf(NULL, 0,
                 "resume_token %s\n"
                 "last_commit %s\n"
                 "last_updated %s\n",
                 resume_token == NULL ? "" : resume_token, hex, ts);
    if (n < 0) {
        free(path);
        return scribe_set_error(SCRIBE_EADAPTER, "adapter state too large");
    }
    body_len = (size_t)n;
    body = (char *)malloc(body_len + 1u);
    if (body == NULL) {
        free(path);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate adapter state");
    }
    (void)snprintf(body, body_len + 1u,
                   "resume_token %s\n"
                   "last_commit %s\n"
                   "last_updated %s\n",
                   resume_token == NULL ? "" : resume_token, hex, ts);
    err = scribe_write_file_atomic(path, (const uint8_t *)body, body_len);
    free(body);
    free(path);
    return err;
}

static scribe_error_t read_adapter_state(scribe_ctx *ctx, char **out_resume_token,
                                         uint8_t out_last_commit[SCRIBE_HASH_SIZE]) {
    char *path;
    uint8_t *bytes = NULL;
    size_t len = 0;
    char *text;
    char *line1;
    char *line2;
    char *line3;
    char *nl;
    scribe_error_t err;

    if (out_resume_token == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid adapter state output");
    }
    *out_resume_token = NULL;
    path = scribe_path_join(ctx->repo_path, "adapter-state/mongodb");
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = scribe_read_file(path, &bytes, &len);
    free(path);
    if (err != SCRIBE_OK) {
        return err;
    }
    text = (char *)bytes;
    line1 = text;
    nl = strchr(line1, '\n');
    if (nl == NULL) {
        free(bytes);
        return scribe_set_error(SCRIBE_EADAPTER, "malformed MongoDB adapter state");
    }
    *nl = '\0';
    line2 = nl + 1;
    nl = strchr(line2, '\n');
    if (nl == NULL) {
        free(bytes);
        return scribe_set_error(SCRIBE_EADAPTER, "malformed MongoDB adapter state");
    }
    *nl = '\0';
    line3 = nl + 1;
    nl = strchr(line3, '\n');
    if (nl == NULL || nl[1] != '\0') {
        free(bytes);
        return scribe_set_error(SCRIBE_EADAPTER, "malformed MongoDB adapter state");
    }
    *nl = '\0';
    if (strncmp(line1, "resume_token ", 13u) != 0 || strncmp(line2, "last_commit ", 12u) != 0 ||
        strncmp(line3, "last_updated ", 13u) != 0) {
        free(bytes);
        return scribe_set_error(SCRIBE_EADAPTER, "malformed MongoDB adapter state");
    }
    if (scribe_hash_from_hex(line2 + 12u, out_last_commit) != SCRIBE_OK) {
        free(bytes);
        return SCRIBE_EADAPTER;
    }
    *out_resume_token = strdup(line1 + 13u);
    free(bytes);
    if (*out_resume_token == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate MongoDB resume token");
    }
    return SCRIBE_OK;
}

static scribe_error_t mark_adapter_state_invalid(scribe_ctx *ctx) {
    uint8_t head[SCRIBE_HASH_SIZE];
    scribe_error_t err = scribe_refs_read(ctx, "refs/heads/main", head);

    if (err != SCRIBE_OK) {
        return err;
    }
    return write_adapter_state(ctx, SCRIBE_MONGO_STATE_INVALID, head);
}

static scribe_error_t commit_results(scribe_ctx *ctx, mongo_results *results, uint8_t out_commit[SCRIBE_HASH_SIZE]) {
    scribe_change_batch batch;
    mongo_snapshot_node *root;
    uint8_t root_hash[SCRIBE_HASH_SIZE];
    size_t i;
    scribe_error_t err;

    root = snapshot_node_new();
    if (root == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate Mongo snapshot root");
    }
    for (i = 0; i < results->count; i++) {
        err = snapshot_add_result(ctx, root, &results->items[i]);
        if (err != SCRIBE_OK) {
            snapshot_node_free(root);
            return err;
        }
    }
    err = snapshot_write_tree(ctx, root, root_hash);
    snapshot_node_free(root);
    if (err != SCRIBE_OK) {
        return err;
    }
    memset(&batch, 0, sizeof(batch));
    batch.events = NULL;
    batch.event_count = 0;
    batch.author = (scribe_identity){"unknown", "", "unknown"};
    batch.committer = (scribe_identity){"scribe-mongo", "", "scribe"};
    batch.process = (scribe_process_info){"mongo-bootstrap", SCRIBE_VERSION, "", ""};
    batch.timestamp_unix_nanos = unix_nanos_now();
    batch.message = "mongo bootstrap";
    batch.message_len = strlen(batch.message);
    return scribe_commit_root_internal(ctx, root_hash, &batch, out_commit);
}

static scribe_error_t run_bootstrap(scribe_ctx *ctx, mongoc_client_t *client, const mongo_watch_scope *scope) {
    mongo_task_queue queue;
    mongo_results results;
    pthread_t *threads = NULL;
    mongo_worker_ctx *worker_ctxs = NULL;
    long workers;
    long i;
    uint8_t commit_hash[SCRIBE_HASH_SIZE];
    char commit_hex[SCRIBE_HEX_HASH_SIZE + 1];
    char *start_resume_token = NULL;
    scribe_error_t err;

    err = capture_start_resume_token(client, scope, &start_resume_token);
    if (err != SCRIBE_OK) {
        return err;
    }
    workers = worker_count(ctx);
    if (workers < 1) {
        workers = 1;
    }
    threads = (pthread_t *)calloc((size_t)workers, sizeof(*threads));
    worker_ctxs = (mongo_worker_ctx *)calloc((size_t)workers, sizeof(*worker_ctxs));
    if (threads == NULL || worker_ctxs == NULL) {
        free(threads);
        free(worker_ctxs);
        free(start_resume_token);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate Mongo worker pool");
    }
    if ((err = task_queue_init(&queue, ctx->config.event_queue_capacity)) != SCRIBE_OK ||
        (err = results_init(&results)) != SCRIBE_OK) {
        free(threads);
        free(worker_ctxs);
        free(start_resume_token);
        return err;
    }
    for (i = 0; i < workers; i++) {
        worker_ctxs[i].queue = &queue;
        worker_ctxs[i].results = &results;
        if (pthread_create(&threads[i], NULL, worker_main, &worker_ctxs[i]) != 0) {
            err = scribe_set_error(SCRIBE_ERR, "failed to start Mongo worker");
            workers = i;
            break;
        }
    }
    if (err == SCRIBE_OK) {
        err = enqueue_all_documents(ctx, client, scope, &queue);
    }
    task_queue_close(&queue);
    for (i = 0; i < workers; i++) {
        (void)pthread_join(threads[i], NULL);
        if (err == SCRIBE_OK && worker_ctxs[i].err != SCRIBE_OK) {
            err = worker_ctxs[i].err;
        }
    }
    if (err == SCRIBE_OK) {
        err = commit_results(ctx, &results, commit_hash);
    }
    if (err == SCRIBE_OK) {
        err = write_adapter_state(ctx, start_resume_token, commit_hash);
    }
    if (err == SCRIBE_OK) {
        scribe_hash_to_hex(commit_hash, commit_hex);
        scribe_log_msg(ctx, SCRIBE_LOG_INFO, "mongo", "bootstrap commit %s", commit_hex);
    }
    results_destroy(&results);
    task_queue_destroy(&queue);
    free(threads);
    free(worker_ctxs);
    free(start_resume_token);
    return err;
}

static volatile sig_atomic_t g_shutdown_requested = 0;

static void mongo_signal_handler(int signo) {
    (void)signo;
    g_shutdown_requested = 1;
}

static scribe_error_t install_signal_handlers(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = mongo_signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) != 0 || sigaction(SIGINT, &sa, NULL) != 0) {
        return scribe_set_error(SCRIBE_ERR, "failed to install MongoDB signal handlers");
    }
    return SCRIBE_OK;
}

typedef struct {
    const char **path;
    uint8_t *payload;
    size_t payload_len;
} mongo_watch_change;

typedef struct {
    mongo_watch_change *items;
    size_t count;
    size_t cap;
    char *resume_token;
    char *txn_key;
    int64_t timestamp_unix_nanos;
} mongo_watch_batch;

static void watch_change_free(mongo_watch_change *change) {
    if (change == NULL) {
        return;
    }
    if (change->path != NULL) {
        free((void *)change->path[0]);
        free((void *)change->path[1]);
        free((void *)change->path[2]);
        free(change->path);
    }
    free(change->payload);
    memset(change, 0, sizeof(*change));
}

static void watch_batch_clear(mongo_watch_batch *batch) {
    size_t i;

    if (batch == NULL) {
        return;
    }
    for (i = 0; i < batch->count; i++) {
        watch_change_free(&batch->items[i]);
    }
    free(batch->items);
    free(batch->resume_token);
    free(batch->txn_key);
    memset(batch, 0, sizeof(*batch));
}

static scribe_error_t watch_batch_reserve(mongo_watch_batch *batch) {
    mongo_watch_change *grown;
    size_t new_cap;

    if (batch->count < batch->cap) {
        return SCRIBE_OK;
    }
    new_cap = batch->cap == 0 ? 8u : batch->cap * 2u;
    grown = (mongo_watch_change *)realloc(batch->items, sizeof(*grown) * new_cap);
    if (grown == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to grow MongoDB change batch");
    }
    memset(grown + batch->cap, 0, sizeof(*grown) * (new_cap - batch->cap));
    batch->items = grown;
    batch->cap = new_cap;
    return SCRIBE_OK;
}

static scribe_error_t watch_batch_add(mongo_watch_batch *batch, mongo_watch_change *change, char **resume_token,
                                      int64_t timestamp_unix_nanos) {
    if (watch_batch_reserve(batch) != SCRIBE_OK) {
        return SCRIBE_ENOMEM;
    }
    if (batch->count == 0) {
        batch->timestamp_unix_nanos = timestamp_unix_nanos;
    }
    free(batch->resume_token);
    batch->resume_token = *resume_token;
    *resume_token = NULL;
    batch->items[batch->count++] = *change;
    memset(change, 0, sizeof(*change));
    return SCRIBE_OK;
}

static scribe_error_t watch_batch_commit(scribe_ctx *ctx, mongo_watch_batch *watch_batch) {
    scribe_change_event *events;
    scribe_change_batch batch;
    uint8_t commit_hash[SCRIBE_HASH_SIZE];
    char commit_hex[SCRIBE_HEX_HASH_SIZE + 1];
    size_t i;
    scribe_error_t err;

    if (watch_batch->count == 0) {
        return SCRIBE_OK;
    }
    events = (scribe_change_event *)calloc(watch_batch->count, sizeof(*events));
    if (events == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate MongoDB commit events");
    }
    for (i = 0; i < watch_batch->count; i++) {
        events[i].path = watch_batch->items[i].path;
        events[i].path_len = 3u;
        events[i].payload = watch_batch->items[i].payload;
        events[i].payload_len = watch_batch->items[i].payload_len;
    }
    memset(&batch, 0, sizeof(batch));
    batch.events = events;
    batch.event_count = watch_batch->count;
    batch.author = (scribe_identity){"unknown", "", "unknown"};
    batch.committer = (scribe_identity){"scribe-mongo", "", "scribe"};
    batch.process = (scribe_process_info){"mongo-watch", SCRIBE_VERSION, "", ""};
    batch.timestamp_unix_nanos = watch_batch->timestamp_unix_nanos;
    batch.message = watch_batch->txn_key == NULL ? "mongo change stream" : "mongo transaction";
    batch.message_len = strlen(batch.message);
    err = scribe_commit_batch(ctx, &batch, commit_hash);
    free(events);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = write_adapter_state(ctx, watch_batch->resume_token, commit_hash);
    if (err != SCRIBE_OK) {
        return err;
    }
    scribe_hash_to_hex(commit_hash, commit_hex);
    scribe_log_msg(ctx, SCRIBE_LOG_INFO, "mongo", "change stream commit %s with %zu event(s)", commit_hex,
                   watch_batch->count);
    watch_batch_clear(watch_batch);
    return SCRIBE_OK;
}

static int token_is_usable(const char *resume_token) {
    return resume_token != NULL && resume_token[0] != '\0' && strcmp(resume_token, SCRIBE_MONGO_STATE_INVALID) != 0;
}

static int mongo_resume_error_is_unusable(const char *message) {
    return message != NULL &&
           (strstr(message, "cannot resume stream") != NULL || strstr(message, "resume token was not found") != NULL ||
            strstr(message, "Resume token was not found") != NULL);
}

static scribe_error_t resume_token_to_base64(const bson_t *token, char **out) {
    if (token == NULL || bson_empty(token)) {
        return scribe_set_error(SCRIBE_EADAPTER, "MongoDB change stream event has no resume token");
    }
    *out = base64_encode(bson_get_data(token), token->len);
    if (*out == NULL) {
        return SCRIBE_ENOMEM;
    }
    return SCRIBE_OK;
}

static scribe_error_t open_change_stream(mongoc_client_t *client, const mongo_watch_scope *scope,
                                         const char *resume_token, mongoc_change_stream_t **out,
                                         int *resume_token_unusable) {
    bson_t pipeline;
    bson_t opts;
    bson_t resume_doc;
    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    bson_error_t error;
    const bson_t *error_doc = NULL;
    mongoc_change_stream_t *stream;
    scribe_error_t err = SCRIBE_OK;
    int has_resume_doc = 0;

    if (resume_token_unusable != NULL) {
        *resume_token_unusable = 0;
    }
    bson_init(&pipeline);
    bson_init(&opts);
    BSON_APPEND_UTF8(&opts, "fullDocument", "updateLookup");
    BSON_APPEND_UTF8(&opts, "fullDocumentBeforeChange", "whenAvailable");
    BSON_APPEND_INT32(&opts, "maxAwaitTimeMS", 500);
    BSON_APPEND_BOOL(&opts, "showExpandedEvents", true);
    if (token_is_usable(resume_token)) {
        err = base64_decode(resume_token, &decoded, &decoded_len);
        if (err != SCRIBE_OK) {
            bson_destroy(&pipeline);
            bson_destroy(&opts);
            return err;
        }
        if (!bson_init_static(&resume_doc, decoded, decoded_len)) {
            free(decoded);
            bson_destroy(&pipeline);
            bson_destroy(&opts);
            return scribe_set_error(SCRIBE_EADAPTER, "malformed MongoDB resume token BSON");
        }
        has_resume_doc = 1;
        BSON_APPEND_DOCUMENT(&opts, "resumeAfter", &resume_doc);
    }
    stream = watch_from_scope(client, scope, &pipeline, &opts);
    bson_destroy(&pipeline);
    bson_destroy(&opts);
    free(decoded);
    if (stream == NULL) {
        return scribe_set_error(SCRIBE_EADAPTER, "failed to open MongoDB change stream");
    }
    if (mongoc_change_stream_error_document(stream, &error, &error_doc)) {
        (void)error_doc;
        if (has_resume_doc && resume_token_unusable != NULL && mongo_resume_error_is_unusable(error.message)) {
            *resume_token_unusable = 1;
        }
        mongoc_change_stream_destroy(stream);
        return scribe_set_error(SCRIBE_EADAPTER, "failed to open MongoDB change stream: %s", error.message);
    }
    if (has_resume_doc) {
        scribe_log_msg(NULL, SCRIBE_LOG_DEBUG, "mongo", "opened MongoDB change stream from saved resume token");
    }
    *out = stream;
    return SCRIBE_OK;
}

static scribe_error_t event_document(const bson_t *event, const char *field, bson_t *out) {
    bson_iter_t iter;
    const uint8_t *data;
    uint32_t len;

    if (!bson_iter_init_find(&iter, event, field) || !BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        return scribe_set_error(SCRIBE_EADAPTER, "MongoDB change stream event missing document field '%s'", field);
    }
    bson_iter_document(&iter, &len, &data);
    if (!bson_init_static(out, data, len)) {
        return scribe_set_error(SCRIBE_EADAPTER, "MongoDB change stream event has malformed document field '%s'",
                                field);
    }
    return SCRIBE_OK;
}

static scribe_error_t event_operation(const bson_t *event, const char **out) {
    bson_iter_t iter;
    uint32_t len;

    if (!bson_iter_init_find(&iter, event, "operationType") || !BSON_ITER_HOLDS_UTF8(&iter)) {
        return scribe_set_error(SCRIBE_EADAPTER, "MongoDB change stream event missing operationType");
    }
    *out = bson_iter_utf8(&iter, &len);
    (void)len;
    return SCRIBE_OK;
}

static mongo_event_kind classify_operation(const char *op) {
    if (strcmp(op, "insert") == 0 || strcmp(op, "update") == 0 || strcmp(op, "replace") == 0 ||
        strcmp(op, "modify") == 0 || strcmp(op, "delete") == 0) {
        return MONGO_EVENT_DATA;
    }
    if (strcmp(op, "invalidate") == 0 || strcmp(op, "drop") == 0 || strcmp(op, "dropDatabase") == 0 ||
        strcmp(op, "rename") == 0) {
        return MONGO_EVENT_INVALIDATE;
    }
    return MONGO_EVENT_IGNORED;
}

static scribe_error_t event_namespace(const bson_t *event, char **out_db, char **out_coll) {
    bson_t ns;
    bson_iter_t iter;
    uint32_t len;
    const char *db;
    const char *coll;

    if (event_document(event, "ns", &ns) != SCRIBE_OK) {
        return SCRIBE_EADAPTER;
    }
    if (!bson_iter_init_find(&iter, &ns, "db") || !BSON_ITER_HOLDS_UTF8(&iter)) {
        return scribe_set_error(SCRIBE_EADAPTER, "MongoDB change stream event missing ns.db");
    }
    db = bson_iter_utf8(&iter, &len);
    (void)len;
    if (!bson_iter_init_find(&iter, &ns, "coll") || !BSON_ITER_HOLDS_UTF8(&iter)) {
        return scribe_set_error(SCRIBE_EADAPTER, "MongoDB change stream event missing ns.coll");
    }
    coll = bson_iter_utf8(&iter, &len);
    (void)len;
    *out_db = strdup(db);
    *out_coll = strdup(coll);
    if (*out_db == NULL || *out_coll == NULL) {
        free(*out_db);
        free(*out_coll);
        *out_db = NULL;
        *out_coll = NULL;
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate MongoDB namespace");
    }
    return SCRIBE_OK;
}

static scribe_error_t event_document_id(const bson_t *event, char **out_id) {
    bson_t key;

    if (event_document(event, "documentKey", &key) != SCRIBE_OK) {
        return SCRIBE_EADAPTER;
    }
    return scribe_mongo_canonicalize_id(&key, out_id);
}

static scribe_error_t build_watch_change(const bson_t *event, const char *op, mongo_watch_change *out) {
    char *db = NULL;
    char *coll = NULL;
    char *id = NULL;
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    const char **path;
    scribe_error_t err;

    memset(out, 0, sizeof(*out));
    err = event_namespace(event, &db, &coll);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = event_document_id(event, &id);
    if (err != SCRIBE_OK) {
        free(db);
        free(coll);
        return err;
    }
    if (strcmp(op, "delete") != 0) {
        bson_t full_doc;
        err = event_document(event, "fullDocument", &full_doc);
        if (err != SCRIBE_OK) {
            free(db);
            free(coll);
            free(id);
            return err;
        }
        err = scribe_mongo_canonicalize_bson(&full_doc, &payload, &payload_len);
        if (err != SCRIBE_OK) {
            free(db);
            free(coll);
            free(id);
            return err;
        }
    }
    path = (const char **)calloc(3u, sizeof(char *));
    if (path == NULL) {
        free(db);
        free(coll);
        free(id);
        free(payload);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate MongoDB change path");
    }
    path[0] = db;
    path[1] = coll;
    path[2] = id;
    out->path = path;
    out->payload = payload;
    out->payload_len = payload_len;
    return SCRIBE_OK;
}

static int64_t event_cluster_time_unix_nanos(const bson_t *event) {
    bson_iter_t iter;
    uint32_t seconds;
    uint32_t increment;

    if (!bson_iter_init_find(&iter, event, "clusterTime") || !BSON_ITER_HOLDS_TIMESTAMP(&iter)) {
        return unix_nanos_now();
    }
    bson_iter_timestamp(&iter, &seconds, &increment);
    /* MongoDB clusterTime is a Timestamp(seconds, increment). This preserves
     * intra-second ordering; if increment grows large, strict cross-second
     * monotonicity is not guaranteed and v1 accepts that. */
    return (int64_t)seconds * INT64_C(1000000000) + (int64_t)increment;
}

static char *event_transaction_key(const bson_t *event) {
    bson_t lsid;
    bson_iter_t iter;
    char *lsid_json;
    char *canonical = NULL;
    size_t canonical_len = 0;
    int64_t txn_number;
    char *key;
    int n;

    if (event_document(event, "lsid", &lsid) != SCRIBE_OK) {
        return NULL;
    }
    if (!bson_iter_init_find(&iter, event, "txnNumber") ||
        !(BSON_ITER_HOLDS_INT32(&iter) || BSON_ITER_HOLDS_INT64(&iter))) {
        return NULL;
    }
    txn_number = bson_iter_as_int64(&iter);
    lsid_json = bson_as_canonical_extended_json(&lsid, NULL);
    if (lsid_json == NULL) {
        return NULL;
    }
    if (scribe_mongo_canonicalize_json(lsid_json, &canonical, &canonical_len) != SCRIBE_OK) {
        bson_free(lsid_json);
        return NULL;
    }
    bson_free(lsid_json);
    n = snprintf(NULL, 0, "%.*s/%lld", (int)canonical_len, canonical, (long long)txn_number);
    if (n < 0) {
        free(canonical);
        return NULL;
    }
    key = (char *)malloc((size_t)n + 1u);
    if (key == NULL) {
        free(canonical);
        return NULL;
    }
    (void)snprintf(key, (size_t)n + 1u, "%.*s/%lld", (int)canonical_len, canonical, (long long)txn_number);
    free(canonical);
    return key;
}

static scribe_error_t stream_resume_token(mongoc_change_stream_t *stream, const bson_t *event, char **out) {
    const bson_t *token = mongoc_change_stream_get_resume_token(stream);
    bson_t event_id;

    if (token != NULL && !bson_empty(token)) {
        return resume_token_to_base64(token, out);
    }
    if (event_document(event, "_id", &event_id) == SCRIBE_OK) {
        return resume_token_to_base64(&event_id, out);
    }
    return scribe_set_error(SCRIBE_EADAPTER, "MongoDB change stream event has no resume token");
}

static scribe_error_t handle_data_event(scribe_ctx *ctx, mongo_watch_batch *batch, const bson_t *event, const char *op,
                                        char **resume_token) {
    mongo_watch_change change;
    char *txn_key = NULL;
    int64_t ts = event_cluster_time_unix_nanos(event);
    scribe_error_t err;

    memset(&change, 0, sizeof(change));
    err = build_watch_change(event, op, &change);
    if (err != SCRIBE_OK) {
        return err;
    }
    txn_key = event_transaction_key(event);
    if (txn_key == NULL) {
        err = watch_batch_commit(ctx, batch);
        if (err != SCRIBE_OK) {
            watch_change_free(&change);
            return err;
        }
        err = watch_batch_add(batch, &change, resume_token, ts);
        if (err == SCRIBE_OK) {
            err = watch_batch_commit(ctx, batch);
        }
        if (err != SCRIBE_OK) {
            watch_change_free(&change);
        }
        return err;
    }
    if (batch->count != 0 && (batch->txn_key == NULL || strcmp(batch->txn_key, txn_key) != 0)) {
        err = watch_batch_commit(ctx, batch);
        if (err != SCRIBE_OK) {
            free(txn_key);
            watch_change_free(&change);
            return err;
        }
    }
    if (batch->count == 0) {
        batch->txn_key = txn_key;
        txn_key = NULL;
    }
    err = watch_batch_add(batch, &change, resume_token, ts);
    free(txn_key);
    if (err != SCRIBE_OK) {
        watch_change_free(&change);
    }
    return err;
}

static scribe_error_t restart_bootstrap_after_invalidate(scribe_ctx *ctx, mongoc_client_t *client,
                                                         const mongo_watch_scope *scope, mongo_watch_batch *batch,
                                                         char **resume_token) {
    scribe_error_t err;

    err = watch_batch_commit(ctx, batch);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = mark_adapter_state_invalid(ctx);
    if (err != SCRIBE_OK) {
        return err;
    }
    scribe_log_msg(ctx, SCRIBE_LOG_WARN, "mongo", "change stream resume token is unusable; restarting bootstrap");
    err = run_bootstrap(ctx, client, scope);
    if (err != SCRIBE_OK) {
        return err;
    }
    free(*resume_token);
    *resume_token = NULL;
    {
        uint8_t last_commit[SCRIBE_HASH_SIZE];
        err = read_adapter_state(ctx, resume_token, last_commit);
    }
    return err;
}

static scribe_error_t run_change_stream(scribe_ctx *ctx, mongoc_client_t *client, const mongo_watch_scope *scope,
                                        char **resume_token) {
    scribe_error_t err;

    while (!g_shutdown_requested) {
        mongoc_change_stream_t *stream = NULL;
        mongo_watch_batch batch;
        int restart_stream = 0;
        int resume_token_unusable = 0;

        memset(&batch, 0, sizeof(batch));
        err = open_change_stream(client, scope, *resume_token, &stream, &resume_token_unusable);
        if (err != SCRIBE_OK) {
            if (resume_token_unusable) {
                err = restart_bootstrap_after_invalidate(ctx, client, scope, &batch, resume_token);
                watch_batch_clear(&batch);
                if (err != SCRIBE_OK) {
                    return err;
                }
                continue;
            }
            watch_batch_clear(&batch);
            return err;
        }
        scribe_log_msg(ctx, SCRIBE_LOG_INFO, "mongo", "watching MongoDB change stream");
        while (!restart_stream) {
            const bson_t *event = NULL;
            bson_error_t error;
            const bson_t *error_doc = NULL;
            const char *op = NULL;
            char *event_token = NULL;
            mongo_event_kind kind;

            if (g_shutdown_requested && batch.txn_key == NULL) {
                break;
            }
            if (mongoc_change_stream_next(stream, &event)) {
                err = event_operation(event, &op);
                if (err != SCRIBE_OK) {
                    break;
                }
                err = stream_resume_token(stream, event, &event_token);
                if (err != SCRIBE_OK) {
                    break;
                }
                kind = classify_operation(op);
                if (kind != MONGO_EVENT_DATA) {
                    err = watch_batch_commit(ctx, &batch);
                    if (err != SCRIBE_OK) {
                        free(event_token);
                        break;
                    }
                }
                if (kind == MONGO_EVENT_DATA) {
                    err = handle_data_event(ctx, &batch, event, op, &event_token);
                } else if (kind == MONGO_EVENT_INVALIDATE) {
                    free(event_token);
                    err = restart_bootstrap_after_invalidate(ctx, client, scope, &batch, resume_token);
                    restart_stream = 1;
                } else {
                    scribe_log_msg(ctx, SCRIBE_LOG_INFO, "mongo", "ignored MongoDB change stream event '%s'", op);
                    free(event_token);
                    err = SCRIBE_OK;
                }
                if (err != SCRIBE_OK) {
                    break;
                }
                continue;
            }
            if (mongoc_change_stream_error_document(stream, &error, &error_doc)) {
                (void)error_doc;
                if (g_shutdown_requested) {
                    err = watch_batch_commit(ctx, &batch);
                } else if (token_is_usable(*resume_token) && mongo_resume_error_is_unusable(error.message)) {
                    err = restart_bootstrap_after_invalidate(ctx, client, scope, &batch, resume_token);
                    restart_stream = 1;
                } else {
                    err = scribe_set_error(SCRIBE_EADAPTER, "MongoDB change stream failed: %s", error.message);
                }
                break;
            }
            err = watch_batch_commit(ctx, &batch);
            if (err != SCRIBE_OK) {
                break;
            }
        }
        if (g_shutdown_requested && err == SCRIBE_OK) {
            err = watch_batch_commit(ctx, &batch);
        }
        watch_batch_clear(&batch);
        mongoc_change_stream_destroy(stream);
        if (err != SCRIBE_OK) {
            return err;
        }
    }
    scribe_log_msg(ctx, SCRIBE_LOG_INFO, "mongo", "shutdown requested; MongoDB watch stopped cleanly");
    return SCRIBE_OK;
}

scribe_error_t scribe_mongo_watch_bootstrap(scribe_ctx *ctx, const char *uri) {
    mongoc_client_t *client = NULL;
    mongo_watch_scope scope;
    char *resume_token = NULL;
    uint8_t last_commit[SCRIBE_HASH_SIZE];
    scribe_error_t err;

    if (uri == NULL || uri[0] == '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "MongoDB URI is required");
    }
    g_shutdown_requested = 0;
    err = install_signal_handlers();
    if (err != SCRIBE_OK) {
        return err;
    }
    mongoc_init();
    err = mongo_watch_scope_from_uri(uri, &scope);
    if (err != SCRIBE_OK) {
        mongoc_cleanup();
        return err;
    }
    if (scope.database != NULL) {
        scribe_log_msg(ctx, SCRIBE_LOG_INFO, "mongo", "using MongoDB database-scoped watch for '%s'", scope.database);
    }
    client = mongoc_client_new(uri);
    if (client == NULL) {
        mongo_watch_scope_destroy(&scope);
        mongoc_cleanup();
        return scribe_set_error(SCRIBE_EADAPTER, "failed to create MongoDB client");
    }
    err = verify_topology_with_retry(ctx, client);
    if (err != SCRIBE_OK) {
        mongoc_client_destroy(client);
        mongo_watch_scope_destroy(&scope);
        mongoc_cleanup();
        return err;
    }
    err = read_adapter_state(ctx, &resume_token, last_commit);
    if (err == SCRIBE_ENOT_FOUND || !token_is_usable(resume_token)) {
        free(resume_token);
        resume_token = NULL;
        err = run_bootstrap(ctx, client, &scope);
        if (err == SCRIBE_OK) {
            err = read_adapter_state(ctx, &resume_token, last_commit);
        }
    }
    if (err == SCRIBE_OK) {
        err = run_change_stream(ctx, client, &scope, &resume_token);
    }
    free(resume_token);
    mongoc_client_destroy(client);
    mongo_watch_scope_destroy(&scope);
    mongoc_cleanup();
    return err;
}
