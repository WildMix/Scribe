/*
 * Scribe - A protocol for Verifiable Data Lineage
 * storage/database.h - SQLite database operations
 */

#ifndef SCRIBE_STORAGE_DATABASE_H
#define SCRIBE_STORAGE_DATABASE_H

#include "scribe/types.h"
#include "scribe/error.h"
#include "scribe/core/envelope.h"

/* Database handle (opaque) */
typedef struct scribe_db scribe_db_t;

/* Database lifecycle */
scribe_db_t *scribe_db_open(const char *path);
void scribe_db_close(scribe_db_t *db);
scribe_error_t scribe_db_init_schema(scribe_db_t *db);

/* Transaction control */
scribe_error_t scribe_db_begin(scribe_db_t *db);
scribe_error_t scribe_db_commit(scribe_db_t *db);
scribe_error_t scribe_db_rollback(scribe_db_t *db);

/* Commit storage */
scribe_error_t scribe_db_store_commit(scribe_db_t *db, const scribe_envelope_t *env);
scribe_envelope_t *scribe_db_load_commit(scribe_db_t *db, const scribe_hash_t *hash);
int scribe_db_commit_exists(scribe_db_t *db, const scribe_hash_t *hash);

/* History queries */
typedef struct {
    scribe_hash_t *hashes;
    size_t count;
} scribe_db_commit_list_t;

scribe_db_commit_list_t *scribe_db_get_history(scribe_db_t *db,
                                                const scribe_hash_t *from,
                                                size_t limit);
void scribe_db_commit_list_free(scribe_db_commit_list_t *list);

/* Query by author/process */
scribe_db_commit_list_t *scribe_db_find_by_author(scribe_db_t *db,
                                                   const char *author_id);
scribe_db_commit_list_t *scribe_db_find_by_process(scribe_db_t *db,
                                                    const char *process_name);

/* Refs management */
scribe_error_t scribe_db_get_ref(scribe_db_t *db, const char *name, scribe_hash_t *out_hash);
scribe_error_t scribe_db_set_ref(scribe_db_t *db, const char *name, const scribe_hash_t *hash);

/* Statistics */
size_t scribe_db_commit_count(scribe_db_t *db);

#endif /* SCRIBE_STORAGE_DATABASE_H */
