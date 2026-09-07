/*
 * Content-addressed object storage.
 *
 * Scribe stores blobs, trees, and commits as zstd-compressed loose objects under
 * `.scribe/objects`. The object hash is BLAKE3 over the uncompressed typed
 * envelope, so reads can verify both identity and payload framing before higher
 * layers parse object contents.
 */
#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"
#include "util/leb128.h"

#include "blake3.h"
#include "zstd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Hashes an arbitrary byte buffer with BLAKE3-256. Object code uses this for
 * envelopes and helper code uses the same fixed output size everywhere.
 */
static void hash_bytes(const uint8_t *bytes, size_t len, uint8_t out[SCRIBE_HASH_SIZE]) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, bytes, len);
    blake3_hasher_finalize(&hasher, out, SCRIBE_HASH_SIZE);
}

/*
 * Builds the uncompressed object envelope `<type><payload-len><payload>`.
 * The returned buffer is heap-owned because it is both hashed and compressed
 * before being written to disk.
 */
static scribe_error_t build_envelope(uint8_t type, const uint8_t *payload, size_t payload_len, uint8_t **out,
                                     size_t *out_len) {
    uint8_t leb[10];
    size_t leb_len;
    uint8_t *buf;

    /*
     * Scribe hashes this uncompressed envelope, not the compressed file.
     * The envelope gives every object a typed byte representation:
     *
     *   byte 0                  object type: blob/tree/commit
     *   bytes 1..N              unsigned LEB128 payload length
     *   remaining bytes         raw payload
     *
     * Hashing the type and length along with the payload prevents a blob,
     * tree, and commit with identical payload bytes from sharing a hash, and it
     * lets readers reject truncated or overlong decompressed data.
     */
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

/*
 * Computes the loose-object path for a hash using the v1 two-character fanout
 * directory layout. The returned string is heap-owned by the caller.
 */
char *scribe_object_path(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    char dirpart[16];
    char *objects;
    char *dir;
    char *path;

    /*
     * Loose objects are sharded by the first two hex characters:
     * objects/ab/cdef... instead of objects/abcdef... This keeps directory
     * sizes reasonable for large v1 stores and matches the
     * iterator layout used by list-objects and fsck.
     */
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

/*
 * Checks whether an object currently exists in either loose or packed storage
 * without reading and verifying the full payload. Writers use this to make
 * content-addressed writes idempotent.
 */
scribe_error_t scribe_object_has(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    char *path = scribe_object_path(ctx, hash);
    int exists;

    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    exists = scribe_file_exists(path);
    free(path);
    if (exists) {
        return SCRIBE_OK;
    }
    return scribe_pack_has(ctx, hash);
}

/*
 * Writes an object if its content-addressed file does not already exist. The
 * object is enveloped, hashed, compressed, and atomically published under the
 * loose-object path derived from the hash.
 */
scribe_error_t scribe_object_write_loose_opts(scribe_ctx *ctx, uint8_t type, const uint8_t *payload, size_t payload_len,
                                              int sync_file, int sync_parent, uint8_t out_hash[SCRIBE_HASH_SIZE],
                                              uint8_t *out_fanout, int *out_written) {
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

    if (out_written != NULL) {
        *out_written = 0;
    }
    if (payload == NULL && payload_len != 0) {
        return scribe_set_error(SCRIBE_EINVAL, "payload is NULL with non-zero length");
    }
    /*
     * Write path:
     *   1. build the uncompressed typed envelope;
     *   2. hash the envelope to get the content address;
     *   3. skip the write if that object already exists;
     *   4. compress the envelope and atomically publish the loose object file.
     *
     * The idempotent "already exists" case matters because the same MongoDB
     * document bytes can be observed repeatedly and should reuse one blob.
     */
    err = build_envelope(type, payload, payload_len, &envelope, &envelope_len);
    if (err != SCRIBE_OK) {
        return err;
    }
    hash_bytes(envelope, envelope_len, out_hash);
    if (out_fanout != NULL) {
        *out_fanout = out_hash[0];
    }
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
    err = scribe_write_file_atomic_opts(path, compressed, compressed_len, sync_file, sync_parent);
    free(path);
    free(compressed);
    if (err == SCRIBE_OK && out_written != NULL) {
        *out_written = 1;
    }
    return err;
}

scribe_error_t scribe_object_write(scribe_ctx *ctx, uint8_t type, const uint8_t *payload, size_t payload_len,
                                   uint8_t out_hash[SCRIBE_HASH_SIZE]) {
    return scribe_object_write_loose_opts(ctx, type, payload, payload_len, 1, 1, out_hash, NULL, NULL);
}

scribe_error_t scribe_object_fsync_fanout_dirs(scribe_ctx *ctx, const bool touched[256]) {
    static const char digits[] = "0123456789abcdef";
    char *objects;
    size_t i;
    scribe_error_t err = SCRIBE_OK;

    if (ctx == NULL || touched == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid object fanout fsync arguments");
    }
    objects = scribe_path_join(ctx->repo_path, "objects");
    if (objects == NULL) {
        return SCRIBE_ENOMEM;
    }
    for (i = 0; i < 256u && err == SCRIBE_OK; i++) {
        char dirpart[3];
        char *dir;

        if (!touched[i]) {
            continue;
        }
        dirpart[0] = digits[i >> 4u];
        dirpart[1] = digits[i & 0x0fu];
        dirpart[2] = '\0';
        dir = scribe_path_join(objects, dirpart);
        if (dir == NULL) {
            err = SCRIBE_ENOMEM;
            break;
        }
        err = scribe_fsync_dir(dir);
        free(dir);
    }
    if (err == SCRIBE_OK) {
        err = scribe_fsync_dir(objects);
    }
    free(objects);
    return err;
}

/*
 * Reads and verifies one object. Loose storage is checked first because it is
 * the low-latency write path and preserves v1 behavior. If no loose file exists,
 * the pack backend searches published `.idx` files and reads the corresponding
 * record from `.scribe/packs/`.
 */
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
        if (err == SCRIBE_ENOT_FOUND) {
            return scribe_pack_read_object(ctx, hash, out);
        }
        return err;
    }
    /*
     * Read path is deliberately strict. A caller asking for hash H receives an
     * object only if the file decompresses cleanly, the decompressed envelope
     * rehashes to H, and the embedded payload length exactly matches the
     * remaining bytes. This is the verification that fsck relies on, and every
     * command that reads objects gets the same corruption checks for free.
     */
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

/*
 * Releases the envelope buffer owned by a read object and clears all object
 * fields so accidental reuse is easier to notice during debugging.
 */
void scribe_object_free(scribe_object *obj) {
    if (obj != NULL) {
        free(obj->envelope);
        memset(obj, 0, sizeof(*obj));
    }
}

/*
 * Returns whether a character is valid lowercase hex for object path names.
 */
static int is_hex_char(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

/*
 * Validates a string slice as lowercase hex without requiring it to be
 * NUL-terminated. Object iteration uses this for fanout directories and file names.
 */
static int is_hex_string(const char *s, size_t len) {
    size_t i;

    if (s == NULL) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (!is_hex_char(s[i])) {
            return 0;
        }
    }
    return 1;
}

typedef struct {
    scribe_object_visit_fn visit;
    void *user;
    char dir_hex[3];
} object_file_iter;

/*
 * Visits one file name inside an object fanout directory. Only names that look
 * like the remaining 62 hex characters of a loose object are converted to a
 * hash and passed to the caller's visitor.
 */
static scribe_error_t visit_object_file(const char *name, void *vctx) {
    object_file_iter *it = (object_file_iter *)vctx;
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    uint8_t hash[SCRIBE_HASH_SIZE];
    scribe_error_t err;

    /*
     * Ignore files that do not look like loose object names. Operators may
     * leave editor temp files or filesystem metadata in objects/; those should
     * not break iteration. A file that has a valid object-shaped name but bad
     * contents will still fail later when the visitor reads it.
     */
    if (strlen(name) != 62u || !is_hex_string(name, 62u)) {
        return SCRIBE_OK;
    }
    snprintf(hex, sizeof(hex), "%s%s", it->dir_hex, name);
    err = scribe_hash_from_hex(hex, hash);
    if (err != SCRIBE_OK) {
        return err;
    }
    return it->visit(hash, it->user);
}

typedef struct {
    scribe_ctx *ctx;
    scribe_object_visit_fn visit;
    void *user;
} object_dir_iter;

/*
 * Visits one fanout directory under objects/. Valid two-character hex
 * directories are opened and their files are delegated to visit_object_file().
 */
static scribe_error_t visit_object_dir(const char *name, void *vctx) {
    object_dir_iter *it = (object_dir_iter *)vctx;
    object_file_iter file_it;
    char *objects;
    char *dir;
    scribe_error_t err;

    if (strlen(name) != 2u || !is_hex_string(name, 2u)) {
        return SCRIBE_OK;
    }
    objects = scribe_path_join(it->ctx->repo_path, "objects");
    if (objects == NULL) {
        return SCRIBE_ENOMEM;
    }
    dir = scribe_path_join(objects, name);
    free(objects);
    if (dir == NULL) {
        return SCRIBE_ENOMEM;
    }
    file_it.visit = it->visit;
    file_it.user = it->user;
    file_it.dir_hex[0] = name[0];
    file_it.dir_hex[1] = name[1];
    file_it.dir_hex[2] = '\0';
    err = scribe_list_dir(dir, visit_object_file, &file_it);
    free(dir);
    return err == SCRIBE_ENOT_FOUND ? SCRIBE_OK : err;
}

/*
 * Iterates every object hash known to the object store. Loose objects are
 * visited first, followed by objects named by published pack indexes. The
 * iterator intentionally reports storage entries, so a hash duplicated in loose
 * and packed storage can be visited twice; fsck treats that as duplicate
 * storage rather than a different logical object.
 */
scribe_error_t scribe_object_iter(scribe_ctx *ctx, scribe_object_visit_fn visit, void *user) {
    object_dir_iter it;
    char *objects;
    scribe_error_t err;

    if (ctx == NULL || visit == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid object iterator arguments");
    }
    objects = scribe_path_join(ctx->repo_path, "objects");
    if (objects == NULL) {
        return SCRIBE_ENOMEM;
    }
    it.ctx = ctx;
    it.visit = visit;
    it.user = user;
    err = scribe_list_dir(objects, visit_object_dir, &it);
    free(objects);
    if (err != SCRIBE_OK && err != SCRIBE_ENOT_FOUND) {
        return err;
    }
    return scribe_pack_iter(ctx, visit, user);
}

typedef struct {
    scribe_loose_object_visit_fn visit;
    void *user;
    char dir_hex[3];
    char *dir_path;
} loose_file_iter;

/*
 * Visits one loose object file with its filesystem metadata. GC uses the mtime
 * to enforce the prune grace period and the path to unlink only after any pack
 * publication has completed.
 */
static scribe_error_t visit_loose_object_file(const char *name, void *vctx) {
    loose_file_iter *it = (loose_file_iter *)vctx;
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    uint8_t hash[SCRIBE_HASH_SIZE];
    char *path;
    struct stat st;
    scribe_error_t err;

    if (strlen(name) != 62u || !is_hex_string(name, 62u)) {
        return SCRIBE_OK;
    }
    snprintf(hex, sizeof(hex), "%s%s", it->dir_hex, name);
    err = scribe_hash_from_hex(hex, hash);
    if (err != SCRIBE_OK) {
        return err;
    }
    path = scribe_path_join(it->dir_path, name);
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    if (stat(path, &st) != 0) {
        free(path);
        return errno == ENOENT ? SCRIBE_OK : scribe_set_error(SCRIBE_EIO, "failed to stat loose object");
    }
    if (!S_ISREG(st.st_mode)) {
        free(path);
        return SCRIBE_OK;
    }
    if (st.st_size < 0 || (uintmax_t)st.st_size > (uintmax_t)SIZE_MAX) {
        free(path);
        return scribe_set_error(SCRIBE_EIO, "invalid loose object size");
    }
    err = it->visit(hash, path, st.st_mtime, (size_t)st.st_size, it->user);
    free(path);
    return err;
}

typedef struct {
    scribe_ctx *ctx;
    scribe_loose_object_visit_fn visit;
    void *user;
} loose_dir_iter;

/*
 * Opens one two-hex-character fanout directory below objects/ and delegates its
 * object-shaped file names to visit_loose_object_file().
 */
static scribe_error_t visit_loose_object_dir(const char *name, void *vctx) {
    loose_dir_iter *it = (loose_dir_iter *)vctx;
    loose_file_iter file_it;
    char *objects;
    char *dir;
    scribe_error_t err;

    if (strlen(name) != 2u || !is_hex_string(name, 2u)) {
        return SCRIBE_OK;
    }
    objects = scribe_path_join(it->ctx->repo_path, "objects");
    if (objects == NULL) {
        return SCRIBE_ENOMEM;
    }
    dir = scribe_path_join(objects, name);
    free(objects);
    if (dir == NULL) {
        return SCRIBE_ENOMEM;
    }
    file_it.visit = it->visit;
    file_it.user = it->user;
    file_it.dir_path = dir;
    file_it.dir_hex[0] = name[0];
    file_it.dir_hex[1] = name[1];
    file_it.dir_hex[2] = '\0';
    err = scribe_list_dir(dir, visit_loose_object_file, &file_it);
    free(dir);
    return err == SCRIBE_ENOT_FOUND ? SCRIBE_OK : err;
}

/*
 * Iterates only loose-object storage entries. Unlike scribe_object_iter(), this
 * deliberately does not visit packs; maintenance commands need to distinguish
 * the loose files they may prune or roll into packs from immutable pack records.
 */
scribe_error_t scribe_loose_object_iter(scribe_ctx *ctx, scribe_loose_object_visit_fn visit, void *user) {
    loose_dir_iter it;
    char *objects;
    scribe_error_t err;

    if (ctx == NULL || visit == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid loose-object iterator arguments");
    }
    objects = scribe_path_join(ctx->repo_path, "objects");
    if (objects == NULL) {
        return SCRIBE_ENOMEM;
    }
    it.ctx = ctx;
    it.visit = visit;
    it.user = user;
    err = scribe_list_dir(objects, visit_loose_object_dir, &it);
    free(objects);
    return err == SCRIBE_ENOT_FOUND ? SCRIBE_OK : err;
}

/*
 * Removes one loose object file by hash. Missing files are treated as success so
 * gc remains safe if another maintenance process already removed a duplicate
 * before this process reached it.
 */
scribe_error_t scribe_loose_object_delete(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    char *path;

    if (ctx == NULL || hash == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid loose-object delete arguments");
    }
    path = scribe_object_path(ctx, hash);
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        free(path);
        return scribe_set_error(SCRIBE_EIO, "failed to delete loose object");
    }
    free(path);
    return SCRIBE_OK;
}

/*
 * Returns the compressed on-disk byte size of an object. Loose objects report
 * their file size; packed objects report the compressed record payload length
 * stored in the pack index.
 */
scribe_error_t scribe_object_compressed_size(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE], size_t *out) {
    struct stat st;
    char *path;

    if (ctx == NULL || hash == NULL || out == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid compressed-size arguments");
    }
    path = scribe_object_path(ctx, hash);
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    if (stat(path, &st) != 0) {
        free(path);
        if (errno == ENOENT) {
            return scribe_pack_compressed_size(ctx, hash, out);
        }
        return scribe_set_error(SCRIBE_EIO, "failed to stat object");
    }
    free(path);
    if (st.st_size < 0 || (uintmax_t)st.st_size > (uintmax_t)SIZE_MAX) {
        return scribe_set_error(SCRIBE_EIO, "invalid compressed object size");
    }
    *out = (size_t)st.st_size;
    return SCRIBE_OK;
}
