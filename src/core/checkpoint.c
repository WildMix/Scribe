/*
 * Server-owned adapter checkpoint persistence.
 *
 * Adapter checkpoints live under .scribe/adapter-state/, but only the Scribe
 * server writes them. Each checkpoint records the commit that made the resume
 * state safe, followed by adapter-owned opaque bytes.
 */
#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checkpoint_component_valid(const char *value) {
    size_t i;

    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    for (i = 0u; value[i] != '\0'; i++) {
        char ch = value[i];

        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
              ch == '_' || ch == '.')) {
            return 0;
        }
    }
    return 1;
}

static scribe_error_t checkpoint_paths(scribe_ctx *ctx, const char *adapter, const char *key, int create_dir,
                                       char **out_dir, char **out_file) {
    char *root = NULL;
    char *dir = NULL;
    char *file = NULL;
    scribe_error_t err = SCRIBE_OK;

    if (ctx == NULL || !checkpoint_component_valid(adapter) || !checkpoint_component_valid(key) || out_dir == NULL ||
        out_file == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid checkpoint name");
    }
    *out_dir = NULL;
    *out_file = NULL;
    root = scribe_path_join(ctx->repo_path, "adapter-state");
    if (root == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate checkpoint path");
    }
    dir = scribe_path_join(root, adapter);
    if (dir == NULL) {
        free(root);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate checkpoint path");
    }
    if (create_dir) {
        err = scribe_mkdir_p(dir);
    }
    if (err == SCRIBE_OK) {
        file = scribe_path_join(dir, key);
        if (file == NULL) {
            err = scribe_set_error(SCRIBE_ENOMEM, "failed to allocate checkpoint path");
        }
    }
    free(root);
    if (err != SCRIBE_OK) {
        free(dir);
        free(file);
        return err;
    }
    *out_dir = dir;
    *out_file = file;
    return SCRIBE_OK;
}

scribe_error_t scribe_checkpoint_write(scribe_ctx *ctx, const char *adapter, const char *key, const uint8_t *value,
                                       size_t value_len, const uint8_t commit_hash[SCRIBE_HASH_SIZE]) {
    char commit_hex[SCRIBE_HEX_HASH_SIZE + 1];
    char header[96];
    int header_len;
    uint8_t *body = NULL;
    char *dir = NULL;
    char *file = NULL;
    scribe_error_t err;

    if (ctx == NULL || (value == NULL && value_len != 0u) || commit_hash == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid checkpoint write");
    }
    err = checkpoint_paths(ctx, adapter, key, 1, &dir, &file);
    if (err != SCRIBE_OK) {
        return err;
    }
    scribe_hash_to_hex(commit_hash, commit_hex);
    header_len = snprintf(header, sizeof(header), "version 1\ncommit %s\n\n", commit_hex);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        free(dir);
        free(file);
        return scribe_set_error(SCRIBE_EIO, "failed to format checkpoint");
    }
    body = (uint8_t *)malloc((size_t)header_len + value_len);
    if (body == NULL) {
        free(dir);
        free(file);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate checkpoint");
    }
    memcpy(body, header, (size_t)header_len);
    if (value_len != 0u) {
        memcpy(body + (size_t)header_len, value, value_len);
    }
    err = scribe_write_file_atomic(file, body, (size_t)header_len + value_len);
    if (err == SCRIBE_OK) {
        err = scribe_fsync_dir(dir);
    }
    free(body);
    free(dir);
    free(file);
    return err;
}

scribe_error_t scribe_checkpoint_read(scribe_ctx *ctx, const char *adapter, const char *key, uint8_t **out_value,
                                      size_t *out_value_len, char out_commit_hex[SCRIBE_HEX_HASH_SIZE + 1]) {
    uint8_t *bytes = NULL;
    size_t len = 0u;
    const char *commit_prefix = "commit ";
    uint8_t parsed[SCRIBE_HASH_SIZE];
    char *dir = NULL;
    char *file = NULL;
    char *cursor;
    char *value;
    scribe_error_t err;

    if (out_value == NULL || out_value_len == NULL || out_commit_hex == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid checkpoint read");
    }
    *out_value = NULL;
    *out_value_len = 0u;
    out_commit_hex[0] = '\0';
    err = checkpoint_paths(ctx, adapter, key, 0, &dir, &file);
    free(dir);
    if (err != SCRIBE_OK) {
        free(file);
        return err;
    }
    err = scribe_read_file(file, &bytes, &len);
    free(file);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (len < strlen("version 1\ncommit ") + SCRIBE_HEX_HASH_SIZE + strlen("\n\n") ||
        memcmp(bytes, "version 1\n", strlen("version 1\n")) != 0) {
        free(bytes);
        return scribe_set_error(SCRIBE_EMALFORMED, "malformed checkpoint");
    }
    cursor = (char *)bytes + strlen("version 1\n");
    if (memcmp(cursor, commit_prefix, strlen(commit_prefix)) != 0) {
        free(bytes);
        return scribe_set_error(SCRIBE_EMALFORMED, "malformed checkpoint commit");
    }
    cursor += strlen(commit_prefix);
    memcpy(out_commit_hex, cursor, SCRIBE_HEX_HASH_SIZE);
    out_commit_hex[SCRIBE_HEX_HASH_SIZE] = '\0';
    if (scribe_hash_from_hex(out_commit_hex, parsed) != SCRIBE_OK) {
        free(bytes);
        return scribe_set_error(SCRIBE_EMALFORMED, "malformed checkpoint commit hash");
    }
    cursor += SCRIBE_HEX_HASH_SIZE;
    if (cursor[0] != '\n' || cursor[1] != '\n') {
        free(bytes);
        return scribe_set_error(SCRIBE_EMALFORMED, "malformed checkpoint body separator");
    }
    cursor += 2;
    *out_value_len = len - (size_t)((uint8_t *)cursor - bytes);
    value = (char *)malloc(*out_value_len + 1u);
    if (value == NULL) {
        free(bytes);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate checkpoint value");
    }
    if (*out_value_len != 0u) {
        memcpy(value, cursor, *out_value_len);
    }
    value[*out_value_len] = '\0';
    free(bytes);
    *out_value = (uint8_t *)value;
    return SCRIBE_OK;
}
