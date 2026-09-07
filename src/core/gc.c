/*
 * Garbage collection and storage maintenance.
 *
 * M3 keeps Scribe's object model unchanged while adding operator-controlled
 * cleanup: mark reachable objects from refs, prune old dangling loose objects,
 * roll reachable loose objects into packs, and consolidate published packs.
 * All graph walking still goes through object-store reads so loose and packed
 * storage receive the same envelope/hash verification.
 */
#include "core/internal.h"

#include "util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SCRIBE_GC_SECONDS_PER_DAY 86400

typedef scribe_hash_set gc_hash_set;

typedef struct {
    uint8_t *hashes;
    size_t count;
    size_t cap;
} gc_hash_list;

typedef struct {
    uint8_t *ids;
    size_t count;
    size_t cap;
} gc_pack_id_list;

typedef struct {
    scribe_ctx *ctx;
    gc_hash_set reachable;
    gc_hash_set consolidated;
    gc_hash_list prune_loose;
    gc_hash_list delete_packed_loose;
    gc_pack_id_list old_packs;
    scribe_pack_writer *writer;
    time_t now;
    long grace_seconds;
    int dry_run;
    int prune_now;
    int pack;
    int consolidate;
    size_t ref_count;
    size_t loose_count;
    size_t dangling_loose;
    size_t grace_kept_loose;
    size_t prune_candidates;
    size_t pruned_loose;
    size_t reachable_loose;
    size_t already_packed_loose;
    size_t newly_packed_loose;
    size_t deleted_reachable_loose;
    size_t packs_written;
    size_t old_pack_count;
    size_t packed_records_seen;
    size_t consolidated_kept;
    size_t consolidated_removed_unreachable;
    size_t consolidated_removed_duplicate;
    size_t deleted_old_packs;
} gc_state;

/*
 * Returns whether a hash exists in a small in-memory set. GC is an explicit
 * maintenance command, so the first implementation favors simple arrays over a
 * more complex hash table; the design already documents that very large stores
 * may use significant memory while building reachability sets.
 */
static int gc_hash_set_has(const gc_hash_set *set, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    return scribe_hash_set_has(set, hash);
}

/*
 * Adds a hash to a set and reports whether it was already present.
 */
static scribe_error_t gc_hash_set_add(gc_hash_set *set, const uint8_t hash[SCRIBE_HASH_SIZE], int *already) {
    return scribe_hash_set_add(set, hash, already);
}

/*
 * Releases memory held by a hash set.
 */
static void gc_hash_set_destroy(gc_hash_set *set) { scribe_hash_set_destroy(set); }

/*
 * Appends a hash to an ordered list. Lists are used for deferred deletion so gc
 * never deletes a loose object until any required replacement pack is visible.
 */
static scribe_error_t gc_hash_list_add(gc_hash_list *list, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    uint8_t *grown;

    if (list->count == list->cap) {
        size_t new_cap = list->cap == 0u ? 128u : list->cap * 2u;
        grown = (uint8_t *)realloc(list->hashes, new_cap * SCRIBE_HASH_SIZE);
        if (grown == NULL) {
            return scribe_set_error(SCRIBE_ENOMEM, "failed to grow gc deletion list");
        }
        list->hashes = grown;
        list->cap = new_cap;
    }
    memcpy(list->hashes + list->count * SCRIBE_HASH_SIZE, hash, SCRIBE_HASH_SIZE);
    list->count++;
    return SCRIBE_OK;
}

/*
 * Releases memory held by a hash list.
 */
static void gc_hash_list_destroy(gc_hash_list *list) {
    free(list->hashes);
    memset(list, 0, sizeof(*list));
}

/*
 * Records a published pack id so consolidation can delete exactly the old pack
 * set after its replacement pack has been verified.
 */
static scribe_error_t gc_pack_id_list_add(gc_pack_id_list *list, const uint8_t pack_id[16]) {
    uint8_t *grown;

    if (list->count == list->cap) {
        size_t new_cap = list->cap == 0u ? 16u : list->cap * 2u;
        grown = (uint8_t *)realloc(list->ids, new_cap * 16u);
        if (grown == NULL) {
            return scribe_set_error(SCRIBE_ENOMEM, "failed to grow gc pack list");
        }
        list->ids = grown;
        list->cap = new_cap;
    }
    memcpy(list->ids + list->count * 16u, pack_id, 16u);
    list->count++;
    return SCRIBE_OK;
}

/*
 * Releases memory held by a pack-id list.
 */
static void gc_pack_id_list_destroy(gc_pack_id_list *list) {
    free(list->ids);
    memset(list, 0, sizeof(*list));
}

static scribe_error_t gc_walk_object(gc_state *st, const uint8_t hash[SCRIBE_HASH_SIZE], uint8_t expected_type);

/*
 * Walks every edge named by a tree object. Parsing the tree verifies its sorted
 * entry structure; the recursive walk verifies that every child object exists
 * and has the type promised by the tree entry.
 */
static scribe_error_t gc_walk_tree(gc_state *st, const scribe_object *obj) {
    scribe_arena arena;
    scribe_tree_entry *entries = NULL;
    size_t count = 0;
    size_t i;
    scribe_error_t err;

    if (obj->payload_len > (SIZE_MAX - 4096u) / 8u) {
        return scribe_set_error(SCRIBE_ENOMEM, "tree object too large for gc arena");
    }
    err = scribe_arena_init(&arena, obj->payload_len * 8u + 4096u);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_tree_parse(obj->payload, obj->payload_len, &arena, &entries, &count);
    if (err != SCRIBE_OK) {
        scribe_arena_destroy(&arena);
        return err;
    }
    for (i = 0; i < count; i++) {
        err = gc_walk_object(st, entries[i].hash, entries[i].type);
        if (err != SCRIBE_OK) {
            scribe_arena_destroy(&arena);
            return err;
        }
    }
    scribe_arena_destroy(&arena);
    return SCRIBE_OK;
}

/*
 * Walks a commit's root tree and parent edge. This makes every historical blob
 * reachable, not only the newest snapshot.
 */
static scribe_error_t gc_walk_commit(gc_state *st, const scribe_object *obj) {
    scribe_arena arena;
    scribe_commit_view view;
    scribe_error_t err;

    err = scribe_arena_init(&arena, obj->payload_len + 4096u);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_commit_parse(obj->payload, obj->payload_len, &arena, &view);
    if (err == SCRIBE_OK) {
        err = gc_walk_object(st, view.root_tree, SCRIBE_OBJECT_TREE);
    }
    if (err == SCRIBE_OK && view.has_parent) {
        err = gc_walk_object(st, view.parent, SCRIBE_OBJECT_COMMIT);
    }
    scribe_arena_destroy(&arena);
    return err;
}

/*
 * Adds one object to the reachable set, verifies it through scribe_object_read(),
 * and recurses through tree/commit edges.
 */
static scribe_error_t gc_walk_object(gc_state *st, const uint8_t hash[SCRIBE_HASH_SIZE], uint8_t expected_type) {
    scribe_object obj;
    int already = 0;
    scribe_error_t err;

    err = gc_hash_set_add(&st->reachable, hash, &already);
    if (err != SCRIBE_OK || already) {
        return err;
    }
    err = scribe_object_read(st->ctx, hash, &obj);
    if (err != SCRIBE_OK) {
        return err == SCRIBE_ENOT_FOUND ? scribe_set_error(SCRIBE_ECORRUPT, "missing reachable object") : err;
    }
    if (obj.type != expected_type) {
        scribe_object_free(&obj);
        return scribe_set_error(SCRIBE_ECORRUPT, "reachable object has wrong type");
    }
    if (obj.type == SCRIBE_OBJECT_TREE) {
        err = gc_walk_tree(st, &obj);
    } else if (obj.type == SCRIBE_OBJECT_COMMIT) {
        err = gc_walk_commit(st, &obj);
    }
    scribe_object_free(&obj);
    return err;
}

/*
 * Ref iterator callback used to seed reachability. Every ref file under
 * `.scribe/refs` is treated as a root commit.
 */
static scribe_error_t gc_visit_ref(const char *name, const uint8_t hash[SCRIBE_HASH_SIZE], void *user) {
    gc_state *st = (gc_state *)user;

    (void)name;
    st->ref_count++;
    return gc_walk_object(st, hash, SCRIBE_OBJECT_COMMIT);
}

/*
 * Builds the full reachable set from all refs. In current v1/v2 repositories
 * this is normally only refs/heads/main, but the implementation is ready for
 * M4 branches and tags.
 */
static scribe_error_t gc_build_reachable(gc_state *st) { return scribe_refs_iter(st->ctx, gc_visit_ref, st); }

/*
 * Opens the streaming pack writer lazily so a no-op gc does not publish an empty
 * pack file.
 */
static scribe_error_t gc_ensure_writer(gc_state *st) {
    if (st->writer != NULL) {
        return SCRIBE_OK;
    }
    return scribe_pack_writer_begin(st->ctx, &st->writer);
}

/*
 * Finishes the current writer if it contains records. The writer API aborts an
 * empty writer, so this helper can be used unconditionally by cleanup paths.
 */
static scribe_error_t gc_finish_writer(gc_state *st) {
    size_t objects = 0;
    uint64_t bytes = 0;
    scribe_error_t err;

    if (st->writer == NULL) {
        return SCRIBE_OK;
    }
    scribe_pack_writer_stats(st->writer, &objects, &bytes);
    (void)bytes;
    err = scribe_pack_writer_finish(&st->writer, NULL);
    if (err == SCRIBE_OK && objects != 0u) {
        st->packs_written++;
    }
    return err;
}

/*
 * Adds one object to the current pack writer. The helper honors pack target
 * thresholds for loose-object roll-up, publishing the current pack and opening a
 * new one on the next object once the configured target is reached.
 */
static scribe_error_t gc_pack_object_with_rollover(gc_state *st, uint8_t type, const uint8_t *payload,
                                                   size_t payload_len) {
    uint8_t hash[SCRIBE_HASH_SIZE];
    size_t objects = 0;
    uint64_t bytes = 0;
    scribe_error_t err;

    err = gc_ensure_writer(st);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_pack_writer_add(st->writer, type, payload, payload_len, hash);
    if (err != SCRIBE_OK) {
        return err;
    }
    scribe_pack_writer_stats(st->writer, &objects, &bytes);
    if ((st->ctx->config.pack_target_object_count != 0u && objects >= st->ctx->config.pack_target_object_count) ||
        (st->ctx->config.pack_target_size != 0u && bytes >= (uint64_t)st->ctx->config.pack_target_size)) {
        err = gc_finish_writer(st);
    }
    return err;
}

/*
 * Returns whether a loose object's mtime is outside the configured grace window.
 */
static int gc_loose_is_expired(const gc_state *st, time_t mtime) {
    if (st->prune_now || st->grace_seconds <= 0) {
        return 1;
    }
    return difftime(st->now, mtime) >= (double)st->grace_seconds;
}

/*
 * Loose-object scan callback. It validates each loose object, classifies it as
 * reachable or dangling, schedules old dangling objects for pruning, and, when
 * --pack is requested, schedules reachable loose objects for packed replacement.
 */
static scribe_error_t gc_visit_loose_object(const uint8_t hash[SCRIBE_HASH_SIZE], const char *path, time_t mtime,
                                            size_t compressed_size, void *user) {
    gc_state *st = (gc_state *)user;
    scribe_object obj;
    scribe_error_t err;

    (void)path;
    (void)compressed_size;
    st->loose_count++;
    err = scribe_object_read(st->ctx, hash, &obj);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (!gc_hash_set_has(&st->reachable, hash)) {
        st->dangling_loose++;
        if (gc_loose_is_expired(st, mtime)) {
            st->prune_candidates++;
            if (!st->dry_run) {
                err = gc_hash_list_add(&st->prune_loose, hash);
            }
        } else {
            st->grace_kept_loose++;
        }
        scribe_object_free(&obj);
        return err;
    }

    st->reachable_loose++;
    if (st->pack) {
        err = scribe_pack_has(st->ctx, hash);
        if (err == SCRIBE_OK) {
            st->already_packed_loose++;
        } else if (err == SCRIBE_ENOT_FOUND) {
            st->newly_packed_loose++;
            if (!st->dry_run) {
                err = gc_pack_object_with_rollover(st, obj.type, obj.payload, obj.payload_len);
            } else {
                err = SCRIBE_OK;
            }
        }
        if (err == SCRIBE_OK && !st->dry_run) {
            err = gc_hash_list_add(&st->delete_packed_loose, hash);
        }
    }
    scribe_object_free(&obj);
    return err;
}

/*
 * Deletes every loose object named by a deferred deletion list.
 */
static scribe_error_t gc_delete_loose_list(gc_state *st, gc_hash_list *list, size_t *deleted) {
    size_t i;

    for (i = 0; i < list->count; i++) {
        scribe_error_t err = scribe_loose_object_delete(st->ctx, list->hashes + i * SCRIBE_HASH_SIZE);
        if (err != SCRIBE_OK) {
            return err;
        }
        (*deleted)++;
    }
    return SCRIBE_OK;
}

/*
 * Executes dangling-prune and reachable-loose roll-up decisions.
 */
static scribe_error_t gc_process_loose(gc_state *st) {
    scribe_error_t err;

    err = scribe_loose_object_iter(st->ctx, gc_visit_loose_object, st);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (st->dry_run) {
        return SCRIBE_OK;
    }
    err = gc_finish_writer(st);
    if (err != SCRIBE_OK) {
        return err;
    }
    /*
     * Verify the newly visible pack set before deleting loose copies. This is
     * the safety boundary for --pack: if pack publication or verification fails,
     * reachable loose files remain untouched.
     */
    err = scribe_pack_validate_all(st->ctx, NULL);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = gc_delete_loose_list(st, &st->delete_packed_loose, &st->deleted_reachable_loose);
    if (err != SCRIBE_OK) {
        return err;
    }
    return gc_delete_loose_list(st, &st->prune_loose, &st->pruned_loose);
}

/*
 * Records one old pack id before consolidation writes its replacement pack.
 */
static scribe_error_t gc_visit_pack_file(const uint8_t pack_id[16], void *user) {
    gc_state *st = (gc_state *)user;

    st->old_pack_count++;
    return gc_pack_id_list_add(&st->old_packs, pack_id);
}

/*
 * Pack-object iterator callback for consolidation. It keeps the first reachable
 * record for each hash, drops unreachable records, and drops duplicate packed
 * records. The replacement writer is not finished until iteration completes, so
 * the iterator cannot accidentally see the newly written pack.
 */
static scribe_error_t gc_visit_packed_object_for_consolidate(const uint8_t hash[SCRIBE_HASH_SIZE], void *user) {
    gc_state *st = (gc_state *)user;
    scribe_object obj;
    int already = 0;
    scribe_error_t err;

    st->packed_records_seen++;
    if (!gc_hash_set_has(&st->reachable, hash)) {
        st->consolidated_removed_unreachable++;
        return SCRIBE_OK;
    }
    err = gc_hash_set_add(&st->consolidated, hash, &already);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (already) {
        st->consolidated_removed_duplicate++;
        return SCRIBE_OK;
    }
    st->consolidated_kept++;
    if (st->dry_run) {
        return SCRIBE_OK;
    }
    err = scribe_object_read(st->ctx, hash, &obj);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = gc_pack_object_with_rollover(st, obj.type, obj.payload, obj.payload_len);
    scribe_object_free(&obj);
    return err;
}

/*
 * Deletes the old pack set after its consolidated replacement has been written
 * and validated.
 */
static scribe_error_t gc_delete_old_packs(gc_state *st) {
    size_t i;

    for (i = 0; i < st->old_packs.count; i++) {
        scribe_error_t err = scribe_pack_delete_file_pair(st->ctx, st->old_packs.ids + i * 16u);
        if (err != SCRIBE_OK) {
            return err;
        }
        st->deleted_old_packs++;
    }
    return SCRIBE_OK;
}

/*
 * Rewrites published packs into a clean pack set containing exactly one packed
 * record for each reachable packed object. Loose objects are not read by direct
 * filesystem paths; they are only used indirectly if the normal object reader
 * finds a loose duplicate before a packed duplicate.
 */
static scribe_error_t gc_consolidate_packs(gc_state *st) {
    scribe_error_t err;
    size_t i;

    err = scribe_pack_file_iter(st->ctx, gc_visit_pack_file, st);
    if (err != SCRIBE_OK || st->old_pack_count == 0u) {
        return err;
    }
    for (i = 0; i < st->old_packs.count; i++) {
        err = scribe_pack_iter_file(st->ctx, st->old_packs.ids + i * 16u, gc_visit_packed_object_for_consolidate, st);
        if (err != SCRIBE_OK) {
            return err;
        }
    }
    if (st->dry_run) {
        return SCRIBE_OK;
    }
    err = gc_finish_writer(st);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_pack_validate_all(st->ctx, NULL);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = gc_delete_old_packs(st);
    if (err != SCRIBE_OK) {
        return err;
    }
    return scribe_pack_validate_all(st->ctx, NULL);
}

/*
 * Prints a compact human-readable summary. Tests use these stable phrases, and
 * operators can read the output as "what was considered" followed by "what
 * changed".
 */
static void gc_print_summary(const gc_state *st) {
    const char *prefix = st->dry_run ? "gc: dry-run:" : "gc:";

    printf("gc: %zu ref(s), %zu reachable object(s), %zu loose object(s), %zu dangling loose object(s)\n",
           st->ref_count, st->reachable.count, st->loose_count, st->dangling_loose);
    printf("%s prune %zu loose object(s), keep %zu by grace\n", prefix,
           st->dry_run ? st->prune_candidates : st->pruned_loose, st->grace_kept_loose);
    if (st->pack) {
        printf("%s pack %zu reachable loose object(s), %zu already packed, %zu loose copy/copies removed, %zu pack(s) "
               "written\n",
               prefix, st->newly_packed_loose, st->already_packed_loose,
               st->dry_run ? st->reachable_loose : st->deleted_reachable_loose, st->packs_written);
    }
    if (st->consolidate) {
        printf("%s consolidate %zu old pack(s), keep %zu packed object(s), remove %zu unreachable and %zu duplicate "
               "packed record(s), delete %zu old pack(s), write %zu pack(s)\n",
               prefix, st->old_pack_count, st->consolidated_kept, st->consolidated_removed_unreachable,
               st->consolidated_removed_duplicate, st->dry_run ? st->old_pack_count : st->deleted_old_packs,
               st->packs_written);
    }
}

/*
 * Releases all gc-owned memory and aborts any unpublished writer left after an
 * error. Published packs are never removed from cleanup; only explicit
 * consolidation deletion removes old published pack pairs.
 */
static void gc_state_destroy(gc_state *st) {
    scribe_pack_writer_abort(&st->writer);
    gc_hash_set_destroy(&st->reachable);
    gc_hash_set_destroy(&st->consolidated);
    gc_hash_list_destroy(&st->prune_loose);
    gc_hash_list_destroy(&st->delete_packed_loose);
    gc_pack_id_list_destroy(&st->old_packs);
}

/*
 * Implements `scribe gc`.
 */
scribe_error_t scribe_cli_gc(scribe_ctx *ctx, int dry_run, int prune_now, int pack, int consolidate) {
    gc_state st;
    scribe_error_t err;
    size_t ignored_pack_count = 0;

    if (ctx == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid gc context");
    }
    memset(&st, 0, sizeof(st));
    st.ctx = ctx;
    st.dry_run = dry_run;
    st.prune_now = prune_now;
    st.pack = pack;
    st.consolidate = consolidate;
    st.now = time(NULL);
    if (ctx->config.gc_prune_grace_days > 0) {
        st.grace_seconds = (long)ctx->config.gc_prune_grace_days * SCRIBE_GC_SECONDS_PER_DAY;
    }

    err = scribe_pack_validate_all(ctx, &ignored_pack_count);
    if (err == SCRIBE_OK) {
        err = gc_build_reachable(&st);
    }
    if (err == SCRIBE_OK) {
        err = gc_process_loose(&st);
    }
    if (err == SCRIBE_OK && consolidate) {
        err = gc_consolidate_packs(&st);
    }
    if (err == SCRIBE_OK) {
        gc_print_summary(&st);
    }
    gc_state_destroy(&st);
    return err;
}
