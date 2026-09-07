/*
 * Commit revision resolution, log/show output, and tree diffs.
 *
 * This file implements the user-facing history commands that compare commit
 * root trees: log path filtering, show metadata, cat-object pretty output, and
 * diff. The diff algorithm treats blobs as opaque bytes and reports changes at
 * Scribe path level.
 */
#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Reads a commit object and parses it into an arena-backed view. The arena must
 * outlive the returned view because parsed string fields point into arena memory.
 */
scribe_error_t scribe_read_commit_view(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE], scribe_arena *arena,
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

void scribe_format_time_utc(int64_t unix_nanos, char *out, size_t out_size) {
    const int64_t nanos_per_second = 1000000000LL;
    int64_t seconds = unix_nanos / nanos_per_second;
    int64_t nanos = unix_nanos % nanos_per_second;
    time_t t;
    struct tm tm_utc;

    if (out == NULL || out_size == 0u) {
        return;
    }
    if (nanos < 0) {
        nanos += nanos_per_second;
        seconds--;
    }
    t = (time_t)seconds;
    if ((int64_t)t != seconds || gmtime_r(&t, &tm_utc) == NULL) {
        snprintf(out, out_size, "unknown");
        return;
    }
    if (snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02d.%09lldZ", tm_utc.tm_year + 1900, tm_utc.tm_mon + 1,
                 tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, (long long)nanos) < 0) {
        snprintf(out, out_size, "unknown");
    }
}

static int parse_fixed_digits(const char **p, int count, int *out) {
    int value = 0;
    int i;

    for (i = 0; i < count; i++) {
        unsigned char ch = (unsigned char)(*p)[i];

        if (!isdigit(ch)) {
            return 0;
        }
        value = value * 10 + (int)(ch - '0');
    }
    *p += count;
    *out = value;
    return 1;
}

static int consume_char(const char **p, char ch) {
    if (**p != ch) {
        return 0;
    }
    (*p)++;
    return 1;
}

static scribe_error_t parse_numeric_nanos(const char *text, int64_t *out_nanos) {
    char *end = NULL;
    intmax_t parsed;

    errno = 0;
    parsed = strtoimax(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE || parsed < INT64_MIN || parsed > INT64_MAX) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid timestamp '%s'", text);
    }
    *out_nanos = (int64_t)parsed;
    return SCRIBE_OK;
}

static scribe_error_t parse_iso_utc_nanos(const char *text, int64_t *out_nanos) {
    const int64_t nanos_per_second = 1000000000LL;
    const char *p = text;
    struct tm tm_utc;
    struct tm check;
    int year;
    int month;
    int day;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int requested_year;
    int requested_month;
    int requested_day;
    int requested_hour;
    int requested_minute;
    int requested_second;
    int64_t frac_nanos = 0;
    int frac_digits = 0;
    time_t epoch_seconds;
    int64_t seconds;

    memset(&tm_utc, 0, sizeof(tm_utc));
    if (!parse_fixed_digits(&p, 4, &year) || !consume_char(&p, '-') || !parse_fixed_digits(&p, 2, &month) ||
        !consume_char(&p, '-') || !parse_fixed_digits(&p, 2, &day)) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid timestamp '%s'", text);
    }
    if (*p == 'T' || *p == 't' || *p == ' ') {
        p++;
        if (!parse_fixed_digits(&p, 2, &hour) || !consume_char(&p, ':') || !parse_fixed_digits(&p, 2, &minute) ||
            !consume_char(&p, ':') || !parse_fixed_digits(&p, 2, &second)) {
            return scribe_set_error(SCRIBE_EINVAL, "invalid timestamp '%s'", text);
        }
        if (*p == '.') {
            p++;
            while (isdigit((unsigned char)*p)) {
                if (frac_digits < 9) {
                    frac_nanos = frac_nanos * 10 + (int64_t)(*p - '0');
                } else {
                    return scribe_set_error(SCRIBE_EINVAL, "timestamp has more than 9 fractional digits '%s'", text);
                }
                frac_digits++;
                p++;
            }
            if (frac_digits == 0) {
                return scribe_set_error(SCRIBE_EINVAL, "invalid timestamp '%s'", text);
            }
            while (frac_digits < 9) {
                frac_nanos *= 10;
                frac_digits++;
            }
        }
    }
    if (*p == 'Z' || *p == 'z') {
        p++;
    }
    if (*p != '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "timestamps must be UTC ISO-8601 or nanoseconds: '%s'", text);
    }
    requested_year = year;
    requested_month = month;
    requested_day = day;
    requested_hour = hour;
    requested_minute = minute;
    requested_second = second;
    tm_utc.tm_year = year - 1900;
    tm_utc.tm_mon = month - 1;
    tm_utc.tm_mday = day;
    tm_utc.tm_hour = hour;
    tm_utc.tm_min = minute;
    tm_utc.tm_sec = second;
    tm_utc.tm_isdst = 0;
    errno = 0;
    epoch_seconds = timegm(&tm_utc);
    if (epoch_seconds == (time_t)-1 && errno == EOVERFLOW) {
        return scribe_set_error(SCRIBE_EINVAL, "timestamp is out of range '%s'", text);
    }
    if (gmtime_r(&epoch_seconds, &check) == NULL || check.tm_year + 1900 != requested_year ||
        check.tm_mon + 1 != requested_month || check.tm_mday != requested_day || check.tm_hour != requested_hour ||
        check.tm_min != requested_minute || check.tm_sec != requested_second) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid timestamp '%s'", text);
    }
    seconds = (int64_t)epoch_seconds;
    if (seconds > INT64_MAX / nanos_per_second || seconds < INT64_MIN / nanos_per_second) {
        return scribe_set_error(SCRIBE_EINVAL, "timestamp is out of range '%s'", text);
    }
    if (seconds * nanos_per_second > INT64_MAX - frac_nanos) {
        return scribe_set_error(SCRIBE_EINVAL, "timestamp is out of range '%s'", text);
    }
    *out_nanos = seconds * nanos_per_second + frac_nanos;
    return SCRIBE_OK;
}

scribe_error_t scribe_parse_time_arg(const char *text, int64_t *out_nanos) {
    const char *p;

    if (text == NULL || text[0] == '\0' || out_nanos == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "timestamp value is required");
    }
    p = text;
    if (*p == '-' || *p == '+') {
        p++;
    }
    while (*p != '\0' && isdigit((unsigned char)*p)) {
        p++;
    }
    if (*p == '\0' && p != text && !(text[0] == '+' && text[1] == '\0') && !(text[0] == '-' && text[1] == '\0')) {
        return parse_numeric_nanos(text, out_nanos);
    }
    return parse_iso_utc_nanos(text, out_nanos);
}

/*
 * Resolves the small v1 revision language to a full commit hash. Supported
 * forms are HEAD, HEAD~N, a full 64-character commit hash, a full ref name, o
 * a branch/tag shorthand introduced in v2 M4.
 */
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
            err = scribe_read_commit_view(ctx, out, &arena, &view);
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
    if (strlen(rev) == SCRIBE_HEX_HASH_SIZE) {
        return scribe_hash_from_hex(rev, out);
    }
    {
        scribe_error_t ref_err = scribe_refs_resolve_name(ctx, rev, out);
        if (ref_err != SCRIBE_ENOT_FOUND) {
            return ref_err;
        }
    }
    return scribe_set_error(SCRIBE_ENOT_FOUND, "revision '%s' not found", rev);
}

typedef scribe_error_t (*diff_visit_fn)(char status, const char *path, void *user);

static scribe_error_t diff_roots(scribe_ctx *ctx, const uint8_t *old_root, const uint8_t new_root[SCRIBE_HASH_SIZE],
                                 diff_visit_fn visit, void *user);
static scribe_error_t print_diff_visit(char status, const char *path, void *user);
static const char *type_name(uint8_t type);

typedef struct {
    size_t count;
} diff_count_state;

/*
 * Diff visitor that only increments a counter. log --oneline --paths uses it to
 * append `[N changed]` without printing every changed path.
 */
static scribe_error_t count_diff_visit(char status, const char *path, void *user) {
    diff_count_state *state = (diff_count_state *)user;

    (void)status;
    (void)path;
    state->count++;
    return SCRIBE_OK;
}

/*
 * Compares two path-resolution results for log path filtering. A path changed
 * when it appears, disappears, changes type, or resolves to a different hash.
 */
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

/*
 * Returns the human annotation for path appearance/disappearance. Modifications
 * return NULL because the command already emits the commit without extra text.
 */
static const char *path_change_annotation(const scribe_path_resolution *parent, const scribe_path_resolution *current) {
    if (parent->state == SCRIBE_PATH_ABSENT && current->state != SCRIBE_PATH_ABSENT) {
        return "added";
    }
    if (parent->state != SCRIBE_PATH_ABSENT && current->state == SCRIBE_PATH_ABSENT) {
        return "deleted";
    }
    return NULL;
}

/*
 * Counts leaf-level changed paths between two root trees. old_root may be NULL
 * for an initial commit, in which case every leaf in new_root is counted as added.
 */
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

/*
 * Converts diff status letters into words for path-centric show output. The
 * existing diff/log commands keep the compact A/M/D form; show <path> is meant
 * to be read as a small history report, so the longer words are clearer.
 */
static const char *change_word(char status) {
    if (status == 'A') {
        return "added";
    }
    if (status == 'M') {
        return "modified";
    }
    if (status == 'D') {
        return "deleted";
    }
    return "changed";
}

/*
 * Returns true when a changed leaf belongs to the requested path. Exact matches
 * handle blob-level queries; prefix matches handle tree-level queries such as
 * "db/users", where any changed child document should be reported.
 */
static int path_is_under_filter(const char *path, const char *filter) {
    size_t filter_len = strlen(filter);

    if (filter_len == 0u) {
        return 0;
    }
    if (strcmp(path, filter) == 0) {
        return 1;
    }
    return strncmp(path, filter, filter_len) == 0 && path[filter_len] == '/';
}

typedef struct {
    scribe_ctx *ctx;
    const uint8_t *parent_root;
    const uint8_t *current_root;
    const char *filter;
    const char *commit_hex;
    int printed_commit;
    size_t count;
} show_path_history_state;

/*
 * Resolves the object hash that should be shown for one changed path. Additions
 * and modifications point at the current commit's object; deletions point at the
 * parent object because the path no longer exists in the current tree.
 */
static scribe_error_t resolve_changed_object(scribe_ctx *ctx, char status, const uint8_t *parent_root,
                                             const uint8_t *current_root, const char *path,
                                             scribe_path_resolution *out) {
    const uint8_t *root = status == 'D' ? parent_root : current_root;
    scribe_error_t err;

    if (root == NULL) {
        return scribe_set_error(SCRIBE_ECORRUPT, "diff path has no root for change resolution");
    }
    err = scribe_tree_resolve_path(ctx, root, path, out);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (out->state == SCRIBE_PATH_ABSENT) {
        return scribe_set_error(SCRIBE_ECORRUPT, "diff path could not be resolved");
    }
    return SCRIBE_OK;
}

/*
 * Visitor used by `scribe show <path>`. It filters leaf diffs to the requested
 * path/subtree, prints the commit header lazily only when a matching change is
 * found, and includes the object hash that the path changed to/from.
 */
static scribe_error_t show_path_history_visit(char status, const char *path, void *user) {
    show_path_history_state *state = (show_path_history_state *)user;
    scribe_path_resolution resolved;
    char object_hex[SCRIBE_HEX_HASH_SIZE + 1];
    scribe_error_t err;

    if (!path_is_under_filter(path, state->filter)) {
        return SCRIBE_OK;
    }
    err = resolve_changed_object(state->ctx, status, state->parent_root, state->current_root, path, &resolved);
    if (err != SCRIBE_OK) {
        return err;
    }
    scribe_hash_to_hex(resolved.hash, object_hex);
    if (!state->printed_commit) {
        printf("commit %s\n", state->commit_hex);
        state->printed_commit = 1;
    }
    printf("  %s %s %s %s\n", change_word(status), type_name((uint8_t)resolved.state), object_hex, path);
    state->count++;
    return SCRIBE_OK;
}

/*
 * Implements the path-history form of `scribe show <path>`. It walks HEAD's
 * parent chain newest-to-oldest, diffs each commit against its parent, and
 * prints only changed blob/tree paths under the requested path.
 */
static scribe_error_t scribe_cli_show_history(scribe_ctx *ctx, const char *path) {
    uint8_t hash[SCRIBE_HASH_SIZE];
    scribe_error_t err;

    if (path == NULL || path[0] == '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "show path must not be empty");
    }
    err = scribe_refs_read(ctx, "refs/heads/main", hash);
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
        const uint8_t *parent_root = NULL;
        uint8_t next_hash[SCRIBE_HASH_SIZE];
        char commit_hex[SCRIBE_HEX_HASH_SIZE + 1];
        show_path_history_state state;

        err = scribe_arena_init(&arena, 4096);
        if (err != SCRIBE_OK) {
            return err;
        }
        err = scribe_read_commit_view(ctx, hash, &arena, &view);
        if (err != SCRIBE_OK) {
            scribe_arena_destroy(&arena);
            return err;
        }
        if (view.has_parent) {
            err = scribe_arena_init(&parent_arena, 4096);
            if (err == SCRIBE_OK) {
                err = scribe_read_commit_view(ctx, view.parent, &parent_arena, &parent_view);
            }
            if (err != SCRIBE_OK) {
                scribe_arena_destroy(&parent_arena);
                scribe_arena_destroy(&arena);
                return err;
            }
            parent_root = parent_view.root_tree;
            scribe_hash_copy(next_hash, view.parent);
        }
        scribe_hash_to_hex(hash, commit_hex);
        memset(&state, 0, sizeof(state));
        state.ctx = ctx;
        state.parent_root = parent_root;
        state.current_root = view.root_tree;
        state.filter = path;
        state.commit_hex = commit_hex;
        err = diff_roots(ctx, parent_root, view.root_tree, show_path_history_visit, &state);
        scribe_arena_destroy(&parent_arena);
        scribe_arena_destroy(&arena);
        if (err != SCRIBE_OK) {
            return err;
        }
        if (!view.has_parent) {
            break;
        }
        scribe_hash_copy(hash, next_hash);
    }
    return SCRIBE_OK;
}

/*
 * Identifies arguments that should be treated strictly as revisions. Othe
 * unresolved show arguments fall through to the path-history form so users can
 * run `scribe show db/users/alice` without a separate command.
 */
static int show_argument_slice_is_revision_syntax(const char *arg, size_t len) {
    size_t i;

    if ((len == 4u && memcmp(arg, "HEAD", 4u) == 0) || (len >= 5u && memcmp(arg, "HEAD~", 5u) == 0) ||
        (len > 5u && memcmp(arg, "refs/", 5u) == 0)) {
        return 1;
    }
    if (len != SCRIBE_HEX_HASH_SIZE) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (!((arg[i] >= '0' && arg[i] <= '9') || (arg[i] >= 'a' && arg[i] <= 'f') ||
              (arg[i] >= 'A' && arg[i] <= 'F'))) {
            return 0;
        }
    }
    return 1;
}

static int show_argument_is_revision_syntax(const char *arg) {
    return show_argument_slice_is_revision_syntax(arg, strlen(arg));
}

/*
 * Implements `scribe log`. It walks the parent chain from HEAD, optionally
 * filters commits by one path, optionally prints changed paths, and applies -n
 * to the number of commits emitted rather than the number scanned.
 */
scribe_error_t scribe_cli_log(scribe_ctx *ctx, int oneline, size_t limit, int show_paths, const char *path_filter,
                              const scribe_time_range *time_range) {
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
        err = scribe_read_commit_view(ctx, hash, &arena, &view);
        if (err != SCRIBE_OK) {
            scribe_arena_destroy(&arena);
            return err;
        }
        if (view.has_parent) {
            err = scribe_arena_init(&parent_arena, 4096);
            if (err == SCRIBE_OK) {
                err = scribe_read_commit_view(ctx, view.parent, &parent_arena, &parent_view);
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
        if (emit && time_range != NULL) {
            if (time_range->has_since && view.committer_time < time_range->since_nanos) {
                emit = 0;
            }
            if (time_range->has_until && view.committer_time > time_range->until_nanos) {
                emit = 0;
            }
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
                printf("committer %s <%s> %lld\n", view.committer_name, view.committer_email,
                       (long long)view.committer_time);
                {
                    char committed_at[64];

                    scribe_format_time_utc(view.committer_time, committed_at, sizeof(committed_at));
                    printf("committed_at %s\n\n", committed_at);
                }
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

/*
 * Implements `scribe show <commit>` and delegates `scribe show <commit>:<path>`
 * to the path-inspection implementation. The commit form prints metadata and
 * diffs the commit against its parent for the changes section.
 */
scribe_error_t scribe_cli_show(scribe_ctx *ctx, const char *rev) {
    uint8_t hash[SCRIBE_HASH_SIZE];
    scribe_arena arena;
    scribe_commit_view view;
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    scribe_error_t err;

    if (strchr(rev, ':') != NULL) {
        const char *colon = strchr(rev, ':');
        size_t prefix_len = (size_t)(colon - rev);
        char *prefix = (char *)malloc(prefix_len + 1u);

        if (prefix == NULL) {
            return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate show revision prefix");
        }
        memcpy(prefix, rev, prefix_len);
        prefix[prefix_len] = '\0';
        err = scribe_resolve_commit(ctx, prefix, hash);
        if (err == SCRIBE_OK) {
            free(prefix);
            return scribe_cli_show_path(ctx, rev);
        }
        if (show_argument_slice_is_revision_syntax(prefix, prefix_len)) {
            free(prefix);
            return err;
        }
        free(prefix);
        return scribe_cli_show_history(ctx, rev);
    }
    err = scribe_resolve_commit(ctx, rev, hash);
    if (err != SCRIBE_OK) {
        if (show_argument_is_revision_syntax(rev)) {
            return err;
        }
        return scribe_cli_show_history(ctx, rev);
    }
    err = scribe_arena_init(&arena, 4096);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_read_commit_view(ctx, hash, &arena, &view);
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
    {
        char committed_at[64];

        scribe_format_time_utc(view.committer_time, committed_at, sizeof(committed_at));
        printf("committed_at %s\n", committed_at);
    }
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

/*
 * Converts an object type byte into the label used by cat-object and tree
 * pretty-printing. Unknown types return "unknown" for diagnostics.
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
    return "unknown";
}

/*
 * Pretty-prints a tree object payload as `<type> <hash>\t<name>` entries. This
 * is the cat-object -p representation for a raw tree object.
 */
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

/*
 * Implements `scribe-cli cat-object`. Full object hashes are read directly.
 * Revision placeholders such as HEAD and HEAD~N resolve to commit objects so
 * the command line accepts the same commit placeholders everywhere a commit
 * hash would work.
 */
scribe_error_t scribe_cli_cat_object(scribe_ctx *ctx, char mode, const char *spec) {
    uint8_t hash[SCRIBE_HASH_SIZE];
    scribe_object obj;
    scribe_error_t err = scribe_hash_from_hex(spec, hash);

    if (err != SCRIBE_OK) {
        err = scribe_resolve_commit(ctx, spec, hash);
        if (err != SCRIBE_OK) {
            return err;
        }
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
        } else if (obj.type == SCRIBE_OBJECT_BLOB) {
            if (obj.payload_len != 0u && fwrite(obj.payload, 1, obj.payload_len, stdout) != obj.payload_len) {
                err = scribe_set_error(SCRIBE_EIO, "failed to write blob payload");
            }
        } else {
            fwrite(obj.payload, 1, obj.payload_len, stdout);
            if (obj.payload_len == 0u || obj.payload[obj.payload_len - 1u] != '\n') {
                printf("\n");
            }
        }
    } else {
        err = scribe_set_error(SCRIBE_EINVAL, "unknown cat-object mode");
    }
    scribe_object_free(&obj);
    return err;
}

/*
 * Diff visitor that prints one changed path, optionally with an indentation
 * prefix supplied through the user pointer.
 */
static scribe_error_t print_diff_visit(char status, const char *path, void *user) {
    const char *indent = user == NULL ? "" : (const char *)user;

    printf("%s%c %s\n", indent, status, path);
    return SCRIBE_OK;
}

/*
 * Builds a slash-separated child path from an existing prefix and one entry
 * name. The caller owns the returned heap string.
 */
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

/*
 * Reads and parses a tree object for diffing. The arena is reinitialized to a
 * capacity based on the object payload so large trees can be parsed safely.
 */
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

/*
 * Reports every leaf under a blob or tree as added/deleted. This turns subtree
 * additions/deletions into the leaf-level output users expect from diff/log --paths.
 */
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

/*
 * Recursively diffs two tree objects using their canonical byte-sorted entry
 * order. Matching tree entries recurse; matching blob entries with different
 * hashes report a modification.
 */
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

/*
 * Diffs two root trees, treating a missing old root as an initial commit where
 * every leaf in the new root is an added path.
 */
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

/*
 * Implements `scribe diff`. With one revision it compares that commit's parent
 * to the commit; with two revisions it compares the two resolved root trees.
 */
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
        err = scribe_read_commit_view(ctx, b_hash, &arena_b, &b_commit);
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
    err = scribe_read_commit_view(ctx, a_hash, &arena_a, &a_commit);
    if (err == SCRIBE_OK) {
        err = scribe_read_commit_view(ctx, b_hash, &arena_b, &b_commit);
    }
    if (err == SCRIBE_OK) {
        err = diff_trees(ctx, a_commit.root_tree, b_commit.root_tree, "", print_diff_visit, "");
    }
    scribe_arena_destroy(&arena_a);
    scribe_arena_destroy(&arena_b);
    return err;
}
