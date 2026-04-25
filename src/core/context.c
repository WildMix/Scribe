#include "core/internal.h"

#include "util/error.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>

scribe_error_t scribe_init_repository(const char *path) {
    scribe_config cfg;
    char *objects;
    char *refs;
    char *adapter_state;
    char *head;
    char *log_path;
    scribe_error_t err;

    if (path == NULL || path[0] == '\0') {
        return scribe_set_error(SCRIBE_EPATH, "empty repository path");
    }
    err = scribe_default_config(&cfg);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_mkdir_p(path);
    if (err != SCRIBE_OK) {
        return err;
    }
    objects = scribe_path_join(path, "objects");
    refs = scribe_path_join(path, "refs/heads");
    adapter_state = scribe_path_join(path, "adapter-state");
    head = scribe_path_join(path, "HEAD");
    log_path = scribe_path_join(path, "log");
    if (objects == NULL || refs == NULL || adapter_state == NULL || head == NULL || log_path == NULL) {
        free(objects);
        free(refs);
        free(adapter_state);
        free(head);
        free(log_path);
        return SCRIBE_ENOMEM;
    }
    if ((err = scribe_mkdir_p(objects)) != SCRIBE_OK || (err = scribe_mkdir_p(refs)) != SCRIBE_OK ||
        (err = scribe_mkdir_p(adapter_state)) != SCRIBE_OK || (err = scribe_write_config(path, &cfg)) != SCRIBE_OK ||
        (err = scribe_write_file_atomic(head, (const uint8_t *)"ref: refs/heads/main\n", 21u)) != SCRIBE_OK ||
        (err = scribe_write_file_atomic(log_path, (const uint8_t *)"", 0u)) != SCRIBE_OK) {
        free(objects);
        free(refs);
        free(adapter_state);
        free(head);
        free(log_path);
        return err;
    }
    free(objects);
    free(refs);
    free(adapter_state);
    free(head);
    free(log_path);
    return SCRIBE_OK;
}

scribe_error_t scribe_open(const char *path, int writable, scribe_ctx **out) {
    scribe_ctx *ctx;
    scribe_error_t err;

    if (path == NULL || out == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid open arguments");
    }
    err = scribe_log_configure_from_env();
    if (err != SCRIBE_OK) {
        return err;
    }
    ctx = (scribe_ctx *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate context");
    }
    ctx->repo_path = strdup(path);
    ctx->writable = writable;
    ctx->lock_fd = -1;
    if (ctx->repo_path == NULL) {
        free(ctx);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate repository path");
    }
    err = scribe_read_config(path, &ctx->config);
    if (err != SCRIBE_OK) {
        scribe_close(ctx);
        return err;
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
    scribe_log_msg(ctx, SCRIBE_LOG_DEBUG, "repo", "opened store");
    *out = ctx;
    return SCRIBE_OK;
}

void scribe_close(scribe_ctx *ctx) {
    if (ctx == NULL) {
        return;
    }
    scribe_log_close(ctx);
    scribe_unlock_repo(ctx);
    free(ctx->repo_path);
    free(ctx);
}
