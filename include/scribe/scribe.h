#ifndef SCRIBE_SCRIBE_H
#define SCRIBE_SCRIBE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCRIBE_HASH_SIZE 32
#define SCRIBE_HEX_HASH_SIZE 64
#define SCRIBE_VERSION "1.1.1"

typedef enum {
    SCRIBE_OK = 0,
    SCRIBE_ERR = 1,
    SCRIBE_ENOT_FOUND = 2,
    SCRIBE_EEXISTS = 3,
    SCRIBE_ELOCKED = 4,
    SCRIBE_EREF_STALE = 5,
    SCRIBE_EIO = 6,
    SCRIBE_ECORRUPT = 7,
    SCRIBE_EINVAL = 8,
    SCRIBE_EMALFORMED = 9,
    SCRIBE_EPROTOCOL = 10,
    SCRIBE_ECONFIG = 11,
    SCRIBE_EADAPTER = 12,
    SCRIBE_ENOMEM = 13,
    SCRIBE_ENOSYS = 14,
    SCRIBE_EINTERRUPTED = 15,
    SCRIBE_EPATH = 16,
    SCRIBE_EHASH = 17,
    SCRIBE_ECONCURRENT = 18,
    SCRIBE_ESHUTDOWN = 19,
} scribe_error_t;

typedef struct scribe_ctx scribe_ctx;

typedef struct {
    const char *name;
    const char *email;
    const char *source;
} scribe_identity;

typedef struct {
    const char *name;
    const char *version;
    const char *params;
    const char *correlation_id;
} scribe_process_info;

typedef struct {
    const char *const *path;
    size_t path_len;
    const uint8_t *payload;
    size_t payload_len;
} scribe_change_event;

typedef struct {
    const scribe_change_event *events;
    size_t event_count;
    scribe_identity author;
    scribe_identity committer;
    scribe_process_info process;
    int64_t timestamp_unix_nanos;
    const char *message;
    size_t message_len;
} scribe_change_batch;

const char *scribe_last_error_detail(void);
const char *scribe_error_symbol(scribe_error_t err);

scribe_error_t scribe_init_repository(const char *path);
scribe_error_t scribe_open(const char *path, int writable, scribe_ctx **out);
void scribe_close(scribe_ctx *ctx);

scribe_error_t scribe_commit_batch(scribe_ctx *ctx, const scribe_change_batch *batch,
                                   uint8_t out_commit_hash[SCRIBE_HASH_SIZE]);

#ifdef __cplusplus
}
#endif

#endif
