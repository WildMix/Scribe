#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

scribe_error_t scribe_resolve_commit(scribe_ctx *ctx, const char *rev, uint8_t out[SCRIBE_HASH_SIZE]) {
    if (rev == NULL || strcmp(rev, "HEAD") == 0) {
        return scribe_refs_read(ctx, "refs/heads/main", out);
    }
    /*
     * v1 intentionally supports only a tiny revision language: HEAD, HEAD~N,
     * or a full 64-hex hash. HEAD~N is resolved by repeatedly parsing parent
     * commits; abbreviated hashes are display-only and are not accepted here.
     */
    if (strncmp(rev, "HEAD~", 5) == 0) {
        char *end = NULL;
        unsigned long n = strtoul(rev + 5, &end, 10);
        unsigned long i;
        scribe_error_t err = scribe_refs_read(ctx, "refs/heads/main", out);
        if (err != SCRIBE_OK) {
            return err;
        }
        if (end == rev + 5 || *end != '\0') {
            return scribe_set_error(SCRIBE_EINVAL, "invalid revision '%s'", rev);
        }
        for (i = 0; i < n; i++) {
            scribe_arena arena;
            scribe_commit_view view;
            err = scribe_arena_init(&arena, 4096);
            if (err != SCRIBE_OK) {
                return err;
            }
            err = read_commit_view(ctx, out, &arena, &view);
            if (err == SCRIBE_OK) {
                if (!view.has_parent) {
                    err = scribe_set_error(SCRIBE_ENOT_FOUND, "revision '%s' has no parent", rev);
                } else {
                    scribe_hash_copy(out, view.parent);
                }
            }
            scribe_arena_destroy(&arena);
            if (err != SCRIBE_OK) {
                return err;
            }
        }
        return SCRIBE_OK;
    }
    return scribe_hash_from_hex(rev, out);
}

typedef scribe_error_t (*diff_visit_fn)(char status, const char *path, void *user);

static scribe_error_t diff_roots(scribe_ctx *ctx, const uint8_t *old_root, const uint8_t new_root[SCRIBE_HASH_SIZE],
                                 diff_visit_fn visit, void *user);
static scribe_error_t print_diff_visit(char status, const char *path, void *user);

typedef struct {
    size_t count;
} diff_count_state;

static scribe_error_t count_diff_visit(char status, const char *path, void *user) {
    diff_count_state *state = (diff_count_state *)user;

    (void)status;
    (void)path;
    state->count++;
    return SCRIBE_OK;
}

static int path_resolution_changed(const scribe_path_resolution *parent, const scribe_path_resolution *current) {
    /*
     * Path-history filtering compares object identity, not blob contents. A
     * tree-level filter works because any child change produces a different
     * tree hash; a blob-level filter works because changed payload bytes produce
     * a different blob hash.
     */
    if (parent->state != current->state) {
        return 1;
    }
    if (current->state == SCRIBE_PATH_ABSENT) {
        return 0;
    }
    return scribe_hash_cmp(parent->hash, current->hash) != 0;
}

static const char *path_change_annotation(const scribe_path_resolution *parent, const scribe_path_resolution *current) {
    if (parent->state == SCRIBE_PATH_ABSENT && current->state != SCRIBE_PATH_ABSENT) {
        return "added";
    }
    if (parent->state != SCRIBE_PATH_ABSENT && current->state == SCRIBE_PATH_ABSENT) {
        return "deleted";
    }
    return NULL;
}

static scribe_error_t count_changed_paths(scribe_ctx *ctx, const uint8_t *old_root,
                                          const uint8_t new_root[SCRIBE_HASH_SIZE], size_t *out) {
    diff_count_state state;
    scribe_error_t err;

    state.count = 0;
    err = diff_roots(ctx, old_root, new_root, count_diff_visit, &state);
    if (err != SCRIBE_OK) {
        return err;
    }
    *out = state.count;
    return SCRIBE_OK;
}

scribe_error_t scribe_cli_log(scribe_ctx *ctx, int oneline, size_t limit, int show_paths, const char *path_filter) {
    uint8_t hash[SCRIBE_HASH_SIZE];
    size_t emitted = 0;
    scribe_error_t err = scribe_refs_read(ctx, "refs/heads/main", hash);

    if (err == SCRIBE_ENOT_FOUND) {
        return SCRIBE_OK;
    }
    if (err != SCRIBE_OK) {
        return err;
    }
    while (1) {
        scribe_arena arena = {0};
        scribe_arena parent_arena = {0};
        scribe_commit_view view;
        scribe_commit_view parent_view;
        uint8_t next_hash[SCRIBE_HASH_SIZE];
        const uint8_t *parent_root = NULL;
        const char *annotation = NULL;
        int emit = 1;
        int have_parent_view = 0;
        size_t changed_count = 0;
        char hex[SCRIBE_HEX_HASH_SIZE + 1];

        err = scribe_arena_init(&arena, 4096);
        if (err != SCRIBE_OK) {
            return err;
        }
        err = read_commit_view(ctx, hash, &arena, &view);
        if (err != SCRIBE_OK) {
            scribe_arena_destroy(&arena);
            return err;
        }
        if (view.has_parent) {
            err = scribe_arena_init(&parent_arena, 4096);
            if (err == SCRIBE_OK) {
                err = read_commit_view(ctx, view.parent, &parent_arena, &parent_view);
            }
            if (err != SCRIBE_OK) {
                scribe_arena_destroy(&parent_arena);
                scribe_arena_destroy(&arena);
                return err;
            }
            parent_root = parent_view.root_tree;
            have_parent_view = 1;
        }
        /*
         * A path filter affects only which commits are emitted. The scan still
         * walks every parent commit until history ends or the emitted-count
         * limit is reached, so "-n 3 -- path" means "three commits that changed
         * path", not "scan three commits total".
         */
        if (path_filter != NULL) {
            scribe_path_resolution current_path;
            scribe_path_resolution parent_path;

            err = scribe_tree_resolve_path(ctx, view.root_tree, path_filter, &current_path);
            if (err == SCRIBE_OK && have_parent_view) {
                err = scribe_tree_resolve_path(ctx, parent_view.root_tree, path_filter, &parent_path);
            } else {
                memset(&parent_path, 0, sizeof(parent_path));
            }
            if (err != SCRIBE_OK) {
                scribe_arena_destroy(&parent_arena);
                scribe_arena_destroy(&arena);
                return err;
            }
            emit = path_resolution_changed(&parent_path, &current_path);
            annotation = path_change_annotation(&parent_path, &current_path);
        }
        if (emit && show_paths && oneline) {
            err = count_changed_paths(ctx, parent_root, view.root_tree, &changed_count);
            if (err != SCRIBE_OK) {
                scribe_arena_destroy(&parent_arena);
                scribe_arena_destroy(&arena);
                return err;
            }
        }
        scribe_hash_to_hex(hash, hex);
        if (emit) {
            if (oneline) {
                printf("%.12s", hex);
                if (view.message_len != 0) {
                    printf(" %.*s", (int)view.message_len, (const char *)view.message);
                }
                if (show_paths) {
                    printf(" [%zu changed]", changed_count);
                }
                if (annotation != NULL) {
                    printf(" (%s)", annotation);
                }
                printf("\n");
            } else {
                printf("commit %s\n", hex);
                if (view.has_parent) {
                    char parent_hex[SCRIBE_HEX_HASH_SIZE + 1];
                    scribe_hash_to_hex(view.parent, parent_hex);
                    printf("parent %s\n", parent_hex);
                }
                printf("author %s <%s> %lld\n", view.author_name, view.author_email, (long long)view.author_time);
                printf("committer %s <%s> %lld\n\n", view.committer_name, view.committer_email,
                       (long long)view.committer_time);
                if (view.message_len != 0) {
                    printf("%.*s\n", (int)view.message_len, (const char *)view.message);
                }
                printf("\n");
                if (annotation != NULL) {
                    printf("  (%s)\n", annotation);
                }
                if (show_paths) {
                    err = diff_roots(ctx, parent_root, view.root_tree, print_diff_visit, "  ");
                    if (err != SCRIBE_OK) {
                        scribe_arena_destroy(&parent_arena);
                        scribe_arena_destroy(&arena);
                        return err;
                    }
                }
                if (annotation != NULL || show_paths) {
                    printf("\n");
                }
            }
            emitted++;
        }
        if (view.has_parent) {
            scribe_hash_copy(next_hash, view.parent);
        }
        scribe_arena_destroy(&parent_arena);
        scribe_arena_destroy(&arena);
        if ((limit != 0 && emitted >= limit) || !view.has_parent) {
            break;
        }
        if (!view.has_parent) {
            break;
        }
        scribe_hash_copy(hash, next_hash);
    }
    return SCRIBE_OK;
}

scribe_error_t scribe_cli_show(scribe_ctx *ctx, const char *rev) {
    uint8_t hash[SCRIBE_HASH_SIZE];
    scribe_arena arena;
    scribe_commit_view view;
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    scribe_error_t err;

    if (strchr(rev, ':') != NULL) {
        return scribe_cli_show_path(ctx, rev);
    }
    err = scribe_resolve_commit(ctx, rev, hash);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_arena_init(&arena, 4096);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = read_commit_view(ctx, hash, &arena, &view);
    if (err != SCRIBE_OK) {
        scribe_arena_destroy(&arena);
        return err;
    }
    scribe_hash_to_hex(hash, hex);
    printf("commit %s\n", hex);
    if (view.has_parent) {
        char parent_hex[SCRIBE_HEX_HASH_SIZE + 1];
        scribe_hash_to_hex(view.parent, parent_hex);
        printf("parent %s\n", parent_hex);
    }
    printf("author %s <%s> %lld\n", view.author_name, view.author_email, (long long)view.author_time);
    printf("committer %s <%s> %lld\n", view.committer_name, view.committer_email, (long long)view.committer_time);
    printf("process %s %s %s %s\n\n", view.process_name, view.process_version, view.process_params,
           view.process_correlation_id);
    if (view.message_len != 0) {
        printf("%.*s\n", (int)view.message_len, (const char *)view.message);
    }
    printf("\nchanges:\n");
    if (view.has_parent) {
        char parent_hex[SCRIBE_HEX_HASH_SIZE + 1];
        scribe_hash_to_hex(view.parent, parent_hex);
        err = scribe_cli_diff(ctx, parent_hex, hex);
    }
    scribe_arena_destroy(&arena);
    return err;
}

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
    return "unknown";
}

static scribe_error_t pretty_tree(scribe_object *obj) {
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
        char hex[SCRIBE_HEX_HASH_SIZE + 1];
        scribe_hash_to_hex(entries[i].hash, hex);
        printf("%s %s\t%s\n", type_name(entries[i].type), hex, entries[i].name);
    }
    scribe_arena_destroy(&arena);
    return SCRIBE_OK;
}

scribe_error_t scribe_cli_cat_object(scribe_ctx *ctx, char mode, const char *hex) {
    uint8_t hash[SCRIBE_HASH_SIZE];
    scribe_object obj;
    scribe_error_t err = scribe_hash_from_hex(hex, hash);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_object_read(ctx, hash, &obj);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (mode == 't') {
        printf("%s\n", type_name(obj.type));
    } else if (mode == 's') {
        printf("%zu\n", obj.payload_len);
    } else if (mode == 'p') {
        if (obj.type == SCRIBE_OBJECT_TREE) {
            err = pretty_tree(&obj);
        } else {
            fwrite(obj.payload, 1, obj.payload_len, stdout);
            if (obj.type != SCRIBE_OBJECT_BLOB || obj.payload_len == 0 || obj.payload[obj.payload_len - 1u] != '\n') {
                printf("\n");
            }
        }
    } else {
        err = scribe_set_error(SCRIBE_EINVAL, "unknown cat-object mode");
    }
    scribe_object_free(&obj);
    return err;
}

static scribe_error_t print_diff_visit(char status, const char *path, void *user) {
    const char *indent = user == NULL ? "" : (const char *)user;

    printf("%s%c %s\n", indent, status, path);
    return SCRIBE_OK;
}

static scribe_error_t join_path(const char *prefix, const char *name, char **out) {
    size_t plen = strlen(prefix);
    size_t nlen = strlen(name);
    size_t len = nlen;
    char *path;

    if (plen != 0) {
        if (plen > SIZE_MAX - 1u || plen + 1u > SIZE_MAX - nlen) {
            return scribe_set_error(SCRIBE_ENOMEM, "diff path is too large");
        }
        len = plen + 1u + nlen;
    }
    if (len == SIZE_MAX) {
        return scribe_set_error(SCRIBE_ENOMEM, "diff path is too large");
    }
    path = (char *)malloc(len + 1u);
    if (path == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate diff path");
    }
    if (plen != 0) {
        memcpy(path, prefix, plen);
        path[plen] = '/';
        memcpy(path + plen + 1u, name, nlen);
    } else {
        memcpy(path, name, nlen);
    }
    path[len] = '\0';
    *out = path;
    return SCRIBE_OK;
}

static scribe_error_t parse_tree_object(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE], scribe_arena *arena,
                                        scribe_tree_entry **entries, size_t *count) {
    scribe_object obj;
    scribe_error_t err = scribe_object_read(ctx, hash, &obj);
    size_t arena_capacity = 0;
    if (err != SCRIBE_OK) {
        return err;
    }
    if (obj.type != SCRIBE_OBJECT_TREE) {
        scribe_object_free(&obj);
        return scribe_set_error(SCRIBE_ECORRUPT, "expected tree while diffing");
    }
    err = scribe_tree_parse_arena_capacity(obj.payload_len, &arena_capacity);
    if (err == SCRIBE_OK) {
        scribe_arena_destroy(arena);
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

static scribe_error_t report_all(scribe_ctx *ctx, char status, const uint8_t hash[SCRIBE_HASH_SIZE], uint8_t type,
                                 const char *path, diff_visit_fn visit, void *user) {
    /*
     * When an entire subtree is added or deleted, user-facing diff output is
     * still leaf-oriented. Descend until blobs are reached and report every leaf
     * path with the same status.
     */
    if (type == SCRIBE_OBJECT_BLOB) {
        return visit(status, path, user);
    }
    if (type != SCRIBE_OBJECT_TREE) {
        return scribe_set_error(SCRIBE_ECORRUPT, "invalid tree entry type while diffing");
    }
    {
        scribe_arena arena = {0};
        scribe_tree_entry *entries = NULL;
        size_t count = 0;
        size_t i;
        scribe_error_t err = scribe_arena_init(&arena, 4096);
        if (err != SCRIBE_OK) {
            return err;
        }
        err = parse_tree_object(ctx, hash, &arena, &entries, &count);
        if (err != SCRIBE_OK) {
            scribe_arena_destroy(&arena);
            return err;
        }
        for (i = 0; i < count; i++) {
            char *child = NULL;
            err = join_path(path, entries[i].name, &child);
            if (err != SCRIBE_OK) {
                scribe_arena_destroy(&arena);
                return err;
            }
            err = report_all(ctx, status, entries[i].hash, entries[i].type, child, visit, user);
            free(child);
            if (err != SCRIBE_OK) {
                scribe_arena_destroy(&arena);
                return err;
            }
        }
        scribe_arena_destroy(&arena);
    }
    return SCRIBE_OK;
}

static scribe_error_t diff_trees(scribe_ctx *ctx, const uint8_t a_hash[SCRIBE_HASH_SIZE],
                                 const uint8_t b_hash[SCRIBE_HASH_SIZE], const char *prefix, diff_visit_fn visit,
                                 void *user) {
    scribe_arena arena_a = {0};
    scribe_arena arena_b = {0};
    scribe_tree_entry *a = NULL;
    scribe_tree_entry *b = NULL;
    size_t ac = 0;
    size_t bc = 0;
    size_t ai = 0;
    size_t bi = 0;
    scribe_error_t err;

    if (scribe_hash_cmp(a_hash, b_hash) == 0) {
        return SCRIBE_OK;
    }
    if ((err = scribe_arena_init(&arena_a, 4096)) != SCRIBE_OK ||
        (err = scribe_arena_init(&arena_b, 4096)) != SCRIBE_OK) {
        scribe_arena_destroy(&arena_a);
        scribe_arena_destroy(&arena_b);
        return err;
    }
    err = parse_tree_object(ctx, a_hash, &arena_a, &a, &ac);
    if (err == SCRIBE_OK) {
        err = parse_tree_object(ctx, b_hash, &arena_b, &b, &bc);
    }
    if (err != SCRIBE_OK) {
        scribe_arena_destroy(&arena_a);
        scribe_arena_destroy(&arena_b);
        return err;
    }
    /*
     * Both tree payloads are strictly byte-sorted by name. This lets diff use a
     * merge walk: names that exist only on the left are deletions, only on the
     * right are additions, and matching names recurse or become modifications.
     */
    while (ai < ac || bi < bc) {
        int cmp;
        char *path;
        if (ai >= ac) {
            cmp = 1;
        } else if (bi >= bc) {
            cmp = -1;
        } else {
            size_t min = a[ai].name_len < b[bi].name_len ? a[ai].name_len : b[bi].name_len;
            cmp = memcmp(a[ai].name, b[bi].name, min);
            if (cmp == 0 && a[ai].name_len != b[bi].name_len) {
                cmp = a[ai].name_len < b[bi].name_len ? -1 : 1;
            }
        }
        if (cmp < 0) {
            err = join_path(prefix, a[ai].name, &path);
            if (err != SCRIBE_OK) {
                break;
            }
            err = report_all(ctx, 'D', a[ai].hash, a[ai].type, path, visit, user);
            free(path);
            ai++;
        } else if (cmp > 0) {
            err = join_path(prefix, b[bi].name, &path);
            if (err != SCRIBE_OK) {
                break;
            }
            err = report_all(ctx, 'A', b[bi].hash, b[bi].type, path, visit, user);
            free(path);
            bi++;
        } else {
            err = join_path(prefix, a[ai].name, &path);
            if (err != SCRIBE_OK) {
                break;
            }
            if (scribe_hash_cmp(a[ai].hash, b[bi].hash) != 0) {
                if (a[ai].type == SCRIBE_OBJECT_TREE && b[bi].type == SCRIBE_OBJECT_TREE) {
                    err = diff_trees(ctx, a[ai].hash, b[bi].hash, path, visit, user);
                } else {
                    err = visit('M', path, user);
                }
            }
            free(path);
            ai++;
            bi++;
        }
        if (err != SCRIBE_OK) {
            break;
        }
    }
    scribe_arena_destroy(&arena_a);
    scribe_arena_destroy(&arena_b);
    return err;
}

static scribe_error_t diff_roots(scribe_ctx *ctx, const uint8_t *old_root, const uint8_t new_root[SCRIBE_HASH_SIZE],
                                 diff_visit_fn visit, void *user) {
    if (visit == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "diff visitor is NULL");
    }
    if (old_root == NULL) {
        return report_all(ctx, 'A', new_root, SCRIBE_OBJECT_TREE, "", visit, user);
    }
    return diff_trees(ctx, old_root, new_root, "", visit, user);
}

scribe_error_t scribe_cli_diff(scribe_ctx *ctx, const char *a_rev, const char *b_rev) {
    uint8_t a_hash[SCRIBE_HASH_SIZE];
    uint8_t b_hash[SCRIBE_HASH_SIZE];
    scribe_arena arena_a = {0};
    scribe_arena arena_b = {0};
    scribe_commit_view a_commit;
    scribe_commit_view b_commit;
    scribe_error_t err;

    if (b_rev == NULL) {
        err = scribe_resolve_commit(ctx, a_rev, b_hash);
        if (err != SCRIBE_OK) {
            return err;
        }
        err = scribe_arena_init(&arena_b, 4096);
        if (err != SCRIBE_OK) {
            return err;
        }
        err = read_commit_view(ctx, b_hash, &arena_b, &b_commit);
        if (err != SCRIBE_OK) {
            scribe_arena_destroy(&arena_b);
            return err;
        }
        if (!b_commit.has_parent) {
            scribe_arena_destroy(&arena_b);
            return SCRIBE_OK;
        }
        scribe_hash_copy(a_hash, b_commit.parent);
        scribe_arena_destroy(&arena_b);
    } else {
        if ((err = scribe_resolve_commit(ctx, a_rev, a_hash)) != SCRIBE_OK ||
            (err = scribe_resolve_commit(ctx, b_rev, b_hash)) != SCRIBE_OK) {
            return err;
        }
    }
    if ((err = scribe_arena_init(&arena_a, 4096)) != SCRIBE_OK ||
        (err = scribe_arena_init(&arena_b, 4096)) != SCRIBE_OK) {
        scribe_arena_destroy(&arena_a);
        scribe_arena_destroy(&arena_b);
        return err;
    }
    err = read_commit_view(ctx, a_hash, &arena_a, &a_commit);
    if (err == SCRIBE_OK) {
        err = read_commit_view(ctx, b_hash, &arena_b, &b_commit);
    }
    if (err == SCRIBE_OK) {
        err = diff_trees(ctx, a_commit.root_tree, b_commit.root_tree, "", print_diff_visit, "");
    }
    scribe_arena_destroy(&arena_a);
    scribe_arena_destroy(&arena_b);
    return err;
}
