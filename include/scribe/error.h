/*
 * Scribe - A protocol for Verifiable Data Lineage
 * error.h - Error codes and handling
 */

#ifndef SCRIBE_ERROR_H
#define SCRIBE_ERROR_H

/* Error codes */
typedef enum {
    SCRIBE_OK = 0,

    /* General errors */
    SCRIBE_ERR_NOMEM = -1,          /* Out of memory */
    SCRIBE_ERR_INVALID_ARG = -2,    /* Invalid argument */
    SCRIBE_ERR_NOT_FOUND = -3,      /* Resource not found */

    /* Repository errors */
    SCRIBE_ERR_NOT_A_REPO = -10,    /* Not inside a repository */
    SCRIBE_ERR_REPO_EXISTS = -11,   /* Repository already exists */
    SCRIBE_ERR_REPO_CORRUPT = -12,  /* Repository is corrupted */

    /* Storage errors */
    SCRIBE_ERR_IO = -20,            /* I/O error */
    SCRIBE_ERR_DB = -21,            /* Database error */
    SCRIBE_ERR_OBJECT_MISSING = -22,/* Object not in store */

    /* Hash/crypto errors */
    SCRIBE_ERR_HASH_MISMATCH = -30, /* Hash verification failed */
    SCRIBE_ERR_CRYPTO = -31,        /* Cryptographic operation failed */

    /* PostgreSQL errors */
    SCRIBE_ERR_PG_CONNECT = -40,    /* Connection failed */
    SCRIBE_ERR_PG_QUERY = -41,      /* Query failed */
    SCRIBE_ERR_PG_REPLICATION = -42,/* Replication error */

    /* Parse errors */
    SCRIBE_ERR_JSON_PARSE = -50,    /* JSON parsing failed */
    SCRIBE_ERR_JSON_SCHEMA = -51,   /* JSON schema validation failed */
} scribe_error_t;

/* Get human-readable error string */
const char *scribe_error_string(scribe_error_t err);

/* Thread-local error details */
void scribe_set_error_detail(const char *fmt, ...);
const char *scribe_get_error_detail(void);
void scribe_clear_error_detail(void);

#endif /* SCRIBE_ERROR_H */
