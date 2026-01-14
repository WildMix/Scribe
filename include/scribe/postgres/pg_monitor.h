/*
 * Scribe - A protocol for Verifiable Data Lineage
 * postgres/pg_monitor.h - PostgreSQL change monitoring
 */

#ifndef SCRIBE_POSTGRES_PG_MONITOR_H
#define SCRIBE_POSTGRES_PG_MONITOR_H

#include "scribe/error.h"
#include "scribe/types.h"

/* CDC mode selection */
typedef enum {
    SCRIBE_PG_MODE_TRIGGER,     /* Audit trigger-based CDC */
    SCRIBE_PG_MODE_LOGICAL      /* Logical replication-based CDC */
} scribe_pg_mode_t;

/* Row change captured from PostgreSQL */
typedef struct {
    char *table_name;           /* Source table */
    char *operation;            /* INSERT, UPDATE, DELETE */
    char *primary_key_json;     /* JSON-encoded PK */
    char *before_json;          /* Full row before (NULL for INSERT) */
    char *after_json;           /* Full row after (NULL for DELETE) */
    int64_t transaction_id;     /* PostgreSQL txid */
    int64_t lsn;               /* Log Sequence Number */
} scribe_pg_change_t;

/* Monitor configuration */
typedef struct {
    char *connection_string;    /* PostgreSQL connection string */
    scribe_pg_mode_t mode;      /* CDC mode */
    char **tables;              /* Tables to watch */
    size_t table_count;
    int poll_interval_ms;       /* Polling interval */
    char *slot_name;            /* Logical replication slot */
    char *publication_name;     /* Publication name */
} scribe_pg_monitor_config_t;

/* Monitor handle */
typedef struct scribe_pg_monitor scribe_pg_monitor_t;

/* Callback for changes */
typedef void (*scribe_pg_change_callback)(const scribe_pg_change_t *change, void *user_data);

/* Monitor lifecycle */
scribe_pg_monitor_t *scribe_pg_monitor_new(const scribe_pg_monitor_config_t *config);
void scribe_pg_monitor_free(scribe_pg_monitor_t *monitor);

/* Setup (creates triggers or replication slot) */
scribe_error_t scribe_pg_monitor_setup(scribe_pg_monitor_t *monitor);

/* Cleanup (removes triggers or replication slot) */
scribe_error_t scribe_pg_monitor_cleanup(scribe_pg_monitor_t *monitor);

/* Start monitoring (blocking) */
scribe_error_t scribe_pg_monitor_start(scribe_pg_monitor_t *monitor,
                                        scribe_pg_change_callback callback,
                                        void *user_data);

/* Stop monitoring */
scribe_error_t scribe_pg_monitor_stop(scribe_pg_monitor_t *monitor);

/* Check if running */
int scribe_pg_monitor_is_running(const scribe_pg_monitor_t *monitor);

/* Manual poll (for non-blocking usage) */
scribe_error_t scribe_pg_monitor_poll(scribe_pg_monitor_t *monitor,
                                       scribe_pg_change_t **out_changes,
                                       size_t *out_count);

/* Cleanup captured changes */
void scribe_pg_changes_free(scribe_pg_change_t *changes, size_t count);
void scribe_pg_change_free(scribe_pg_change_t *change);

/* Get last LSN processed */
int64_t scribe_pg_monitor_get_lsn(const scribe_pg_monitor_t *monitor);

#endif /* SCRIBE_POSTGRES_PG_MONITOR_H */
