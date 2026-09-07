/*
 * Pack-file storage backend.
 *
 * Packs are an additive v2 storage format: the object envelope and hash domain
 * stay exactly the same as loose objects, but many compressed envelopes can be
 * stored in one `.pack` file with a companion `.idx` for hash lookup. This file
 * implements the private one-shot writer used by M1 tests, the dual-read lookup
 * helpers used by object.c, and the validation path used by fsck.
 */
#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"
#include "util/leb128.h"

#include "blake3.h"
#include "zstd.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SCRIBE_PACK_VERSION 1u
#define SCRIBE_PACK_HEADER_SIZE 56u
#define SCRIBE_PACK_INDEX_HEADER_SIZE 32u
#define SCRIBE_PACK_FANOUT_SIZE 256u
#define SCRIBE_PACK_INDEX_ENTRY_SIZE 65u
#define SCRIBE_PACK_RECORD_HEADER_MAX (SCRIBE_HASH_SIZE + 1u + 10u + 10u + 10u)

typedef struct {
    uint8_t hash[SCRIBE_HASH_SIZE];
    uint64_t offset;
    uint64_t compressed_len;
    uint64_t uncompressed_len;
    uint8_t type;
    uint64_t namespace_tag;
} scribe_pack_index_entry;

typedef struct {
    uint8_t pack_id[16];
    scribe_pack_index_entry *entries;
    size_t count;
} scribe_pack_index;

typedef struct {
    uint8_t pack_id[16];
    uint64_t flags;
    uint64_t object_count;
    uint64_t dictionary_offset;
    uint64_t namespace_table_offset;
    const uint8_t *dictionary;
    size_t dictionary_len;
} scribe_pack_header;

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} pack_buffer;

struct scribe_pack_writer {
    scribe_ctx *ctx;
    uint8_t pack_id[16];
    FILE *file;
    char *pack_tmp_path;
    char *pack_final_path;
    char *idx_final_path;
    scribe_pack_index_entry *entries;
    scribe_hash_set seen;
    size_t count;
    size_t cap;
    uint64_t offset;
    uint64_t compressed_bytes;
};

static uint64_t g_pack_id_counter = 0;
static scribe_perf_trace g_perf_trace;

int scribe_perf_trace_enabled(void) {
    const char *value = getenv("SCRIBE_TRACE_PERF");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

void scribe_perf_trace_reset(void) { memset(&g_perf_trace, 0, sizeof(g_perf_trace)); }

void scribe_perf_trace_snapshot(scribe_perf_trace *out) {
    if (out != NULL) {
        *out = g_perf_trace;
    }
}

void scribe_perf_trace_add_object_type_count_ns(uint64_t ns) { g_perf_trace.object_type_count_ns += ns; }

uint64_t scribe_perf_now_ns(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static int is_object_type(uint8_t type) {
    return type == SCRIBE_OBJECT_BLOB || type == SCRIBE_OBJECT_TREE || type == SCRIBE_OBJECT_COMMIT;
}

static void hash_bytes(const uint8_t *bytes, size_t len, uint8_t out[SCRIBE_HASH_SIZE]) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, bytes, len);
    blake3_hasher_finalize(&hasher, out, SCRIBE_HASH_SIZE);
}

static scribe_error_t write_all_file(FILE *file, const void *data, size_t len) {
    if (len != 0u && fwrite(data, 1u, len, file) != len) {
        return scribe_set_error(SCRIBE_EIO, "failed to write pack file");
    }
    return SCRIBE_OK;
}

static scribe_error_t fsync_parent_dir(const char *path) {
    char *copy;
    char *slash;
    int fd;

    copy = strdup(path);
    if (copy == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate pack parent path");
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
        return scribe_set_error(SCRIBE_EIO, "failed to open pack parent directory");
    }
    if (fsync(fd) != 0) {
        close(fd);
        return scribe_set_error(SCRIBE_EIO, "failed to fsync pack parent directory");
    }
    close(fd);
    return SCRIBE_OK;
}

static scribe_error_t flush_and_sync_file(FILE *file) {
    int fd;

    if (fflush(file) != 0) {
        return scribe_set_error(SCRIBE_EIO, "failed to flush pack file");
    }
    fd = fileno(file);
    if (fd < 0 || fsync(fd) != 0) {
        return scribe_set_error(SCRIBE_EIO, "failed to fsync pack file");
    }
    return SCRIBE_OK;
}

static uint32_t read_u32le(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static uint64_t read_u64le(const uint8_t *p) {
    uint64_t value = 0;
    size_t i;

    for (i = 0; i < 8u; i++) {
        value |= ((uint64_t)p[i]) << (i * 8u);
    }
    return value;
}

static void write_u32le(uint8_t *p, uint32_t value) {
    size_t i;

    for (i = 0; i < 4u; i++) {
        p[i] = (uint8_t)((value >> (i * 8u)) & 0xffu);
    }
}

static void write_u64le(uint8_t *p, uint64_t value) {
    size_t i;

    for (i = 0; i < 8u; i++) {
        p[i] = (uint8_t)((value >> (i * 8u)) & 0xffu);
    }
}

static scribe_error_t size_from_u64(uint64_t value, size_t *out) {
    if (value > (uint64_t)SIZE_MAX) {
        return scribe_set_error(SCRIBE_ECORRUPT, "pack value exceeds addressable size");
    }
    *out = (size_t)value;
    return SCRIBE_OK;
}

static scribe_error_t payload_len_from_envelope_len(uint64_t envelope_len64, size_t *out) {
    size_t envelope_len;
    size_t leb_len;

    if (out == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid pack payload length output");
    }
    if (size_from_u64(envelope_len64, &envelope_len) != SCRIBE_OK) {
        return scribe_set_error(SCRIBE_ECORRUPT, "packed object envelope is too large");
    }
    if (envelope_len < 2u) {
        return scribe_set_error(SCRIBE_ECORRUPT, "packed object envelope length is invalid");
    }
    for (leb_len = 1u; leb_len <= 10u; leb_len++) {
        size_t payload_len;
        uint8_t leb[10];

        if (envelope_len < 1u + leb_len) {
            break;
        }
        payload_len = envelope_len - 1u - leb_len;
        if (scribe_leb128_encode((uint64_t)payload_len, leb) == leb_len) {
            *out = payload_len;
            return SCRIBE_OK;
        }
    }
    return scribe_set_error(SCRIBE_ECORRUPT, "packed object payload length is invalid");
}

static scribe_error_t buffer_reserve(pack_buffer *buf, size_t extra) {
    size_t needed;
    size_t cap;
    uint8_t *grown;

    if (extra > SIZE_MAX - buf->len) {
        return scribe_set_error(SCRIBE_ENOMEM, "pack buffer too large");
    }
    needed = buf->len + extra;
    if (needed <= buf->cap) {
        return SCRIBE_OK;
    }
    cap = buf->cap == 0u ? 4096u : buf->cap;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2u) {
            cap = needed;
            break;
        }
        cap *= 2u;
    }
    grown = (uint8_t *)realloc(buf->data, cap);
    if (grown == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to grow pack buffer");
    }
    buf->data = grown;
    buf->cap = cap;
    return SCRIBE_OK;
}

static scribe_error_t buffer_append(pack_buffer *buf, const void *data, size_t len) {
    scribe_error_t err;

    err = buffer_reserve(buf, len);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (len != 0u) {
        memcpy(buf->data + buf->len, data, len);
    }
    buf->len += len;
    return SCRIBE_OK;
}

static scribe_error_t buffer_append_u8(pack_buffer *buf, uint8_t value) { return buffer_append(buf, &value, 1u); }

static scribe_error_t buffer_append_u32le(pack_buffer *buf, uint32_t value) {
    uint8_t tmp[4];

    write_u32le(tmp, value);
    return buffer_append(buf, tmp, sizeof(tmp));
}

static scribe_error_t buffer_append_u64le(pack_buffer *buf, uint64_t value) {
    uint8_t tmp[8];

    write_u64le(tmp, value);
    return buffer_append(buf, tmp, sizeof(tmp));
}

static scribe_error_t buffer_append_leb(pack_buffer *buf, uint64_t value) {
    uint8_t tmp[10];
    size_t len = scribe_leb128_encode(value, tmp);

    return buffer_append(buf, tmp, len);
}

static scribe_error_t build_envelope(uint8_t type, const uint8_t *payload, size_t payload_len, uint8_t **out,
                                     size_t *out_len) {
    uint8_t leb[10];
    size_t leb_len;
    uint8_t *buf;

    leb_len = scribe_leb128_encode((uint64_t)payload_len, leb);
    if (payload_len > SIZE_MAX - 1u - leb_len) {
        return scribe_set_error(SCRIBE_ENOMEM, "object envelope too large");
    }
    buf = (uint8_t *)malloc(1u + leb_len + payload_len);
    if (buf == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate object envelope");
    }
    buf[0] = type;
    memcpy(buf + 1u, leb, leb_len);
    if (payload_len != 0u) {
        memcpy(buf + 1u + leb_len, payload, payload_len);
    }
    *out = buf;
    *out_len = 1u + leb_len + payload_len;
    return SCRIBE_OK;
}

static void pack_id_to_hex(const uint8_t id[16], char out[33]) {
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < 16u; i++) {
        out[i * 2u] = digits[id[i] >> 4u];
        out[i * 2u + 1u] = digits[id[i] & 0x0fu];
    }
    out[32] = '\0';
}

static int has_suffix(const char *name, const char *suffix);

static int is_pack_id_hex(const char *name) {
    size_t i;

    if (name == NULL || strlen(name) < 36u) {
        return 0;
    }
    for (i = 0; i < 32u; i++) {
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int hex_nibble(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    return -1;
}

static int pack_id_from_idx_name(const char *name, uint8_t out[16]) {
    size_t i;

    if (!is_pack_id_hex(name) || !has_suffix(name, ".idx") || strlen(name) != 36u) {
        return 0;
    }
    for (i = 0; i < 16u; i++) {
        int hi = hex_nibble(name[i * 2u]);
        int lo = hex_nibble(name[i * 2u + 1u]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        out[i] = (uint8_t)(((unsigned)hi << 4u) | (unsigned)lo);
    }
    return 1;
}

static int has_suffix(const char *name, const char *suffix) {
    size_t name_len;
    size_t suffix_len;

    if (name == NULL || suffix == NULL) {
        return 0;
    }
    name_len = strlen(name);
    suffix_len = strlen(suffix);
    return name_len >= suffix_len && strcmp(name + name_len - suffix_len, suffix) == 0;
}

static char *pack_dir_path(scribe_ctx *ctx) { return scribe_path_join(ctx->repo_path, "packs"); }

static char *pack_child_path(scribe_ctx *ctx, const char *name) {
    char *packs;
    char *path;

    packs = pack_dir_path(ctx);
    if (packs == NULL) {
        return NULL;
    }
    path = scribe_path_join(packs, name);
    free(packs);
    return path;
}

static char *pack_file_path_from_id(scribe_ctx *ctx, const uint8_t pack_id[16], const char *suffix) {
    char id_hex[33];
    char name[40];

    pack_id_to_hex(pack_id, id_hex);
    if (snprintf(name, sizeof(name), "%s%s", id_hex, suffix) >= (int)sizeof(name)) {
        (void)scribe_set_error(SCRIBE_EPATH, "pack file name too long");
        return NULL;
    }
    return pack_child_path(ctx, name);
}

static char *pack_path_from_idx_name(scribe_ctx *ctx, const char *idx_name) {
    char pack_name[40];

    if (!is_pack_id_hex(idx_name) || !has_suffix(idx_name, ".idx") || strlen(idx_name) != 36u) {
        return NULL;
    }
    memcpy(pack_name, idx_name, 32u);
    memcpy(pack_name + 32u, ".pack", 6u);
    return pack_child_path(ctx, pack_name);
}

static char *idx_path_from_idx_name(scribe_ctx *ctx, const char *idx_name) {
    if (!is_pack_id_hex(idx_name) || !has_suffix(idx_name, ".idx") || strlen(idx_name) != 36u) {
        return NULL;
    }
    return pack_child_path(ctx, idx_name);
}

static void make_pack_id(scribe_pack_object *objects, size_t count, uint8_t out[16]) {
    blake3_hasher hasher;
    uint8_t full[SCRIBE_HASH_SIZE];
    struct timespec ts;
    uint64_t value;
    size_t i;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    }
    blake3_hasher_init(&hasher);
    value = (uint64_t)ts.tv_sec;
    blake3_hasher_update(&hasher, &value, sizeof(value));
    value = (uint64_t)ts.tv_nsec;
    blake3_hasher_update(&hasher, &value, sizeof(value));
    value = (uint64_t)(unsigned long)getpid();
    blake3_hasher_update(&hasher, &value, sizeof(value));
    value = (uint64_t)count;
    blake3_hasher_update(&hasher, &value, sizeof(value));
    for (i = 0; i < count; i++) {
        blake3_hasher_update(&hasher, &objects[i].type, sizeof(objects[i].type));
        blake3_hasher_update(&hasher, objects[i].hash, SCRIBE_HASH_SIZE);
    }
    blake3_hasher_finalize(&hasher, full, sizeof(full));
    memcpy(out, full, 16u);
}

static void make_stream_pack_id(scribe_ctx *ctx, uint8_t out[16]) {
    blake3_hasher hasher;
    uint8_t full[SCRIBE_HASH_SIZE];
    struct timespec ts;
    uint64_t value;
    const char *repo = ctx == NULL ? "" : ctx->repo_path;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    }
    blake3_hasher_init(&hasher);
    value = (uint64_t)ts.tv_sec;
    blake3_hasher_update(&hasher, &value, sizeof(value));
    value = (uint64_t)ts.tv_nsec;
    blake3_hasher_update(&hasher, &value, sizeof(value));
    value = (uint64_t)(unsigned long)getpid();
    blake3_hasher_update(&hasher, &value, sizeof(value));
    value = g_pack_id_counter++;
    blake3_hasher_update(&hasher, &value, sizeof(value));
    blake3_hasher_update(&hasher, repo, strlen(repo));
    blake3_hasher_finalize(&hasher, full, sizeof(full));
    memcpy(out, full, 16u);
}

static scribe_error_t pack_writer_reserve(scribe_pack_writer *writer) {
    scribe_pack_index_entry *grown;
    size_t new_cap;

    if (writer->count < writer->cap) {
        return SCRIBE_OK;
    }
    new_cap = writer->cap == 0u ? 1024u : writer->cap * 2u;
    grown = (scribe_pack_index_entry *)realloc(writer->entries, sizeof(*writer->entries) * new_cap);
    if (grown == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to grow streaming pack index");
    }
    writer->entries = grown;
    writer->cap = new_cap;
    return SCRIBE_OK;
}

static scribe_error_t write_pack_header_to_file(FILE *file, const uint8_t pack_id[16], uint64_t object_count,
                                                uint64_t namespace_table_offset) {
    uint8_t header[SCRIBE_PACK_HEADER_SIZE];

    memset(header, 0, sizeof(header));
    memcpy(header, "SPCK", 4u);
    write_u32le(header + 4u, SCRIBE_PACK_VERSION);
    memcpy(header + 8u, pack_id, 16u);
    write_u64le(header + 24u, 0u);
    write_u64le(header + 32u, object_count);
    write_u64le(header + 40u, 0u);
    write_u64le(header + 48u, namespace_table_offset);
    if (fseeko(file, 0, SEEK_SET) != 0) {
        return scribe_set_error(SCRIBE_EIO, "failed to seek pack header");
    }
    if (fwrite(header, 1u, sizeof(header), file) != sizeof(header)) {
        return scribe_set_error(SCRIBE_EIO, "failed to write pack header");
    }
    if (fseeko(file, 0, SEEK_END) != 0) {
        return scribe_set_error(SCRIBE_EIO, "failed to seek pack end");
    }
    return SCRIBE_OK;
}

static int compare_index_entries(const void *a, const void *b) {
    const scribe_pack_index_entry *ea = (const scribe_pack_index_entry *)a;
    const scribe_pack_index_entry *eb = (const scribe_pack_index_entry *)b;

    return memcmp(ea->hash, eb->hash, SCRIBE_HASH_SIZE);
}

static scribe_error_t build_index(const uint8_t pack_id[16], scribe_pack_index_entry *entries, size_t count,
                                  pack_buffer *out) {
    uint32_t fanout[SCRIBE_PACK_FANOUT_SIZE];
    uint32_t cumulative = 0;
    size_t i;

    if (count > (size_t)UINT32_MAX) {
        return scribe_set_error(SCRIBE_EINVAL, "pack too large for v2 fanout table");
    }
    memset(fanout, 0, sizeof(fanout));
    qsort(entries, count, sizeof(*entries), compare_index_entries);
    for (i = 0; i < count; i++) {
        if (i != 0u && memcmp(entries[i - 1u].hash, entries[i].hash, SCRIBE_HASH_SIZE) == 0) {
            return scribe_set_error(SCRIBE_EEXISTS, "duplicate object in pack");
        }
        fanout[entries[i].hash[0]]++;
    }
    for (i = 0; i < SCRIBE_PACK_FANOUT_SIZE; i++) {
        if (fanout[i] > UINT32_MAX - cumulative) {
            return scribe_set_error(SCRIBE_ECORRUPT, "pack fanout overflow");
        }
        cumulative += fanout[i];
        fanout[i] = cumulative;
    }
    if (buffer_append(out, "SIDX", 4u) != SCRIBE_OK || buffer_append_u32le(out, SCRIBE_PACK_VERSION) != SCRIBE_OK ||
        buffer_append(out, pack_id, 16u) != SCRIBE_OK || buffer_append_u64le(out, (uint64_t)count) != SCRIBE_OK) {
        return SCRIBE_ENOMEM;
    }
    for (i = 0; i < SCRIBE_PACK_FANOUT_SIZE; i++) {
        scribe_error_t err = buffer_append_u32le(out, fanout[i]);
        if (err != SCRIBE_OK) {
            return err;
        }
    }
    for (i = 0; i < count; i++) {
        scribe_error_t err;
        err = buffer_append(out, entries[i].hash, SCRIBE_HASH_SIZE);
        if (err == SCRIBE_OK) {
            err = buffer_append_u64le(out, entries[i].offset);
        }
        if (err == SCRIBE_OK) {
            err = buffer_append_u64le(out, entries[i].compressed_len);
        }
        if (err == SCRIBE_OK) {
            err = buffer_append_u64le(out, entries[i].uncompressed_len);
        }
        if (err == SCRIBE_OK) {
            err = buffer_append_u8(out, entries[i].type);
        }
        if (err == SCRIBE_OK) {
            err = buffer_append_u64le(out, entries[i].namespace_tag);
        }
        if (err != SCRIBE_OK) {
            return err;
        }
    }
    return SCRIBE_OK;
}

scribe_error_t scribe_pack_write_objects(scribe_ctx *ctx, scribe_pack_object *objects, size_t count,
                                         uint8_t out_pack_id[16]) {
    pack_buffer pack = {0};
    pack_buffer idx = {0};
    scribe_pack_index_entry *entries = NULL;
    uint8_t pack_id[16];
    char *packs = NULL;
    char *pack_path = NULL;
    char *idx_path = NULL;
    size_t i;
    scribe_error_t err = SCRIBE_OK;

    if (ctx == NULL || objects == NULL || count == 0u) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid pack write arguments");
    }
    for (i = 0; i < count; i++) {
        uint8_t *envelope = NULL;
        size_t envelope_len = 0;

        if (!is_object_type(objects[i].type) || (objects[i].payload == NULL && objects[i].payload_len != 0u)) {
            return scribe_set_error(SCRIBE_EINVAL, "invalid packed object");
        }
        err = build_envelope(objects[i].type, objects[i].payload, objects[i].payload_len, &envelope, &envelope_len);
        if (err != SCRIBE_OK) {
            return err;
        }
        hash_bytes(envelope, envelope_len, objects[i].hash);
        free(envelope);
    }
    make_pack_id(objects, count, pack_id);
    if (out_pack_id != NULL) {
        memcpy(out_pack_id, pack_id, 16u);
    }
    packs = pack_dir_path(ctx);
    if (packs == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = scribe_mkdir_p(packs);
    free(packs);
    if (err != SCRIBE_OK) {
        return err;
    }
    entries = (scribe_pack_index_entry *)calloc(count, sizeof(*entries));
    if (entries == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate pack index entries");
    }
    err = buffer_append(&pack, "SPCK", 4u);
    if (err == SCRIBE_OK) {
        err = buffer_append_u32le(&pack, SCRIBE_PACK_VERSION);
    }
    if (err == SCRIBE_OK) {
        err = buffer_append(&pack, pack_id, 16u);
    }
    if (err == SCRIBE_OK) {
        err = buffer_append_u64le(&pack, 0u);
    }
    if (err == SCRIBE_OK) {
        err = buffer_append_u64le(&pack, (uint64_t)count);
    }
    if (err == SCRIBE_OK) {
        err = buffer_append_u64le(&pack, 0u);
    }
    if (err == SCRIBE_OK) {
        err = buffer_append_u64le(&pack, 0u);
    }
    for (i = 0; err == SCRIBE_OK && i < count; i++) {
        uint8_t *envelope = NULL;
        uint8_t *compressed = NULL;
        size_t envelope_len = 0;
        size_t bound;
        size_t compressed_len;

        err = build_envelope(objects[i].type, objects[i].payload, objects[i].payload_len, &envelope, &envelope_len);
        if (err != SCRIBE_OK) {
            break;
        }
        bound = ZSTD_compressBound(envelope_len);
        compressed = (uint8_t *)malloc(bound);
        if (compressed == NULL) {
            free(envelope);
            err = scribe_set_error(SCRIBE_ENOMEM, "failed to allocate packed compressed object");
            break;
        }
        compressed_len = ZSTD_compress(compressed, bound, envelope, envelope_len, ctx->config.compression_level);
        if (ZSTD_isError(compressed_len)) {
            free(envelope);
            free(compressed);
            err = scribe_set_error(SCRIBE_EIO, "zstd compression failed: %s", ZSTD_getErrorName(compressed_len));
            break;
        }
        memcpy(entries[i].hash, objects[i].hash, SCRIBE_HASH_SIZE);
        entries[i].offset = (uint64_t)pack.len;
        entries[i].compressed_len = (uint64_t)compressed_len;
        entries[i].uncompressed_len = (uint64_t)envelope_len;
        entries[i].type = objects[i].type;
        entries[i].namespace_tag = 0u;
        err = buffer_append(&pack, objects[i].hash, SCRIBE_HASH_SIZE);
        if (err == SCRIBE_OK) {
            err = buffer_append_u8(&pack, objects[i].type);
        }
        if (err == SCRIBE_OK) {
            err = buffer_append_leb(&pack, (uint64_t)envelope_len);
        }
        if (err == SCRIBE_OK) {
            err = buffer_append_leb(&pack, (uint64_t)compressed_len);
        }
        if (err == SCRIBE_OK) {
            err = buffer_append_leb(&pack, 0u);
        }
        if (err == SCRIBE_OK) {
            err = buffer_append(&pack, compressed, compressed_len);
        }
        free(envelope);
        free(compressed);
    }
    if (err == SCRIBE_OK) {
        write_u64le(pack.data + 48u, (uint64_t)pack.len);
        err = buffer_append_leb(&pack, 0u);
    }
    if (err == SCRIBE_OK) {
        err = build_index(pack_id, entries, count, &idx);
    }
    if (err == SCRIBE_OK) {
        pack_path = pack_file_path_from_id(ctx, pack_id, ".pack");
        idx_path = pack_file_path_from_id(ctx, pack_id, ".idx");
        if (pack_path == NULL || idx_path == NULL) {
            err = SCRIBE_ENOMEM;
        }
    }
    if (err == SCRIBE_OK) {
        err = scribe_write_file_atomic(pack_path, pack.data, pack.len);
    }
    if (err == SCRIBE_OK) {
        err = scribe_write_file_atomic(idx_path, idx.data, idx.len);
    }
    free(pack_path);
    free(idx_path);
    free(entries);
    free(pack.data);
    free(idx.data);
    return err;
}

scribe_error_t scribe_pack_writer_begin(scribe_ctx *ctx, scribe_pack_writer **out) {
    scribe_pack_writer *writer;
    char *packs;
    size_t tmp_len;
    scribe_error_t err;

    if (ctx == NULL || out == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid pack writer arguments");
    }
    *out = NULL;
    writer = (scribe_pack_writer *)calloc(1u, sizeof(*writer));
    if (writer == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate pack writer");
    }
    writer->ctx = ctx;
    make_stream_pack_id(ctx, writer->pack_id);
    packs = pack_dir_path(ctx);
    if (packs == NULL) {
        free(writer);
        return SCRIBE_ENOMEM;
    }
    err = scribe_mkdir_p(packs);
    free(packs);
    if (err != SCRIBE_OK) {
        free(writer);
        return err;
    }
    writer->pack_final_path = pack_file_path_from_id(ctx, writer->pack_id, ".pack");
    writer->idx_final_path = pack_file_path_from_id(ctx, writer->pack_id, ".idx");
    if (writer->pack_final_path == NULL || writer->idx_final_path == NULL) {
        scribe_pack_writer_abort(&writer);
        return SCRIBE_ENOMEM;
    }
    tmp_len = strlen(writer->pack_final_path) + 32u;
    writer->pack_tmp_path = (char *)malloc(tmp_len);
    if (writer->pack_tmp_path == NULL) {
        scribe_pack_writer_abort(&writer);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate pack temporary path");
    }
    if (snprintf(writer->pack_tmp_path, tmp_len, "%s.tmp.%ld", writer->pack_final_path, (long)getpid()) >=
        (int)tmp_len) {
        scribe_pack_writer_abort(&writer);
        return scribe_set_error(SCRIBE_EPATH, "pack temporary path too long");
    }
    writer->file = fopen(writer->pack_tmp_path, "wb+");
    if (writer->file == NULL) {
        scribe_pack_writer_abort(&writer);
        return scribe_set_error(SCRIBE_EIO, "failed to create temporary pack file");
    }
    err = write_pack_header_to_file(writer->file, writer->pack_id, 0u, 0u);
    if (err != SCRIBE_OK) {
        scribe_pack_writer_abort(&writer);
        return err;
    }
    writer->offset = SCRIBE_PACK_HEADER_SIZE;
    *out = writer;
    return SCRIBE_OK;
}

static scribe_error_t pack_writer_write(scribe_pack_writer *writer, const void *data, size_t len) {
    scribe_error_t err;

    if (len > UINT64_MAX - writer->offset) {
        return scribe_set_error(SCRIBE_ENOMEM, "streaming pack is too large");
    }
    err = write_all_file(writer->file, data, len);
    if (err != SCRIBE_OK) {
        return err;
    }
    writer->offset += (uint64_t)len;
    return SCRIBE_OK;
}

scribe_error_t scribe_pack_writer_add(scribe_pack_writer *writer, uint8_t type, const uint8_t *payload,
                                      size_t payload_len, uint8_t out_hash[SCRIBE_HASH_SIZE]) {
    uint8_t *envelope = NULL;
    uint8_t *compressed = NULL;
    size_t envelope_len = 0;
    size_t compressed_bound;
    size_t compressed_len;
    uint8_t leb_uncompressed[10];
    uint8_t leb_compressed[10];
    uint8_t leb_namespace[10];
    size_t leb_uncompressed_len;
    size_t leb_compressed_len;
    size_t leb_namespace_len;
    scribe_pack_index_entry *entry;
    uint64_t record_offset;
    int already = 0;
    scribe_error_t err;

    if (writer == NULL || writer->file == NULL || !is_object_type(type) || (payload == NULL && payload_len != 0u)) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid pack writer add arguments");
    }
    err = build_envelope(type, payload, payload_len, &envelope, &envelope_len);
    if (err != SCRIBE_OK) {
        return err;
    }
    hash_bytes(envelope, envelope_len, out_hash);
    err = scribe_hash_set_add(&writer->seen, out_hash, &already);
    if (err != SCRIBE_OK) {
        free(envelope);
        return err;
    }
    if (already) {
        free(envelope);
        return SCRIBE_OK;
    }
    compressed_bound = ZSTD_compressBound(envelope_len);
    compressed = (uint8_t *)malloc(compressed_bound);
    if (compressed == NULL) {
        free(envelope);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate packed compressed object");
    }
    compressed_len =
        ZSTD_compress(compressed, compressed_bound, envelope, envelope_len, writer->ctx->config.compression_level);
    free(envelope);
    if (ZSTD_isError(compressed_len)) {
        free(compressed);
        return scribe_set_error(SCRIBE_EIO, "zstd compression failed: %s", ZSTD_getErrorName(compressed_len));
    }
    err = pack_writer_reserve(writer);
    if (err != SCRIBE_OK) {
        free(compressed);
        return err;
    }
    leb_uncompressed_len = scribe_leb128_encode((uint64_t)envelope_len, leb_uncompressed);
    leb_compressed_len = scribe_leb128_encode((uint64_t)compressed_len, leb_compressed);
    leb_namespace_len = scribe_leb128_encode(0u, leb_namespace);
    record_offset = writer->offset;
    err = pack_writer_write(writer, out_hash, SCRIBE_HASH_SIZE);
    if (err == SCRIBE_OK) {
        err = pack_writer_write(writer, &type, 1u);
    }
    if (err == SCRIBE_OK) {
        err = pack_writer_write(writer, leb_uncompressed, leb_uncompressed_len);
    }
    if (err == SCRIBE_OK) {
        err = pack_writer_write(writer, leb_compressed, leb_compressed_len);
    }
    if (err == SCRIBE_OK) {
        err = pack_writer_write(writer, leb_namespace, leb_namespace_len);
    }
    if (err == SCRIBE_OK) {
        err = pack_writer_write(writer, compressed, compressed_len);
    }
    free(compressed);
    if (err != SCRIBE_OK) {
        return err;
    }
    entry = &writer->entries[writer->count++];
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->hash, out_hash, SCRIBE_HASH_SIZE);
    entry->offset = record_offset;
    entry->compressed_len = (uint64_t)compressed_len;
    entry->uncompressed_len = (uint64_t)envelope_len;
    entry->type = type;
    entry->namespace_tag = 0u;
    if ((uint64_t)compressed_len > UINT64_MAX - writer->compressed_bytes) {
        writer->compressed_bytes = UINT64_MAX;
    } else {
        writer->compressed_bytes += (uint64_t)compressed_len;
    }
    return SCRIBE_OK;
}

void scribe_pack_writer_stats(scribe_pack_writer *writer, size_t *out_objects, uint64_t *out_compressed_bytes) {
    if (out_objects != NULL) {
        *out_objects = writer == NULL ? 0u : writer->count;
    }
    if (out_compressed_bytes != NULL) {
        *out_compressed_bytes = writer == NULL ? 0u : writer->compressed_bytes;
    }
}

scribe_error_t scribe_pack_writer_finish(scribe_pack_writer **writer_ptr, uint8_t out_pack_id[16]) {
    scribe_pack_writer *writer;
    pack_buffer idx = {0};
    uint64_t namespace_table_offset;
    scribe_error_t err = SCRIBE_OK;

    if (writer_ptr == NULL || *writer_ptr == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid pack writer finish arguments");
    }
    writer = *writer_ptr;
    if (out_pack_id != NULL) {
        memcpy(out_pack_id, writer->pack_id, 16u);
    }
    if (writer->count == 0u) {
        scribe_pack_writer_abort(writer_ptr);
        return SCRIBE_OK;
    }
    namespace_table_offset = writer->offset;
    err = pack_writer_write(writer, "\0", 1u);
    if (err == SCRIBE_OK) {
        err = write_pack_header_to_file(writer->file, writer->pack_id, (uint64_t)writer->count, namespace_table_offset);
    }
    if (err == SCRIBE_OK) {
        err = flush_and_sync_file(writer->file);
    }
    if (writer->file != NULL) {
        if (fclose(writer->file) != 0 && err == SCRIBE_OK) {
            err = scribe_set_error(SCRIBE_EIO, "failed to close temporary pack file");
        }
        writer->file = NULL;
    }
    if (err == SCRIBE_OK) {
        err = build_index(writer->pack_id, writer->entries, writer->count, &idx);
    }
    if (err == SCRIBE_OK && rename(writer->pack_tmp_path, writer->pack_final_path) != 0) {
        err = scribe_set_error(SCRIBE_EIO, "failed to publish pack file");
    }
    if (err == SCRIBE_OK) {
        err = fsync_parent_dir(writer->pack_final_path);
    }
    if (err == SCRIBE_OK) {
        err = scribe_write_file_atomic(writer->idx_final_path, idx.data, idx.len);
    }
    free(idx.data);
    if (err != SCRIBE_OK) {
        scribe_pack_writer_abort(writer_ptr);
        return err;
    }
    free(writer->pack_tmp_path);
    free(writer->pack_final_path);
    free(writer->idx_final_path);
    free(writer->entries);
    scribe_hash_set_destroy(&writer->seen);
    free(writer);
    *writer_ptr = NULL;
    return SCRIBE_OK;
}

void scribe_pack_writer_abort(scribe_pack_writer **writer_ptr) {
    scribe_pack_writer *writer;

    if (writer_ptr == NULL || *writer_ptr == NULL) {
        return;
    }
    writer = *writer_ptr;
    if (writer->file != NULL) {
        fclose(writer->file);
        writer->file = NULL;
    }
    if (writer->pack_tmp_path != NULL) {
        unlink(writer->pack_tmp_path);
    }
    free(writer->pack_tmp_path);
    free(writer->pack_final_path);
    free(writer->idx_final_path);
    free(writer->entries);
    scribe_hash_set_destroy(&writer->seen);
    free(writer);
    *writer_ptr = NULL;
}

static void pack_index_destroy(scribe_pack_index *idx) {
    if (idx != NULL) {
        free(idx->entries);
        memset(idx, 0, sizeof(*idx));
    }
}

static scribe_error_t parse_index_file(const char *path, scribe_pack_index *out) {
    uint8_t *bytes = NULL;
    size_t len = 0;
    uint64_t count64;
    size_t count;
    size_t expected_len;
    size_t off;
    uint32_t fanout[SCRIBE_PACK_FANOUT_SIZE];
    uint32_t counts[SCRIBE_PACK_FANOUT_SIZE];
    uint32_t cumulative = 0;
    size_t i;
    scribe_error_t err;

    g_perf_trace.pack_index_parses++;
    memset(out, 0, sizeof(*out));
    err = scribe_read_file(path, &bytes, &len);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (len < SCRIBE_PACK_INDEX_HEADER_SIZE + SCRIBE_PACK_FANOUT_SIZE * 4u || memcmp(bytes, "SIDX", 4u) != 0 ||
        read_u32le(bytes + 4u) != SCRIBE_PACK_VERSION) {
        free(bytes);
        return scribe_set_error(SCRIBE_ECORRUPT, "invalid pack index header");
    }
    memcpy(out->pack_id, bytes + 8u, 16u);
    count64 = read_u64le(bytes + 24u);
    if (count64 > (uint64_t)UINT32_MAX || count64 > (uint64_t)(SIZE_MAX / sizeof(*out->entries))) {
        free(bytes);
        return scribe_set_error(SCRIBE_ECORRUPT, "pack index too large");
    }
    count = (size_t)count64;
    if (count >
        (SIZE_MAX - SCRIBE_PACK_INDEX_HEADER_SIZE - SCRIBE_PACK_FANOUT_SIZE * 4u) / SCRIBE_PACK_INDEX_ENTRY_SIZE) {
        free(bytes);
        return scribe_set_error(SCRIBE_ECORRUPT, "pack index length overflow");
    }
    expected_len = SCRIBE_PACK_INDEX_HEADER_SIZE + SCRIBE_PACK_FANOUT_SIZE * 4u + count * SCRIBE_PACK_INDEX_ENTRY_SIZE;
    if (len != expected_len) {
        free(bytes);
        return scribe_set_error(SCRIBE_ECORRUPT, "pack index length mismatch");
    }
    out->entries = (scribe_pack_index_entry *)calloc(count == 0u ? 1u : count, sizeof(*out->entries));
    if (out->entries == NULL) {
        free(bytes);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate pack index");
    }
    memset(counts, 0, sizeof(counts));
    off = SCRIBE_PACK_INDEX_HEADER_SIZE;
    for (i = 0; i < SCRIBE_PACK_FANOUT_SIZE; i++) {
        fanout[i] = read_u32le(bytes + off);
        off += 4u;
        if (fanout[i] < cumulative || fanout[i] > (uint32_t)count) {
            pack_index_destroy(out);
            free(bytes);
            return scribe_set_error(SCRIBE_ECORRUPT, "pack index fanout is not monotonic");
        }
        cumulative = fanout[i];
    }
    if (cumulative != (uint32_t)count) {
        pack_index_destroy(out);
        free(bytes);
        return scribe_set_error(SCRIBE_ECORRUPT, "pack index fanout count mismatch");
    }
    for (i = 0; i < count; i++) {
        memcpy(out->entries[i].hash, bytes + off, SCRIBE_HASH_SIZE);
        off += SCRIBE_HASH_SIZE;
        out->entries[i].offset = read_u64le(bytes + off);
        off += 8u;
        out->entries[i].compressed_len = read_u64le(bytes + off);
        off += 8u;
        out->entries[i].uncompressed_len = read_u64le(bytes + off);
        off += 8u;
        out->entries[i].type = bytes[off];
        off += 1u;
        out->entries[i].namespace_tag = read_u64le(bytes + off);
        off += 8u;
        if (!is_object_type(out->entries[i].type)) {
            pack_index_destroy(out);
            free(bytes);
            return scribe_set_error(SCRIBE_ECORRUPT, "pack index object type is invalid");
        }
        if (i != 0u && memcmp(out->entries[i - 1u].hash, out->entries[i].hash, SCRIBE_HASH_SIZE) >= 0) {
            pack_index_destroy(out);
            free(bytes);
            return scribe_set_error(SCRIBE_ECORRUPT, "pack index entries are not strictly sorted");
        }
        counts[out->entries[i].hash[0]]++;
    }
    cumulative = 0;
    for (i = 0; i < SCRIBE_PACK_FANOUT_SIZE; i++) {
        if (counts[i] > UINT32_MAX - cumulative) {
            pack_index_destroy(out);
            free(bytes);
            return scribe_set_error(SCRIBE_ECORRUPT, "pack index fanout overflow");
        }
        cumulative += counts[i];
        if (fanout[i] != cumulative) {
            pack_index_destroy(out);
            free(bytes);
            return scribe_set_error(SCRIBE_ECORRUPT, "pack index fanout does not match entries");
        }
    }
    out->count = count;
    free(bytes);
    return SCRIBE_OK;
}

static const scribe_pack_index_entry *pack_index_find(const scribe_pack_index *idx,
                                                      const uint8_t hash[SCRIBE_HASH_SIZE]) {
    size_t lo = 0;
    size_t hi = idx->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        int cmp = memcmp(idx->entries[mid].hash, hash, SCRIBE_HASH_SIZE);
        if (cmp == 0) {
            return &idx->entries[mid];
        }
        if (cmp < 0) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

static scribe_error_t parse_pack_header(const uint8_t *bytes, size_t len, const uint8_t expected_pack_id[16],
                                        size_t expected_count, scribe_pack_header *out) {
    uint64_t namespace_count;
    size_t dict_off;
    size_t namespace_off;
    size_t used;
    scribe_error_t err;

    if (len < SCRIBE_PACK_HEADER_SIZE || memcmp(bytes, "SPCK", 4u) != 0 ||
        read_u32le(bytes + 4u) != SCRIBE_PACK_VERSION) {
        return scribe_set_error(SCRIBE_ECORRUPT, "invalid pack header");
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->pack_id, bytes + 8u, 16u);
    if (memcmp(out->pack_id, expected_pack_id, 16u) != 0) {
        return scribe_set_error(SCRIBE_ECORRUPT, "pack/index id mismatch");
    }
    out->flags = read_u64le(bytes + 24u);
    out->object_count = read_u64le(bytes + 32u);
    out->dictionary_offset = read_u64le(bytes + 40u);
    out->namespace_table_offset = read_u64le(bytes + 48u);
    if (out->flags != 0u || out->object_count != (uint64_t)expected_count) {
        return scribe_set_error(SCRIBE_ECORRUPT, "pack header values do not match index");
    }
    err = size_from_u64(out->namespace_table_offset, &namespace_off);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (namespace_off < SCRIBE_PACK_HEADER_SIZE || namespace_off >= len) {
        return scribe_set_error(SCRIBE_ECORRUPT, "pack namespace table offset is invalid");
    }
    err = scribe_leb128_decode(bytes + namespace_off, len - namespace_off, &namespace_count, &used);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (out->dictionary_offset != 0u) {
        uint64_t dict_len64;
        err = size_from_u64(out->dictionary_offset, &dict_off);
        if (err != SCRIBE_OK) {
            return err;
        }
        if (dict_off < SCRIBE_PACK_HEADER_SIZE || dict_off >= namespace_off) {
            return scribe_set_error(SCRIBE_ECORRUPT, "pack dictionary offset is invalid");
        }
        err = scribe_leb128_decode(bytes + dict_off, namespace_off - dict_off, &dict_len64, &used);
        if (err != SCRIBE_OK) {
            return err;
        }
        if (dict_len64 > (uint64_t)(namespace_off - dict_off - used)) {
            return scribe_set_error(SCRIBE_ECORRUPT, "pack dictionary length is invalid");
        }
        out->dictionary = bytes + dict_off + used;
        out->dictionary_len = (size_t)dict_len64;
    }
    return SCRIBE_OK;
}

static scribe_error_t unpack_envelope(uint8_t *envelope, size_t envelope_len, const uint8_t hash[SCRIBE_HASH_SIZE],
                                      uint8_t expected_type, scribe_object *out) {
    uint8_t actual[SCRIBE_HASH_SIZE];
    uint64_t payload_len64;
    size_t leb_used;
    scribe_error_t err;

    hash_bytes(envelope, envelope_len, actual);
    if (memcmp(actual, hash, SCRIBE_HASH_SIZE) != 0) {
        free(envelope);
        return scribe_set_error(SCRIBE_ECORRUPT, "packed object hash mismatch");
    }
    if (envelope_len < 2u || envelope[0] != expected_type) {
        free(envelope);
        return scribe_set_error(SCRIBE_ECORRUPT, "packed object envelope type mismatch");
    }
    err = scribe_leb128_decode(envelope + 1u, envelope_len - 1u, &payload_len64, &leb_used);
    if (err != SCRIBE_OK) {
        free(envelope);
        return err;
    }
    if (payload_len64 > (uint64_t)SIZE_MAX || 1u + leb_used + (size_t)payload_len64 != envelope_len) {
        free(envelope);
        return scribe_set_error(SCRIBE_ECORRUPT, "packed object envelope length mismatch");
    }
    if (out == NULL) {
        free(envelope);
        return SCRIBE_OK;
    }
    out->type = envelope[0];
    out->envelope = envelope;
    out->envelope_len = envelope_len;
    out->payload = envelope + 1u + leb_used;
    out->payload_len = (size_t)payload_len64;
    return SCRIBE_OK;
}

static scribe_error_t decompress_pack_record(const uint8_t *compressed, size_t compressed_len,
                                             const scribe_pack_header *header, const scribe_pack_index_entry *entry,
                                             scribe_object *out) {
    size_t uncompressed_len;
    uint8_t *envelope;
    size_t result;
    scribe_error_t err = size_from_u64(entry->uncompressed_len, &uncompressed_len);

    if (err != SCRIBE_OK) {
        return err;
    }
    envelope = (uint8_t *)malloc(uncompressed_len == 0u ? 1u : uncompressed_len);
    if (envelope == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate packed object envelope");
    }
    if (header->dictionary != NULL) {
        ZSTD_DCtx *dctx = ZSTD_createDCtx();
        if (dctx == NULL) {
            free(envelope);
            return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate zstd decompressor");
        }
        result = ZSTD_decompress_usingDict(dctx, envelope, uncompressed_len, compressed, compressed_len,
                                           header->dictionary, header->dictionary_len);
        ZSTD_freeDCtx(dctx);
    } else {
        result = ZSTD_decompress(envelope, uncompressed_len, compressed, compressed_len);
    }
    if (ZSTD_isError(result) || result != uncompressed_len) {
        free(envelope);
        return scribe_set_error(SCRIBE_ECORRUPT, "packed object decompression failed");
    }
    g_perf_trace.pack_decompressed_bytes += (uint64_t)uncompressed_len;
    return unpack_envelope(envelope, uncompressed_len, entry->hash, entry->type, out);
}

static scribe_error_t read_record_from_pack(const uint8_t *pack_bytes, size_t pack_len,
                                            const scribe_pack_header *header, const scribe_pack_index_entry *entry,
                                            scribe_object *out) {
    size_t off;
    size_t compressed_len;
    size_t used;
    uint64_t record_uncompressed;
    uint64_t record_compressed;
    uint64_t record_namespace;
    scribe_error_t err;

    err = size_from_u64(entry->offset, &off);
    if (err == SCRIBE_OK) {
        err = size_from_u64(entry->compressed_len, &compressed_len);
    }
    if (err != SCRIBE_OK) {
        return err;
    }
    if (off >= pack_len || off + 33u > (size_t)header->namespace_table_offset) {
        return scribe_set_error(SCRIBE_ECORRUPT, "packed object offset is invalid");
    }
    if (memcmp(pack_bytes + off, entry->hash, SCRIBE_HASH_SIZE) != 0) {
        return scribe_set_error(SCRIBE_ECORRUPT, "packed object record hash mismatch");
    }
    off += SCRIBE_HASH_SIZE;
    if (pack_bytes[off] != entry->type) {
        return scribe_set_error(SCRIBE_ECORRUPT, "packed object record type mismatch");
    }
    off++;
    err = scribe_leb128_decode(pack_bytes + off, pack_len - off, &record_uncompressed, &used);
    if (err != SCRIBE_OK) {
        return err;
    }
    off += used;
    err = scribe_leb128_decode(pack_bytes + off, pack_len - off, &record_compressed, &used);
    if (err != SCRIBE_OK) {
        return err;
    }
    off += used;
    err = scribe_leb128_decode(pack_bytes + off, pack_len - off, &record_namespace, &used);
    if (err != SCRIBE_OK) {
        return err;
    }
    off += used;
    if (record_uncompressed != entry->uncompressed_len || record_compressed != entry->compressed_len ||
        record_namespace != entry->namespace_tag || compressed_len > (size_t)header->namespace_table_offset - off) {
        return scribe_set_error(SCRIBE_ECORRUPT, "packed object record metadata mismatch");
    }
    return decompress_pack_record(pack_bytes + off, compressed_len, header, entry, out);
}

static scribe_error_t read_exact_fd(int fd, uint64_t offset, uint8_t *out, size_t len) {
    size_t done = 0;

    if (len == 0u) {
        return SCRIBE_OK;
    }
    if (offset > (uint64_t)LLONG_MAX || (uint64_t)len > (uint64_t)LLONG_MAX - offset) {
        return scribe_set_error(SCRIBE_ECORRUPT, "pack read range is too large");
    }
    while (done < len) {
        ssize_t n = pread(fd, out + done, len - done, (off_t)(offset + (uint64_t)done));

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return scribe_set_error(SCRIBE_EIO, "failed to read pack file");
        }
        if (n == 0) {
            return scribe_set_error(SCRIBE_ECORRUPT, "short read from pack file");
        }
        done += (size_t)n;
    }
    g_perf_trace.pack_file_bytes_read += (uint64_t)len;
    return SCRIBE_OK;
}

static scribe_error_t parse_pack_header_range(int fd, uint64_t file_size, const scribe_pack_index *idx,
                                              scribe_pack_header *out, uint8_t **out_dictionary_block) {
    uint8_t header_bytes[SCRIBE_PACK_HEADER_SIZE];
    uint8_t namespace_probe[10];
    uint64_t namespace_count;
    size_t namespace_probe_len;
    size_t used;
    scribe_error_t err;

    memset(out, 0, sizeof(*out));
    *out_dictionary_block = NULL;
    if (file_size < SCRIBE_PACK_HEADER_SIZE) {
        return scribe_set_error(SCRIBE_ECORRUPT, "invalid pack header");
    }
    err = read_exact_fd(fd, 0u, header_bytes, sizeof(header_bytes));
    if (err != SCRIBE_OK) {
        return err;
    }
    if (memcmp(header_bytes, "SPCK", 4u) != 0 || read_u32le(header_bytes + 4u) != SCRIBE_PACK_VERSION) {
        return scribe_set_error(SCRIBE_ECORRUPT, "invalid pack header");
    }
    memcpy(out->pack_id, header_bytes + 8u, 16u);
    if (memcmp(out->pack_id, idx->pack_id, 16u) != 0) {
        return scribe_set_error(SCRIBE_ECORRUPT, "pack/index id mismatch");
    }
    out->flags = read_u64le(header_bytes + 24u);
    out->object_count = read_u64le(header_bytes + 32u);
    out->dictionary_offset = read_u64le(header_bytes + 40u);
    out->namespace_table_offset = read_u64le(header_bytes + 48u);
    if (out->flags != 0u || out->object_count != (uint64_t)idx->count) {
        return scribe_set_error(SCRIBE_ECORRUPT, "pack header values do not match index");
    }
    if (out->namespace_table_offset < SCRIBE_PACK_HEADER_SIZE || out->namespace_table_offset >= file_size) {
        return scribe_set_error(SCRIBE_ECORRUPT, "pack namespace table offset is invalid");
    }
    namespace_probe_len = (size_t)(file_size - out->namespace_table_offset);
    if (namespace_probe_len > sizeof(namespace_probe)) {
        namespace_probe_len = sizeof(namespace_probe);
    }
    err = read_exact_fd(fd, out->namespace_table_offset, namespace_probe, namespace_probe_len);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_leb128_decode(namespace_probe, namespace_probe_len, &namespace_count, &used);
    if (err != SCRIBE_OK) {
        return err;
    }
    (void)namespace_count;
    if (out->dictionary_offset != 0u) {
        uint8_t *block;
        uint64_t dict_len64;
        size_t block_len;

        if (out->dictionary_offset < SCRIBE_PACK_HEADER_SIZE || out->dictionary_offset >= out->namespace_table_offset) {
            return scribe_set_error(SCRIBE_ECORRUPT, "pack dictionary offset is invalid");
        }
        err = size_from_u64(out->namespace_table_offset - out->dictionary_offset, &block_len);
        if (err != SCRIBE_OK) {
            return err;
        }
        block = (uint8_t *)malloc(block_len == 0u ? 1u : block_len);
        if (block == NULL) {
            return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate pack dictionary");
        }
        err = read_exact_fd(fd, out->dictionary_offset, block, block_len);
        if (err != SCRIBE_OK) {
            free(block);
            return err;
        }
        err = scribe_leb128_decode(block, block_len, &dict_len64, &used);
        if (err != SCRIBE_OK) {
            free(block);
            return err;
        }
        if (dict_len64 > (uint64_t)(block_len - used)) {
            free(block);
            return scribe_set_error(SCRIBE_ECORRUPT, "pack dictionary length is invalid");
        }
        out->dictionary = block + used;
        out->dictionary_len = (size_t)dict_len64;
        *out_dictionary_block = block;
    }
    return SCRIBE_OK;
}

static scribe_error_t read_record_from_pack_fd(int fd, const scribe_pack_header *header,
                                               const scribe_pack_index_entry *entry, scribe_object *out) {
    uint8_t record_header[SCRIBE_PACK_RECORD_HEADER_MAX];
    uint8_t *compressed = NULL;
    uint64_t record_uncompressed;
    uint64_t record_compressed;
    uint64_t record_namespace;
    uint64_t data_offset;
    size_t header_len;
    size_t compressed_len;
    size_t off = 0;
    size_t used;
    scribe_error_t err;

    if (entry->offset >= header->namespace_table_offset ||
        header->namespace_table_offset - entry->offset < SCRIBE_HASH_SIZE + 1u) {
        return scribe_set_error(SCRIBE_ECORRUPT, "packed object offset is invalid");
    }
    header_len = (size_t)(header->namespace_table_offset - entry->offset);
    if (header_len > sizeof(record_header)) {
        header_len = sizeof(record_header);
    }
    err = read_exact_fd(fd, entry->offset, record_header, header_len);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (header_len < SCRIBE_HASH_SIZE + 1u || memcmp(record_header, entry->hash, SCRIBE_HASH_SIZE) != 0) {
        return scribe_set_error(SCRIBE_ECORRUPT, "packed object record hash mismatch");
    }
    off += SCRIBE_HASH_SIZE;
    if (record_header[off] != entry->type) {
        return scribe_set_error(SCRIBE_ECORRUPT, "packed object record type mismatch");
    }
    off++;
    err = scribe_leb128_decode(record_header + off, header_len - off, &record_uncompressed, &used);
    if (err != SCRIBE_OK) {
        return err;
    }
    off += used;
    err = scribe_leb128_decode(record_header + off, header_len - off, &record_compressed, &used);
    if (err != SCRIBE_OK) {
        return err;
    }
    off += used;
    err = scribe_leb128_decode(record_header + off, header_len - off, &record_namespace, &used);
    if (err != SCRIBE_OK) {
        return err;
    }
    off += used;
    err = size_from_u64(entry->compressed_len, &compressed_len);
    if (err != SCRIBE_OK) {
        return err;
    }
    data_offset = entry->offset + (uint64_t)off;
    if (record_uncompressed != entry->uncompressed_len || record_compressed != entry->compressed_len ||
        record_namespace != entry->namespace_tag || data_offset > header->namespace_table_offset ||
        entry->compressed_len > header->namespace_table_offset - data_offset) {
        return scribe_set_error(SCRIBE_ECORRUPT, "packed object record metadata mismatch");
    }
    compressed = (uint8_t *)malloc(compressed_len == 0u ? 1u : compressed_len);
    if (compressed == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate packed compressed object");
    }
    err = read_exact_fd(fd, data_offset, compressed, compressed_len);
    if (err == SCRIBE_OK) {
        err = decompress_pack_record(compressed, compressed_len, header, entry, out);
    }
    free(compressed);
    return err;
}

static scribe_error_t read_pack_entry(const char *pack_path, const scribe_pack_index *idx,
                                      const scribe_pack_index_entry *entry, scribe_object *out) {
    uint8_t *dictionary_block = NULL;
    scribe_pack_header header;
    struct stat st;
    int fd;
    scribe_error_t err = SCRIBE_OK;

    g_perf_trace.pack_file_opens++;
    fd = open(pack_path, O_RDONLY);
    if (fd < 0) {
        return errno == ENOENT ? scribe_set_error(SCRIBE_ECORRUPT, "pack file missing")
                               : scribe_set_error(SCRIBE_EIO, "failed to open pack file");
    }
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        err = scribe_set_error(SCRIBE_EIO, "failed to stat pack file");
    }
    if (err == SCRIBE_OK) {
        err = parse_pack_header_range(fd, (uint64_t)st.st_size, idx, &header, &dictionary_block);
    }
    if (err == SCRIBE_OK) {
        err = read_record_from_pack_fd(fd, &header, entry, out);
    }
    free(dictionary_block);
    close(fd);
    return err;
}

typedef struct {
    scribe_ctx *ctx;
    const uint8_t *hash;
    scribe_object *out;
    int found;
    scribe_error_t err;
} pack_find_ctx;

static scribe_error_t visit_pack_for_read(const char *name, void *vctx) {
    pack_find_ctx *ctx = (pack_find_ctx *)vctx;
    char *idx_path;
    char *pack_path;
    scribe_pack_index idx;
    const scribe_pack_index_entry *entry;
    scribe_error_t err;

    if (ctx->found || !has_suffix(name, ".idx")) {
        return SCRIBE_OK;
    }
    idx_path = idx_path_from_idx_name(ctx->ctx, name);
    pack_path = pack_path_from_idx_name(ctx->ctx, name);
    if (idx_path == NULL || pack_path == NULL) {
        free(idx_path);
        free(pack_path);
        return SCRIBE_OK;
    }
    err = parse_index_file(idx_path, &idx);
    if (err != SCRIBE_OK) {
        free(idx_path);
        free(pack_path);
        ctx->err = err;
        return err;
    }
    entry = pack_index_find(&idx, ctx->hash);
    if (entry != NULL) {
        err = read_pack_entry(pack_path, &idx, entry, ctx->out);
        if (err == SCRIBE_OK) {
            ctx->found = 1;
        } else {
            ctx->err = err;
        }
    }
    pack_index_destroy(&idx);
    free(idx_path);
    free(pack_path);
    return err;
}

scribe_error_t scribe_pack_read_object(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE], scribe_object *out) {
    pack_find_ctx fctx;
    char *packs;
    scribe_error_t err;

    if (ctx == NULL || hash == NULL || out == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid pack read arguments");
    }
    g_perf_trace.packed_object_reads++;
    memset(out, 0, sizeof(*out));
    memset(&fctx, 0, sizeof(fctx));
    fctx.ctx = ctx;
    fctx.hash = hash;
    fctx.out = out;
    packs = pack_dir_path(ctx);
    if (packs == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = scribe_list_dir(packs, visit_pack_for_read, &fctx);
    free(packs);
    if (err == SCRIBE_ENOT_FOUND) {
        return scribe_set_error(SCRIBE_ENOT_FOUND, "object not found");
    }
    if (err != SCRIBE_OK) {
        return fctx.err != SCRIBE_OK ? fctx.err : err;
    }
    return fctx.found ? SCRIBE_OK : scribe_set_error(SCRIBE_ENOT_FOUND, "object not found");
}

scribe_error_t scribe_pack_has(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    size_t compressed_size = 0;
    scribe_error_t err = scribe_pack_compressed_size(ctx, hash, &compressed_size);

    (void)compressed_size;
    return err;
}

typedef struct {
    scribe_ctx *ctx;
    scribe_object_visit_fn visit;
    void *user;
} pack_iter_ctx;

static scribe_error_t visit_pack_for_iter(const char *name, void *vctx) {
    pack_iter_ctx *ctx = (pack_iter_ctx *)vctx;
    char *idx_path;
    scribe_pack_index idx;
    size_t i;
    scribe_error_t err;

    if (!has_suffix(name, ".idx")) {
        return SCRIBE_OK;
    }
    idx_path = idx_path_from_idx_name(ctx->ctx, name);
    if (idx_path == NULL) {
        return SCRIBE_OK;
    }
    err = parse_index_file(idx_path, &idx);
    free(idx_path);
    if (err != SCRIBE_OK) {
        return err;
    }
    for (i = 0; i < idx.count; i++) {
        err = ctx->visit(idx.entries[i].hash, ctx->user);
        if (err != SCRIBE_OK) {
            pack_index_destroy(&idx);
            return err;
        }
    }
    pack_index_destroy(&idx);
    return SCRIBE_OK;
}

scribe_error_t scribe_pack_iter(scribe_ctx *ctx, scribe_object_visit_fn visit, void *user) {
    pack_iter_ctx ictx;
    char *packs;
    scribe_error_t err;

    if (ctx == NULL || visit == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid pack iterator arguments");
    }
    packs = pack_dir_path(ctx);
    if (packs == NULL) {
        return SCRIBE_ENOMEM;
    }
    ictx.ctx = ctx;
    ictx.visit = visit;
    ictx.user = user;
    err = scribe_list_dir(packs, visit_pack_for_iter, &ictx);
    free(packs);
    return err == SCRIBE_ENOT_FOUND ? SCRIBE_OK : err;
}

typedef struct {
    scribe_ctx *ctx;
    scribe_pack_meta_visit_fn visit;
    void *user;
} pack_meta_iter_ctx;

static scribe_error_t visit_pack_for_meta_iter(const char *name, void *vctx) {
    pack_meta_iter_ctx *ctx = (pack_meta_iter_ctx *)vctx;
    char *idx_path;
    scribe_pack_index idx;
    size_t i;
    scribe_error_t err;

    if (!has_suffix(name, ".idx")) {
        return SCRIBE_OK;
    }
    idx_path = idx_path_from_idx_name(ctx->ctx, name);
    if (idx_path == NULL) {
        return SCRIBE_OK;
    }
    err = parse_index_file(idx_path, &idx);
    free(idx_path);
    if (err != SCRIBE_OK) {
        return err;
    }
    for (i = 0; i < idx.count; i++) {
        err = ctx->visit(idx.entries[i].hash, idx.entries[i].type, ctx->user);
        if (err != SCRIBE_OK) {
            pack_index_destroy(&idx);
            return err;
        }
    }
    pack_index_destroy(&idx);
    return SCRIBE_OK;
}

scribe_error_t scribe_pack_meta_iter(scribe_ctx *ctx, scribe_pack_meta_visit_fn visit, void *user) {
    pack_meta_iter_ctx ictx;
    char *packs;
    scribe_error_t err;

    if (ctx == NULL || visit == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid pack metadata iterator arguments");
    }
    packs = pack_dir_path(ctx);
    if (packs == NULL) {
        return SCRIBE_ENOMEM;
    }
    ictx.ctx = ctx;
    ictx.visit = visit;
    ictx.user = user;
    err = scribe_list_dir(packs, visit_pack_for_meta_iter, &ictx);
    free(packs);
    return err == SCRIBE_ENOT_FOUND ? SCRIBE_OK : err;
}

typedef struct {
    scribe_ctx *ctx;
    scribe_pack_entry_meta_visit_fn visit;
    void *user;
} pack_entry_meta_iter_ctx;

static scribe_error_t visit_pack_for_entry_meta_iter(const char *name, void *vctx) {
    pack_entry_meta_iter_ctx *ctx = (pack_entry_meta_iter_ctx *)vctx;
    char *idx_path;
    scribe_pack_index idx;
    size_t i;
    scribe_error_t err;

    if (!has_suffix(name, ".idx")) {
        return SCRIBE_OK;
    }
    idx_path = idx_path_from_idx_name(ctx->ctx, name);
    if (idx_path == NULL) {
        return SCRIBE_OK;
    }
    err = parse_index_file(idx_path, &idx);
    free(idx_path);
    if (err != SCRIBE_OK) {
        return err;
    }
    for (i = 0; i < idx.count; i++) {
        size_t payload_len = 0;
        size_t compressed_size = 0;

        err = payload_len_from_envelope_len(idx.entries[i].uncompressed_len, &payload_len);
        if (err == SCRIBE_OK) {
            err = size_from_u64(idx.entries[i].compressed_len, &compressed_size);
        }
        if (err == SCRIBE_OK) {
            err = ctx->visit(idx.entries[i].hash, idx.entries[i].type, payload_len, compressed_size, ctx->user);
        }
        if (err != SCRIBE_OK) {
            pack_index_destroy(&idx);
            return err;
        }
    }
    pack_index_destroy(&idx);
    return SCRIBE_OK;
}

scribe_error_t scribe_pack_entry_meta_iter(scribe_ctx *ctx, scribe_pack_entry_meta_visit_fn visit, void *user) {
    pack_entry_meta_iter_ctx ictx;
    char *packs;
    scribe_error_t err;

    if (ctx == NULL || visit == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid pack entry metadata iterator arguments");
    }
    packs = pack_dir_path(ctx);
    if (packs == NULL) {
        return SCRIBE_ENOMEM;
    }
    ictx.ctx = ctx;
    ictx.visit = visit;
    ictx.user = user;
    err = scribe_list_dir(packs, visit_pack_for_entry_meta_iter, &ictx);
    free(packs);
    return err == SCRIBE_ENOT_FOUND ? SCRIBE_OK : err;
}

/*
 * Iterates the index entries of one published pack. Consolidation records the
 * old pack ids before writing replacements and then uses this function so new
 * replacement packs are never included in the same consolidation pass.
 */
scribe_error_t scribe_pack_iter_file(scribe_ctx *ctx, const uint8_t pack_id[16], scribe_object_visit_fn visit,
                                     void *user) {
    char *idx_path;
    scribe_pack_index idx;
    size_t i;
    scribe_error_t err;

    if (ctx == NULL || pack_id == NULL || visit == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid single-pack iterator arguments");
    }
    idx_path = pack_file_path_from_id(ctx, pack_id, ".idx");
    if (idx_path == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = parse_index_file(idx_path, &idx);
    free(idx_path);
    if (err != SCRIBE_OK) {
        return err == SCRIBE_ENOT_FOUND ? SCRIBE_OK : err;
    }
    for (i = 0; i < idx.count; i++) {
        err = visit(idx.entries[i].hash, user);
        if (err != SCRIBE_OK) {
            pack_index_destroy(&idx);
            return err;
        }
    }
    pack_index_destroy(&idx);
    return SCRIBE_OK;
}

typedef struct {
    scribe_ctx *ctx;
    const uint8_t *hash;
    size_t *out;
    int found;
} pack_size_ctx;

static scribe_error_t visit_pack_for_size(const char *name, void *vctx) {
    pack_size_ctx *ctx = (pack_size_ctx *)vctx;
    char *idx_path;
    scribe_pack_index idx;
    const scribe_pack_index_entry *entry;
    scribe_error_t err;

    if (ctx->found || !has_suffix(name, ".idx")) {
        return SCRIBE_OK;
    }
    idx_path = idx_path_from_idx_name(ctx->ctx, name);
    if (idx_path == NULL) {
        return SCRIBE_OK;
    }
    err = parse_index_file(idx_path, &idx);
    free(idx_path);
    if (err != SCRIBE_OK) {
        return err;
    }
    entry = pack_index_find(&idx, ctx->hash);
    if (entry != NULL) {
        err = size_from_u64(entry->compressed_len, ctx->out);
        if (err == SCRIBE_OK) {
            ctx->found = 1;
        }
    }
    pack_index_destroy(&idx);
    return err;
}

scribe_error_t scribe_pack_compressed_size(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE], size_t *out) {
    pack_size_ctx sctx;
    char *packs;
    scribe_error_t err;

    if (ctx == NULL || hash == NULL || out == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid pack compressed-size arguments");
    }
    memset(&sctx, 0, sizeof(sctx));
    sctx.ctx = ctx;
    sctx.hash = hash;
    sctx.out = out;
    packs = pack_dir_path(ctx);
    if (packs == NULL) {
        return SCRIBE_ENOMEM;
    }
    err = scribe_list_dir(packs, visit_pack_for_size, &sctx);
    free(packs);
    if (err == SCRIBE_ENOT_FOUND) {
        return scribe_set_error(SCRIBE_ENOT_FOUND, "object not found");
    }
    if (err != SCRIBE_OK) {
        return err;
    }
    return sctx.found ? SCRIBE_OK : scribe_set_error(SCRIBE_ENOT_FOUND, "object not found");
}

typedef struct {
    scribe_ctx *ctx;
    size_t count;
    scribe_pack_meta_visit_fn visit;
    void *user;
} pack_validate_ctx;

static scribe_error_t validate_pack_file(const char *pack_path, const scribe_pack_index *idx) {
    uint8_t *pack_bytes = NULL;
    size_t pack_len = 0;
    scribe_pack_header header;
    size_t i;
    scribe_error_t err;

    g_perf_trace.pack_validations++;
    g_perf_trace.pack_file_opens++;
    g_perf_trace.full_pack_reads++;
    err = scribe_read_file(pack_path, &pack_bytes, &pack_len);
    if (err != SCRIBE_OK) {
        return err == SCRIBE_ENOT_FOUND ? scribe_set_error(SCRIBE_ECORRUPT, "pack file missing") : err;
    }
    g_perf_trace.pack_file_bytes_read += (uint64_t)pack_len;
    err = parse_pack_header(pack_bytes, pack_len, idx->pack_id, idx->count, &header);
    for (i = 0; err == SCRIBE_OK && i < idx->count; i++) {
        err = read_record_from_pack(pack_bytes, pack_len, &header, &idx->entries[i], NULL);
    }
    free(pack_bytes);
    return err;
}

static scribe_error_t visit_pack_for_validate(const char *name, void *vctx) {
    pack_validate_ctx *ctx = (pack_validate_ctx *)vctx;
    char *idx_path;
    char *pack_path;
    scribe_pack_index idx;
    scribe_error_t err;

    if (!has_suffix(name, ".idx")) {
        return SCRIBE_OK;
    }
    idx_path = idx_path_from_idx_name(ctx->ctx, name);
    pack_path = pack_path_from_idx_name(ctx->ctx, name);
    if (idx_path == NULL || pack_path == NULL) {
        free(idx_path);
        free(pack_path);
        return SCRIBE_OK;
    }
    err = parse_index_file(idx_path, &idx);
    if (err == SCRIBE_OK) {
        err = validate_pack_file(pack_path, &idx);
    }
    if (err == SCRIBE_OK && ctx->visit != NULL) {
        size_t i;

        for (i = 0; err == SCRIBE_OK && i < idx.count; i++) {
            err = ctx->visit(idx.entries[i].hash, idx.entries[i].type, ctx->user);
        }
    }
    if (err == SCRIBE_OK) {
        ctx->count += idx.count;
    }
    pack_index_destroy(&idx);
    free(idx_path);
    free(pack_path);
    return err;
}

scribe_error_t scribe_pack_validate_all_meta(scribe_ctx *ctx, size_t *out_count, scribe_pack_meta_visit_fn visit,
                                             void *user) {
    pack_validate_ctx vctx;
    char *packs;
    uint64_t started;
    scribe_error_t err;

    if (ctx == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid pack validation context");
    }
    memset(&vctx, 0, sizeof(vctx));
    vctx.ctx = ctx;
    vctx.visit = visit;
    vctx.user = user;
    packs = pack_dir_path(ctx);
    if (packs == NULL) {
        return SCRIBE_ENOMEM;
    }
    started = scribe_perf_now_ns();
    err = scribe_list_dir(packs, visit_pack_for_validate, &vctx);
    if (started != 0u) {
        uint64_t ended = scribe_perf_now_ns();
        if (ended >= started) {
            g_perf_trace.pack_validation_ns += ended - started;
        }
    }
    free(packs);
    if (err == SCRIBE_ENOT_FOUND) {
        vctx.count = 0;
        err = SCRIBE_OK;
    }
    if (err == SCRIBE_OK && out_count != NULL) {
        *out_count = vctx.count;
    }
    return err;
}

scribe_error_t scribe_pack_validate_all(scribe_ctx *ctx, size_t *out_count) {
    return scribe_pack_validate_all_meta(ctx, out_count, NULL, NULL);
}

typedef struct {
    scribe_pack_file_visit_fn visit;
    void *user;
} pack_file_iter_ctx;

/*
 * Visits one published pack index name and exposes its pack id to maintenance
 * code. The `.idx` file is the publication point, so iterating indexes is the
 * correct way to identify visible packs.
 */
static scribe_error_t visit_pack_file_for_iter(const char *name, void *vctx) {
    pack_file_iter_ctx *ctx = (pack_file_iter_ctx *)vctx;
    uint8_t pack_id[16];

    if (!pack_id_from_idx_name(name, pack_id)) {
        return SCRIBE_OK;
    }
    return ctx->visit(pack_id, ctx->user);
}

/*
 * Iterates published pack ids by scanning index files in `.scribe/packs`.
 * pack files without matching indexes, so gc follows the same visibility rule.
 */
scribe_error_t scribe_pack_file_iter(scribe_ctx *ctx, scribe_pack_file_visit_fn visit, void *user) {
    pack_file_iter_ctx ictx;
    char *packs;
    scribe_error_t err;

    if (ctx == NULL || visit == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid pack-file iterator arguments");
    }
    packs = pack_dir_path(ctx);
    if (packs == NULL) {
        return SCRIBE_ENOMEM;
    }
    ictx.visit = visit;
    ictx.user = user;
    err = scribe_list_dir(packs, visit_pack_file_for_iter, &ictx);
    free(packs);
    return err == SCRIBE_ENOT_FOUND ? SCRIBE_OK : err;
}

/*
 * Removes one immutable pack pair after replacement has been verified. The idx
 * is unlinked first because its presence is the publication marker; after that,
 * normal readers ignore the pack even if a crash leaves the `.pack` file behind.
 */
scribe_error_t scribe_pack_delete_file_pair(scribe_ctx *ctx, const uint8_t pack_id[16]) {
    char *idx_path;
    char *pack_path;
    scribe_error_t err = SCRIBE_OK;

    if (ctx == NULL || pack_id == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid pack delete arguments");
    }
    idx_path = pack_file_path_from_id(ctx, pack_id, ".idx");
    pack_path = pack_file_path_from_id(ctx, pack_id, ".pack");
    if (idx_path == NULL || pack_path == NULL) {
        free(idx_path);
        free(pack_path);
        return SCRIBE_ENOMEM;
    }
    if (unlink(idx_path) != 0 && errno != ENOENT) {
        err = scribe_set_error(SCRIBE_EIO, "failed to delete pack index");
    }
    if (err == SCRIBE_OK && unlink(pack_path) != 0 && errno != ENOENT) {
        err = scribe_set_error(SCRIBE_EIO, "failed to delete pack file");
    }
    if (err == SCRIBE_OK) {
        err = fsync_parent_dir(pack_path);
    }
    free(idx_path);
    free(pack_path);
    return err;
}
