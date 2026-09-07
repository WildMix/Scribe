/*
 * Repository lifecycle management.
 *
 * This file owns initialization of the `.scribe/` directory skeleton and the
 * open/close path shared by every command. Opening a context loads config,
 * configures logging, and optionally acquires the writer lock before highe
 * layers touch refs or objects.
 */
#include "core/internal.h"

#include "util/error.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>

/*
 * Creates a new repository skeleton at path. The function writes HEAD, config,
 * log, objects/, packs/, refs/heads/, logs/refs/heads/, remotes/, and
 * adapter-state/, but deliberately leaves refs/heads/main absent until the
 * first commit publishes history.
 */
scribe_error_t scribe_init_repository(const char *path) {
    scribe_config cfg;
    char *objects;
    char *packs;
    char *refs;
    char *logs;
    char *remotes;
    char *adapter_state;
    char *head;
    char *log_path;
    scribe_error_t err;

    if (path == NULL || path[0] == '\0') {
        return scribe_set_error(SCRIBE_EPATH, "empty repository path");
    }
    /*
     * init creates only the repository skeleton. refs/heads/main is deliberately
     * absent until the first commit, so an empty repository can be distinguished
     * from a repository whose main ref points at a commit.
     */
    err = scribe_default_config(&cfg);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_mkdir_p(path);
    if (err != SCRIBE_OK) {
        return err;
    }
    objects = scribe_path_join(path, "objects");
    packs = scribe_path_join(path, "packs");
    refs = scribe_path_join(path, "refs/heads");
    logs = scribe_path_join(path, "logs/refs/heads");
    remotes = scribe_path_join(path, "remotes");
    adapter_state = scribe_path_join(path, "adapter-state");
    head = scribe_path_join(path, "HEAD");
    log_path = scribe_path_join(path, "log");
    if (objects == NULL || packs == NULL || refs == NULL || logs == NULL || remotes == NULL || adapter_state == NULL ||
        head == NULL || log_path == NULL) {
        free(objects);
        free(packs);
        free(refs);
        free(logs);
        free(remotes);
        free(adapter_state);
        free(head);
        free(log_path);
        return SCRIBE_ENOMEM;
    }
    if ((err = scribe_mkdir_p(objects)) != SCRIBE_OK || (err = scribe_mkdir_p(packs)) != SCRIBE_OK ||
        (err = scribe_mkdir_p(refs)) != SCRIBE_OK || (err = scribe_mkdir_p(logs)) != SCRIBE_OK ||
        (err = scribe_mkdir_p(remotes)) != SCRIBE_OK || (err = scribe_mkdir_p(adapter_state)) != SCRIBE_OK ||
        (err = scribe_write_config(path, &cfg)) != SCRIBE_OK ||
        (err = scribe_write_file_atomic(head, (const uint8_t *)"ref: refs/heads/main\n", 21u)) != SCRIBE_OK ||
        (err = scribe_write_file_atomic(log_path, (const uint8_t *)"", 0u)) != SCRIBE_OK) {
        free(objects);
        free(packs);
        free(refs);
        free(logs);
        free(remotes);
        free(adapter_state);
        free(head);
        free(log_path);
        return err;
    }
    free(objects);
    free(packs);
    free(refs);
    free(logs);
    free(remotes);
    free(adapter_state);
    free(head);
    free(log_path);
    return SCRIBE_OK;
}

/*
 * Opens an existing repository context. Writable opens take the repository lock
 * before returning; read-only opens skip the lock because immutable objects and
 * atomic refs make concurrent inspection safe.
 */
scribe_error_t scribe_open(const char *path, int writable, scribe_ctx **out) {
    scribe_ctx *ctx;
    scribe_error_t err;

    if (path == NULL || out == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid open arguments");
    }
    /*
     * Open centralizes per-command setup: read SCRIBE_LOG_LEVEL, allocate the
     * context, parse config, optionally acquire the writer lock, and open the
     * operational log. Read-only commands skip the lock because objects are
     * immutable and refs are replaced atomically.
     */
    err = scribe_log_configure_from_env();
    if (err != SCRIBE_OK) {
        return err;
    }
    ctx = (scribe_ctx *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate context");
    }
    ctx->writable = writable;
    ctx->lock_fd = -1;
    err = scribe_read_config(path, &ctx->config);
    if (err != SCRIBE_OK) {
        scribe_close(ctx);
        return err;
    }
    ctx->repo_path = realpath(path, NULL);
    if (ctx->repo_path == NULL) {
        ctx->repo_path = strdup(path);
    }
    if (ctx->repo_path == NULL) {
        scribe_close(ctx);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate repository path");
    }
    if (writable) {
        err = scribe_lock_repo(ctx);
        if (err != SCRIBE_OK) {
            scribe_close(ctx);
            return err;
        }
    }
    err = scribe_log_open(ctx);
    if (err != SCRIBE_OK) {
        scribe_close(ctx);
        return err;
    }
    *out = ctx;
    return SCRIBE_OK;
}

/*
 * Releases every resource owned by a context: log file, lock, repository path,
 * and the context allocation itself. It accepts NULL so cleanup paths can call
 * it after partial-open failures.
 */
void scribe_close(scribe_ctx *ctx) {
    if (ctx == NULL) {
        return;
    }
    scribe_log_close(ctx);
    scribe_unlock_repo(ctx);
    free(ctx->repo_path);
    free(ctx);
}
