/*
 * Bundle export and import.
 *
 * A Scribe bundle is a single self-checking file used to move a repository
 * snapshot between stores without a network protocol. It carries raw storage
 * files (loose objects, packs, pack indexes) plus direct refs. Import writes
 * storage entries first as ordinary files, then publishes refs through the ref
 * layer so local reflogs record the import operation.
 */
#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"

#include "blake3.h"
#include "zstd.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SCRIBE_BUNDLE_HEADER "SCRIBE-BUNDLE 1\n"

typedef struct {
    scribe_ctx *ctx;
    FILE *file;
    blake3_hasher hasher;
    size_t loose_count;
    size_t pack_file_count;
    size_t ref_count;
} bundle_create_state;

typedef struct {
    uint8_t (*items)[SCRIBE_HASH_SIZE];
    size_t count;
    size_t cap;
} bundle_hash_list;

typedef struct {
    bundle_create_state base;
    scribe_hash_filter_fn include;
    void *include_user;
    bundle_hash_list written;
} bundle_filtered_state;

/*
 * Writes all bytes to a stream. fwrite() is wrapped because every bundle entry
 * is length-sensitive: short writes would make a bundle impossible to parse.
 */
static scribe_error_t write_all(FILE *file, const void *data, size_t len) {
    if (len != 0u && fwrite(data, 1u, len, file) != len) {
        return scribe_set_error(SCRIBE_EIO, "failed to write bundle");
    }
    return SCRIBE_OK;
}

/*
 * Writes bytes and feeds the same bytes into the bundle manifest hash. The
 * final `checksum` line is intentionally not hashed; everything before it is.
 */
static scribe_error_t write_hashed(FILE *file, blake3_hasher *hasher, const void *data, size_t len) {
    scribe_error_t err = write_all(file, data, len);

    if (err != SCRIBE_OK) {
        return err;
    }
    blake3_hasher_update(hasher, data, len);
    return SCRIBE_OK;
}

/*
 * Writes one typed bundle entry. The header is text so humans can inspect the
 * outline, while the payload is copied byte-for-byte and may contain arbitrary
 * binary compressed object data.
 */
static scribe_error_t write_bundle_entry(bundle_create_state *st, const char *kind, const char *path,
                                         const uint8_t *payload, size_t payload_len) {
    char header[PATH_MAX + 128];
    int n;
    scribe_error_t err;

    n = snprintf(header, sizeof(header), "entry %s %s %zu\n", kind, path, payload_len);
    if (n < 0 || (size_t)n >= sizeof(header)) {
        return scribe_set_error(SCRIBE_EPATH, "bundle entry path too long");
    }
    err = write_hashed(st->file, &st->hasher, header, (size_t)n);
    if (err == SCRIBE_OK && payload_len != 0u) {
        err = write_hashed(st->file, &st->hasher, payload, payload_len);
    }
    if (err == SCRIBE_OK) {
        err = write_hashed(st->file, &st->hasher, "\n", 1u);
    }
    return err;
}

/*
 * Converts a pack id to the 32-hex filename stem used under `.scribe/packs`.
 */
static void pack_id_to_hex(const uint8_t pack_id[16], char out[33]) {
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < 16u; i++) {
        out[i * 2u] = digits[pack_id[i] >> 4u];
        out[i * 2u + 1u] = digits[pack_id[i] & 0x0fu];
    }
    out[32] = '\0';
}

static int bundle_hash_list_contains(const bundle_hash_list *list, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    size_t i;

    for (i = 0; i < list->count; i++) {
        if (memcmp(list->items[i], hash, SCRIBE_HASH_SIZE) == 0) {
            return 1;
        }
    }
    return 0;
}

static scribe_error_t bundle_hash_list_add(bundle_hash_list *list, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    uint8_t(*grown)[SCRIBE_HASH_SIZE];
    size_t new_cap;

    if (list->count == list->cap) {
        new_cap = list->cap == 0u ? 1024u : list->cap * 2u;
        grown = (uint8_t(*)[SCRIBE_HASH_SIZE])realloc(list->items, sizeof(*list->items) * new_cap);
        if (grown == NULL) {
            return scribe_set_error(SCRIBE_ENOMEM, "failed to grow bundle hash set");
        }
        list->items = grown;
        list->cap = new_cap;
    }
    memcpy(list->items[list->count], hash, SCRIBE_HASH_SIZE);
    list->count++;
    return SCRIBE_OK;
}

static void bundle_hash_list_destroy(bundle_hash_list *list) {
    if (list != NULL) {
        free(list->items);
        memset(list, 0, sizeof(*list));
    }
}

/*
 * Reads a repository-relative file and appends it as one bundle entry. This is
 * used for raw pack/index files and loose-object compressed files.
 */
static scribe_error_t bundle_file_entry(bundle_create_state *st, const char *kind, const char *rel_path) {
    char *path = scribe_path_join(st->ctx->repo_path, rel_path);
    uint8_t *bytes = NULL;
    size_t len = 0;
    scribe_error_t err;

    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = scribe_read_file(path, &bytes, &len);
    free(path);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = write_bundle_entry(st, kind, rel_path, bytes, len);
    free(bytes);
    return err;
}

/*
 * Loose-object iterator callback. The iterator gives us the absolute file path
 * for stat/delete use; bundle export derives the canonical relative path from
 * the hash so import can validate the object name independently.
 */
static scribe_error_t bundle_visit_loose(const uint8_t hash[SCRIBE_HASH_SIZE], const char *path, time_t mtime,
                                         size_t compressed_size, void *user) {
    bundle_create_state *st = (bundle_create_state *)user;
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    char rel[80];
    scribe_error_t err;

    (void)path;
    (void)mtime;
    (void)compressed_size;
    scribe_hash_to_hex(hash, hex);
    if (snprintf(rel, sizeof(rel), "objects/%.2s/%s", hex, hex + 2) >= (int)sizeof(rel)) {
        return scribe_set_error(SCRIBE_EPATH, "loose object path too long");
    }
    err = bundle_file_entry(st, "loose", rel);
    if (err == SCRIBE_OK) {
        st->loose_count++;
    }
    return err;
}

/*
 * Pack iterator callback. The `.pack` payload is written before `.idx` because
 * the index is the visibility marker for readers.
 */
static scribe_error_t bundle_visit_pack(const uint8_t pack_id[16], void *user) {
    bundle_create_state *st = (bundle_create_state *)user;
    char id_hex[33];
    char rel[48];
    scribe_error_t err;

    pack_id_to_hex(pack_id, id_hex);
    if (snprintf(rel, sizeof(rel), "packs/%s.pack", id_hex) >= (int)sizeof(rel)) {
        return scribe_set_error(SCRIBE_EPATH, "pack path too long");
    }
    err = bundle_file_entry(st, "pack", rel);
    if (err != SCRIBE_OK) {
        return err;
    }
    st->pack_file_count++;
    if (snprintf(rel, sizeof(rel), "packs/%s.idx", id_hex) >= (int)sizeof(rel)) {
        return scribe_set_error(SCRIBE_EPATH, "pack index path too long");
    }
    err = bundle_file_entry(st, "idx", rel);
    if (err == SCRIBE_OK) {
        st->pack_file_count++;
    }
    return err;
}

/*
 * Ref iterator callback. Refs are written after storage entries so import
 * publishes names only after the referenced objects have already been copied.
 */
static scribe_error_t bundle_visit_ref(const char *name, const uint8_t hash[SCRIBE_HASH_SIZE], void *user) {
    bundle_create_state *st = (bundle_create_state *)user;
    char payload[SCRIBE_HEX_HASH_SIZE + 2];
    scribe_error_t err;

    scribe_hash_to_hex(hash, payload);
    payload[SCRIBE_HEX_HASH_SIZE] = '\n';
    payload[SCRIBE_HEX_HASH_SIZE + 1u] = '\0';
    err = write_bundle_entry(st, "ref", name, (const uint8_t *)payload, SCRIBE_HEX_HASH_SIZE + 1u);
    if (err == SCRIBE_OK) {
        st->ref_count++;
    }
    return err;
}

/*
 * Writes one logical object into a filtered sync bundle. The filtered bundle
 * deliberately serializes every missing object as a loose entry, even if the
 * source object currently lives in a pack. That keeps push/pull object-granular:
 * an existing receiver never has to accept an entire pack just because one
 * object in it is missing.
 */
static scribe_error_t bundle_visit_filtered_object(const uint8_t hash[SCRIBE_HASH_SIZE], void *user) {
    bundle_filtered_state *st = (bundle_filtered_state *)user;
    scribe_object obj;
    uint8_t *compressed = NULL;
    size_t bound;
    size_t compressed_len;
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    char rel[80];
    scribe_error_t err;

    if (st->include != NULL && !st->include(hash, st->include_user)) {
        return SCRIBE_OK;
    }
    if (bundle_hash_list_contains(&st->written, hash)) {
        return SCRIBE_OK;
    }
    err = scribe_object_read(st->base.ctx, hash, &obj);
    if (err != SCRIBE_OK) {
        return err;
    }
    bound = ZSTD_compressBound(obj.envelope_len);
    compressed = (uint8_t *)malloc(bound);
    if (compressed == NULL) {
        scribe_object_free(&obj);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate filtered bundle object");
    }
    compressed_len =
        ZSTD_compress(compressed, bound, obj.envelope, obj.envelope_len, st->base.ctx->config.compression_level);
    if (ZSTD_isError(compressed_len)) {
        free(compressed);
        scribe_object_free(&obj);
        return scribe_set_error(SCRIBE_EIO, "zstd compression failed: %s", ZSTD_getErrorName(compressed_len));
    }
    scribe_hash_to_hex(hash, hex);
    if (snprintf(rel, sizeof(rel), "objects/%.2s/%s", hex, hex + 2) >= (int)sizeof(rel)) {
        free(compressed);
        scribe_object_free(&obj);
        return scribe_set_error(SCRIBE_EPATH, "filtered bundle object path too long");
    }
    err = write_bundle_entry(&st->base, "loose", rel, compressed, compressed_len);
    free(compressed);
    scribe_object_free(&obj);
    if (err == SCRIBE_OK) {
        err = bundle_hash_list_add(&st->written, hash);
    }
    if (err == SCRIBE_OK) {
        st->base.loose_count++;
    }
    return err;
}

/*
 * Creates a bundle from the currently open repository. The command validates
 * every pack before export so the bundle never knowingly captures broken pack
 * storage. The quiet helper is also used by push/pull, where the bundle is a
 * network payload rather than a user-facing artifact.
 */
scribe_error_t scribe_bundle_create_file(scribe_ctx *ctx, const char *file, size_t *out_loose, size_t *out_pack_files,
                                         size_t *out_refs) {
    bundle_create_state st;
    uint8_t digest[SCRIBE_HASH_SIZE];
    char digest_hex[SCRIBE_HEX_HASH_SIZE + 1];
    char checksum_line[96];
    int n;
    scribe_error_t err;

    if (ctx == NULL || file == NULL || file[0] == '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "bundle output file is required");
    }
    memset(&st, 0, sizeof(st));
    st.ctx = ctx;
    st.file = fopen(file, "wb");
    if (st.file == NULL) {
        return scribe_set_error(SCRIBE_EIO, "failed to create bundle '%s'", file);
    }
    blake3_hasher_init(&st.hasher);
    err = write_hashed(st.file, &st.hasher, SCRIBE_BUNDLE_HEADER, strlen(SCRIBE_BUNDLE_HEADER));
    if (err == SCRIBE_OK) {
        err = scribe_pack_validate_all(ctx, NULL);
    }
    if (err == SCRIBE_OK) {
        err = scribe_loose_object_iter(ctx, bundle_visit_loose, &st);
    }
    if (err == SCRIBE_OK) {
        err = scribe_pack_file_iter(ctx, bundle_visit_pack, &st);
    }
    if (err == SCRIBE_OK) {
        err = scribe_refs_iter(ctx, bundle_visit_ref, &st);
    }
    if (err == SCRIBE_OK) {
        blake3_hasher_finalize(&st.hasher, digest, sizeof(digest));
        scribe_hash_to_hex(digest, digest_hex);
        n = snprintf(checksum_line, sizeof(checksum_line), "checksum %s\n", digest_hex);
        if (n < 0 || (size_t)n >= sizeof(checksum_line)) {
            err = scribe_set_error(SCRIBE_EPATH, "bundle checksum line too long");
        } else {
            err = write_all(st.file, checksum_line, (size_t)n);
        }
    }
    if (err == SCRIBE_OK && fflush(st.file) != 0) {
        err = scribe_set_error(SCRIBE_EIO, "failed to flush bundle");
    }
    if (err == SCRIBE_OK && fsync(fileno(st.file)) != 0) {
        err = scribe_set_error(SCRIBE_EIO, "failed to sync bundle");
    }
    if (fclose(st.file) != 0 && err == SCRIBE_OK) {
        err = scribe_set_error(SCRIBE_EIO, "failed to close bundle");
    }
    if (err == SCRIBE_OK) {
        if (out_loose != NULL) {
            *out_loose = st.loose_count;
        }
        if (out_pack_files != NULL) {
            *out_pack_files = st.pack_file_count;
        }
        if (out_refs != NULL) {
            *out_refs = st.ref_count;
        }
    }
    return err;
}

/*
 * Creates a sync bundle containing only logical objects accepted by the filter,
 * plus all refs. This is used by push/pull negotiation after the receiver has
 * advertised the object hashes it already owns. Unlike archival bundle create,
 * this path never copies whole pack files; packed objects that are missing on
 * the receiver are emitted as ordinary loose-object entries.
 */
scribe_error_t scribe_bundle_create_filtered_file(scribe_ctx *ctx, const char *file, scribe_hash_filter_fn include,
                                                  void *include_user, size_t *out_storage_entries, size_t *out_refs) {
    bundle_filtered_state st;
    uint8_t digest[SCRIBE_HASH_SIZE];
    char digest_hex[SCRIBE_HEX_HASH_SIZE + 1];
    char checksum_line[96];
    int n;
    scribe_error_t err;

    if (ctx == NULL || file == NULL || file[0] == '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "filtered bundle output file is required");
    }
    memset(&st, 0, sizeof(st));
    st.base.ctx = ctx;
    st.include = include;
    st.include_user = include_user;
    st.base.file = fopen(file, "wb");
    if (st.base.file == NULL) {
        return scribe_set_error(SCRIBE_EIO, "failed to create filtered bundle '%s'", file);
    }
    blake3_hasher_init(&st.base.hasher);
    err = write_hashed(st.base.file, &st.base.hasher, SCRIBE_BUNDLE_HEADER, strlen(SCRIBE_BUNDLE_HEADER));
    if (err == SCRIBE_OK) {
        err = scribe_pack_validate_all(ctx, NULL);
    }
    if (err == SCRIBE_OK) {
        err = scribe_object_iter(ctx, bundle_visit_filtered_object, &st);
    }
    if (err == SCRIBE_OK) {
        err = scribe_refs_iter(ctx, bundle_visit_ref, &st.base);
    }
    if (err == SCRIBE_OK) {
        blake3_hasher_finalize(&st.base.hasher, digest, sizeof(digest));
        scribe_hash_to_hex(digest, digest_hex);
        n = snprintf(checksum_line, sizeof(checksum_line), "checksum %s\n", digest_hex);
        if (n < 0 || (size_t)n >= sizeof(checksum_line)) {
            err = scribe_set_error(SCRIBE_EPATH, "bundle checksum line too long");
        } else {
            err = write_all(st.base.file, checksum_line, (size_t)n);
        }
    }
    if (err == SCRIBE_OK && fflush(st.base.file) != 0) {
        err = scribe_set_error(SCRIBE_EIO, "failed to flush filtered bundle");
    }
    if (err == SCRIBE_OK && fsync(fileno(st.base.file)) != 0) {
        err = scribe_set_error(SCRIBE_EIO, "failed to sync filtered bundle");
    }
    if (fclose(st.base.file) != 0 && err == SCRIBE_OK) {
        err = scribe_set_error(SCRIBE_EIO, "failed to close filtered bundle");
    }
    if (err == SCRIBE_OK) {
        if (out_storage_entries != NULL) {
            *out_storage_entries = st.base.loose_count;
        }
        if (out_refs != NULL) {
            *out_refs = st.base.ref_count;
        }
    }
    bundle_hash_list_destroy(&st.written);
    return err;
}

scribe_error_t scribe_cli_bundle_create(scribe_ctx *ctx, const char *file) {
    size_t loose_count = 0;
    size_t pack_file_count = 0;
    size_t ref_count = 0;
    scribe_error_t err = scribe_bundle_create_file(ctx, file, &loose_count, &pack_file_count, &ref_count);

    if (err == SCRIBE_OK) {
        printf("bundle %s: %zu loose object(s), %zu pack file(s), %zu ref(s)\n", file, loose_count, pack_file_count,
               ref_count);
    }
    return err;
}

/*
 * Parses one newline-terminated line from an in-memory bundle. The returned
 * pointer aliases the bundle buffer and the length excludes the newline.
 */
static scribe_error_t read_bundle_line(const uint8_t *bytes, size_t len, size_t *pos, const char **line,
                                       size_t *line_len) {
    const uint8_t *nl;

    if (*pos > len) {
        return scribe_set_error(SCRIBE_ECORRUPT, "bundle parser moved past end");
    }
    nl = (const uint8_t *)memchr(bytes + *pos, '\n', len - *pos);
    if (nl == NULL) {
        return scribe_set_error(SCRIBE_ECORRUPT, "bundle line missing newline");
    }
    *line = (const char *)(bytes + *pos);
    *line_len = (size_t)(nl - (bytes + *pos));
    *pos += *line_len + 1u;
    return SCRIBE_OK;
}

/*
 * Returns whether a character is lowercase hex. Bundle import uses this before
 * accepting file names that become object or pack paths.
 */
static int is_hex_char(char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }

/*
 * Validates `objects/xx/yyyy...` and extracts the full hash represented by the
 * path. The path, not the compressed payload, names the loose object file.
 */
static scribe_error_t validate_loose_relpath(const char *path, uint8_t out_hash[SCRIBE_HASH_SIZE]) {
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    size_t i;

    if (path == NULL || strlen(path) != 73u || strncmp(path, "objects/", 8) != 0 || path[10] != '/') {
        return scribe_set_error(SCRIBE_ECORRUPT, "invalid loose object path in bundle");
    }
    for (i = 0; i < 2u; i++) {
        if (!is_hex_char(path[8u + i])) {
            return scribe_set_error(SCRIBE_ECORRUPT, "invalid loose object path in bundle");
        }
        hex[i] = path[8u + i];
    }
    for (i = 0; i < 62u; i++) {
        if (!is_hex_char(path[11u + i])) {
            return scribe_set_error(SCRIBE_ECORRUPT, "invalid loose object path in bundle");
        }
        hex[2u + i] = path[11u + i];
    }
    hex[SCRIBE_HEX_HASH_SIZE] = '\0';
    return scribe_hash_from_hex(hex, out_hash);
}

/*
 * Validates `packs/<32-hex>.<suffix>` for pack and index bundle entries.
 */
static scribe_error_t validate_pack_relpath(const char *path, const char *suffix) {
    size_t suffix_len = strlen(suffix);
    size_t i;

    if (path == NULL || strlen(path) != 6u + 32u + suffix_len || strncmp(path, "packs/", 6) != 0 ||
        strcmp(path + 6u + 32u, suffix) != 0) {
        return scribe_set_error(SCRIBE_ECORRUPT, "invalid pack path in bundle");
    }
    for (i = 0; i < 32u; i++) {
        if (!is_hex_char(path[6u + i])) {
            return scribe_set_error(SCRIBE_ECORRUPT, "invalid pack path in bundle");
        }
    }
    return SCRIBE_OK;
}

/*
 * Creates the parent directory for a repository-relative import target.
 */
static scribe_error_t mkdir_import_parent(const char *path) {
    char *copy;
    char *slash;
    scribe_error_t err;

    copy = strdup(path);
    if (copy == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate import path");
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
 * Writes a raw storage payload below the target repository. Paths are validated
 * before this helper is called, so joining with repo_path cannot escape.
 */
static scribe_error_t import_storage_file(scribe_ctx *ctx, const char *rel_path, const uint8_t *payload,
                                          size_t payload_len) {
    char *path = scribe_path_join(ctx->repo_path, rel_path);
    scribe_error_t err;

    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = mkdir_import_parent(path);
    if (err == SCRIBE_OK) {
        err = scribe_write_file_atomic(path, payload, payload_len);
    }
    free(path);
    return err;
}

/*
 * Returns whether old_hash is an ancestor of new_hash in the single-parent
 * commit chain. Sync uses this as its fast-forward test before publishing a
 * received branch ref.
 */
static scribe_error_t ref_is_ancestor(scribe_ctx *ctx, const uint8_t old_hash[SCRIBE_HASH_SIZE],
                                      const uint8_t new_hash[SCRIBE_HASH_SIZE], int *out) {
    uint8_t current[SCRIBE_HASH_SIZE];
    scribe_error_t err;

    *out = 0;
    scribe_hash_copy(current, new_hash);
    for (;;) {
        scribe_object obj;
        scribe_arena arena = {0};
        scribe_commit_view view;

        if (scribe_hash_cmp(current, old_hash) == 0) {
            *out = 1;
            return SCRIBE_OK;
        }
        err = scribe_object_read(ctx, current, &obj);
        if (err != SCRIBE_OK) {
            return err;
        }
        if (obj.type != SCRIBE_OBJECT_COMMIT) {
            scribe_object_free(&obj);
            return scribe_set_error(SCRIBE_ECORRUPT, "ref target is not a commit");
        }
        err = scribe_arena_init(&arena, obj.payload_len + 4096u);
        if (err == SCRIBE_OK) {
            err = scribe_commit_parse(obj.payload, obj.payload_len, &arena, &view);
        }
        scribe_object_free(&obj);
        if (err != SCRIBE_OK) {
            scribe_arena_destroy(&arena);
            return err;
        }
        if (!view.has_parent) {
            scribe_arena_destroy(&arena);
            return SCRIBE_OK;
        }
        scribe_hash_copy(current, view.parent);
        scribe_arena_destroy(&arena);
    }
}

/*
 * Publishes one ref payload from a bundle. Normal bundle import force-updates
 * refs; sync import requires fast-forward movement so one archive cannot
 * silently rewrite another archive's visible history.
 */
static scribe_error_t import_bundle_ref(scribe_ctx *ctx, const char *path, const uint8_t *payload, size_t payload_len,
                                        const char *reason, int fast_forward_refs) {
    char hex[SCRIBE_HEX_HASH_SIZE + 1];
    uint8_t hash[SCRIBE_HASH_SIZE];
    uint8_t current[SCRIBE_HASH_SIZE];
    scribe_error_t err;

    if (payload_len != SCRIBE_HEX_HASH_SIZE + 1u || payload[SCRIBE_HEX_HASH_SIZE] != '\n') {
        return scribe_set_error(SCRIBE_ECORRUPT, "invalid ref payload in bundle");
    }
    memcpy(hex, payload, SCRIBE_HEX_HASH_SIZE);
    hex[SCRIBE_HEX_HASH_SIZE] = '\0';
    err = scribe_hash_from_hex(hex, hash);
    if (err == SCRIBE_OK) {
        err = scribe_object_has(ctx, hash);
    }
    if (err != SCRIBE_OK) {
        return err;
    }
    if (fast_forward_refs) {
        err = scribe_refs_read(ctx, path, current);
        if (err == SCRIBE_OK) {
            int is_ancestor = 0;

            if (scribe_hash_cmp(current, hash) == 0) {
                return SCRIBE_OK;
            }
            if (strncmp(path, "refs/tags/", 10u) == 0) {
                return scribe_set_error(SCRIBE_EREF_STALE, "ref '%s' is a tag and cannot be moved by sync", path);
            }
            err = ref_is_ancestor(ctx, current, hash, &is_ancestor);
            if (err != SCRIBE_OK) {
                return err;
            }
            if (!is_ancestor) {
                return scribe_set_error(SCRIBE_EREF_STALE, "ref '%s' update is not fast-forward", path);
            }
        } else if (err != SCRIBE_ENOT_FOUND) {
            return err;
        }
    }
    return scribe_refs_update(ctx, path, hash, reason == NULL ? "bundle-import" : reason);
}

/*
 * Imports one parsed bundle entry. Loose objects are verified after writing by
 * reading them through the normal object store; packs are validated as a whole
 * after all entries are imported.
 */
static scribe_error_t import_bundle_entry_ex(scribe_ctx *ctx, const char *kind, const char *path,
                                             const uint8_t *payload, size_t payload_len, const char *reason,
                                             int fast_forward_refs) {
    if (strcmp(kind, "loose") == 0) {
        uint8_t hash[SCRIBE_HASH_SIZE];
        scribe_object obj;
        scribe_error_t err = validate_loose_relpath(path, hash);

        if (err == SCRIBE_OK) {
            err = import_storage_file(ctx, path, payload, payload_len);
        }
        if (err == SCRIBE_OK) {
            err = scribe_object_read(ctx, hash, &obj);
        }
        if (err == SCRIBE_OK) {
            scribe_object_free(&obj);
        }
        return err;
    }
    if (strcmp(kind, "pack") == 0) {
        scribe_error_t err = validate_pack_relpath(path, ".pack");
        return err == SCRIBE_OK ? import_storage_file(ctx, path, payload, payload_len) : err;
    }
    if (strcmp(kind, "idx") == 0) {
        scribe_error_t err = validate_pack_relpath(path, ".idx");
        return err == SCRIBE_OK ? import_storage_file(ctx, path, payload, payload_len) : err;
    }
    if (strcmp(kind, "ref") == 0) {
        return import_bundle_ref(ctx, path, payload, payload_len, reason, fast_forward_refs);
    }
    return scribe_set_error(SCRIBE_ECORRUPT, "unknown bundle entry kind '%s'", kind);
}

/*
 * Parses an entry header into kind, path, and payload length. The returned
 * kind/path buffers are heap-owned by the caller.
 */
static scribe_error_t parse_entry_header(const char *line, size_t line_len, char **out_kind, char **out_path,
                                         size_t *out_payload_len) {
    char *copy;
    char *kind;
    char *path;
    char *len_text;
    char *end = NULL;
    unsigned long long value;

    if (line_len < 8u || strncmp(line, "entry ", 6) != 0) {
        return scribe_set_error(SCRIBE_ECORRUPT, "invalid bundle entry header");
    }
    copy = (char *)malloc(line_len + 1u);
    if (copy == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate bundle header");
    }
    memcpy(copy, line, line_len);
    copy[line_len] = '\0';
    kind = copy + 6;
    path = strchr(kind, ' ');
    if (path == NULL) {
        free(copy);
        return scribe_set_error(SCRIBE_ECORRUPT, "invalid bundle entry header");
    }
    *path++ = '\0';
    len_text = strchr(path, ' ');
    if (len_text == NULL) {
        free(copy);
        return scribe_set_error(SCRIBE_ECORRUPT, "invalid bundle entry header");
    }
    *len_text++ = '\0';
    if (kind[0] == '\0' || path[0] == '\0' || len_text[0] == '\0') {
        free(copy);
        return scribe_set_error(SCRIBE_ECORRUPT, "invalid bundle entry header");
    }
    errno = 0;
    value = strtoull(len_text, &end, 10);
    if (errno != 0 || end == len_text || *end != '\0' || value > (unsigned long long)SIZE_MAX) {
        free(copy);
        return scribe_set_error(SCRIBE_ECORRUPT, "invalid bundle payload length");
    }
    *out_kind = strdup(kind);
    *out_path = strdup(path);
    free(copy);
    if (*out_kind == NULL || *out_path == NULL) {
        free(*out_kind);
        free(*out_path);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate bundle entry metadata");
    }
    *out_payload_len = (size_t)value;
    return SCRIBE_OK;
}

/*
 * Ensures a target path is an initialized Scribe repository before import opens
 * it. Fresh paths are initialized; existing repositories are left intact.
 */
static scribe_error_t ensure_import_target(const char *target) {
    char *config;
    scribe_error_t err;

    if (target == NULL || target[0] == '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "bundle import target is required");
    }
    config = scribe_path_join(target, "config");
    if (config == NULL) {
        return SCRIBE_ENOMEM;
    }
    if (scribe_file_exists(config)) {
        free(config);
        return SCRIBE_OK;
    }
    free(config);
    err = scribe_init_repository(target);
    return err == SCRIBE_EEXISTS ? SCRIBE_OK : err;
}

/*
 * Validates the outer bundle framing and BLAKE3 checksum without importing
 * anything. Import runs this pass first so a corrupted bundle cannot partially
 * modify the target repository before the checksum mismatch is discovered.
 */
static scribe_error_t validate_bundle_bytes(const uint8_t *bytes, size_t len) {
    size_t pos = 0;
    scribe_error_t err;
    const char *line = NULL;
    size_t line_len = 0;

    err = read_bundle_line(bytes, len, &pos, &line, &line_len);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (line_len != strlen(SCRIBE_BUNDLE_HEADER) - 1u ||
        memcmp(line, SCRIBE_BUNDLE_HEADER, strlen(SCRIBE_BUNDLE_HEADER) - 1u) != 0) {
        return scribe_set_error(SCRIBE_ECORRUPT, "invalid bundle header");
    }
    while (pos < len) {
        size_t line_start = pos;

        err = read_bundle_line(bytes, len, &pos, &line, &line_len);
        if (err != SCRIBE_OK) {
            return err;
        }
        if (line_len > 9u && strncmp(line, "checksum ", 9) == 0) {
            blake3_hasher hasher;
            uint8_t actual[SCRIBE_HASH_SIZE];
            uint8_t expected[SCRIBE_HASH_SIZE];
            char hex[SCRIBE_HEX_HASH_SIZE + 1];

            if (line_len != 9u + SCRIBE_HEX_HASH_SIZE || pos != len) {
                return scribe_set_error(SCRIBE_ECORRUPT, "invalid bundle checksum line");
            }
            memcpy(hex, line + 9u, SCRIBE_HEX_HASH_SIZE);
            hex[SCRIBE_HEX_HASH_SIZE] = '\0';
            err = scribe_hash_from_hex(hex, expected);
            if (err != SCRIBE_OK) {
                return err;
            }
            blake3_hasher_init(&hasher);
            blake3_hasher_update(&hasher, bytes, line_start);
            blake3_hasher_finalize(&hasher, actual, sizeof(actual));
            if (memcmp(actual, expected, SCRIBE_HASH_SIZE) != 0) {
                return scribe_set_error(SCRIBE_ECORRUPT, "bundle checksum mismatch");
            }
            return SCRIBE_OK;
        }
        {
            char *kind = NULL;
            char *path = NULL;
            size_t payload_len = 0;

            err = parse_entry_header(line, line_len, &kind, &path, &payload_len);
            free(kind);
            free(path);
            if (err != SCRIBE_OK) {
                return err;
            }
            if (payload_len > len - pos || pos + payload_len >= len || bytes[pos + payload_len] != '\n') {
                return scribe_set_error(SCRIBE_ECORRUPT, "bundle payload is truncated");
            }
            pos += payload_len + 1u;
        }
    }
    return scribe_set_error(SCRIBE_ECORRUPT, "bundle missing checksum");
}

/*
 * Imports a bundle into an already-open repository. The bundle checksum is
 * verified before any payload is trusted, and refs can optionally be constrained
 * to fast-forward updates for push/pull synchronization.
 */
scribe_error_t scribe_bundle_import_file(scribe_ctx *ctx, const char *file, const char *reason, int fast_forward_refs,
                                         size_t *out_storage_entries, size_t *out_ref_entries) {
    uint8_t *bytes = NULL;
    size_t len = 0;
    size_t pos = 0;
    size_t storage_entries = 0;
    size_t ref_entries = 0;
    scribe_error_t err;

    if (ctx == NULL || file == NULL || file[0] == '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "bundle import requires an open target and <file>");
    }
    err = scribe_read_file(file, &bytes, &len);
    if (err == SCRIBE_OK) {
        err = validate_bundle_bytes(bytes, len);
    }
    if (err != SCRIBE_OK) {
        free(bytes);
        return err;
    }
    {
        const char *line = NULL;
        size_t line_len = 0;

        err = read_bundle_line(bytes, len, &pos, &line, &line_len);
        if (err == SCRIBE_OK && (line_len != strlen(SCRIBE_BUNDLE_HEADER) - 1u ||
                                 memcmp(line, SCRIBE_BUNDLE_HEADER, strlen(SCRIBE_BUNDLE_HEADER) - 1u) != 0)) {
            err = scribe_set_error(SCRIBE_ECORRUPT, "invalid bundle header");
        }
    }
    while (err == SCRIBE_OK && pos < len) {
        size_t line_start = pos;
        const char *line = NULL;
        size_t line_len = 0;

        err = read_bundle_line(bytes, len, &pos, &line, &line_len);
        if (err != SCRIBE_OK) {
            break;
        }
        if (line_len > 9u && strncmp(line, "checksum ", 9) == 0) {
            (void)line_start;
            break;
        } else {
            char *kind = NULL;
            char *path = NULL;
            size_t payload_len = 0;
            const uint8_t *payload;

            err = parse_entry_header(line, line_len, &kind, &path, &payload_len);
            if (err == SCRIBE_OK) {
                if (payload_len > len - pos || pos + payload_len >= len || bytes[pos + payload_len] != '\n') {
                    err = scribe_set_error(SCRIBE_ECORRUPT, "bundle payload is truncated");
                } else {
                    payload = bytes + pos;
                    pos += payload_len + 1u;
                    err = import_bundle_entry_ex(ctx, kind, path, payload, payload_len, reason, fast_forward_refs);
                    if (err == SCRIBE_OK) {
                        if (strcmp(kind, "ref") == 0) {
                            ref_entries++;
                        } else {
                            storage_entries++;
                        }
                    }
                }
            }
            free(kind);
            free(path);
        }
    }
    if (err == SCRIBE_OK) {
        err = scribe_pack_validate_all(ctx, NULL);
    }
    if (err == SCRIBE_OK) {
        if (out_storage_entries != NULL) {
            *out_storage_entries = storage_entries;
        }
        if (out_ref_entries != NULL) {
            *out_ref_entries = ref_entries;
        }
    }
    free(bytes);
    return err;
}

/*
 * Imports a bundle into a target repository path. The bundle checksum is
 * verified before any payload is trusted, and all imported refs go through
 * scribe_refs_update() so the target records local reflog entries.
 */
scribe_error_t scribe_cli_bundle_import(const char *file, const char *target) {
    scribe_ctx *ctx = NULL;
    size_t storage_entries = 0;
    size_t ref_entries = 0;
    scribe_error_t err;

    if (file == NULL || file[0] == '\0' || target == NULL || target[0] == '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "bundle import requires <file> and <target>");
    }
    err = ensure_import_target(target);
    if (err == SCRIBE_OK) {
        err = scribe_open(target, 1, &ctx);
    }
    if (err == SCRIBE_OK) {
        err = scribe_bundle_import_file(ctx, file, "bundle-import", 0, &storage_entries, &ref_entries);
    }
    if (err == SCRIBE_OK) {
        printf("imported bundle %s into %s: %zu storage entry(s), %zu ref(s)\n", file, target, storage_entries,
               ref_entries);
    }
    scribe_close(ctx);
    return err;
}
