#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

scribe_error_t scribe_lock_repo(scribe_ctx *ctx) {
    char *path;
    char diag[256];
    int n;

    path = scribe_path_join(ctx->repo_path, "lock");
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
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

void scribe_unlock_repo(scribe_ctx *ctx) {
    if (ctx != NULL && ctx->lock_fd >= 0) {
        (void)flock(ctx->lock_fd, LOCK_UN);
        close(ctx->lock_fd);
        ctx->lock_fd = -1;
    }
}

static char *ref_path(scribe_ctx *ctx, const char *name) { return scribe_path_join(ctx->repo_path, name); }

scribe_error_t scribe_refs_read(scribe_ctx *ctx, const char *name, uint8_t out[SCRIBE_HASH_SIZE]) {
    char *path;
    uint8_t *bytes = NULL;
    size_t len = 0;
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    scribe_error_t err;

    path = ref_path(ctx, name);
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = scribe_read_file(path, &bytes, &len);
    free(path);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (len != SCRIBE_HEX_HASH_SIZE + 1u || bytes[SCRIBE_HEX_HASH_SIZE] != '\n') {
        free(bytes);
        return scribe_set_error(SCRIBE_ECORRUPT, "malformed ref '%s'", name);
    }
    memcpy(hex, bytes, SCRIBE_HEX_HASH_SIZE);
    hex[SCRIBE_HEX_HASH_SIZE] = '\0';
    free(bytes);
    return scribe_hash_from_hex(hex, out);
}

scribe_error_t scribe_refs_cas(scribe_ctx *ctx, const char *name, const uint8_t *expected,
                               const uint8_t new_hash[SCRIBE_HASH_SIZE]) {
    uint8_t current[SCRIBE_HASH_SIZE];
    char hex[SCRIBE_HEX_HASH_SIZE + 2];
    char *path;
    scribe_error_t err;

    err = scribe_refs_read(ctx, name, current);
    if (err == SCRIBE_ENOT_FOUND) {
        if (expected != NULL) {
            return scribe_set_error(SCRIBE_EREF_STALE, "ref '%s' does not exist", name);
        }
    } else if (err != SCRIBE_OK) {
        return err;
    } else {
        if (expected == NULL || scribe_hash_cmp(current, expected) != 0) {
            return scribe_set_error(SCRIBE_EREF_STALE, "ref '%s' changed", name);
        }
    }

    path = ref_path(ctx, name);
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    scribe_hash_to_hex(new_hash, hex);
    hex[SCRIBE_HEX_HASH_SIZE] = '\n';
    hex[SCRIBE_HEX_HASH_SIZE + 1u] = '\0';
    err = scribe_write_file_atomic(path, (const uint8_t *)hex, SCRIBE_HEX_HASH_SIZE + 1u);
    free(path);
    return err;
}
