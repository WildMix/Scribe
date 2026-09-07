/*
 * Repository integrity checker.
 *
 * The fsck command verifies the reachable object graph starting from every
 * direct ref under `.scribe/refs`, then scans objects to report valid but unreachable
 * objects as dangling. Dangling objects are warnings in v1 because interrupted
 * writes can leave them behind before a ref update publishes a commit.
 */
#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    scribe_ctx *ctx;
    scribe_hash_set visited;
    scribe_hash_set storage;
    size_t dangling;
    size_t duplicates;
    size_t refs;
} fsck_state;

/*
 * fsck keeps an in-memory set of hashes reached from refs. The set
 * is intentionally simple because v1 fsck is an operator diagnostic command,
 * not a high-throughput query path. The visited set has two jobs:
 *
 *   1. avoid walking the same object more than once when multiple commits share
 *      subtrees or blobs, including history shared by branches/tags;
 *   2. identify dangling loose objects during the later full object scan.
 */
/*
 * Returns whether the visited set already contains a hash. The set is linea
 * because fsck is an operator diagnostic path and typical v1 stores are small
 * enough that clarity is more important than a hash table here.
 */
static int visited_has(fsck_state *st, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    return scribe_hash_set_has(&st->visited, hash);
}

/*
 * Adds a hash to the visited set unless it is already present. The already flag
 * lets graph walking avoid recursing through shared subtrees more than once.
 */
static scribe_error_t visited_add(fsck_state *st, const uint8_t hash[SCRIBE_HASH_SIZE], int *already) {
    return scribe_hash_set_add(&st->visited, hash, already);
}

static scribe_error_t storage_add(fsck_state *st, const uint8_t hash[SCRIBE_HASH_SIZE], int *already) {
    return scribe_hash_set_add(&st->storage, hash, already);
}

static scribe_error_t fsck_walk_object(fsck_state *st, const uint8_t hash[SCRIBE_HASH_SIZE], uint8_t expected_type);

/*
 * Walks every child entry referenced by a tree object. Tree parsing verifies the
 * tree payload itself; this function adds the graph traversal from each entry's
 * recorded type and hash.
 */
static scribe_error_t fsck_walk_tree(fsck_state *st, scribe_object *obj) {
    scribe_arena arena;
    scribe_tree_entry *entries = NULL;
    size_t count = 0;
    size_t i;
    scribe_error_t err;

    if (obj->payload_len > (SIZE_MAX - 4096u) / 8u) {
        return scribe_set_error(SCRIBE_ENOMEM, "tree object too large for fsck arena");
    }
    err = scribe_arena_init(&arena, obj->payload_len * 8u + 4096u);
    if (err != SCRIBE_OK) {
        return err;
    }
    /*
     * Tree parsing checks the tree payload itself: entry type bytes, name
     * lengths, strictly sorted names, and duplicate prevention. Each entry also
     * tells fsck what object type should be found at the child hash; a mismatch
     * is corruption because parent objects define the type contract.
     */
    err = scribe_tree_parse(obj->payload, obj->payload_len, &arena, &entries, &count);
    if (err != SCRIBE_OK) {
        scribe_arena_destroy(&arena);
        return err;
    }
    for (i = 0; i < count; i++) {
        err = fsck_walk_object(st, entries[i].hash, entries[i].type);
        if (err != SCRIBE_OK) {
            scribe_arena_destroy(&arena);
            return err;
        }
    }
    scribe_arena_destroy(&arena);
    return SCRIBE_OK;
}

/*
 * Walks the two graph edges owned by a commit: its root tree and optional
 * parent commit. Following parents is what makes reachability include history,
 * not only the newest snapshot.
 */
static scribe_error_t fsck_walk_commit(fsck_state *st, scribe_object *obj) {
    scribe_arena arena;
    scribe_commit_view view;
    scribe_error_t err;

    err = scribe_arena_init(&arena, obj->payload_len + 4096u);
    if (err != SCRIBE_OK) {
        return err;
    }
    /*
     * Commit parsing validates the text headers. The reachability graph then
     * follows both edges a commit can name: the root tree and the optional
     * parent commit. Following the whole parent chain is what lets fsck decide
     * whether an old blob/tree is still reachable through history.
     */
    err = scribe_commit_parse(obj->payload, obj->payload_len, &arena, &view);
    if (err != SCRIBE_OK) {
        scribe_arena_destroy(&arena);
        return err;
    }
    err = fsck_walk_object(st, view.root_tree, SCRIBE_OBJECT_TREE);
    if (err == SCRIBE_OK && view.has_parent) {
        err = fsck_walk_object(st, view.parent, SCRIBE_OBJECT_COMMIT);
    }
    scribe_arena_destroy(&arena);
    return err;
}

/*
 * Verifies and walks one object by hash. The expected_type comes from the parent
 * ref or tree entry, so a readable object with the wrong type is still corrupt.
 */
static scribe_error_t fsck_walk_object(fsck_state *st, const uint8_t hash[SCRIBE_HASH_SIZE], uint8_t expected_type) {
    scribe_object obj;
    int already = 0;
    scribe_error_t err = visited_add(st, hash, &already);

    if (err != SCRIBE_OK || already) {
        return err;
    }
    /*
     * scribe_object_read performs the envelope and BLAKE3 verification. fsck
     * adds graph-level checks on top of that: referenced objects must exist and
     * their actual type must match the type recorded by the parent tree or ref.
     */
    err = scribe_object_read(st->ctx, hash, &obj);
    if (err != SCRIBE_OK) {
        return err == SCRIBE_ENOT_FOUND ? scribe_set_error(SCRIBE_ECORRUPT, "missing reachable object") : err;
    }
    if (obj.type != expected_type) {
        scribe_object_free(&obj);
        return scribe_set_error(SCRIBE_ECORRUPT, "reachable object has wrong type");
    }
    if (obj.type == SCRIBE_OBJECT_TREE) {
        err = fsck_walk_tree(st, &obj);
    } else if (obj.type == SCRIBE_OBJECT_COMMIT) {
        err = fsck_walk_commit(st, &obj);
    }
    scribe_object_free(&obj);
    return err;
}

/*
 * Visits every storage entry reported by the object-store iterator. At this
 * point pack_validate_all() has already verified packed records directly, while
 * scribe_object_read verifies the normal read path for loose or packed objects.
 */
static scribe_error_t fsck_scan_object(const uint8_t hash[SCRIBE_HASH_SIZE], void *vctx) {
    fsck_state *st = (fsck_state *)vctx;
    scribe_object obj;
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    int already = 0;
    scribe_error_t err;

    err = storage_add(st, hash, &already);
    if (err != SCRIBE_OK) {
        return err;
    }
    scribe_hash_to_hex(hash, hex);
    if (already) {
        printf("warning: duplicate storage entry %s\n", hex);
        st->duplicates++;
        return SCRIBE_OK;
    }
    err = scribe_object_read(st->ctx, hash, &obj);
    if (err != SCRIBE_OK) {
        return err == SCRIBE_ENOT_FOUND ? scribe_set_error(SCRIBE_ECORRUPT, "stored object disappeared during fsck")
                                        : err;
    }
    scribe_object_free(&obj);
    /*
     * Dangling means "present in the object store but absent from the
     * reachability set built from all refs". It is only a warning:
     * interrupted writes may leave valid objects on disk before a ref update
     * publishes a commit, and pruning is deferred to v2 gc.
     */
    if (!visited_has(st, hash)) {
        printf("warning: dangling object %s\n", hex);
        st->dangling++;
    }
    return SCRIBE_OK;
}

/*
 * Seeds reachability from one direct ref. M4 branches and tags are first-class
 * roots for fsck, so a commit reachable only from a tag must not be reported as
 * dangling.
 */
static scribe_error_t fsck_visit_ref(const char *name, const uint8_t hash[SCRIBE_HASH_SIZE], void *user) {
    fsck_state *st = (fsck_state *)user;

    (void)name;
    st->refs++;
    return fsck_walk_object(st, hash, SCRIBE_OBJECT_COMMIT);
}

static void fsck_timestamp_utc(char out[32]) {
    time_t now = time(NULL);
    struct tm tm_utc;

    if (gmtime_r(&now, &tm_utc) == NULL || strftime(out, 32u, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        strcpy(out, "1970-01-01T00:00:00Z");
    }
}

static scribe_error_t fsck_write_health(scribe_ctx *ctx, const char *status) {
    char ts[32];
    char body[128];
    char *path;
    int n;
    scribe_error_t err;

    fsck_timestamp_utc(ts);
    n = snprintf(body, sizeof(body), "last_fsck_at %s\nlast_fsck_status %s\n", ts, status);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return scribe_set_error(SCRIBE_ENOMEM, "fsck health record is too large");
    }
    path = scribe_path_join(ctx->repo_path, "fsck-status");
    if (path == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate fsck health path");
    }
    err = scribe_write_file_atomic(path, (const uint8_t *)body, (size_t)n);
    free(path);
    return err;
}

static scribe_error_t fsck_return_with_health(scribe_ctx *ctx, scribe_error_t err) {
    scribe_error_t health_err;

    if (err == SCRIBE_OK) {
        return fsck_write_health(ctx, "ok");
    }
    health_err = fsck_write_health(ctx, "failed");
    (void)health_err;
    return err;
}

/*
 * Implements `scribe fsck`: build the reachable set from every direct ref, scan
 * the object store for unvisited hashes, print dangling warnings, and finish
 * with a summary count.
 */
scribe_error_t scribe_cli_fsck(scribe_ctx *ctx) {
    fsck_state st;
    size_t packed_objects = 0;
    scribe_error_t err;

    memset(&st, 0, sizeof(st));
    st.ctx = ctx;
    err = scribe_pack_validate_all(ctx, &packed_objects);
    if (err != SCRIBE_OK) {
        return fsck_return_with_health(ctx, err);
    }
    (void)packed_objects;
    err = scribe_refs_iter(ctx, fsck_visit_ref, &st);
    if (err != SCRIBE_OK) {
        scribe_hash_set_destroy(&st.visited);
        scribe_hash_set_destroy(&st.storage);
        return fsck_return_with_health(ctx, err);
    }
    if (st.refs == 0u) {
        printf("fsck: no commits\n");
    }
    err = scribe_object_iter(ctx, fsck_scan_object, &st);
    if (err != SCRIBE_OK) {
        scribe_hash_set_destroy(&st.visited);
        scribe_hash_set_destroy(&st.storage);
        return fsck_return_with_health(ctx, err);
    }
    printf("fsck: %zu reachable objects, %zu dangling objects", st.visited.count, st.dangling);
    if (st.duplicates != 0u) {
        printf(", %zu duplicate storage entries", st.duplicates);
    }
    printf("\n");
    scribe_hash_set_destroy(&st.visited);
    scribe_hash_set_destroy(&st.storage);
    return fsck_return_with_health(ctx, SCRIBE_OK);
}
