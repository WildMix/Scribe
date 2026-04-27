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

scribe_error_t scribe_cli_log(scribe_ctx *ctx, int oneline, size_t limit) {
    uint8_t hash[SCRIBE_HASH_SIZE];
    size_t seen = 0;
    scribe_error_t err = scribe_refs_read(ctx, "refs/heads/main", hash);

    if (err == SCRIBE_ENOT_FOUND) {
        return SCRIBE_OK;
    }
    if (err != SCRIBE_OK) {
        return err;
    }
    while (limit == 0 || seen < limit) {
        scribe_arena arena;
        scribe_commit_view view;
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
        scribe_hash_to_hex(hash, hex);
        if (oneline) {
            printf("%.12s", hex);
            if (view.message_len != 0) {
                printf(" %.*s", (int)view.message_len, (const char *)view.message);
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
        }
        seen++;
        if (!view.has_parent) {
            scribe_arena_destroy(&arena);
            break;
        }
        scribe_hash_copy(hash, view.parent);
        scribe_arena_destroy(&arena);
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

static char *join_path(scribe_arena *arena, const char *prefix, const char *name) {
    size_t plen = strlen(prefix);
    size_t nlen = strlen(name);
    char *out = (char *)scribe_arena_alloc(arena, plen + (plen == 0 ? 0u : 1u) + nlen + 1u, _Alignof(char));
    if (out == NULL) {
        return NULL;
    }
    if (plen != 0) {
        memcpy(out, prefix, plen);
        out[plen] = '/';
        memcpy(out + plen + 1u, name, nlen);
        out[plen + 1u + nlen] = '\0';
    } else {
        memcpy(out, name, nlen);
        out[nlen] = '\0';
    }
    return out;
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

static scribe_error_t report_all(scribe_ctx *ctx, scribe_arena *path_arena, char status,
                                 const uint8_t hash[SCRIBE_HASH_SIZE], uint8_t type, const char *path) {
    if (type == SCRIBE_OBJECT_BLOB) {
        printf("%c %s\n", status, path);
        return SCRIBE_OK;
    }
    {
        scribe_arena arena;
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
            char *child = join_path(path_arena, path, entries[i].name);
            if (child == NULL) {
                scribe_arena_destroy(&arena);
                return SCRIBE_ENOMEM;
            }
            err = report_all(ctx, path_arena, status, entries[i].hash, entries[i].type, child);
            if (err != SCRIBE_OK) {
                scribe_arena_destroy(&arena);
                return err;
            }
        }
        scribe_arena_destroy(&arena);
    }
    return SCRIBE_OK;
}

static scribe_error_t diff_trees(scribe_ctx *ctx, scribe_arena *path_arena, const uint8_t a_hash[SCRIBE_HASH_SIZE],
                                 const uint8_t b_hash[SCRIBE_HASH_SIZE], const char *prefix) {
    scribe_arena arena_a;
    scribe_arena arena_b;
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
            path = join_path(path_arena, prefix, a[ai].name);
            if (path == NULL) {
                err = SCRIBE_ENOMEM;
                break;
            }
            err = report_all(ctx, path_arena, 'D', a[ai].hash, a[ai].type, path);
            ai++;
        } else if (cmp > 0) {
            path = join_path(path_arena, prefix, b[bi].name);
            if (path == NULL) {
                err = SCRIBE_ENOMEM;
                break;
            }
            err = report_all(ctx, path_arena, 'A', b[bi].hash, b[bi].type, path);
            bi++;
        } else {
            path = join_path(path_arena, prefix, a[ai].name);
            if (path == NULL) {
                err = SCRIBE_ENOMEM;
                break;
            }
            if (scribe_hash_cmp(a[ai].hash, b[bi].hash) != 0) {
                if (a[ai].type == SCRIBE_OBJECT_TREE && b[bi].type == SCRIBE_OBJECT_TREE) {
                    err = diff_trees(ctx, path_arena, a[ai].hash, b[bi].hash, path);
                } else {
                    printf("M %s\n", path);
                }
            }
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

scribe_error_t scribe_cli_diff(scribe_ctx *ctx, const char *a_rev, const char *b_rev) {
    uint8_t a_hash[SCRIBE_HASH_SIZE];
    uint8_t b_hash[SCRIBE_HASH_SIZE];
    scribe_arena arena_a;
    scribe_arena arena_b;
    scribe_arena path_arena;
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
        (err = scribe_arena_init(&arena_b, 4096)) != SCRIBE_OK ||
        (err = scribe_arena_init(&path_arena, 65536)) != SCRIBE_OK) {
        scribe_arena_destroy(&arena_a);
        scribe_arena_destroy(&arena_b);
        scribe_arena_destroy(&path_arena);
        return err;
    }
    err = read_commit_view(ctx, a_hash, &arena_a, &a_commit);
    if (err == SCRIBE_OK) {
        err = read_commit_view(ctx, b_hash, &arena_b, &b_commit);
    }
    if (err == SCRIBE_OK) {
        err = diff_trees(ctx, &path_arena, a_commit.root_tree, b_commit.root_tree, "");
    }
    scribe_arena_destroy(&arena_a);
    scribe_arena_destroy(&arena_b);
    scribe_arena_destroy(&path_arena);
    return err;
}
