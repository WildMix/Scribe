/*
 * Object and path inspection commands.
 *
 * This file implements ls-tree, list-objects, commit:path resolution for show,
 * and the shared path resolver used by log filtering. It deliberately goes
 * through object-store iterators and object reads so verification remains
 * centralized and future storage backends can keep the CLI behavior unchanged.
 */
#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *hashes;
    size_t count;
    size_t cap;
} hash_set;

/*
 * Maps an object type byte to the public type string used in list/tree output.
 * NULL means the type is not valid in the current context.
 */
static const char *type_name(uint8_t type) {
    if (type == SCRIBE_OBJECT_BLOB) {
        return "blob";
    }
    if (type == SCRIBE_OBJECT_TREE) {
        return "tree";
    }
    if (type == SCRIBE_OBJECT_COMMIT) {
        return "commit";
    }
    return NULL;
}

/*
 * Converts an object type byte into the bitmask used by list-objects --type
 * filtering. Unknown types map to zero and are treated as corruption later.
 */
static int type_mask(uint8_t type) {
    if (type == SCRIBE_OBJECT_BLOB) {
        return SCRIBE_LIST_TYPE_BLOB;
    }
    if (type == SCRIBE_OBJECT_TREE) {
        return SCRIBE_LIST_TYPE_TREE;
    }
    if (type == SCRIBE_OBJECT_COMMIT) {
        return SCRIBE_LIST_TYPE_COMMIT;
    }
    return 0;
}

/*
 * Returns whether an in-memory hash set already contains a hash. The set is a
 * simple linear array because reachable-listing is an inspection command.
 */
static int hash_set_has(const hash_set *set, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    size_t i;

    for (i = 0; i < set->count; i++) {
        if (memcmp(set->hashes + i * SCRIBE_HASH_SIZE, hash, SCRIBE_HASH_SIZE) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * Adds a hash to the reachable set, growing the backing array as needed. The
 * already flag lets callers avoid re-walking shared history or shared subtrees.
 */
static scribe_error_t hash_set_add(hash_set *set, const uint8_t hash[SCRIBE_HASH_SIZE], int *already) {
    uint8_t *grown;

    *already = hash_set_has(set, hash);
    if (*already) {
        return SCRIBE_OK;
    }
    if (set->count == set->cap) {
        size_t new_cap = set->cap == 0 ? 128u : set->cap * 2u;
        if (new_cap < set->cap || new_cap > SIZE_MAX / SCRIBE_HASH_SIZE) {
            return scribe_set_error(SCRIBE_ENOMEM, "reachable object set is too large");
        }
        grown = (uint8_t *)realloc(set->hashes, new_cap * SCRIBE_HASH_SIZE);
        if (grown == NULL) {
            return scribe_set_error(SCRIBE_ENOMEM, "failed to grow reachable object set");
        }
        set->hashes = grown;
        set->cap = new_cap;
    }
    memcpy(set->hashes + set->count * SCRIBE_HASH_SIZE, hash, SCRIBE_HASH_SIZE);
    set->count++;
    return SCRIBE_OK;
}

/*
 * Frees the memory owned by a hash set and clears it. This is safe to call on an
 * empty or partially initialized set.
 */
static void hash_set_destroy(hash_set *set) {
    if (set != NULL) {
        free(set->hashes);
        memset(set, 0, sizeof(*set));
    }
}

/*
 * Reads a commit object and parses it into an arena-backed view. This local
 * helper keeps inspect.c independent from diff.c's private read helper.
 */
static scribe_error_t read_commit_view(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE], scribe_arena *arena,
                                       scribe_commit_view *out) {
    scribe_object obj;
    scribe_error_t err = scribe_object_read(ctx, hash, &obj);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (obj.type != SCRIBE_OBJECT_COMMIT) {
        scribe_object_free(&obj);
        return scribe_set_error(SCRIBE_ECORRUPT, "object is not a commit");
    }
    err = scribe_commit_parse(obj.payload, obj.payload_len, arena, out);
    scribe_object_free(&obj);
    return err;
}

/*
 * Reads a tree object and parses its entries into the supplied arena. The arena
 * is initialized here because callers frequently use this as a one-shot parse.
 */
static scribe_error_t read_tree_entries(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE], scribe_arena *arena,
                                        scribe_tree_entry **entries, size_t *count) {
    scribe_object obj;
    scribe_error_t err;
    size_t arena_capacity = 0;

    err = scribe_arena_init(arena, 0);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_object_read(ctx, hash, &obj);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (obj.type != SCRIBE_OBJECT_TREE) {
        scribe_object_free(&obj);
        return scribe_set_error(SCRIBE_ECORRUPT, "object is not a tree");
    }
    err = scribe_tree_parse_arena_capacity(obj.payload_len, &arena_capacity);
    if (err == SCRIBE_OK) {
        err = scribe_arena_init(arena, arena_capacity);
    }
    if (err != SCRIBE_OK) {
        scribe_object_free(&obj);
        return err;
    }
    err = scribe_tree_parse(obj.payload, obj.payload_len, arena, entries, count);
    scribe_object_free(&obj);
    return err;
}

/*
 * Joins a tree-listing prefix and entry name into a slash-separated display
 * path. Lengths are explicit because tree entry names are byte strings with
 * stored lengths.
 */
static scribe_error_t join_tree_path(const char *prefix, size_t prefix_len, const char *name, size_t name_len,
                                     char **out, size_t *out_len) {
    char *path;
    size_t len = name_len;

    if (prefix_len != 0) {
        if (prefix_len > SIZE_MAX - 1u || prefix_len + 1u > SIZE_MAX - name_len) {
            return scribe_set_error(SCRIBE_ENOMEM, "tree path is too large");
        }
        len = prefix_len + 1u + name_len;
    }
    if (len == SIZE_MAX) {
        return scribe_set_error(SCRIBE_ENOMEM, "tree path is too large");
    }
    path = (char *)malloc(len + 1u);
    if (path == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate tree path");
    }
    if (prefix_len != 0) {
        memcpy(path, prefix, prefix_len);
        path[prefix_len] = '/';
        memcpy(path + prefix_len + 1u, name, name_len);
    } else {
        memcpy(path, name, name_len);
    }
    path[len] = '\0';
    *out = path;
    *out_len = len;
    return SCRIBE_OK;
}

/*
 * Recursively prints every entry under a tree in storage order. Tree entries are
 * printed before their descendants so users can see each level down to blobs.
 */
static scribe_error_t print_tree_entries_recursive(scribe_ctx *ctx, const uint8_t tree_hash[SCRIBE_HASH_SIZE],
                                                   const char *prefix, size_t prefix_len) {
    scribe_arena arena;
    scribe_tree_entry *entries = NULL;
    size_t count = 0;
    size_t i;
    scribe_error_t err;

    /*
     * ls-tree is recursive by design. Print the current entry first, then
     * descend into subtrees using the full path assembled so far. The path is
     * not shell-quoted because Scribe path components cannot contain tabs or
     * newlines, and Mongo-shaped paths are meant to be copied exactly.
     */
    err = read_tree_entries(ctx, tree_hash, &arena, &entries, &count);
    if (err != SCRIBE_OK) {
        scribe_arena_destroy(&arena);
        return err;
    }
    for (i = 0; i < count; i++) {
        char hex[SCRIBE_HEX_HASH_SIZE + 1];
        char *path = NULL;
        size_t path_len = 0;
        const char *name = type_name(entries[i].type);
        if (name == NULL || entries[i].type == SCRIBE_OBJECT_COMMIT) {
            scribe_arena_destroy(&arena);
            return scribe_set_error(SCRIBE_ECORRUPT, "invalid tree entry type");
        }
        err = join_tree_path(prefix, prefix_len, entries[i].name, entries[i].name_len, &path, &path_len);
        if (err != SCRIBE_OK) {
            scribe_arena_destroy(&arena);
            return err;
        }
        scribe_hash_to_hex(entries[i].hash, hex);
        printf("%s\t%s\t", name, hex);
        fwrite(path, 1, path_len, stdout);
        fputc('\n', stdout);
        if (entries[i].type == SCRIBE_OBJECT_TREE) {
            err = print_tree_entries_recursive(ctx, entries[i].hash, path, path_len);
            if (err != SCRIBE_OK) {
                free(path);
                scribe_arena_destroy(&arena);
                return err;
            }
        }
        free(path);
    }
    scribe_arena_destroy(&arena);
    return SCRIBE_OK;
}

/*
 * Starts recursive tree printing at an empty path prefix. This wrapper keeps the
 * public ls-tree/show-path code from knowing about prefix bookkeeping.
 */
static scribe_error_t print_tree_entries(scribe_ctx *ctx, const uint8_t tree_hash[SCRIBE_HASH_SIZE]) {
    return print_tree_entries_recursive(ctx, tree_hash, NULL, 0);
}

/*
 * Implements `scribe ls-tree`. Tree hashes are listed directly, commit hashes
 * are resolved to their root tree, and blob hashes fail because they have no entries.
 */
scribe_error_t scribe_cli_ls_tree(scribe_ctx *ctx, const char *hex) {
    uint8_t hash[SCRIBE_HASH_SIZE];
    uint8_t tree_hash[SCRIBE_HASH_SIZE];
    scribe_object obj;
    scribe_error_t err = scribe_hash_from_hex(hex, hash);

    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_object_read(ctx, hash, &obj);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (obj.type == SCRIBE_OBJECT_TREE) {
        scribe_hash_copy(tree_hash, hash);
    } else if (obj.type == SCRIBE_OBJECT_COMMIT) {
        scribe_arena arena;
        scribe_commit_view view;
        err = scribe_arena_init(&arena, obj.payload_len + 4096u);
        if (err == SCRIBE_OK) {
            err = scribe_commit_parse(obj.payload, obj.payload_len, &arena, &view);
        }
        if (err == SCRIBE_OK) {
            scribe_hash_copy(tree_hash, view.root_tree);
        }
        scribe_arena_destroy(&arena);
        if (err != SCRIBE_OK) {
            scribe_object_free(&obj);
            return err;
        }
    } else if (obj.type == SCRIBE_OBJECT_BLOB) {
        scribe_object_free(&obj);
        return scribe_set_error(SCRIBE_EINVAL, "ls-tree requires a tree or commit object, got blob");
    } else {
        scribe_object_free(&obj);
        return scribe_set_error(SCRIBE_ECORRUPT, "unknown object type");
    }
    scribe_object_free(&obj);
    return print_tree_entries(ctx, tree_hash);
}

static scribe_error_t walk_reachable_object(hash_set *set, scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE],
                                            uint8_t expected_type);

/*
 * Adds all child objects referenced by a reachable tree to the reachable set.
 * Each child is walked according to the type recorded in the tree entry.
 */
static scribe_error_t walk_reachable_tree(hash_set *set, scribe_ctx *ctx, scribe_object *obj) {
    scribe_arena arena;
    scribe_tree_entry *entries = NULL;
    size_t count = 0;
    size_t i;
    size_t arena_capacity = 0;
    scribe_error_t err = scribe_tree_parse_arena_capacity(obj->payload_len, &arena_capacity);

    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_arena_init(&arena, arena_capacity);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_tree_parse(obj->payload, obj->payload_len, &arena, &entries, &count);
    if (err != SCRIBE_OK) {
        scribe_arena_destroy(&arena);
        return err;
    }
    for (i = 0; i < count; i++) {
        err = walk_reachable_object(set, ctx, entries[i].hash, entries[i].type);
        if (err != SCRIBE_OK) {
            scribe_arena_destroy(&arena);
            return err;
        }
    }
    scribe_arena_destroy(&arena);
    return SCRIBE_OK;
}

/*
 * Adds the root tree and parent commit referenced by a reachable commit. This
 * makes --reachable include transitive history, not only the HEAD snapshot.
 */
static scribe_error_t walk_reachable_commit(hash_set *set, scribe_ctx *ctx, scribe_object *obj) {
    scribe_arena arena;
    scribe_commit_view view;
    scribe_error_t err = scribe_arena_init(&arena, obj->payload_len + 4096u);

    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_commit_parse(obj->payload, obj->payload_len, &arena, &view);
    if (err == SCRIBE_OK) {
        err = walk_reachable_object(set, ctx, view.root_tree, SCRIBE_OBJECT_TREE);
    }
    if (err == SCRIBE_OK && view.has_parent) {
        err = walk_reachable_object(set, ctx, view.parent, SCRIBE_OBJECT_COMMIT);
    }
    scribe_arena_destroy(&arena);
    return err;
}

/*
 * Adds and verifies one object in the reachable walk. Expected type mismatches
 * are corruption because parent objects define the child type contract.
 */
static scribe_error_t walk_reachable_object(hash_set *set, scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE],
                                            uint8_t expected_type) {
    scribe_object obj;
    int already = 0;
    scribe_error_t err = hash_set_add(set, hash, &already);

    if (err != SCRIBE_OK || already) {
        return err;
    }
    err = scribe_object_read(ctx, hash, &obj);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (obj.type != expected_type) {
        scribe_object_free(&obj);
        return scribe_set_error(SCRIBE_ECORRUPT, "reachable object has wrong type");
    }
    if (obj.type == SCRIBE_OBJECT_TREE) {
        err = walk_reachable_tree(set, ctx, &obj);
    } else if (obj.type == SCRIBE_OBJECT_COMMIT) {
        err = walk_reachable_commit(set, ctx, &obj);
    }
    scribe_object_free(&obj);
    return err;
}

/*
 * Builds the full in-memory reachable set for list-objects --reachable. It
 * starts from refs/heads/main and walks parents, root trees, subtrees, and blobs.
 */
static scribe_error_t build_reachable_set(scribe_ctx *ctx, hash_set *set) {
    /*
     * list-objects --reachable uses the same graph idea as fsck: start at
     * refs/heads/main, follow every parent commit, and follow every root tree
     * down to blobs. The result is held in memory and then used as a filter over
     * the object-store iterator.
     */
    uint8_t head[SCRIBE_HASH_SIZE];
    scribe_error_t err = scribe_refs_read(ctx, "refs/heads/main", head);

    if (err == SCRIBE_ENOT_FOUND) {
        return SCRIBE_OK;
    }
    if (err != SCRIBE_OK) {
        return err;
    }
    return walk_reachable_object(set, ctx, head, SCRIBE_OBJECT_COMMIT);
}

/*
 * Validates a list-objects output template before any objects are printed. This
 * prevents a bad placeholder from producing partial output.
 */
static scribe_error_t validate_format(const char *format) {
    const char *p;

    if (format == NULL || format[0] == '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "list-objects format is empty");
    }
    /*
     * Validate the whole template before iterating objects. That way a typo
     * such as %X fails immediately instead of printing a partial object list.
     */
    for (p = format; *p != '\0'; p++) {
        if (*p != '%') {
            continue;
        }
        p++;
        if (*p == '\0') {
            return scribe_set_error(SCRIBE_EINVAL, "invalid list-objects format placeholder");
        }
        if (*p != 'H' && *p != 'T' && *p != 'S' && *p != 'C' && *p != '%') {
            return scribe_set_error(SCRIBE_EINVAL, "invalid list-objects format placeholder '%%%c'", *p);
        }
    }
    return SCRIBE_OK;
}

typedef struct {
    scribe_ctx *ctx;
    hash_set *reachable_set;
    int reachable_only;
    int type_mask;
    const char *format;
} list_objects_state;

/*
 * Prints one object according to the already-validated list-objects format
 * string. `%C` performs the extra stat needed for compressed size.
 */
static scribe_error_t print_formatted_object(list_objects_state *state, const uint8_t hash[SCRIBE_HASH_SIZE],
                                             const scribe_object *obj) {
    const char *p;
    const char *name = type_name(obj->type);
    char hex[SCRIBE_HEX_HASH_SIZE + 1];

    if (name == NULL) {
        return scribe_set_error(SCRIBE_ECORRUPT, "unknown object type");
    }
    scribe_hash_to_hex(hash, hex);
    for (p = state->format; *p != '\0'; p++) {
        if (*p != '%') {
            fputc(*p, stdout);
            continue;
        }
        p++;
        if (*p == 'H') {
            fputs(hex, stdout);
        } else if (*p == 'T') {
            fputs(name, stdout);
        } else if (*p == 'S') {
            printf("%zu", obj->payload_len);
        } else if (*p == 'C') {
            size_t compressed_size = 0;
            scribe_error_t err = scribe_object_compressed_size(state->ctx, hash, &compressed_size);
            if (err != SCRIBE_OK) {
                return err;
            }
            printf("%zu", compressed_size);
        } else if (*p == '%') {
            fputc('%', stdout);
        } else {
            return scribe_set_error(SCRIBE_EINVAL, "invalid list-objects format placeholder '%%%c'", *p);
        }
    }
    fputc('\n', stdout);
    return SCRIBE_OK;
}

/*
 * Object-store iterator callback for list-objects. It applies reachable and
 * type filters, reads the object for verification/metadata, then prints it.
 */
static scribe_error_t list_object_visit(const uint8_t hash[SCRIBE_HASH_SIZE], void *user) {
    list_objects_state *state = (list_objects_state *)user;
    scribe_object obj;
    scribe_error_t err;
    int mask;

    if (state->reachable_only && !hash_set_has(state->reachable_set, hash)) {
        return SCRIBE_OK;
    }
    err = scribe_object_read(state->ctx, hash, &obj);
    if (err != SCRIBE_OK) {
        return err;
    }
    mask = type_mask(obj.type);
    if (mask == 0) {
        scribe_object_free(&obj);
        return scribe_set_error(SCRIBE_ECORRUPT, "unknown object type");
    }
    if (state->type_mask != 0 && (state->type_mask & mask) == 0) {
        scribe_object_free(&obj);
        return SCRIBE_OK;
    }
    err = print_formatted_object(state, hash, &obj);
    scribe_object_free(&obj);
    return err;
}

/*
 * Implements `scribe list-objects`. Reachable mode first builds an in-memory
 * graph set, then all modes iterate the object store through its iterator API.
 */
scribe_error_t scribe_cli_list_objects(scribe_ctx *ctx, int type_mask_value, int reachable, const char *format) {
    hash_set reachable_set;
    list_objects_state state;
    scribe_error_t err;

    err = validate_format(format);
    if (err != SCRIBE_OK) {
        return err;
    }
    memset(&reachable_set, 0, sizeof(reachable_set));
    if (reachable) {
        err = build_reachable_set(ctx, &reachable_set);
        if (err != SCRIBE_OK) {
            hash_set_destroy(&reachable_set);
            return err;
        }
    }
    state.ctx = ctx;
    state.reachable_set = &reachable_set;
    state.reachable_only = reachable;
    state.type_mask = type_mask_value;
    state.format = format;
    err = scribe_object_iter(ctx, list_object_visit, &state);
    hash_set_destroy(&reachable_set);
    return err;
}

/*
 * Finds a child tree entry by exact byte-for-byte name match. The explicit
 * length comparison lets paths with JSON names be matched without normalization.
 */
static scribe_tree_entry *find_tree_entry(scribe_tree_entry *entries, size_t count, const char *name, size_t name_len) {
    size_t i;

    for (i = 0; i < count; i++) {
        if (entries[i].name_len == name_len && memcmp(entries[i].name, name, name_len) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

/*
 * Resolves a slash-separated path from a root tree to a final object hash and
 * type. Strict mode reports user-facing errors; non-strict mode returns "absent"
 * for log filtering comparisons.
 */
static scribe_error_t resolve_tree_path(scribe_ctx *ctx, const uint8_t root_tree[SCRIBE_HASH_SIZE], const char *path,
                                        uint8_t out_hash[SCRIBE_HASH_SIZE], uint8_t *out_type, int strict) {
    uint8_t current[SCRIBE_HASH_SIZE];
    const char *part = path;

    /*
     * Path resolution is shared by "show commit:path" and log path filtering.
     * strict=1 gives user-facing ENOT_FOUND diagnostics; strict=0 turns missing
     * paths and attempts to descend through blobs into an ABSENT result so log
     * can compare "present vs absent" across commits.
     */
    if (path == NULL || path[0] == '\0') {
        scribe_hash_copy(out_hash, root_tree);
        *out_type = SCRIBE_OBJECT_TREE;
        return SCRIBE_OK;
    }
    scribe_hash_copy(current, root_tree);
    while (1) {
        const char *slash = strchr(part, '/');
        size_t part_len = slash == NULL ? strlen(part) : (size_t)(slash - part);
        int is_last = slash == NULL;
        scribe_arena arena;
        scribe_tree_entry *entries = NULL;
        scribe_tree_entry *entry = NULL;
        size_t count = 0;
        scribe_error_t err = read_tree_entries(ctx, current, &arena, &entries, &count);
        if (err == SCRIBE_OK) {
            entry = find_tree_entry(entries, count, part, part_len);
            if (entry == NULL) {
                if (strict) {
                    err = scribe_set_error(SCRIBE_ENOT_FOUND, "path component '%.*s' not found", (int)part_len, part);
                } else {
                    *out_type = 0;
                    memset(out_hash, 0, SCRIBE_HASH_SIZE);
                }
            }
        }
        if (err == SCRIBE_OK) {
            if (entry == NULL) {
                scribe_arena_destroy(&arena);
                return SCRIBE_OK;
            }
            if (entry->type == SCRIBE_OBJECT_BLOB && !is_last) {
                if (strict) {
                    err = scribe_set_error(SCRIBE_ENOT_FOUND,
                                           "path component '%.*s' resolved to a blob; cannot descend further",
                                           (int)part_len, part);
                } else {
                    *out_type = 0;
                    memset(out_hash, 0, SCRIBE_HASH_SIZE);
                    scribe_arena_destroy(&arena);
                    return SCRIBE_OK;
                }
            } else if (is_last) {
                scribe_hash_copy(out_hash, entry->hash);
                *out_type = entry->type;
            } else {
                scribe_hash_copy(current, entry->hash);
                part = slash + 1;
            }
        }
        scribe_arena_destroy(&arena);
        if (err != SCRIBE_OK || is_last) {
            return err;
        }
    }
}

/*
 * Public non-strict path resolver used by log path filtering. It reports the
 * path state as absent, blob, or tree so history comparison can detect changes.
 */
scribe_error_t scribe_tree_resolve_path(scribe_ctx *ctx, const uint8_t root_tree[SCRIBE_HASH_SIZE], const char *path,
                                        scribe_path_resolution *out) {
    uint8_t type = 0;
    scribe_error_t err;

    if (ctx == NULL || root_tree == NULL || path == NULL || out == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid tree path resolution");
    }
    memset(out, 0, sizeof(*out));
    err = resolve_tree_path(ctx, root_tree, path, out->hash, &type, 0);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (type == SCRIBE_OBJECT_BLOB) {
        out->state = SCRIBE_PATH_BLOB;
    } else if (type == SCRIBE_OBJECT_TREE) {
        out->state = SCRIBE_PATH_TREE;
    } else {
        out->state = SCRIBE_PATH_ABSENT;
    }
    return SCRIBE_OK;
}

/*
 * Writes a blob object's raw payload bytes to stdout. No newline or envelope is
 * added, which makes `scribe show commit:path` suitable for piping to jq/diff.
 */
static scribe_error_t write_blob_payload(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    scribe_object obj;
    scribe_error_t err = scribe_object_read(ctx, hash, &obj);

    if (err != SCRIBE_OK) {
        return err;
    }
    if (obj.type != SCRIBE_OBJECT_BLOB) {
        scribe_object_free(&obj);
        return scribe_set_error(SCRIBE_ECORRUPT, "object is not a blob");
    }
    if (obj.payload_len != 0 && fwrite(obj.payload, 1, obj.payload_len, stdout) != obj.payload_len) {
        scribe_object_free(&obj);
        return scribe_set_error(SCRIBE_EIO, "failed to write blob payload");
    }
    scribe_object_free(&obj);
    return SCRIBE_OK;
}

/*
 * Implements `scribe show <commit>:<path>`. The commit side is resolved first;
 * the path side is walked from the commit root and then printed as raw blob
 * bytes or recursive tree entries.
 */
scribe_error_t scribe_cli_show_path(scribe_ctx *ctx, const char *spec) {
    const char *colon = strchr(spec, ':');
    scribe_arena arena;
    char *rev;
    uint8_t commit_hash[SCRIBE_HASH_SIZE];
    uint8_t target_hash[SCRIBE_HASH_SIZE];
    uint8_t target_type = 0;
    scribe_commit_view view;
    scribe_error_t err;

    /*
     * Split only on the first colon. The left side is a commit revision and the
     * right side is a raw slash-separated tree path. No escaping or path
     * normalization is attempted; callers should quote the whole shell argument
     * when the path contains JSON braces or quotes.
     */
    if (colon == NULL || colon == spec) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid commit:path argument");
    }
    err = scribe_arena_init(&arena, strlen(spec) + 4096u);
    if (err != SCRIBE_OK) {
        return err;
    }
    rev = scribe_arena_strdup_len(&arena, spec, (size_t)(colon - spec));
    if (rev == NULL) {
        scribe_arena_destroy(&arena);
        return SCRIBE_ENOMEM;
    }
    err = scribe_resolve_commit(ctx, rev, commit_hash);
    if (err == SCRIBE_OK) {
        err = read_commit_view(ctx, commit_hash, &arena, &view);
    }
    if (err != SCRIBE_OK) {
        scribe_arena_destroy(&arena);
        return err;
    }
    if (colon[1] == '\0') {
        scribe_hash_copy(target_hash, view.root_tree);
        target_type = SCRIBE_OBJECT_TREE;
    } else {
        err = resolve_tree_path(ctx, view.root_tree, colon + 1, target_hash, &target_type, 1);
        if (err != SCRIBE_OK) {
            scribe_arena_destroy(&arena);
            return err;
        }
    }
    scribe_arena_destroy(&arena);
    if (target_type == SCRIBE_OBJECT_TREE) {
        return print_tree_entries(ctx, target_hash);
    }
    if (target_type == SCRIBE_OBJECT_BLOB) {
        return write_blob_payload(ctx, target_hash);
    }
    return scribe_set_error(SCRIBE_ECORRUPT, "path resolved to invalid object type");
}
