/*
 * Commit tree editing and publication.
 *
 * Despite the filename, this module owns the mutable tree builder used by
 * scribe_commit_batch(). It loads the current root tree, applies blob writes and
 * tombstones, writes new tree objects bottom-up, writes the commit object, and
 * finally advances refs/heads/main with compare-and-swap publication.
 */
#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct tree_node tree_node;

typedef struct {
    char *name;
    uint8_t type;
    uint8_t hash[SCRIBE_HASH_SIZE];
    tree_node *child;
} node_entry;

struct tree_node {
    node_entry *entries;
    size_t count;
    size_t cap;
};

/*
 * Allocates an empty mutable tree node from the work arena. All nodes created
 * during one commit are freed together when that arena is destroyed.
 */
static tree_node *node_new(scribe_arena *arena) {
    tree_node *node = (tree_node *)scribe_arena_alloc(arena, sizeof(*node), _Alignof(tree_node));
    if (node != NULL) {
        memset(node, 0, sizeof(*node));
    }
    return node;
}

/*
 * Finds an entry by exact C-string name in a mutable tree node. The return value
 * is the entry index or -1 when the name is absent.
 */
static ssize_t node_find(tree_node *node, const char *name) {
    size_t i;

    for (i = 0; i < node->count; i++) {
        if (strcmp(node->entries[i].name, name) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

/*
 * Ensures a mutable tree node has room for one more entry. Because arena memory
 * cannot be reallocated in place, growth copies the old entry array into a new
 * arena allocation.
 */
static scribe_error_t node_reserve(scribe_arena *arena, tree_node *node) {
    node_entry *grown;
    size_t new_cap;

    if (node->count < node->cap) {
        return SCRIBE_OK;
    }
    new_cap = node->cap == 0 ? 8u : node->cap * 2u;
    /*
     * tree_node is an arena-backed editable view of a persistent tree. Growing
     * an arena allocation cannot free the old entries, so this is a copy-grow:
     * allocate a larger array, copy existing entries, and leave the old array
     * to be reclaimed when the whole work arena is destroyed.
     */
    grown = (node_entry *)scribe_arena_alloc(arena, sizeof(*grown) * new_cap, _Alignof(node_entry));
    if (grown == NULL) {
        return SCRIBE_ENOMEM;
    }
    if (node->count != 0) {
        memcpy(grown, node->entries, sizeof(*grown) * node->count);
    }
    node->entries = grown;
    node->cap = new_cap;
    return SCRIBE_OK;
}

/*
 * Sets or replaces a child tree entry in a mutable node. Replacing a previous
 * blob with a tree is allowed only after higher-level path validation has
 * decided that the batch semantics require a tree at that name.
 */
static scribe_error_t node_set_tree(scribe_arena *arena, tree_node *node, const char *name, tree_node *child) {
    ssize_t idx = node_find(node, name);

    if (idx >= 0) {
        node_entry *entry = &node->entries[(size_t)idx];
        entry->type = SCRIBE_OBJECT_TREE;
        entry->child = child;
        memset(entry->hash, 0, SCRIBE_HASH_SIZE);
        return SCRIBE_OK;
    }
    if (node_reserve(arena, node) != SCRIBE_OK) {
        return SCRIBE_ENOMEM;
    }
    node->entries[node->count].name = scribe_arena_strdup(arena, name);
    if (node->entries[node->count].name == NULL) {
        return SCRIBE_ENOMEM;
    }
    node->entries[node->count].type = SCRIBE_OBJECT_TREE;
    node->entries[node->count].child = child;
    memset(node->entries[node->count].hash, 0, SCRIBE_HASH_SIZE);
    node->count++;
    return SCRIBE_OK;
}

/*
 * Sets or replaces a blob entry in a mutable node with an already-written blob
 * hash. The entry keeps only the hash because blobs themselves are immutable
 * object-store contents.
 */
static scribe_error_t node_set_blob(scribe_arena *arena, tree_node *node, const char *name,
                                    const uint8_t hash[SCRIBE_HASH_SIZE]) {
    ssize_t idx = node_find(node, name);

    if (idx >= 0) {
        node_entry *entry = &node->entries[(size_t)idx];
        entry->type = SCRIBE_OBJECT_BLOB;
        entry->child = NULL;
        scribe_hash_copy(entry->hash, hash);
        return SCRIBE_OK;
    }
    if (node_reserve(arena, node) != SCRIBE_OK) {
        return SCRIBE_ENOMEM;
    }
    node->entries[node->count].name = scribe_arena_strdup(arena, name);
    if (node->entries[node->count].name == NULL) {
        return SCRIBE_ENOMEM;
    }
    node->entries[node->count].type = SCRIBE_OBJECT_BLOB;
    scribe_hash_copy(node->entries[node->count].hash, hash);
    node->entries[node->count].child = NULL;
    node->count++;
    return SCRIBE_OK;
}

/*
 * Removes an entry by name from a mutable node. Missing entries are a no-op so
 * deleting an already-absent document path remains idempotent.
 */
static void node_delete(tree_node *node, const char *name) {
    ssize_t idx = node_find(node, name);

    if (idx < 0) {
        return;
    }
    if ((size_t)idx + 1u < node->count) {
        memmove(&node->entries[(size_t)idx], &node->entries[(size_t)idx + 1u],
                sizeof(*node->entries) * (node->count - (size_t)idx - 1u));
    }
    node->count--;
}

/*
 * Adds a size contribution while checking for size_t overflow. Arena-capacity
 * estimation uses this so large trees fail as SCRIBE_ENOMEM instead of wrapping.
 */
static scribe_error_t checked_add_size(size_t *total, size_t add) {
    if (*total > SIZE_MAX - add) {
        return scribe_set_error(SCRIBE_ENOMEM, "tree arena size is too large");
    }
    *total += add;
    return SCRIBE_OK;
}

/*
 * Estimates the temporary arena size needed to serialize one mutable tree node.
 * It accounts for the tree-entry array, sorted copy, encoded names, and a small
 * fixed overhead used by the tree serializer.
 */
static scribe_error_t tree_write_arena_capacity(const tree_node *node, size_t *out) {
    size_t i;
    size_t entry_count;
    size_t total = 4096u;

    if (node == NULL || out == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid tree arena capacity input");
    }
    entry_count = node->count == 0 ? 1u : node->count;
    if (entry_count > SIZE_MAX / sizeof(scribe_tree_entry)) {
        return scribe_set_error(SCRIBE_ENOMEM, "tree has too many entries");
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
 * Loads an immutable tree object into mutable arena-owned tree_node structures.
 * This gives the commit builder an editable view of the current root tree before
 * applying the new batch.
 */
static scribe_error_t load_tree(scribe_ctx *ctx, scribe_arena *work, const uint8_t hash[SCRIBE_HASH_SIZE],
                                tree_node **out) {
    scribe_object obj;
    scribe_arena arena;
    scribe_tree_entry *entries = NULL;
    size_t count = 0;
    size_t i;
    tree_node *node;
    scribe_error_t err;
    size_t arena_capacity = 0;

    /*
     * Existing persistent trees are immutable object payloads. To apply a new
     * batch, Scribe first expands the current root tree into mutable tree_node
     * objects allocated from the work arena. Blob entries keep only their hash;
     * subtree entries recursively get editable child nodes.
     */
    err = scribe_object_read(ctx, hash, &obj);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (obj.type != SCRIBE_OBJECT_TREE) {
        scribe_object_free(&obj);
        return scribe_set_error(SCRIBE_ECORRUPT, "expected tree object");
    }
    err = scribe_tree_parse_arena_capacity(obj.payload_len, &arena_capacity);
    if (err == SCRIBE_OK) {
        err = scribe_arena_init(&arena, arena_capacity);
    }
    if (err != SCRIBE_OK) {
        scribe_object_free(&obj);
        return err;
    }
    err = scribe_tree_parse(obj.payload, obj.payload_len, &arena, &entries, &count);
    if (err != SCRIBE_OK) {
        scribe_arena_destroy(&arena);
        scribe_object_free(&obj);
        return err;
    }
    node = node_new(work);
    if (node == NULL) {
        scribe_arena_destroy(&arena);
        scribe_object_free(&obj);
        return SCRIBE_ENOMEM;
    }
    for (i = 0; i < count; i++) {
        if (entries[i].type == SCRIBE_OBJECT_TREE) {
            tree_node *child = NULL;
            err = load_tree(ctx, work, entries[i].hash, &child);
            if (err != SCRIBE_OK) {
                scribe_arena_destroy(&arena);
                scribe_object_free(&obj);
                return err;
            }
            err = node_set_tree(work, node, entries[i].name, child);
        } else {
            err = node_set_blob(work, node, entries[i].name, entries[i].hash);
        }
        if (err != SCRIBE_OK) {
            scribe_arena_destroy(&arena);
            scribe_object_free(&obj);
            return err;
        }
    }
    scribe_arena_destroy(&arena);
    scribe_object_free(&obj);
    *out = node;
    return SCRIBE_OK;
}

/*
 * Recursively estimates additional work-arena capacity needed to load an
 * existing persistent tree. This prevents large repositories from exhausting the
 * fixed commit arena during updates.
 */
static scribe_error_t estimate_tree_work_capacity(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE],
                                                  size_t *capacity) {
    scribe_object obj;
    scribe_arena arena;
    scribe_tree_entry *entries = NULL;
    size_t count = 0;
    size_t i;
    size_t parse_capacity = 0;
    scribe_error_t err;

    /*
     * Loading large Mongo collections can require many tree entries and path
     * strings. Before allocating the work arena, walk the existing tree and add
     * a conservative parse capacity for every subtree so updates to large stores
     * do not fail with arena exhaustion.
     */
    err = scribe_object_read(ctx, hash, &obj);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (obj.type != SCRIBE_OBJECT_TREE) {
        scribe_object_free(&obj);
        return scribe_set_error(SCRIBE_ECORRUPT, "expected tree object");
    }
    err = scribe_tree_parse_arena_capacity(obj.payload_len, &parse_capacity);
    if (err == SCRIBE_OK) {
        err = checked_add_size(capacity, parse_capacity);
    }
    if (err == SCRIBE_OK) {
        err = scribe_arena_init(&arena, parse_capacity);
    }
    if (err != SCRIBE_OK) {
        scribe_object_free(&obj);
        return err;
    }
    err = scribe_tree_parse(obj.payload, obj.payload_len, &arena, &entries, &count);
    scribe_object_free(&obj);
    if (err != SCRIBE_OK) {
        scribe_arena_destroy(&arena);
        return err;
    }
    for (i = 0; i < count; i++) {
        if (entries[i].type == SCRIBE_OBJECT_TREE) {
            err = estimate_tree_work_capacity(ctx, entries[i].hash, capacity);
            if (err != SCRIBE_OK) {
                scribe_arena_destroy(&arena);
                return err;
            }
        }
    }
    scribe_arena_destroy(&arena);
    return SCRIBE_OK;
}

/*
 * Applies one change event to the mutable tree. Non-NULL payloads are written as
 * blob objects and installed at the leaf path; NULL payloads delete the leaf as
 * a tombstone.
 */
static scribe_error_t apply_change(scribe_ctx *ctx, scribe_arena *work, tree_node *root,
                                   const scribe_change_event *ev) {
    tree_node *node = root;
    size_t i;

    /*
     * Each event path is a sequence of tree components followed by one leaf.
     * Intermediate components must be trees. Missing intermediate trees are
     * created on demand. A NULL payload is a tombstone and deletes the leaf;
     * otherwise the payload is written as a blob and the leaf is set to that blob hash.
     */
    for (i = 0; i + 1u < ev->path_len; i++) {
        ssize_t idx = node_find(node, ev->path[i]);
        tree_node *child;
        if (idx >= 0 && node->entries[(size_t)idx].type != SCRIBE_OBJECT_TREE) {
            return scribe_set_error(SCRIBE_ECORRUPT, "path component collides with blob");
        }
        if (idx >= 0) {
            child = node->entries[(size_t)idx].child;
        } else {
            child = node_new(work);
            if (child == NULL) {
                return SCRIBE_ENOMEM;
            }
            if (node_set_tree(work, node, ev->path[i], child) != SCRIBE_OK) {
                return SCRIBE_ENOMEM;
            }
        }
        node = child;
    }
    if (ev->payload == NULL) {
        node_delete(node, ev->path[ev->path_len - 1u]);
        return SCRIBE_OK;
    }
    {
        uint8_t blob_hash[SCRIBE_HASH_SIZE];
        scribe_error_t err = scribe_object_write(ctx, SCRIBE_OBJECT_BLOB, ev->payload, ev->payload_len, blob_hash);
        if (err != SCRIBE_OK) {
            return err;
        }
        return node_set_blob(work, node, ev->path[ev->path_len - 1u], blob_hash);
    }
}

/*
 * Serializes a mutable tree and all of its child trees into immutable tree
 * objects. Children are written first because parent entries need child hashes.
 */
static scribe_error_t write_tree_recursive(scribe_ctx *ctx, tree_node *node, uint8_t out_hash[SCRIBE_HASH_SIZE]) {
    scribe_tree_entry *entries;
    scribe_arena arena;
    uint8_t *payload;
    size_t payload_len;
    size_t i;
    scribe_error_t err;
    size_t arena_capacity = 0;

    /*
     * After all events are applied, the mutable tree is collapsed back into
     * immutable tree objects bottom-up. Child trees are written first so their
     * hashes can be placed in the parent tree entry array. Unchanged subtrees
     * naturally reuse their existing hash because tree serialization is
     * canonical and object writes are content-addressed.
     */
    err = tree_write_arena_capacity(node, &arena_capacity);
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
            err = write_tree_recursive(ctx, node->entries[i].child, entries[i].hash);
            if (err != SCRIBE_OK) {
                scribe_arena_destroy(&arena);
                return err;
            }
        } else {
            scribe_hash_copy(entries[i].hash, node->entries[i].hash);
        }
    }
    err = scribe_tree_serialize(entries, node->count, &arena, &payload, &payload_len);
    if (err != SCRIBE_OK) {
        scribe_arena_destroy(&arena);
        return err;
    }
    err = scribe_object_write(ctx, SCRIBE_OBJECT_TREE, payload, payload_len, out_hash);
    scribe_arena_destroy(&arena);
    return err;
}

/*
 * Reads refs/heads/main and extracts the root tree hash from the current commit.
 * If the repository has no main ref yet, has_parent is false and the caller
 * should start from a new empty root tree.
 */
static scribe_error_t read_head_root_hash(scribe_ctx *ctx, uint8_t parent_hash[SCRIBE_HASH_SIZE], int *has_parent,
                                          uint8_t root_hash[SCRIBE_HASH_SIZE]) {
    scribe_error_t err;

    /*
     * The current root tree is not stored separately from commits. To append a
     * normal change batch, read refs/heads/main, parse the commit it points to,
     * and take that commit's root tree as the editable base.
     */
    err = scribe_refs_read(ctx, "refs/heads/main", parent_hash);
    if (err == SCRIBE_ENOT_FOUND) {
        *has_parent = 0;
        memset(root_hash, 0, SCRIBE_HASH_SIZE);
        return SCRIBE_OK;
    }
    if (err != SCRIBE_OK) {
        return err;
    }
    *has_parent = 1;
    {
        scribe_object commit_obj;
        scribe_arena arena;
        scribe_commit_view view;
        err = scribe_object_read(ctx, parent_hash, &commit_obj);
        if (err != SCRIBE_OK) {
            return err;
        }
        if (commit_obj.type != SCRIBE_OBJECT_COMMIT) {
            scribe_object_free(&commit_obj);
            return scribe_set_error(SCRIBE_ECORRUPT, "main ref does not point to a commit");
        }
        err = scribe_arena_init(&arena, commit_obj.payload_len + 1024u);
        if (err != SCRIBE_OK) {
            scribe_object_free(&commit_obj);
            return err;
        }
        err = scribe_commit_parse(commit_obj.payload, commit_obj.payload_len, &arena, &view);
        if (err == SCRIBE_OK) {
            scribe_hash_copy(root_hash, view.root_tree);
        }
        scribe_arena_destroy(&arena);
        scribe_object_free(&commit_obj);
        return err;
    }
}

/*
 * Builds a normal commit from a change batch. The function applies changes to
 * the current tree, writes all required objects, and publishes the new commit as
 * refs/heads/main only after every object write succeeds.
 */
scribe_error_t scribe_commit_batch_internal(scribe_ctx *ctx, const scribe_change_batch *batch,
                                            uint8_t out_commit_hash[SCRIBE_HASH_SIZE]) {
    tree_node *root = NULL;
    uint8_t parent_hash[SCRIBE_HASH_SIZE];
    uint8_t parent_root_hash[SCRIBE_HASH_SIZE];
    uint8_t root_hash[SCRIBE_HASH_SIZE];
    uint8_t *commit_payload;
    size_t commit_payload_len;
    scribe_arena arena;
    scribe_arena work;
    int has_parent = 0;
    size_t i;
    size_t work_capacity;
    scribe_error_t err;

    if (ctx == NULL || !ctx->writable) {
        return scribe_set_error(SCRIBE_EINVAL, "writable context required");
    }
    /*
     * Commit publication order is important:
     *   1. read the current main ref and root tree;
     *   2. write all new blob/tree/commit objects;
     *   3. atomically compare-and-swap refs/heads/main to the new commit.
     *
     * If the process dies before step 3, objects may be left on disk but are
     * unreachable. That is allowed in v1 and is exactly what fsck reports as
     * dangling. If step 3 fails because the ref changed, history is not overwritten.
     */
    err = read_head_root_hash(ctx, parent_hash, &has_parent, parent_root_hash);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (batch != NULL && batch->event_count > (SIZE_MAX - (1024u * 1024u)) / 4096u) {
        return scribe_set_error(SCRIBE_ENOMEM, "commit batch is too large");
    }
    work_capacity = 1024u * 1024u + (batch == NULL ? 0u : batch->event_count * 4096u);
    if (has_parent) {
        err = estimate_tree_work_capacity(ctx, parent_root_hash, &work_capacity);
        if (err != SCRIBE_OK) {
            return err;
        }
    }
    err = scribe_arena_init(&work, work_capacity);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (has_parent) {
        err = load_tree(ctx, &work, parent_root_hash, &root);
        if (err != SCRIBE_OK) {
            scribe_arena_destroy(&work);
            return err;
        }
    } else {
        root = node_new(&work);
        if (root == NULL) {
            scribe_arena_destroy(&work);
            return SCRIBE_ENOMEM;
        }
    }
    for (i = 0; i < batch->event_count; i++) {
        err = apply_change(ctx, &work, root, &batch->events[i]);
        if (err != SCRIBE_OK) {
            scribe_arena_destroy(&work);
            return err;
        }
    }
    err = write_tree_recursive(ctx, root, root_hash);
    if (err != SCRIBE_OK) {
        scribe_arena_destroy(&work);
        return err;
    }
    err = scribe_arena_init(&arena, 4096u + (batch->message_len * 2u));
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_commit_serialize(root_hash, has_parent ? parent_hash : NULL, batch, &arena, &commit_payload,
                                  &commit_payload_len);
    if (err == SCRIBE_OK) {
        err = scribe_object_write(ctx, SCRIBE_OBJECT_COMMIT, commit_payload, commit_payload_len, out_commit_hash);
    }
    scribe_arena_destroy(&arena);
    scribe_arena_destroy(&work);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_refs_cas(ctx, "refs/heads/main", has_parent ? parent_hash : NULL, out_commit_hash);
    if (err != SCRIBE_OK) {
        return err;
    }
    scribe_log_msg(ctx, SCRIBE_LOG_DEBUG, "commit", "wrote commit");
    scribe_log_flush(ctx);
    return SCRIBE_OK;
}

/*
 * Wraps an already-written root tree in a commit and publishes it. Mongo
 * bootstrap uses this path because it builds a complete snapshot tree directly
 * instead of replaying individual change events through apply_change().
 */
scribe_error_t scribe_commit_root_internal(scribe_ctx *ctx, const uint8_t root_tree[SCRIBE_HASH_SIZE],
                                           const scribe_change_batch *metadata,
                                           uint8_t out_commit_hash[SCRIBE_HASH_SIZE]) {
    uint8_t parent_hash[SCRIBE_HASH_SIZE];
    uint8_t *commit_payload;
    size_t commit_payload_len;
    scribe_arena arena;
    int has_parent = 0;
    scribe_error_t err;

    if (ctx == NULL || !ctx->writable) {
        return scribe_set_error(SCRIBE_EINVAL, "writable context required");
    }
    /*
     * Bootstrap already constructed and wrote the complete snapshot tree. This
     * helper wraps that root tree in a commit and advances the ref, using the
     * same parent/ref CAS rules as normal event batches.
     */
    err = scribe_refs_read(ctx, "refs/heads/main", parent_hash);
    if (err == SCRIBE_ENOT_FOUND) {
        has_parent = 0;
    } else if (err != SCRIBE_OK) {
        return err;
    } else {
        has_parent = 1;
    }
    err = scribe_arena_init(&arena, 4096u + (metadata == NULL ? 0u : metadata->message_len * 2u));
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_commit_serialize_allow_empty(root_tree, has_parent ? parent_hash : NULL, metadata, &arena,
                                              &commit_payload, &commit_payload_len);
    if (err == SCRIBE_OK) {
        err = scribe_object_write(ctx, SCRIBE_OBJECT_COMMIT, commit_payload, commit_payload_len, out_commit_hash);
    }
    scribe_arena_destroy(&arena);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_refs_cas(ctx, "refs/heads/main", has_parent ? parent_hash : NULL, out_commit_hash);
    if (err != SCRIBE_OK) {
        return err;
    }
    scribe_log_msg(ctx, SCRIBE_LOG_DEBUG, "commit", "wrote commit");
    scribe_log_flush(ctx);
    return SCRIBE_OK;
}
