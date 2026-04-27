#ifndef SCRIBE_INTERNAL_H
#define SCRIBE_INTERNAL_H

#include "scribe/scribe.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define SCRIBE_OBJECT_BLOB 0x01u
#define SCRIBE_OBJECT_TREE 0x02u
#define SCRIBE_OBJECT_COMMIT 0x03u

#define SCRIBE_LIST_TYPE_BLOB 0x01
#define SCRIBE_LIST_TYPE_TREE 0x02
#define SCRIBE_LIST_TYPE_COMMIT 0x04

typedef struct {
    int scribe_format_version;
    int compression_level;
    int worker_threads;
    size_t event_queue_capacity;
    int queue_stall_warn_seconds;
    bool adapter_require_pre_post_images;
    int adapter_coalesce_window_ms;
    char adapter_excluded_databases[128];
} scribe_config;

struct scribe_ctx {
    char *repo_path;
    int writable;
    int lock_fd;
    FILE *log_file;
    scribe_config config;
};

typedef struct {
    uint8_t type;
    uint8_t hash[SCRIBE_HASH_SIZE];
    const char *name;
    size_t name_len;
} scribe_tree_entry;

typedef struct {
    uint8_t type;
    uint8_t *payload;
    size_t payload_len;
    uint8_t *envelope;
    size_t envelope_len;
} scribe_object;

typedef scribe_error_t (*scribe_object_visit_fn)(const uint8_t hash[SCRIBE_HASH_SIZE], void *user);

typedef struct {
    uint8_t root_tree[SCRIBE_HASH_SIZE];
    bool has_parent;
    uint8_t parent[SCRIBE_HASH_SIZE];
    char *author_name;
    char *author_email;
    char *author_source;
    int64_t author_time;
    char *committer_name;
    char *committer_email;
    char *committer_source;
    int64_t committer_time;
    char *process_name;
    char *process_version;
    char *process_params;
    char *process_correlation_id;
    uint8_t *message;
    size_t message_len;
} scribe_commit_view;

char *scribe_path_join(const char *a, const char *b);
scribe_error_t scribe_mkdir_p(const char *path);
scribe_error_t scribe_write_file_atomic(const char *path, const uint8_t *bytes, size_t len);
scribe_error_t scribe_read_file(const char *path, uint8_t **out, size_t *out_len);
bool scribe_file_exists(const char *path);
scribe_error_t scribe_list_dir(const char *path, scribe_error_t (*visit)(const char *name, void *ctx), void *ctx);

scribe_error_t scribe_default_config(scribe_config *cfg);
scribe_error_t scribe_write_config(const char *repo_path, const scribe_config *cfg);
scribe_error_t scribe_read_config(const char *repo_path, scribe_config *cfg);

scribe_error_t scribe_lock_repo(scribe_ctx *ctx);
void scribe_unlock_repo(scribe_ctx *ctx);
scribe_error_t scribe_refs_read(scribe_ctx *ctx, const char *name, uint8_t out[SCRIBE_HASH_SIZE]);
scribe_error_t scribe_refs_cas(scribe_ctx *ctx, const char *name, const uint8_t *expected,
                               const uint8_t new_hash[SCRIBE_HASH_SIZE]);

scribe_error_t scribe_object_write(scribe_ctx *ctx, uint8_t type, const uint8_t *payload, size_t payload_len,
                                   uint8_t out_hash[SCRIBE_HASH_SIZE]);
scribe_error_t scribe_object_read(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE], scribe_object *out);
void scribe_object_free(scribe_object *obj);
scribe_error_t scribe_object_has(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE]);
char *scribe_object_path(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE]);
scribe_error_t scribe_object_iter(scribe_ctx *ctx, scribe_object_visit_fn visit, void *user);
scribe_error_t scribe_object_compressed_size(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE], size_t *out);

scribe_error_t scribe_tree_serialize(const scribe_tree_entry *entries, size_t count, scribe_arena *arena, uint8_t **out,
                                     size_t *out_len);
scribe_error_t scribe_tree_parse_arena_capacity(size_t payload_len, size_t *out);
scribe_error_t scribe_tree_parse(const uint8_t *payload, size_t len, scribe_arena *arena,
                                 scribe_tree_entry **out_entries, size_t *out_count);

scribe_error_t scribe_commit_serialize(const uint8_t root_tree[SCRIBE_HASH_SIZE], const uint8_t *parent,
                                       const scribe_change_batch *batch, scribe_arena *arena, uint8_t **out,
                                       size_t *out_len);
scribe_error_t scribe_commit_serialize_allow_empty(const uint8_t root_tree[SCRIBE_HASH_SIZE], const uint8_t *parent,
                                                   const scribe_change_batch *batch, scribe_arena *arena, uint8_t **out,
                                                   size_t *out_len);
scribe_error_t scribe_commit_parse(const uint8_t *payload, size_t len, scribe_arena *arena, scribe_commit_view *out);

scribe_error_t scribe_commit_batch_internal(scribe_ctx *ctx, const scribe_change_batch *batch,
                                            uint8_t out_commit_hash[SCRIBE_HASH_SIZE]);
scribe_error_t scribe_commit_root_internal(scribe_ctx *ctx, const uint8_t root_tree[SCRIBE_HASH_SIZE],
                                           const scribe_change_batch *metadata,
                                           uint8_t out_commit_hash[SCRIBE_HASH_SIZE]);

scribe_error_t scribe_cli_log(scribe_ctx *ctx, int oneline, size_t limit);
scribe_error_t scribe_cli_show(scribe_ctx *ctx, const char *rev);
scribe_error_t scribe_cli_show_path(scribe_ctx *ctx, const char *spec);
scribe_error_t scribe_cli_cat_object(scribe_ctx *ctx, char mode, const char *hex);
scribe_error_t scribe_cli_diff(scribe_ctx *ctx, const char *a, const char *b);
scribe_error_t scribe_cli_fsck(scribe_ctx *ctx);
scribe_error_t scribe_cli_list_objects(scribe_ctx *ctx, int type_mask, int reachable, const char *format);
scribe_error_t scribe_cli_ls_tree(scribe_ctx *ctx, const char *hex);
scribe_error_t scribe_resolve_commit(scribe_ctx *ctx, const char *rev, uint8_t out[SCRIBE_HASH_SIZE]);

scribe_error_t scribe_pipe_commit_batch(scribe_ctx *ctx, FILE *in, FILE *out);

#endif
