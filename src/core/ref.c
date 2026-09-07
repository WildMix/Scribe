/*
 * Repository locking and ref updates.
 *
 * Scribe's adapter-owned ref is `refs/heads/main`, and v2 adds operator branch
 * and tag refs under the same direct-ref store. This module owns writer locking,
 * strict ref-file parsing, compare-and-swap publication for commit writers,
 * force updates for operator refs, tag deletion, and local reflog appends.
 */
#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>

/*
 * Acquires the repository writer lock with nonblocking flock(). The lock file
 * contents are diagnostic only; the open file descriptor and flock state are
 * the actual mutual exclusion mechanism.
 */
scribe_error_t scribe_lock_repo(scribe_ctx *ctx) {
    char *path;
    char diag[256];
    int n;

    path = scribe_path_join(ctx->repo_path, "lock");
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    /*
     * The lock file serves two purposes. flock() on the open descriptor is the
     * actual exclusion mechanism; the file contents are only diagnostics so a
     * second writer can tell the operator which process currently owns the repository.
     */
    ctx->lock_fd = open(path, O_RDWR | O_CREAT, 0666);
    free(path);
    if (ctx->lock_fd < 0) {
        return scribe_set_error(SCRIBE_EIO, "failed to open lock file");
    }
    if (flock(ctx->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        uint8_t *holder = NULL;
        size_t holder_len = 0;
        path = scribe_path_join(ctx->repo_path, "lock");
        if (path != NULL && scribe_read_file(path, &holder, &holder_len) == SCRIBE_OK) {
            (void)holder_len;
            (void)scribe_set_error(SCRIBE_ELOCKED, "repository locked: %s", holder);
            free(holder);
            free(path);
            return SCRIBE_ELOCKED;
        }
        free(path);
        return scribe_set_error(SCRIBE_ELOCKED, "repository locked");
    }
    n = snprintf(diag, sizeof(diag), "pid %ld\nprogram scribe\n", (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(diag) || ftruncate(ctx->lock_fd, 0) != 0 ||
        write(ctx->lock_fd, diag, (size_t)n) != n || fsync(ctx->lock_fd) != 0) {
        return scribe_set_error(SCRIBE_EIO, "failed to write lock diagnostics");
    }
    return SCRIBE_OK;
}

/*
 * Releases the writer lock held by a context, if any. The lock file remains on
 * disk as a diagnostic artifact, but the flock is gone once the descriptor closes.
 */
void scribe_unlock_repo(scribe_ctx *ctx) {
    if (ctx != NULL && ctx->lock_fd >= 0) {
        (void)flock(ctx->lock_fd, LOCK_UN);
        close(ctx->lock_fd);
        ctx->lock_fd = -1;
    }
}

/*
 * Builds the filesystem path for a ref name under the repository root. The
 * caller owns the returned path.
 */
static char *ref_path(scribe_ctx *ctx, const char *name) { return scribe_path_join(ctx->repo_path, name); }

/*
 * Ref names are repository-relative paths below `.scribe/refs`. The CLI keeps
 * names conservative: slash-separated components, no empty components, no `..`,
 * no whitespace, and no atomic-write temporary marker. This keeps refs safe fo
 * direct filesystem storage and also keeps bundle entry headers parseable.
 */
static int ref_name_is_valid(const char *name) {
    const char *p;
    const char *component_start;
    int component_len = 0;

    if (name == NULL || strncmp(name, "refs/", 5) != 0 || name[5] == '\0' || name[0] == '/' ||
        strstr(name, ".tmp.") != NULL) {
        return 0;
    }
    component_start = name;
    for (p = name; *p != '\0'; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch <= 0x20u || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '[' || ch == ']' || ch == '{' ||
            ch == '}' || ch == '"' || ch == '\'') {
            return 0;
        }
        if (*p == '/') {
            if (component_len == 0 || (component_len == 2 && component_start[0] == '.' && component_start[1] == '.')) {
                return 0;
            }
            component_start = p + 1;
            component_len = 0;
        } else {
            component_len++;
        }
    }
    return component_len != 0 && !(component_len == 2 && component_start[0] == '.' && component_start[1] == '.');
}

/*
 * Relative branch/tag names use the same component rules as full refs but are
 * not allowed to smuggle an already-prefixed `refs/...` path into the shorthand
 * command namespace.
 */
static int ref_short_name_is_valid(const char *name) {
    char tmp[PATH_MAX];

    if (name == NULL || name[0] == '\0' || name[0] == '/' || strncmp(name, "refs/", 5) == 0) {
        return 0;
    }
    if (snprintf(tmp, sizeof(tmp), "refs/heads/%s", name) >= (int)sizeof(tmp)) {
        return 0;
    }
    return ref_name_is_valid(tmp);
}

/*
 * Builds `refs/<namespace>/<short-name>` for the branch and tag commands. The
 * returned string is heap-owned. Validation happens before allocation so the
 * command fails with SCRIBE_EINVAL instead of constructing unsafe paths.
 */
static scribe_error_t build_namespaced_ref(const char *prefix, const char *short_name, char **out) {
    size_t prefix_len;
    size_t name_len;
    char *ref;

    if (!ref_short_name_is_valid(short_name)) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid ref name '%s'", short_name == NULL ? "" : short_name);
    }
    prefix_len = strlen(prefix);
    name_len = strlen(short_name);
    if (prefix_len > SIZE_MAX - 2u || prefix_len + 1u > SIZE_MAX - name_len) {
        return scribe_set_error(SCRIBE_EPATH, "ref name too long");
    }
    ref = (char *)malloc(prefix_len + 1u + name_len + 1u);
    if (ref == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate ref name");
    }
    memcpy(ref, prefix, prefix_len);
    ref[prefix_len] = '/';
    memcpy(ref + prefix_len + 1u, short_name, name_len);
    ref[prefix_len + 1u + name_len] = '\0';
    *out = ref;
    return SCRIBE_OK;
}

/*
 * Creates the parent directory for a file path. Ref and reflog updates both use
 * nested paths such as refs/heads/feature/demo, so the leaf file's parent may
 * not exist yet.
 */
static scribe_error_t mkdir_parent_path(const char *path) {
    char *copy;
    char *slash;
    scribe_error_t err;

    copy = strdup(path);
    if (copy == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate parent path");
    }
    slash = strrchr(copy, '/');
    if (slash == NULL) {
        free(copy);
        return SCRIBE_OK;
    }
    if (slash == copy) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    err = scribe_mkdir_p(copy);
    free(copy);
    return err;
}

/*
 * fsyncs the directory containing a path after unlinking a ref. Atomic writes
 * already fsync their parent inside scribe_write_file_atomic(), but deletion
 * needs its own durability boundary.
 */
static scribe_error_t fsync_parent_dir(const char *path) {
    char *copy;
    char *slash;
    int fd;

    copy = strdup(path);
    if (copy == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate parent path");
    }
    slash = strrchr(copy, '/');
    if (slash == NULL) {
        strcpy(copy, ".");
    } else if (slash == copy) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    fd = open(copy, O_RDONLY | O_DIRECTORY);
    free(copy);
    if (fd < 0) {
        return scribe_set_error(SCRIBE_EIO, "failed to open parent directory for fsync");
    }
    if (fsync(fd) != 0) {
        close(fd);
        return scribe_set_error(SCRIBE_EIO, "failed to fsync parent directory");
    }
    close(fd);
    return SCRIBE_OK;
}

/*
 * Formats the current UTC time for local reflog entries. Reflogs are operato
 * metadata, not replicated object data, so wall-clock time is acceptable here.
 */
static void utc_now(char out[32]) {
    time_t now = time(NULL);
    struct tm tm_utc;

    if (gmtime_r(&now, &tm_utc) == NULL || strftime(out, 32u, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        strcpy(out, "1970-01-01T00:00:00Z");
    }
}

/*
 * Appends one local reflog line after a ref update has reached disk. The line
 * format is:
 *
 *   <iso8601-utc> <old-64-hex-or-zero> <new-64-hex-or-zero> <reason>
 *
 * A zero hash marks creation on the old side and deletion on the new side.
 */
static scribe_error_t append_reflog(scribe_ctx *ctx, const char *name, const uint8_t *old_hash, const uint8_t *new_hash,
                                    const char *reason) {
    static const uint8_t zero[SCRIBE_HASH_SIZE] = {0};
    char old_hex[SCRIBE_HEX_HASH_SIZE + 1];
    char new_hex[SCRIBE_HEX_HASH_SIZE + 1];
    char ts[32];
    char line[256];
    char *logs;
    char *path;
    int fd;
    int n;
    size_t off = 0;
    scribe_error_t err;

    logs = scribe_path_join(ctx->repo_path, "logs");
    if (logs == NULL) {
        return SCRIBE_ENOMEM;
    }
    path = scribe_path_join(logs, name);
    free(logs);
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = mkdir_parent_path(path);
    if (err != SCRIBE_OK) {
        free(path);
        return err;
    }
    utc_now(ts);
    scribe_hash_to_hex(old_hash == NULL ? zero : old_hash, old_hex);
    scribe_hash_to_hex(new_hash == NULL ? zero : new_hash, new_hex);
    n = snprintf(line, sizeof(line), "%s %s %s %s\n", ts, old_hex, new_hex,
                 reason == NULL || reason[0] == '\0' ? "update" : reason);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        free(path);
        return scribe_set_error(SCRIBE_EPATH, "reflog line too long");
    }
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) {
        free(path);
        return scribe_set_error(SCRIBE_EIO, "failed to open reflog");
    }
    while (off < (size_t)n) {
        ssize_t written = write(fd, line + off, (size_t)n - off);
        if (written < 0) {
            close(fd);
            free(path);
            return scribe_set_error(SCRIBE_EIO, "failed to write reflog");
        }
        off += (size_t)written;
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        free(path);
        return scribe_set_error(SCRIBE_EIO, "failed to sync reflog");
    }
    err = fsync_parent_dir(path);
    free(path);
    return err;
}

typedef struct {
    scribe_ctx *ctx;
    scribe_ref_visit_fn visit;
    void *user;
} ref_iter_ctx;

/*
 * Returns true for temporary ref files created by scribe_write_file_atomic().
 * Read-only scans can race with another process publishing a ref, and those
 * incomplete names are not part of the logical ref namespace.
 */
static int is_ref_temp_name(const char *name) { return strstr(name, ".tmp.") != NULL; }

/*
 * Recursively walks one directory under `.scribe/refs`. Each regular file is
 * parsed through scribe_refs_read(), so ref iteration gets the same strict
 * 64-hex validation as direct ref reads.
 */
static scribe_error_t refs_iter_dir(ref_iter_ctx *ctx, const char *abs_dir, const char *rel_dir) {
    DIR *dir;
    struct dirent *ent;

    dir = opendir(abs_dir);
    if (dir == NULL) {
        return errno == ENOENT ? SCRIBE_OK : scribe_set_error(SCRIBE_EIO, "failed to open refs directory");
    }
    while ((ent = readdir(dir)) != NULL) {
        char abs_child[PATH_MAX];
        char rel_child[PATH_MAX];
        struct stat st;
        scribe_error_t err;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0 || is_ref_temp_name(ent->d_name)) {
            continue;
        }
        if (snprintf(abs_child, sizeof(abs_child), "%s/%s", abs_dir, ent->d_name) >= (int)sizeof(abs_child) ||
            snprintf(rel_child, sizeof(rel_child), "%s/%s", rel_dir, ent->d_name) >= (int)sizeof(rel_child)) {
            closedir(dir);
            return scribe_set_error(SCRIBE_EPATH, "ref path too long");
        }
        if (stat(abs_child, &st) != 0) {
            closedir(dir);
            return scribe_set_error(SCRIBE_EIO, "failed to stat ref path");
        }
        if (S_ISDIR(st.st_mode)) {
            err = refs_iter_dir(ctx, abs_child, rel_child);
        } else if (S_ISREG(st.st_mode)) {
            uint8_t hash[SCRIBE_HASH_SIZE];
            err = scribe_refs_read(ctx->ctx, rel_child, hash);
            if (err == SCRIBE_OK) {
                err = ctx->visit(rel_child, hash, ctx->user);
            }
        } else {
            err = SCRIBE_OK;
        }
        if (err != SCRIBE_OK) {
            closedir(dir);
            return err;
        }
    }
    closedir(dir);
    return SCRIBE_OK;
}

/*
 * Reads a direct hash ref from disk and parses its canonical 64-hex-plus-newline
 * format. Malformed refs are reported as corruption because they break history.
 */
scribe_error_t scribe_refs_read(scribe_ctx *ctx, const char *name, uint8_t out[SCRIBE_HASH_SIZE]) {
    char *path;
    uint8_t *bytes = NULL;
    size_t len = 0;
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    scribe_error_t err;

    if (!ref_name_is_valid(name)) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid ref name '%s'", name == NULL ? "" : name);
    }
    path = ref_path(ctx, name);
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = scribe_read_file(path, &bytes, &len);
    free(path);
    if (err != SCRIBE_OK) {
        return err;
    }
    /*
     * Refs are intentionally tiny text files: exactly 64 lowercase hex
     * characters plus newline. HEAD is a symbolic file in v1, but actual refs
     * such as refs/heads/main use this direct hash format.
     */
    if (len != SCRIBE_HEX_HASH_SIZE + 1u || bytes[SCRIBE_HEX_HASH_SIZE] != '\n') {
        free(bytes);
        return scribe_set_error(SCRIBE_ECORRUPT, "malformed ref '%s'", name);
    }
    memcpy(hex, bytes, SCRIBE_HEX_HASH_SIZE);
    hex[SCRIBE_HEX_HASH_SIZE] = '\0';
    free(bytes);
    return scribe_hash_from_hex(hex, out);
}

/*
 * Atomically publishes a new ref value if the current value still matches the
 * expected parent hash. Passing expected == NULL means the ref must not exist,
 * which is how the first commit creates refs/heads/main.
 */
scribe_error_t scribe_refs_cas(scribe_ctx *ctx, const char *name, const uint8_t *expected,
                               const uint8_t new_hash[SCRIBE_HASH_SIZE]) {
    uint8_t current[SCRIBE_HASH_SIZE];
    char hex[SCRIBE_HEX_HASH_SIZE + 2];
    char *path;
    scribe_error_t err;
    int have_current = 0;

    /*
     * Compare-and-swap protects the single main ref from accidental overwrite.
     * Writers first remember the parent hash they read, then publish only if the
     * ref still has that value. In normal v1 operation the repository lock means
     * this should succeed, but the CAS also protects against future embedding o
     * lock misuse.
     */
    if (!ref_name_is_valid(name)) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid ref name '%s'", name == NULL ? "" : name);
    }
    err = scribe_refs_read(ctx, name, current);
    if (err == SCRIBE_ENOT_FOUND) {
        if (expected != NULL) {
            return scribe_set_error(SCRIBE_EREF_STALE, "ref '%s' does not exist", name);
        }
    } else if (err != SCRIBE_OK) {
        return err;
    } else {
        have_current = 1;
        if (expected == NULL || scribe_hash_cmp(current, expected) != 0) {
            return scribe_set_error(SCRIBE_EREF_STALE, "ref '%s' changed", name);
        }
    }

    path = ref_path(ctx, name);
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = mkdir_parent_path(path);
    if (err != SCRIBE_OK) {
        free(path);
        return err;
    }
    scribe_hash_to_hex(new_hash, hex);
    hex[SCRIBE_HEX_HASH_SIZE] = '\n';
    hex[SCRIBE_HEX_HASH_SIZE + 1u] = '\0';
    err = scribe_write_file_atomic(path, (const uint8_t *)hex, SCRIBE_HEX_HASH_SIZE + 1u);
    free(path);
    if (err != SCRIBE_OK) {
        return err;
    }
    return append_reflog(ctx, name, have_current ? current : NULL, new_hash, "update");
}

/*
 * Force-updates a ref to a commit hash and records the local reflog entry. This
 * is used by operator-managed refs such as branches and by bundle import, where
 * the caller has already decided the move is allowed.
 */
scribe_error_t scribe_refs_update(scribe_ctx *ctx, const char *name, const uint8_t new_hash[SCRIBE_HASH_SIZE],
                                  const char *reason) {
    uint8_t current[SCRIBE_HASH_SIZE];
    char hex[SCRIBE_HEX_HASH_SIZE + 2];
    char *path;
    scribe_error_t err;
    int have_current = 0;

    if (!ref_name_is_valid(name)) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid ref name '%s'", name == NULL ? "" : name);
    }
    err = scribe_refs_read(ctx, name, current);
    if (err == SCRIBE_OK) {
        have_current = 1;
    } else if (err != SCRIBE_ENOT_FOUND) {
        return err;
    }
    path = ref_path(ctx, name);
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = mkdir_parent_path(path);
    if (err != SCRIBE_OK) {
        free(path);
        return err;
    }
    scribe_hash_to_hex(new_hash, hex);
    hex[SCRIBE_HEX_HASH_SIZE] = '\n';
    hex[SCRIBE_HEX_HASH_SIZE + 1u] = '\0';
    err = scribe_write_file_atomic(path, (const uint8_t *)hex, SCRIBE_HEX_HASH_SIZE + 1u);
    free(path);
    if (err != SCRIBE_OK) {
        return err;
    }
    return append_reflog(ctx, name, have_current ? current : NULL, new_hash, reason);
}

/*
 * Deletes a direct ref and records a reflog line whose new hash is all zeros.
 * Tags use this path; branches currently move but are not deleted by M4.
 */
scribe_error_t scribe_refs_delete(scribe_ctx *ctx, const char *name, int missing_ok, const char *reason) {
    uint8_t current[SCRIBE_HASH_SIZE];
    char *path;
    scribe_error_t err;

    if (!ref_name_is_valid(name)) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid ref name '%s'", name == NULL ? "" : name);
    }
    err = scribe_refs_read(ctx, name, current);
    if (err == SCRIBE_ENOT_FOUND && missing_ok) {
        return SCRIBE_OK;
    }
    if (err != SCRIBE_OK) {
        return err;
    }
    path = ref_path(ctx, name);
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    if (unlink(path) != 0) {
        free(path);
        return scribe_set_error(SCRIBE_EIO, "failed to delete ref '%s'", name);
    }
    err = fsync_parent_dir(path);
    free(path);
    if (err != SCRIBE_OK) {
        return err;
    }
    return append_reflog(ctx, name, current, NULL, reason);
}

/*
 * Resolves a user-supplied ref name to a direct ref hash. Full refs such as
 * refs/tags/v1 are read literally; shorthand first checks refs/heads/<name>,
 * then refs/tags/<name>. Invalid shorthand returns ENOT_FOUND so path-oriented
 * commands can continue treating the argument as a Scribe path.
 */
scribe_error_t scribe_refs_resolve_name(scribe_ctx *ctx, const char *name, uint8_t out[SCRIBE_HASH_SIZE]) {
    char *ref = NULL;
    scribe_error_t err;

    if (name == NULL || name[0] == '\0') {
        return scribe_set_error(SCRIBE_ENOT_FOUND, "ref not found");
    }
    if (strncmp(name, "refs/", 5) == 0) {
        return scribe_refs_read(ctx, name, out);
    }
    err = build_namespaced_ref("refs/heads", name, &ref);
    if (err == SCRIBE_OK) {
        err = scribe_refs_read(ctx, ref, out);
        free(ref);
        if (err != SCRIBE_ENOT_FOUND) {
            return err;
        }
    } else if (err != SCRIBE_EINVAL) {
        return err;
    }
    ref = NULL;
    err = build_namespaced_ref("refs/tags", name, &ref);
    if (err == SCRIBE_OK) {
        err = scribe_refs_read(ctx, ref, out);
        free(ref);
        return err;
    }
    return err == SCRIBE_EINVAL ? scribe_set_error(SCRIBE_ENOT_FOUND, "ref '%s' not found", name) : err;
}

/*
 * Confirms that a resolved hash names a commit object. Branches and tags are
 * commit refs, not arbitrary object refs, so this protects users from pinning a
 * tree/blob hash by mistake.
 */
static scribe_error_t ensure_commit_object(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    scribe_object obj;
    scribe_error_t err = scribe_object_read(ctx, hash, &obj);

    if (err != SCRIBE_OK) {
        return err;
    }
    if (obj.type != SCRIBE_OBJECT_COMMIT) {
        scribe_object_free(&obj);
        return scribe_set_error(SCRIBE_EINVAL, "ref target is not a commit");
    }
    scribe_object_free(&obj);
    return SCRIBE_OK;
}

/*
 * Creates or moves a branch. Branches are intentionally movable operato
 * markers; moving one appends a reflog line so the local movement is auditable.
 */
scribe_error_t scribe_cli_branch(scribe_ctx *ctx, const char *name, const char *commit) {
    char *ref = NULL;
    uint8_t hash[SCRIBE_HASH_SIZE];
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    scribe_error_t err;

    if (name == NULL || name[0] == '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "branch name is required");
    }
    err = build_namespaced_ref("refs/heads", name, &ref);
    if (err == SCRIBE_OK) {
        err = scribe_resolve_commit(ctx, commit == NULL ? "HEAD" : commit, hash);
    }
    if (err == SCRIBE_OK) {
        err = ensure_commit_object(ctx, hash);
    }
    if (err == SCRIBE_OK) {
        err = scribe_refs_update(ctx, ref, hash, "branch");
    }
    if (err == SCRIBE_OK) {
        scribe_hash_to_hex(hash, hex);
        printf("branch %s %s\n", name, hex);
    }
    free(ref);
    return err;
}

/*
 * Implements immutable tag creation, forced tag movement, and tag deletion.
 * The normal create path rejects existing tags; --force is required to move a
 * tag because tags are meant to be stable release/audit markers.
 */
scribe_error_t scribe_cli_tag(scribe_ctx *ctx, int delete_mode, int force, const char *name, const char *commit) {
    char *ref = NULL;
    uint8_t hash[SCRIBE_HASH_SIZE];
    uint8_t existing[SCRIBE_HASH_SIZE];
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    scribe_error_t err;

    if (name == NULL || name[0] == '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "tag name is required");
    }
    err = build_namespaced_ref("refs/tags", name, &ref);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (delete_mode) {
        if (commit != NULL) {
            free(ref);
            return scribe_set_error(SCRIBE_EINVAL, "tag delete does not accept a commit argument");
        }
        err = scribe_refs_delete(ctx, ref, force, "tag-delete");
        if (err == SCRIBE_OK) {
            printf("deleted tag %s\n", name);
        }
        free(ref);
        return err;
    }
    if (commit == NULL || commit[0] == '\0') {
        free(ref);
        return scribe_set_error(SCRIBE_EINVAL, "tag target commit is required");
    }
    err = scribe_refs_read(ctx, ref, existing);
    if (err == SCRIBE_OK && !force) {
        free(ref);
        return scribe_set_error(SCRIBE_EEXISTS, "tag '%s' already exists; use --force to move it", name);
    }
    if (err != SCRIBE_OK && err != SCRIBE_ENOT_FOUND) {
        free(ref);
        return err;
    }
    err = scribe_resolve_commit(ctx, commit, hash);
    if (err == SCRIBE_OK) {
        err = ensure_commit_object(ctx, hash);
    }
    if (err == SCRIBE_OK) {
        err = scribe_refs_update(ctx, ref, hash, force ? "tag-force" : "tag");
    }
    if (err == SCRIBE_OK) {
        scribe_hash_to_hex(hash, hex);
        printf("tag %s %s\n", name, hex);
    }
    free(ref);
    return err;
}

/*
 * Builds the reflog path for a full ref name. This helper deliberately does not
 * validate the ref; callers validate before calling so diagnostics mention the
 * user-facing argument.
 */
static char *reflog_path(scribe_ctx *ctx, const char *ref) {
    char *logs = scribe_path_join(ctx->repo_path, "logs");
    char *path;

    if (logs == NULL) {
        return NULL;
    }
    path = scribe_path_join(logs, ref);
    free(logs);
    return path;
}

/*
 * Resolves a reflog argument. Existing refs are preferred, but deleted tags can
 * still be inspected by checking for an existing log file after the ref file is
 * gone.
 */
static scribe_error_t resolve_reflog_ref(scribe_ctx *ctx, const char *arg, char **out) {
    uint8_t ignored[SCRIBE_HASH_SIZE];
    char *candidate = NULL;
    char *path = NULL;
    scribe_error_t err;

    if (arg == NULL || arg[0] == '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "reflog ref is required");
    }
    if (strcmp(arg, "HEAD") == 0) {
        candidate = strdup("refs/heads/main");
    } else if (strncmp(arg, "refs/", 5) == 0) {
        if (!ref_name_is_valid(arg)) {
            return scribe_set_error(SCRIBE_EINVAL, "invalid ref name '%s'", arg);
        }
        candidate = strdup(arg);
    } else {
        err = build_namespaced_ref("refs/heads", arg, &candidate);
        if (err == SCRIBE_OK) {
            err = scribe_refs_read(ctx, candidate, ignored);
            path = reflog_path(ctx, candidate);
            if (err == SCRIBE_OK || (path != NULL && scribe_file_exists(path))) {
                free(path);
                *out = candidate;
                return SCRIBE_OK;
            }
            free(path);
            free(candidate);
            candidate = NULL;
        }
        err = build_namespaced_ref("refs/tags", arg, &candidate);
        if (err != SCRIBE_OK) {
            return err == SCRIBE_EINVAL ? scribe_set_error(SCRIBE_ENOT_FOUND, "reflog '%s' not found", arg) : err;
        }
    }
    if (candidate == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate reflog ref");
    }
    path = reflog_path(ctx, candidate);
    if (path == NULL) {
        free(candidate);
        return SCRIBE_ENOMEM;
    }
    if (scribe_refs_read(ctx, candidate, ignored) == SCRIBE_OK || scribe_file_exists(path)) {
        free(path);
        *out = candidate;
        return SCRIBE_OK;
    }
    free(path);
    free(candidate);
    return scribe_set_error(SCRIBE_ENOT_FOUND, "reflog '%s' not found", arg);
}

/*
 * Prints the local reflog for one ref exactly as stored. Reflogs are not object
 * data and are not included in bundles; this is a local audit trail only.
 */
scribe_error_t scribe_cli_reflog(scribe_ctx *ctx, const char *ref) {
    char *full_ref = NULL;
    char *path = NULL;
    uint8_t *bytes = NULL;
    size_t len = 0;
    scribe_error_t err = resolve_reflog_ref(ctx, ref, &full_ref);

    if (err != SCRIBE_OK) {
        return err;
    }
    path = reflog_path(ctx, full_ref);
    free(full_ref);
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = scribe_read_file(path, &bytes, &len);
    free(path);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (len != 0u && fwrite(bytes, 1u, len, stdout) != len) {
        free(bytes);
        return scribe_set_error(SCRIBE_EIO, "failed to write reflog output");
    }
    free(bytes);
    return SCRIBE_OK;
}

/*
 * Iterates every direct hash ref under `.scribe/refs`. v1 normally has only
 * refs/heads/main, but gc uses this API so M4 branches and tags become reachable
 * roots automatically once they are introduced.
 */
scribe_error_t scribe_refs_iter(scribe_ctx *ctx, scribe_ref_visit_fn visit, void *user) {
    ref_iter_ctx ictx;
    char *refs;
    scribe_error_t err;

    if (ctx == NULL || visit == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid refs iterator arguments");
    }
    refs = scribe_path_join(ctx->repo_path, "refs");
    if (refs == NULL) {
        return SCRIBE_ENOMEM;
    }
    ictx.ctx = ctx;
    ictx.visit = visit;
    ictx.user = user;
    err = refs_iter_dir(&ictx, refs, "refs");
    free(refs);
    return err;
}
