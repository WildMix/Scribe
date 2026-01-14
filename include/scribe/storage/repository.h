/*
 * Scribe - A protocol for Verifiable Data Lineage
 * storage/repository.h - Repository management
 */

#ifndef SCRIBE_STORAGE_REPOSITORY_H
#define SCRIBE_STORAGE_REPOSITORY_H

#include "scribe/types.h"
#include "scribe/error.h"
#include "scribe/core/envelope.h"

/* Repository configuration */
typedef struct {
    char *author_id;            /* Default author ID */
    char *author_role;          /* Default author role */
    char *pg_connection_string; /* PostgreSQL connection (optional) */
    char **watched_tables;      /* Tables to monitor */
    size_t watched_table_count;
} scribe_config_t;

/* Repository handle */
typedef struct scribe_repo scribe_repo_t;

/* Repository discovery and initialization */
scribe_repo_t *scribe_repo_open(const char *path);
scribe_repo_t *scribe_repo_init(const char *path);
void scribe_repo_close(scribe_repo_t *repo);

/* Check if path is inside a repository */
int scribe_repo_exists(const char *path);

/* Get repository paths */
const char *scribe_repo_get_root(const scribe_repo_t *repo);
const char *scribe_repo_get_db_path(const scribe_repo_t *repo);
const char *scribe_repo_get_objects_path(const scribe_repo_t *repo);

/* Configuration */
scribe_config_t *scribe_config_load(const scribe_repo_t *repo);
scribe_error_t scribe_config_save(const scribe_repo_t *repo, const scribe_config_t *config);
void scribe_config_free(scribe_config_t *config);

/* HEAD management */
scribe_error_t scribe_repo_get_head(const scribe_repo_t *repo, scribe_hash_t *out_hash);
scribe_error_t scribe_repo_set_head(scribe_repo_t *repo, const scribe_hash_t *hash);

/* Commit operations */
scribe_error_t scribe_repo_store_commit(scribe_repo_t *repo, const scribe_envelope_t *env);
scribe_envelope_t *scribe_repo_load_commit(scribe_repo_t *repo, const scribe_hash_t *hash);

/* History queries */
typedef struct {
    scribe_hash_t *hashes;
    size_t count;
} scribe_commit_list_t;

scribe_commit_list_t *scribe_repo_get_history(scribe_repo_t *repo,
                                               const scribe_hash_t *from,
                                               size_t limit);
void scribe_commit_list_free(scribe_commit_list_t *list);

#endif /* SCRIBE_STORAGE_REPOSITORY_H */
