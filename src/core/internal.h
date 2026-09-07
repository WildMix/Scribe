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
    uint8_t *hashes;
    uint8_t *used;
    size_t count;
    size_t cap;
} scribe_hash_set;

typedef enum {
    SCRIBE_COMMIT_STORAGE_AUTO = 0,
    SCRIBE_COMMIT_STORAGE_LOOSE = 1,
    SCRIBE_COMMIT_STORAGE_PACK = 2
} scribe_commit_storage_mode;

typedef enum {
    SCRIBE_LOOSE_FSYNC_PER_OBJECT = 0,
    SCRIBE_LOOSE_FSYNC_COMMIT = 1,
    SCRIBE_LOOSE_FSYNC_NONE = 2
} scribe_loose_fsync_mode;

typedef struct {
    int scribe_format_version;
    int compression_level;
    int worker_threads;
    size_t event_queue_capacity;
    int queue_stall_warn_seconds;
    size_t pack_target_size;
    size_t pack_target_object_count;
    bool pack_dictionary_enabled;
    size_t pack_dictionary_sample;
    size_t pack_rollup_loose_threshold;
    size_t pack_rollup_size_threshold;
    scribe_commit_storage_mode commit_storage;
    size_t commit_pack_event_threshold;
    size_t commit_pack_payload_threshold;
    scribe_loose_fsync_mode commit_loose_fsync;
    int gc_prune_grace_days;
    bool adapter_require_pre_post_images;
    int adapter_coalesce_window_ms;
    char adapter_excluded_databases[128];
    char adapter_attribution_field[128];
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
typedef scribe_error_t (*scribe_loose_object_visit_fn)(const uint8_t hash[SCRIBE_HASH_SIZE], const char *path,
                                                       time_t mtime, size_t compressed_size, void *user);
typedef scribe_error_t (*scribe_ref_visit_fn)(const char *name, const uint8_t hash[SCRIBE_HASH_SIZE], void *user);
typedef scribe_error_t (*scribe_pack_file_visit_fn)(const uint8_t pack_id[16], void *user);
typedef scribe_error_t (*scribe_pack_meta_visit_fn)(const uint8_t hash[SCRIBE_HASH_SIZE], uint8_t type, void *user);
typedef scribe_error_t (*scribe_pack_entry_meta_visit_fn)(const uint8_t hash[SCRIBE_HASH_SIZE], uint8_t type,
                                                          size_t payload_len, size_t compressed_size, void *user);
typedef int (*scribe_hash_filter_fn)(const uint8_t hash[SCRIBE_HASH_SIZE], void *user);

typedef struct {
    uint8_t type;
    const uint8_t *payload;
    size_t payload_len;
    uint8_t hash[SCRIBE_HASH_SIZE];
} scribe_pack_object;

typedef struct scribe_pack_writer scribe_pack_writer;

typedef struct {
    uint64_t pack_validations;
    uint64_t pack_index_parses;
    uint64_t pack_file_opens;
    uint64_t full_pack_reads;
    uint64_t packed_object_reads;
    uint64_t pack_file_bytes_read;
    uint64_t pack_decompressed_bytes;
    uint64_t pack_validation_ns;
    uint64_t object_type_count_ns;
} scribe_perf_trace;

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

typedef struct {
    int has_since;
    int64_t since_nanos;
    int has_until;
    int64_t until_nanos;
} scribe_time_range;

typedef enum {
    SCRIBE_PATH_ABSENT = 0,
    SCRIBE_PATH_BLOB = SCRIBE_OBJECT_BLOB,
    SCRIBE_PATH_TREE = SCRIBE_OBJECT_TREE
} scribe_path_state;

typedef struct {
    scribe_path_state state;
    uint8_t hash[SCRIBE_HASH_SIZE];
} scribe_path_resolution;

typedef struct {
    scribe_ctx *ctx;
    const char *remote;
    const char *url;
    const char *token;
} scribe_repository;

char *scribe_path_join(const char *a, const char *b);
scribe_error_t scribe_mkdir_p(const char *path);
scribe_error_t scribe_write_file_atomic(const char *path, const uint8_t *bytes, size_t len);
scribe_error_t scribe_write_file_atomic_opts(const char *path, const uint8_t *bytes, size_t len, int sync_file,
                                             int sync_parent);
scribe_error_t scribe_fsync_dir(const char *path);
scribe_error_t scribe_read_file(const char *path, uint8_t **out, size_t *out_len);
bool scribe_file_exists(const char *path);
scribe_error_t scribe_list_dir(const char *path, scribe_error_t (*visit)(const char *name, void *ctx), void *ctx);
int scribe_hash_set_has(const scribe_hash_set *set, const uint8_t hash[SCRIBE_HASH_SIZE]);
scribe_error_t scribe_hash_set_add(scribe_hash_set *set, const uint8_t hash[SCRIBE_HASH_SIZE], int *already);
void scribe_hash_set_destroy(scribe_hash_set *set);

scribe_error_t scribe_default_config(scribe_config *cfg);
scribe_error_t scribe_write_config(const char *repo_path, const scribe_config *cfg);
scribe_error_t scribe_read_config(const char *repo_path, scribe_config *cfg);

scribe_error_t scribe_lock_repo(scribe_ctx *ctx);
void scribe_unlock_repo(scribe_ctx *ctx);
scribe_error_t scribe_refs_read(scribe_ctx *ctx, const char *name, uint8_t out[SCRIBE_HASH_SIZE]);
scribe_error_t scribe_refs_cas(scribe_ctx *ctx, const char *name, const uint8_t *expected,
                               const uint8_t new_hash[SCRIBE_HASH_SIZE]);
scribe_error_t scribe_refs_update(scribe_ctx *ctx, const char *name, const uint8_t new_hash[SCRIBE_HASH_SIZE],
                                  const char *reason);
scribe_error_t scribe_refs_delete(scribe_ctx *ctx, const char *name, int missing_ok, const char *reason);
scribe_error_t scribe_refs_resolve_name(scribe_ctx *ctx, const char *name, uint8_t out[SCRIBE_HASH_SIZE]);
scribe_error_t scribe_refs_iter(scribe_ctx *ctx, scribe_ref_visit_fn visit, void *user);

scribe_error_t scribe_object_write(scribe_ctx *ctx, uint8_t type, const uint8_t *payload, size_t payload_len,
                                   uint8_t out_hash[SCRIBE_HASH_SIZE]);
scribe_error_t scribe_object_write_loose_opts(scribe_ctx *ctx, uint8_t type, const uint8_t *payload, size_t payload_len,
                                              int sync_file, int sync_parent, uint8_t out_hash[SCRIBE_HASH_SIZE],
                                              uint8_t *out_fanout, int *out_written);
scribe_error_t scribe_object_fsync_fanout_dirs(scribe_ctx *ctx, const bool touched[256]);
scribe_error_t scribe_object_read(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE], scribe_object *out);
void scribe_object_free(scribe_object *obj);
scribe_error_t scribe_object_has(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE]);
char *scribe_object_path(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE]);
scribe_error_t scribe_object_iter(scribe_ctx *ctx, scribe_object_visit_fn visit, void *user);
scribe_error_t scribe_loose_object_iter(scribe_ctx *ctx, scribe_loose_object_visit_fn visit, void *user);
scribe_error_t scribe_loose_object_delete(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE]);
scribe_error_t scribe_object_compressed_size(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE], size_t *out);
scribe_error_t scribe_pack_write_objects(scribe_ctx *ctx, scribe_pack_object *objects, size_t count,
                                         uint8_t out_pack_id[16]);
scribe_error_t scribe_pack_writer_begin(scribe_ctx *ctx, scribe_pack_writer **out);
scribe_error_t scribe_pack_writer_add(scribe_pack_writer *writer, uint8_t type, const uint8_t *payload,
                                      size_t payload_len, uint8_t out_hash[SCRIBE_HASH_SIZE]);
void scribe_pack_writer_stats(scribe_pack_writer *writer, size_t *out_objects, uint64_t *out_compressed_bytes);
scribe_error_t scribe_pack_writer_finish(scribe_pack_writer **writer, uint8_t out_pack_id[16]);
void scribe_pack_writer_abort(scribe_pack_writer **writer);
scribe_error_t scribe_pack_read_object(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE], scribe_object *out);
scribe_error_t scribe_pack_has(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE]);
scribe_error_t scribe_pack_iter(scribe_ctx *ctx, scribe_object_visit_fn visit, void *user);
scribe_error_t scribe_pack_meta_iter(scribe_ctx *ctx, scribe_pack_meta_visit_fn visit, void *user);
scribe_error_t scribe_pack_entry_meta_iter(scribe_ctx *ctx, scribe_pack_entry_meta_visit_fn visit, void *user);
scribe_error_t scribe_pack_iter_file(scribe_ctx *ctx, const uint8_t pack_id[16], scribe_object_visit_fn visit,
                                     void *user);
scribe_error_t scribe_pack_compressed_size(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE], size_t *out);
scribe_error_t scribe_pack_validate_all(scribe_ctx *ctx, size_t *out_count);
scribe_error_t scribe_pack_validate_all_meta(scribe_ctx *ctx, size_t *out_count, scribe_pack_meta_visit_fn visit,
                                             void *user);
scribe_error_t scribe_pack_file_iter(scribe_ctx *ctx, scribe_pack_file_visit_fn visit, void *user);
scribe_error_t scribe_pack_delete_file_pair(scribe_ctx *ctx, const uint8_t pack_id[16]);
int scribe_perf_trace_enabled(void);
void scribe_perf_trace_reset(void);
void scribe_perf_trace_snapshot(scribe_perf_trace *out);
void scribe_perf_trace_add_object_type_count_ns(uint64_t ns);
uint64_t scribe_perf_now_ns(void);

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
scribe_error_t scribe_commit_batch_to_ref_internal(scribe_ctx *ctx, const char *ref, const char *reason,
                                                   const scribe_change_batch *batch,
                                                   uint8_t out_commit_hash[SCRIBE_HASH_SIZE]);
scribe_error_t scribe_commit_root_internal(scribe_ctx *ctx, const uint8_t root_tree[SCRIBE_HASH_SIZE],
                                           const scribe_change_batch *metadata,
                                           uint8_t out_commit_hash[SCRIBE_HASH_SIZE]);

scribe_error_t scribe_read_commit_view(scribe_ctx *ctx, const uint8_t hash[SCRIBE_HASH_SIZE], scribe_arena *arena,
                                       scribe_commit_view *out);
scribe_error_t scribe_parse_time_arg(const char *text, int64_t *out_nanos);
void scribe_format_time_utc(int64_t unix_nanos, char *out, size_t out_size);
scribe_error_t scribe_cli_log(scribe_ctx *ctx, int oneline, size_t limit, int show_paths, const char *path_filter,
                              const scribe_time_range *time_range);
scribe_error_t scribe_cli_show(scribe_ctx *ctx, const char *rev);
scribe_error_t scribe_cli_show_path(scribe_ctx *ctx, const char *spec);
scribe_error_t scribe_cli_cat_object(scribe_ctx *ctx, char mode, const char *hex);
scribe_error_t scribe_cli_diff(scribe_ctx *ctx, const char *a, const char *b);
scribe_error_t scribe_cli_fsck(scribe_ctx *ctx);
scribe_error_t scribe_cli_gc(scribe_ctx *ctx, int dry_run, int prune_now, int pack, int consolidate);
scribe_error_t scribe_cli_branch(scribe_ctx *ctx, const char *name, const char *commit);
scribe_error_t scribe_cli_tag(scribe_ctx *ctx, int delete_mode, int force, const char *name, const char *commit);
scribe_error_t scribe_cli_reflog(scribe_ctx *ctx, const char *ref);
scribe_error_t scribe_cli_bundle_create(scribe_ctx *ctx, const char *file);
scribe_error_t scribe_cli_bundle_import(const char *file, const char *target);
scribe_error_t scribe_bundle_create_file(scribe_ctx *ctx, const char *file, size_t *out_loose, size_t *out_pack_files,
                                         size_t *out_refs);
scribe_error_t scribe_bundle_create_filtered_file(scribe_ctx *ctx, const char *file, scribe_hash_filter_fn include,
                                                  void *include_user, size_t *out_storage_entries, size_t *out_refs);
scribe_error_t scribe_bundle_import_file(scribe_ctx *ctx, const char *file, const char *reason, int fast_forward_refs,
                                         size_t *out_storage_entries, size_t *out_ref_entries);
scribe_error_t scribe_cli_remote_add(scribe_ctx *ctx, const char *name, const char *url, const char *token);
scribe_error_t scribe_cli_remote_list(scribe_ctx *ctx);
scribe_error_t scribe_cli_remote_remove(scribe_ctx *ctx, const char *name);
scribe_error_t scribe_cli_remote_test(scribe_ctx *ctx, const char *name);
scribe_error_t scribe_cli_remote_get_url(scribe_ctx *ctx, const char *name, char **out_url);
scribe_error_t scribe_cli_serve(scribe_ctx *ctx, const char *listen_addr, const char *cert, const char *key);
scribe_error_t scribe_cli_replicate(scribe_ctx *ctx, const char *remote);
scribe_error_t scribe_ingest_json_commit(scribe_ctx *ctx, const uint8_t *body, size_t body_len,
                                         uint8_t out_commit_hash[SCRIBE_HASH_SIZE]);
scribe_error_t scribe_ingest_content_commit(scribe_ctx *ctx, const char *const *path, size_t path_len,
                                            scribe_ingest_op op, const uint8_t *payload, size_t payload_len,
                                            const char *message, uint8_t out_commit_hash[SCRIBE_HASH_SIZE]);
scribe_error_t scribe_checkpoint_write(scribe_ctx *ctx, const char *adapter, const char *key, const uint8_t *value,
                                       size_t value_len, const uint8_t commit_hash[SCRIBE_HASH_SIZE]);
scribe_error_t scribe_checkpoint_read(scribe_ctx *ctx, const char *adapter, const char *key, uint8_t **out_value,
                                      size_t *out_value_len, char out_commit_hex[SCRIBE_HEX_HASH_SIZE + 1]);
scribe_error_t scribe_cli_remote_info(scribe_ctx *ctx, const char *remote);
scribe_error_t scribe_cli_remote_refs(scribe_ctx *ctx, const char *remote);
scribe_error_t scribe_cli_remote_log(scribe_ctx *ctx, const char *remote, int oneline, size_t limit, int show_paths,
                                     const char *path_filter, const scribe_time_range *time_range);
scribe_error_t scribe_cli_remote_show(scribe_ctx *ctx, const char *remote, const char *spec);
scribe_error_t scribe_cli_remote_cat_object(scribe_ctx *ctx, const char *remote, char mode, const char *hex);
scribe_error_t scribe_cli_remote_diff(scribe_ctx *ctx, const char *remote, const char *a, const char *b);
scribe_error_t scribe_cli_remote_ls_tree(scribe_ctx *ctx, const char *remote, const char *hex);
scribe_error_t scribe_cli_remote_info_url(const char *url, const char *token);
scribe_error_t scribe_cli_remote_check(scribe_ctx *ctx, const char *remote);
scribe_error_t scribe_cli_remote_check_url(const char *url, const char *token);
scribe_error_t scribe_cli_remote_refs_url(const char *url, const char *token);
scribe_error_t scribe_cli_remote_log_url(const char *url, const char *token, int oneline, size_t limit, int show_paths,
                                         const char *path_filter, const scribe_time_range *time_range);
scribe_error_t scribe_cli_remote_show_url(const char *url, const char *token, const char *spec);
scribe_error_t scribe_cli_remote_cat_object_url(const char *url, const char *token, char mode, const char *hex);
scribe_error_t scribe_cli_remote_diff_url(const char *url, const char *token, const char *a, const char *b);
scribe_error_t scribe_cli_remote_ls_tree_url(const char *url, const char *token, const char *hex);
scribe_error_t scribe_cli_remote_ingest(scribe_ctx *ctx, const char *remote, const uint8_t *body, size_t body_len);
scribe_error_t scribe_cli_remote_ingest_url(const char *url, const char *token, const uint8_t *body, size_t body_len);
scribe_error_t scribe_cli_remote_ingest_stream(scribe_ctx *ctx, const char *remote, const uint8_t *body,
                                               size_t body_len);
scribe_error_t scribe_cli_remote_ingest_stream_url(const char *url, const char *token, const uint8_t *body,
                                                   size_t body_len);
scribe_error_t scribe_cli_remote_ingest_content(scribe_ctx *ctx, const char *remote, const char *path,
                                                const char *message, scribe_ingest_op op, const uint8_t *body,
                                                size_t body_len);
scribe_error_t scribe_cli_remote_ingest_content_url(const char *url, const char *token, const char *path,
                                                    const char *message, scribe_ingest_op op, const uint8_t *body,
                                                    size_t body_len);
scribe_error_t scribe_cli_daemon(const char *config_path, const char *self_path);
scribe_error_t scribe_cli_info(scribe_ctx *ctx);
scribe_error_t scribe_cli_status(scribe_ctx *ctx);
scribe_error_t scribe_cli_refs(scribe_ctx *ctx);
scribe_error_t scribe_repository_open(scribe_repository *repo, scribe_ctx *ctx, const char *remote);
scribe_error_t scribe_repository_open_url(scribe_repository *repo, const char *url, const char *token);
void scribe_repository_close(scribe_repository *repo);
scribe_error_t scribe_repository_check(scribe_repository *repo);
scribe_error_t scribe_repository_info(scribe_repository *repo);
scribe_error_t scribe_repository_refs(scribe_repository *repo);
scribe_error_t scribe_repository_log(scribe_repository *repo, int oneline, size_t limit, int show_paths,
                                     const char *path_filter, const scribe_time_range *time_range);
scribe_error_t scribe_repository_show(scribe_repository *repo, const char *spec);
scribe_error_t scribe_repository_cat_object(scribe_repository *repo, char mode, const char *hex);
scribe_error_t scribe_repository_diff(scribe_repository *repo, const char *a, const char *b);
scribe_error_t scribe_repository_ls_tree(scribe_repository *repo, const char *hex);
scribe_error_t scribe_repository_ingest(scribe_repository *repo, const uint8_t *body, size_t body_len);
scribe_error_t scribe_repository_ingest_stream(scribe_repository *repo, const uint8_t *body, size_t body_len);
scribe_error_t scribe_repository_ingest_content(scribe_repository *repo, const char *path, const char *message,
                                                scribe_ingest_op op, const uint8_t *body, size_t body_len);
scribe_error_t scribe_cli_list_objects(scribe_ctx *ctx, int type_mask, int reachable, const char *format);
scribe_error_t scribe_cli_ls_tree(scribe_ctx *ctx, const char *hex);
scribe_error_t scribe_resolve_commit(scribe_ctx *ctx, const char *rev, uint8_t out[SCRIBE_HASH_SIZE]);
scribe_error_t scribe_tree_resolve_path(scribe_ctx *ctx, const uint8_t root_tree[SCRIBE_HASH_SIZE], const char *path,
                                        scribe_path_resolution *out);

scribe_error_t scribe_pipe_commit_batch(scribe_ctx *ctx, FILE *in, FILE *out);
scribe_error_t scribe_ndjson_line_event(const char *text, size_t len, char **out, size_t *out_len);
scribe_error_t scribe_ndjson_parse_line_event(const char *line, size_t len, char **out, size_t *out_len);

#endif
