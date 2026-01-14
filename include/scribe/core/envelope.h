/*
 * Scribe - A protocol for Verifiable Data Lineage
 * core/envelope.h - Commit envelope structure
 */

#ifndef SCRIBE_CORE_ENVELOPE_H
#define SCRIBE_CORE_ENVELOPE_H

#include "scribe/types.h"
#include "scribe/error.h"

/* Author information */
typedef struct {
    char *id;           /* e.g., "user:alice" or "service:etl-worker" */
    char *role;         /* e.g., "data_engineer", "admin", "automated" */
    char *email;        /* Optional: email address */
} scribe_author_t;

/* Process information */
typedef struct {
    char *name;         /* e.g., "monthly_reconciliation.py" */
    char *version;      /* e.g., "git:v2.1.0" or "sha256:abc123" */
    char *params;       /* e.g., "--force-update --dry-run=false" */
    char *source;       /* Optional: source file path or URL */
} scribe_process_t;

/* Data change descriptor */
typedef struct {
    char *table_name;           /* Source table/collection name */
    char *operation;            /* "INSERT", "UPDATE", "DELETE" */
    char *primary_key;          /* JSON-encoded primary key */
    scribe_hash_t before_hash;  /* Hash of row before change (zero for INSERT) */
    scribe_hash_t after_hash;   /* Hash of row after change (zero for DELETE) */
} scribe_change_t;

/* Commit envelope - the core data structure */
typedef struct {
    scribe_hash_t commit_id;        /* SHA-256 of this envelope */
    scribe_hash_t parent_id;        /* Previous commit (zero hash for root) */
    scribe_hash_t tree_hash;        /* Merkle root of changed data */

    scribe_author_t author;         /* Who made this change */
    scribe_process_t process;       /* What process was used */

    time_t timestamp;               /* Unix timestamp of commit */
    char *message;                  /* Optional commit message */

    scribe_change_t *changes;       /* Array of changes in this commit */
    size_t change_count;
    size_t change_capacity;
} scribe_envelope_t;

/* Envelope lifecycle */
scribe_envelope_t *scribe_envelope_new(void);
void scribe_envelope_free(scribe_envelope_t *env);
scribe_envelope_t *scribe_envelope_clone(const scribe_envelope_t *env);

/* Envelope building */
scribe_error_t scribe_envelope_set_author(scribe_envelope_t *env,
                                          const char *id,
                                          const char *role);

scribe_error_t scribe_envelope_set_author_email(scribe_envelope_t *env,
                                                const char *email);

scribe_error_t scribe_envelope_set_process(scribe_envelope_t *env,
                                           const char *name,
                                           const char *version,
                                           const char *params);

scribe_error_t scribe_envelope_set_process_source(scribe_envelope_t *env,
                                                   const char *source);

scribe_error_t scribe_envelope_set_parent(scribe_envelope_t *env,
                                          const scribe_hash_t *parent);

scribe_error_t scribe_envelope_set_message(scribe_envelope_t *env,
                                           const char *message);

scribe_error_t scribe_envelope_set_tree_hash(scribe_envelope_t *env,
                                             const scribe_hash_t *tree_hash);

/* Add a change to the envelope */
scribe_error_t scribe_envelope_add_change(scribe_envelope_t *env,
                                          const char *table_name,
                                          const char *operation,
                                          const char *primary_key,
                                          const scribe_hash_t *before_hash,
                                          const scribe_hash_t *after_hash);

/* Finalization - computes tree hash (if not set) and commit_id */
scribe_error_t scribe_envelope_finalize(scribe_envelope_t *env);

/* Verify envelope integrity */
scribe_error_t scribe_envelope_verify(const scribe_envelope_t *env);

/* JSON serialization */
char *scribe_envelope_to_json(const scribe_envelope_t *env);
scribe_envelope_t *scribe_envelope_from_json(const char *json);

#endif /* SCRIBE_CORE_ENVELOPE_H */
