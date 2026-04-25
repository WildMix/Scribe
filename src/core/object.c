#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"
#include "util/leb128.h"

#include "blake3.h"
#include "zstd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void hash_bytes(const uint8_t *bytes, size_t len, uint8_t out[SCRIBE_HASH_SIZE]) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, bytes, len);
    blake3_hasher_finalize(&hasher, out, SCRIBE_HASH_SIZE);
}

static scribe_error_t build_envelope(uint8_t type, const uint8_t *payload, size_t payload_len, uint8_t **out,
                                     size_t *out_len) {
    uint8_t leb[10];
    size_t leb_len;
    uint8_t *buf;

    leb_len = scribe_leb128_encode((uint64_t)payload_len, leb);
    buf = (uint8_t *)malloc(1u + leb_len + payload_len);
    if (buf == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate object envelope");
    }
    buf[0] = type;
    memcpy(buf + 1u, leb, leb_len);
    if (payload_len != 0) {
        memcpy(buf + 1u + leb_len, payload, payload_len);
    }
    *out = buf;
    *out_len = 1u + leb_len + payload_len;
    return SCRIBE_OK;
}

char *scribe_object_path(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    char dirpart[16];
    char *objects;
    char *dir;
    char *path;

    scribe_hash_to_hex(hash, hex);
    objects = scribe_path_join(ctx->repo_path, "objects");
    if (objects == NULL) {
        return NULL;
    }
    snprintf(dirpart, sizeof(dirpart), "%.2s", hex);
    dir = scribe_path_join(objects, dirpart);
    free(objects);
    if (dir == NULL) {
        return NULL;
    }
    path = scribe_path_join(dir, hex + 2);
    free(dir);
    return path;
}

scribe_error_t scribe_object_has(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    char *path = scribe_object_path(ctx, hash);
    int exists;

    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    exists = scribe_file_exists(path);
    free(path);
    return exists ? SCRIBE_OK : scribe_set_error(SCRIBE_ENOT_FOUND, "object not found");
}

scribe_error_t scribe_object_write(scribe_ctx *ctx, uint8_t type, const uint8_t *payload, size_t payload_len,
                                   uint8_t out_hash[SCRIBE_HASH_SIZE]) {
    uint8_t *envelope = NULL;
    size_t envelope_len = 0;
    size_t bound;
    size_t compressed_len;
    uint8_t *compressed;
    char *path;
    char *objects;
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    char dirpart[16];
    char *dir;
    scribe_error_t err;

    if (payload == NULL && payload_len != 0) {
        return scribe_set_error(SCRIBE_EINVAL, "payload is NULL with non-zero length");
    }
    err = build_envelope(type, payload, payload_len, &envelope, &envelope_len);
    if (err != SCRIBE_OK) {
        return err;
    }
    hash_bytes(envelope, envelope_len, out_hash);
    path = scribe_object_path(ctx, out_hash);
    if (path == NULL) {
        free(envelope);
        return SCRIBE_ENOMEM;
    }
    if (scribe_file_exists(path)) {
        free(path);
        free(envelope);
        return SCRIBE_OK;
    }

    scribe_hash_to_hex(out_hash, hex);
    objects = scribe_path_join(ctx->repo_path, "objects");
    if (objects == NULL) {
        free(path);
        free(envelope);
        return SCRIBE_ENOMEM;
    }
    snprintf(dirpart, sizeof(dirpart), "%.2s", hex);
    dir = scribe_path_join(objects, dirpart);
    free(objects);
    if (dir == NULL) {
        free(path);
        free(envelope);
        return SCRIBE_ENOMEM;
    }
    err = scribe_mkdir_p(dir);
    free(dir);
    if (err != SCRIBE_OK) {
        free(path);
        free(envelope);
        return err;
    }

    bound = ZSTD_compressBound(envelope_len);
    compressed = (uint8_t *)malloc(bound);
    if (compressed == NULL) {
        free(path);
        free(envelope);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate compressed object");
    }
    compressed_len = ZSTD_compress(compressed, bound, envelope, envelope_len, ctx->config.compression_level);
    free(envelope);
    if (ZSTD_isError(compressed_len)) {
        free(path);
        free(compressed);
        return scribe_set_error(SCRIBE_EIO, "zstd compression failed: %s", ZSTD_getErrorName(compressed_len));
    }
    err = scribe_write_file_atomic(path, compressed, compressed_len);
    free(path);
    free(compressed);
    return err;
}

scribe_error_t scribe_object_read(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE], scribe_object *out) {
    char *path;
    uint8_t *compressed = NULL;
    uint8_t *envelope = NULL;
    size_t compressed_len = 0;
    unsigned long long frame_len;
    size_t decompressed_len;
    uint8_t actual[SCRIBE_HASH_SIZE];
    uint64_t payload_len64;
    size_t leb_used;
    scribe_error_t err;

    memset(out, 0, sizeof(*out));
    path = scribe_object_path(ctx, hash);
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = scribe_read_file(path, &compressed, &compressed_len);
    free(path);
    if (err != SCRIBE_OK) {
        return err;
    }
    frame_len = ZSTD_getFrameContentSize(compressed, compressed_len);
    if (frame_len == ZSTD_CONTENTSIZE_ERROR || frame_len == ZSTD_CONTENTSIZE_UNKNOWN) {
        free(compressed);
        return scribe_set_error(SCRIBE_ECORRUPT, "invalid zstd object frame");
    }
    if (frame_len > (unsigned long long)SIZE_MAX) {
        free(compressed);
        return scribe_set_error(SCRIBE_ECORRUPT, "object too large");
    }
    envelope = (uint8_t *)malloc((size_t)frame_len);
    if (envelope == NULL) {
        free(compressed);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate object envelope");
    }
    decompressed_len = ZSTD_decompress(envelope, (size_t)frame_len, compressed, compressed_len);
    free(compressed);
    if (ZSTD_isError(decompressed_len) || decompressed_len != (size_t)frame_len) {
        free(envelope);
        return scribe_set_error(SCRIBE_ECORRUPT, "zstd decompression failed");
    }
    hash_bytes(envelope, decompressed_len, actual);
    if (scribe_hash_cmp(actual, hash) != 0) {
        free(envelope);
        return scribe_set_error(SCRIBE_ECORRUPT, "object hash mismatch");
    }
    if (decompressed_len < 2u) {
        free(envelope);
        return scribe_set_error(SCRIBE_ECORRUPT, "object envelope too short");
    }
    err = scribe_leb128_decode(envelope + 1u, decompressed_len - 1u, &payload_len64, &leb_used);
    if (err != SCRIBE_OK) {
        free(envelope);
        return err;
    }
    if (payload_len64 > SIZE_MAX || 1u + leb_used + (size_t)payload_len64 != decompressed_len) {
        free(envelope);
        return scribe_set_error(SCRIBE_ECORRUPT, "object envelope length mismatch");
    }
    out->type = envelope[0];
    out->envelope = envelope;
    out->envelope_len = decompressed_len;
    out->payload = envelope + 1u + leb_used;
    out->payload_len = (size_t)payload_len64;
    return SCRIBE_OK;
}

void scribe_object_free(scribe_object *obj) {
    if (obj != NULL) {
        free(obj->envelope);
        memset(obj, 0, sizeof(*obj));
    }
}
