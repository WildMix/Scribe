/*
 * MongoDB adapter bootstrap and steady-state watch implementation.
 *
 * This file connects MongoDB change streams to Scribe commits. It bootstraps a
 * deterministic snapshot tree, persists Mongo resume state only after Scribe
 * commits land, consumes change stream events, groups transaction events into
 * one commit, and restarts bootstrap when Mongo invalidates the stream.
 */
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
#include <stdio.h>
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

/*
 * Releases heap memory inside a watch scope. The scope currently contains only
 * the optional database name parsed from the URI.
 */
static void mongo_watch_scope_destroy(mongo_watch_scope *scope) {
    if (scope != NULL) {
        free(scope->database);
        scope->database = NULL;
    }
}

/*
 * Parses a MongoDB URI and records whether the watch should be database-scoped.
 * A URI with a path component watches that database; a URI without one attempts
 * a cluster-scoped watch.
 */
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

/*
 * Encodes raw BSON resume-token bytes into base64 for the line-oriented
 * adapter-state file. The returned string is heap-owned.
 */
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

/*
 * Converts one base64 character into its six-bit value. Invalid characters
 * return -1 so the decoder can reject malformed adapter-state tokens.
 */
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

/*
 * Decodes a base64 resume token back into raw BSON bytes. The output buffer is
 * heap-owned and later passed to bson_init_static().
 */
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

/*
 * Initializes the bounded bootstrap task queue. The queue coordinates one
 * MongoDB enumeration thread with several BSON canonicalization workers.
 */
static scribe_error_t task_queue_init(mongo_task_queue *q, size_t capacity) {
    memset(q, 0, sizeof(*q));
    q->capacity = capacity == 0 ? 64u : capacity;
    if (pthread_mutex_init(&q->mu, NULL) != 0 || pthread_cond_init(&q->not_empty, NULL) != 0 ||
        pthread_cond_init(&q->not_full, NULL) != 0) {
        return scribe_set_error(SCRIBE_ERR, "failed to initialize Mongo task queue");
    }
    return SCRIBE_OK;
}

/*
 * Bootstrap is the only v1 workload that does substantial parallel work. The
 * main thread enumerates MongoDB documents and pushes bson copies into this
 * bounded queue. Worker threads pop tasks, canonicalize BSON to deterministic
 * JSON bytes, derive the database/collection/document-id path, and append their
 * results to a mutex-protected vector. The bounded queue prevents a very large
 * collection from being copied entirely into memory before workers catch up.
 */
/*
 * Destroys synchronization primitives owned by the bootstrap task queue. Tasks
 * must already have been drained or freed by the caller.
 */
static void task_queue_destroy(mongo_task_queue *q) {
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->mu);
}

/*
 * Pushes one document task into the bounded queue, blocking while the queue is
 * full so bootstrap memory use cannot grow without limit.
 */
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

/*
 * Pops one document task for a worker. NULL means the queue is both closed and
 * empty, which tells workers to exit.
 */
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

/*
 * Marks the task queue closed and wakes all workers waiting for more work. This
 * is called after MongoDB enumeration finishes or aborts.
 */
static void task_queue_close(mongo_task_queue *q) {
    pthread_mutex_lock(&q->mu);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
}

/*
 * Frees a queued MongoDB document task, including the copied BSON document and
 * duplicated namespace strings.
 */
static void free_task(mongo_task *task) {
    if (task != NULL) {
        bson_destroy(task->doc);
        free(task->db);
        free(task->coll);
        free(task);
    }
}

/*
 * Initializes the thread-safe bootstrap result list. Workers append canonicalized
 * document results here after processing queue tasks.
 */
static scribe_error_t results_init(mongo_results *results) {
    memset(results, 0, sizeof(*results));
    if (pthread_mutex_init(&results->mu, NULL) != 0) {
        return scribe_set_error(SCRIBE_ERR, "failed to initialize Mongo results");
    }
    return SCRIBE_OK;
}

/*
 * Frees one canonicalized MongoDB result and its path/payload allocations.
 */
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

/*
 * Frees every result and destroys the result-list mutex. This is used after
 * bootstrap commit creation or after a bootstrap failure.
 */
static void results_destroy(mongo_results *results) {
    size_t i;
    for (i = 0; i < results->count; i++) {
        result_free(&results->items[i]);
    }
    free(results->items);
    pthread_mutex_destroy(&results->mu);
}

/*
 * Appends one worker result to the shared result list. The result ownership
 * moves into the list on success.
 */
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

/*
 * Allocates an empty in-memory snapshot node. Bootstrap uses these nodes to
 * assemble database/collection/document-id trees before writing Scribe objects.
 */
static mongo_snapshot_node *snapshot_node_new(void) {
    return (mongo_snapshot_node *)calloc(1, sizeof(mongo_snapshot_node));
}

/*
 * Recursively frees an in-memory snapshot tree. Child pointers are valid only
 * for tree entries; blob entries have no child to free.
 */
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

/*
 * Finds a snapshot entry by exact name and returns its index, or -1 when absent.
 */
static ssize_t snapshot_node_find(mongo_snapshot_node *node, const char *name) {
    size_t i;

    for (i = 0; i < node->count; i++) {
        if (strcmp(node->entries[i].name, name) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

/*
 * Ensures a snapshot node can accept another entry. The snapshot tree uses heap
 * arrays because it survives beyond any one arena parse operation.
 */
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

/*
 * Finds or creates a child tree node under a snapshot node. It fails if an
 * existing blob already occupies the requested tree position.
 */
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

/*
 * Sets or replaces one blob entry in the in-memory snapshot tree. Replacing a
 * previous tree frees that subtree because the leaf is now a document blob.
 */
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

/*
 * Writes one canonicalized document result into the snapshot: the payload is
 * stored as a blob, then its hash is installed at database/collection/id.
 */
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

/*
 * Adds to a size accumulator with overflow checking. Snapshot tree serialization
 * uses this before allocating fixed-size arenas.
 */
static scribe_error_t checked_add_size(size_t *total, size_t add) {
    if (*total > SIZE_MAX - add) {
        return scribe_set_error(SCRIBE_ENOMEM, "MongoDB snapshot tree arena size is too large");
    }
    *total += add;
    return SCRIBE_OK;
}

/*
 * Estimates the arena capacity needed to serialize one snapshot node as a
 * Scribe tree object.
 */
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

/*
 * Recursively writes a snapshot node as immutable Scribe tree objects. Child
 * trees are written first so parent entries can contain their hashes.
 */
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

/*
 * Hashes canonical document payload bytes for deterministic debugging and result
 * ordering checks. Object storage computes its own envelope hash later.
 */
static void hash_payload(const uint8_t *payload, size_t payload_len, uint8_t out[SCRIBE_HASH_SIZE]) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, payload, payload_len);
    blake3_hasher_finalize(&hasher, out, SCRIBE_HASH_SIZE);
}

/*
 * Converts one queued MongoDB document into a Scribe path and canonical payload.
 * Workers call this after receiving a copied BSON document from the queue.
 */
static scribe_error_t process_task(mongo_task *task, mongo_result *out) {
    char *id = NULL;
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    const char **path;
    scribe_error_t err;

    /*
     * The Scribe path for MongoDB is always exactly three components:
     * database, collection, canonical _id. The payload is the complete canonical
     * Extended JSON document. Hashing the payload here is not required for
     * object storage, but it makes worker results deterministic and was used
     * during verification/debugging of bootstrap ordering.
     */
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

/*
 * Worker thread entry point for bootstrap canonicalization. It drains the task
 * queue, records its first error, and appends successful results to the shared list.
 */
static void *worker_main(void *arg) {
    mongo_worker_ctx *ctx = (mongo_worker_ctx *)arg;

    /*
     * Workers keep draining until task_queue_close() has been called and the
     * queue is empty. A worker records the first error it sees in its context;
     * the main bootstrap thread reports that error during cleanup.
     */
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

/*
 * Returns whether a database should be skipped during cluster-scoped bootstrap.
 * The excluded list comes from `.scribe/config` as comma-separated names.
 */
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

/*
 * Runs a MongoDB hello command and verifies the deployment supports change
 * streams, meaning replica set or sharded-cluster topology.
 */
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

/*
 * Retries topology verification while a local test replica set is still
 * electing. The first retry logs a waiting message for operator visibility.
 */
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

/*
 * Opens either a database-scoped or cluster-scoped change stream based on the
 * parsed watch scope. The caller owns the returned stream.
 */
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

/*
 * Opens a short-lived change stream before bootstrap scanning and captures its
 * current resume token. After the snapshot commit lands, steady-state watching
 * resumes from this token to replay writes that raced with the scan.
 */
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

/*
 * Chooses the bootstrap worker count. An explicit config value wins; otherwise
 * use the online CPU count with a minimum of one worker.
 */
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

/*
 * Enumerates every document in one MongoDB collection and pushes copied BSON
 * tasks into the bootstrap queue.
 */
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

/*
 * Lists collections in one database and enqueues all documents from each
 * collection for bootstrap worker processing.
 */
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

/*
 * Enqueues the bootstrap scan scope. Database-scoped watches scan one database;
 * cluster-scoped watches list databases and skip configured excluded names.
 */
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

/*
 * Returns the current wall-clock time as Unix nanoseconds for synthetic commit
 * timestamps such as bootstrap commits.
 */
static int64_t unix_nanos_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * INT64_C(1000000000) + (int64_t)ts.tv_nsec;
}

/*
 * Formats the current UTC time in the ISO-8601 form used by adapter-state.
 */
static void iso8601_now(char out[32]) {
    time_t now = time(NULL);
    struct tm tm_utc;
    if (gmtime_r(&now, &tm_utc) == NULL || strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        strcpy(out, "1970-01-01T00:00:00Z");
    }
}

/*
 * Writes `.scribe/adapter-state/mongodb` after a successful bootstrap or change
 * stream commit. The file records the resume token, last commit hash, and
 * update timestamp in the strict three-line v1 format.
 */
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

    /*
     * Adapter state is the durability boundary between MongoDB and Scribe.
     * Write it only after the corresponding commit has been written and
     * refs/heads/main has advanced. If Scribe exits after the commit but before
     * this file update, restart may replay from an older token, but the commit
     * path is deterministic and ref CAS prevents silent history corruption.
     */
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

/*
 * Reads and validates the MongoDB adapter-state file. The resume token is
 * returned as a heap string and the last commit hash is decoded for callers that
 * want to inspect the stored boundary.
 */
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

    /*
     * Keep this parser deliberately strict. The state file is a tiny
     * three-line text file; accepting extra lines or missing fields would make
     * resume behavior ambiguous and harder to diagnose.
     */
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

/*
 * Rewrites adapter state with the special invalid token while preserving the
 * current HEAD commit hash. This records that a previous resume token can no
 * longer be trusted.
 */
static scribe_error_t mark_adapter_state_invalid(scribe_ctx *ctx) {
    uint8_t head[SCRIBE_HASH_SIZE];
    scribe_error_t err = scribe_refs_read(ctx, "refs/heads/main", head);

    if (err != SCRIBE_OK) {
        return err;
    }
    return write_adapter_state(ctx, SCRIBE_MONGO_STATE_INVALID, head);
}

/*
 * Converts all bootstrap worker results into one snapshot commit. It first
 * builds an in-memory tree, writes blobs/trees, then wraps the root in a commit.
 */
static scribe_error_t commit_results(scribe_ctx *ctx, mongo_results *results, uint8_t out_commit[SCRIBE_HASH_SIZE]) {
    scribe_change_batch batch;
    mongo_snapshot_node *root;
    uint8_t root_hash[SCRIBE_HASH_SIZE];
    size_t i;
    scribe_error_t err;

    /*
     * Bootstrap results are first assembled into an in-memory snapshot tree,
     * then written as immutable Scribe tree objects. The commit contains no
     * per-document events because a bootstrap is a baseline snapshot, not a
     * change stream batch.
     */
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

/*
 * Runs the full MongoDB bootstrap cycle: capture a starting token, start worker
 * threads, enumerate documents, write the snapshot commit, and persist state.
 */
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

    /*
     * Capture a resume token before scanning. The resulting bootstrap commit is
     * a snapshot at approximately that point in the stream; after the commit
     * lands, change-stream consumption resumes from the captured token so writes
     * that race with the scan are replayed through steady state.
     */
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
        scribe_log_msg(ctx, SCRIBE_LOG_INFO, "mongo", "bootstrap commit %s with %zu document(s)", commit_hex,
                       results.count);
    }
    results_destroy(&results);
    task_queue_destroy(&queue);
    free(threads);
    free(worker_ctxs);
    free(start_resume_token);
    return err;
}

static volatile sig_atomic_t g_shutdown_requested = 0;

/*
 * Async-signal-safe handler that only flips the global shutdown flag. The main
 * watch loop observes the flag and performs normal cleanup outside the handler.
 */
static void mongo_signal_handler(int signo) {
    (void)signo;
    g_shutdown_requested = 1;
}

/*
 * Installs SIGTERM/SIGINT handlers for clean adapter shutdown. On shutdown the
 * watch loop drains the current batch before releasing the repository lock.
 */
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
    char operation[8];
} mongo_watch_change;

typedef struct {
    mongo_watch_change *items;
    size_t count;
    size_t cap;
    char *resume_token;
    char *txn_key;
    int64_t timestamp_unix_nanos;
} mongo_watch_batch;

/*
 * Frees one pending change-stream change and clears it. Ownership of path and
 * payload moves into a batch before this cleanup function is used.
 */
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

/*
 * Frees all pending changes, resume token, and transaction key owned by a watch
 * batch. It is safe to call for empty batches.
 */
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

/*
 * Ensures a watch batch has space for one more event. Batches grow only while a
 * MongoDB transaction is open or while a shutdown/invalidation flush is pending.
 */
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

/*
 * Adds a change-stream data event to the current batch and records the newest
 * resume token represented by that batch.
 */
static scribe_error_t watch_batch_add(mongo_watch_batch *batch, mongo_watch_change *change, char **resume_token,
                                      int64_t timestamp_unix_nanos) {
    if (watch_batch_reserve(batch) != SCRIBE_OK) {
        return SCRIBE_ENOMEM;
    }
    /*
     * Ownership moves into the batch: change path/payload and the resume token
     * are nulled in the caller after this succeeds. The batch stores the newest
     * resume token it has seen so adapter-state can resume after the last event
     * included in the commit.
     */
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

/*
 * Emits the human-scannable commit summary requested for mongo-watch. The
 * commit has already been written and adapter-state has already been persisted
 * when this helper runs, so every printed line describes durable history. A
 * single-event commit fits on one line; transaction batches get one commit
 * header followed by the per-document operations contained in that commit.
 */
static void watch_batch_log_commit_summary(scribe_ctx *ctx, const char *commit_hex,
                                           const mongo_watch_batch *watch_batch) {
    size_t i;

    if (watch_batch->count == 1u) {
        const mongo_watch_change *change = &watch_batch->items[0];
        scribe_log_plain(ctx, "commit %.7s  %s %s/%s/%s", commit_hex, change->operation, change->path[0],
                         change->path[1], change->path[2]);
        return;
    }
    scribe_log_plain(ctx, "commit %.7s  transaction %zu events", commit_hex, watch_batch->count);
    for (i = 0; i < watch_batch->count; i++) {
        const mongo_watch_change *change = &watch_batch->items[i];
        scribe_log_plain(ctx, "  %s %s/%s/%s", change->operation, change->path[0], change->path[1], change->path[2]);
    }
}

/*
 * Commits the current watch batch to Scribe and then persists adapter state.
 * Empty batches are a no-op; nonempty batches transfer their change events into
 * a normal scribe_change_batch.
 */
static scribe_error_t watch_batch_commit(scribe_ctx *ctx, mongo_watch_batch *watch_batch) {
    scribe_change_event *events;
    scribe_change_batch batch;
    uint8_t commit_hash[SCRIBE_HASH_SIZE];
    char commit_hex[SCRIBE_HEX_HASH_SIZE + 1];
    size_t i;
    scribe_error_t err;

    /*
     * The commit must land before adapter-state is updated. This ordering is
     * what makes SIGTERM safe: on restart, Scribe either resumes before an
     * uncommitted event or after a committed event, but never records a token
     * for data that failed to enter history.
     */
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
    watch_batch_log_commit_summary(ctx, commit_hex, watch_batch);
    watch_batch_clear(watch_batch);
    return SCRIBE_OK;
}

/*
 * Returns whether an adapter-state resume token should be used. Empty and
 * explicitly invalid tokens force bootstrap instead of stream resume.
 */
static int token_is_usable(const char *resume_token) {
    return resume_token != NULL && resume_token[0] != '\0' && strcmp(resume_token, SCRIBE_MONGO_STATE_INVALID) != 0;
}

/*
 * Recognizes MongoDB errors that mean a saved resume token can no longer be
 * used. These errors trigger invalidate recovery rather than a hard adapter stop.
 */
static int mongo_resume_error_is_unusable(const char *message) {
    return message != NULL &&
           (strstr(message, "cannot resume stream") != NULL || strstr(message, "resume token was not found") != NULL ||
            strstr(message, "Resume token was not found") != NULL);
}

/*
 * Encodes the BSON resume token from a stream/event into adapter-state base64.
 * Missing tokens are fatal because the adapter cannot safely advance its boundary.
 */
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

/*
 * Opens a MongoDB change stream with v1 options and optional resumeAfter token.
 * When Mongo rejects the token as unusable, resume_token_unusable is set so the
 * caller can restart bootstrap automatically.
 */
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
    /*
     * The watch is either cluster-scoped or database-scoped depending on the URI.
     * fullDocument=updateLookup makes updates commit the full post-image
     * document, while fullDocumentBeforeChange=whenAvailable asks MongoDB for
     * pre-images without requiring them for all deployments.
     */
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

/*
 * Extracts a nested BSON document field from a change-stream event using a
 * static view into the event's backing buffer.
 */
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

/*
 * Reads the operationType string from a change-stream event. The returned
 * pointer is owned by the BSON event and remains valid while the event is valid.
 */
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

/*
 * Classifies MongoDB operationType values into data events Scribe commits,
 * invalidation events that restart bootstrap, and DDL/schema events that are logged.
 * v1 records document state, not MongoDB catalog metadata, so catalog-only
 * events such as create/createIndexes do not become commits. Destructive DDL is
 * different: drop/rename/dropDatabase can invalidate the stream or change a
 * whole subtree without per-document events, so bootstrap is the conservative
 * way to converge Scribe's root tree to MongoDB's current state.
 */
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

/*
 * Extracts ns.db and ns.coll from a change-stream event and duplicates them for
 * use in a Scribe path.
 */
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

/*
 * Extracts documentKey from a change-stream event and canonicalizes its `_id`
 * value into the Mongo tree leaf name.
 */
static scribe_error_t event_document_id(const bson_t *event, char **out_id) {
    bson_t key;

    if (event_document(event, "documentKey", &key) != SCRIBE_OK) {
        return SCRIBE_EADAPTER;
    }
    return scribe_mongo_canonicalize_id(&key, out_id);
}

/*
 * Converts a MongoDB data event into a Scribe watch change. Deletes become
 * tombstones; inserts/updates/replaces/modifies store the full canonical document.
 */
static scribe_error_t build_watch_change(const bson_t *event, const char *op, mongo_watch_change *out) {
    char *db = NULL;
    char *coll = NULL;
    char *id = NULL;
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    const char **path;
    scribe_error_t err;

    /*
     * Deletes are tombstones: path with NULL payload. All other data events use
     * fullDocument and store the canonical whole document, not a JSON patch or
     * field-level delta. Core Scribe stays format-agnostic after this point.
     */
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
    (void)snprintf(out->operation, sizeof(out->operation), "%s", op);
    return SCRIBE_OK;
}

/*
 * Converts MongoDB clusterTime Timestamp(seconds, increment) into the v1 Unix
 * nanoseconds field used for commit timestamps.
 */
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

/*
 * Builds a transaction grouping key from lsid and txnNumber when an event is
 * part of a MongoDB transaction. NULL means the event is non-transactional.
 */
static char *event_transaction_key(const bson_t *event) {
    bson_t lsid;
    bson_iter_t iter;
    char *lsid_json;
    char *canonical = NULL;
    size_t canonical_len = 0;
    int64_t txn_number;
    char *key;
    int n;

    /*
     * MongoDB emits every write in a transaction as a separate change stream
     * event. Group them by canonical lsid plus txnNumber so Scribe writes one
     * commit for the whole transaction instead of one commit per document.
     */
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

/*
 * Retrieves the best available resume token for a processed event. libmongoc's
 * stream token is preferred, with the event `_id` as a fallback.
 */
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

/*
 * Adds or commits a data event according to transaction grouping rules.
 * Non-transactional events commit immediately; transaction events stay batched
 * until the transaction key changes or the stream is flushed.
 */
static scribe_error_t handle_data_event(scribe_ctx *ctx, mongo_watch_batch *batch, const bson_t *event, const char *op,
                                        char **resume_token) {
    mongo_watch_change change;
    char *txn_key = NULL;
    int64_t ts = event_cluster_time_unix_nanos(event);
    scribe_error_t err;

    /*
     * Non-transactional writes commit immediately as one-event batches. A
     * transaction stays open until a different transaction key appears, a
     * non-data event forces a flush, shutdown drains it, or the stream ends.
     */
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

/*
 * Handles invalidation and unusable-token recovery. It flushes any current
 * batch, marks adapter state invalid, writes a new bootstrap commit, and returns
 * the fresh resume token from the new adapter state.
 */
static scribe_error_t restart_bootstrap_after_invalidate(scribe_ctx *ctx, mongoc_client_t *client,
                                                         const mongo_watch_scope *scope, mongo_watch_batch *batch,
                                                         char **resume_token) {
    scribe_error_t err;

    /*
     * Invalidate/drop/rename and unusable resume-token errors mean the current
     * stream position cannot be trusted. Preserve old history, mark the old
     * token invalid for diagnostics, write a new bootstrap commit parented to
     * existing history, and then reopen the watch from the new token.
     */
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

/*
 * Main steady-state change stream loop. It opens/reopens streams, consumes
 * events, drains batches on shutdown, and restarts bootstrap on invalidation.
 */
static scribe_error_t run_change_stream(scribe_ctx *ctx, mongoc_client_t *client, const mongo_watch_scope *scope,
                                        char **resume_token) {
    scribe_error_t err;

    /*
     * Outer loop owns stream creation/recreation. Inner loop consumes events
     * from one stream. Shutdown does not interrupt an in-flight transaction or
     * batch: the loop breaks only after watch_batch_commit() has drained the
     * current data into Scribe history and adapter-state.
     */
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

/*
 * Public Mongo adapter entry point used by the CLI. It initializes libmongoc,
 * opens the client, runs bootstrap when state is missing/invalid, then enters
 * steady-state change stream consumption.
 */
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
