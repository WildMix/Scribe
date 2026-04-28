/*
 * Repository integrity checker.
 *
 * The fsck command verifies the reachable object graph starting from
 * refs/heads/main, then scans loose objects to report valid but unreachable
 * objects as dangling. Dangling objects are warnings in v1 because interrupted
 * writes can leave them behind before a ref update publishes a commit.
 */
#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    scribe_ctx *ctx;
    uint8_t *hashes;
    size_t count;
    size_t cap;
    size_t dangling;
} fsck_state;

/*
 * fsck keeps an in-memory set of hashes reached from refs/heads/main. The set
 * is intentionally simple because v1 fsck is an operator diagnostic command,
 * not a high-throughput query path. The visited set has two jobs:
 *
 *   1. avoid walking the same object more than once when multiple commits share
 *      subtrees or blobs;
 *   2. identify dangling loose objects during the later full object scan.
 */
/*
 * Returns whether the visited set already contains a hash. The set is linear
 * because fsck is an operator diagnostic path and typical v1 stores are small
 * enough that clarity is more important than a hash table here.
 */
static int visited_has(fsck_state *st, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    size_t i;

    for (i = 0; i < st->count; i++) {
        if (memcmp(st->hashes + i * SCRIBE_HASH_SIZE, hash, SCRIBE_HASH_SIZE) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * Adds a hash to the visited set unless it is already present. The already flag
 * lets graph walking avoid recursing through shared subtrees more than once.
 */
static scribe_error_t visited_add(fsck_state *st, const uint8_t hash[SCRIBE_HASH_SIZE], int *already) {
    uint8_t *grown;

    *already = visited_has(st, hash);
    if (*already) {
        return SCRIBE_OK;
    }
    if (st->count == st->cap) {
        size_t new_cap = st->cap == 0 ? 128u : st->cap * 2u;
        grown = (uint8_t *)realloc(st->hashes, new_cap * SCRIBE_HASH_SIZE);
        if (grown == NULL) {
            return scribe_set_error(SCRIBE_ENOMEM, "failed to grow fsck visited set");
        }
        st->hashes = grown;
        st->cap = new_cap;
    }
    memcpy(st->hashes + st->count * SCRIBE_HASH_SIZE, hash, SCRIBE_HASH_SIZE);
    st->count++;
    return SCRIBE_OK;
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

typedef struct {
    fsck_state *st;
    char dir_hex[3];
} dangling_dir_ctx;

/*
 * Visits one loose object file during the dangling-object scan. Files whose
 * names are not valid loose-object suffixes are ignored; valid object-shaped
 * names not reached earlier are reported as dangling.
 */
static scribe_error_t visit_object_file(const char *name, void *vctx) {
    dangling_dir_ctx *dctx = (dangling_dir_ctx *)vctx;
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    uint8_t hash[SCRIBE_HASH_SIZE];
    scribe_error_t err;

    if (strlen(name) != 62u) {
        return SCRIBE_OK;
    }
    snprintf(hex, sizeof(hex), "%s%s", dctx->dir_hex, name);
    err = scribe_hash_from_hex(hex, hash);
    if (err != SCRIBE_OK) {
        return SCRIBE_OK;
    }
    /*
     * Dangling means "present in objects/ but absent from the reachability set
     * built from refs/heads/main". It is only a warning in v1. Interrupted
     * writes may leave valid objects on disk before the ref update publishes a
     * commit, and Scribe has no garbage collector yet.
     */
    if (!visited_has(dctx->st, hash)) {
        printf("warning: dangling object %s\n", hex);
        dctx->st->dangling++;
    }
    return SCRIBE_OK;
}

/*
 * Visits one two-character fanout directory under objects/ and delegates each
 * file to visit_object_file(). Missing directories are tolerated so sparse
 * object stores do not fail fsck.
 */
static scribe_error_t visit_object_dir(const char *name, void *vctx) {
    fsck_state *st = (fsck_state *)vctx;
    char *objects;
    char *dir;
    dangling_dir_ctx dctx;
    scribe_error_t err;

    if (strlen(name) != 2u) {
        return SCRIBE_OK;
    }
    objects = scribe_path_join(st->ctx->repo_path, "objects");
    if (objects == NULL) {
        return SCRIBE_ENOMEM;
    }
    dir = scribe_path_join(objects, name);
    free(objects);
    if (dir == NULL) {
        return SCRIBE_ENOMEM;
    }
    dctx.st = st;
    dctx.dir_hex[0] = name[0];
    dctx.dir_hex[1] = name[1];
    dctx.dir_hex[2] = '\0';
    err = scribe_list_dir(dir, visit_object_file, &dctx);
    free(dir);
    return err == SCRIBE_ENOT_FOUND ? SCRIBE_OK : err;
}

/*
 * Implements `scribe fsck`: build the reachable set from main history, scan the
 * loose object store for unvisited hashes, print dangling warnings, and finish
 * with a summary count.
 */
scribe_error_t scribe_cli_fsck(scribe_ctx *ctx) {
    fsck_state st;
    uint8_t head[SCRIBE_HASH_SIZE];
    char *objects;
    scribe_error_t err;

    memset(&st, 0, sizeof(st));
    st.ctx = ctx;
    err = scribe_refs_read(ctx, "refs/heads/main", head);
    if (err == SCRIBE_ENOT_FOUND) {
        printf("fsck: no commits\n");
        return SCRIBE_OK;
    }
    if (err != SCRIBE_OK) {
        return scribe_set_error(SCRIBE_ECORRUPT, "invalid main ref");
    }
    err = fsck_walk_object(&st, head, SCRIBE_OBJECT_COMMIT);
    if (err != SCRIBE_OK) {
        free(st.hashes);
        return err;
    }
    objects = scribe_path_join(ctx->repo_path, "objects");
    if (objects == NULL) {
        free(st.hashes);
        return SCRIBE_ENOMEM;
    }
    err = scribe_list_dir(objects, visit_object_dir, &st);
    free(objects);
    if (err != SCRIBE_OK && err != SCRIBE_ENOT_FOUND) {
        free(st.hashes);
        return err;
    }
    printf("fsck: %zu reachable objects, %zu dangling objects\n", st.count, st.dangling);
    free(st.hashes);
    return SCRIBE_OK;
}
