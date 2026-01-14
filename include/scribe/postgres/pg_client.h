/*
 * Scribe - A protocol for Verifiable Data Lineage
 * postgres/pg_client.h - PostgreSQL connection management
 */

#ifndef SCRIBE_POSTGRES_PG_CLIENT_H
#define SCRIBE_POSTGRES_PG_CLIENT_H

#include "scribe/error.h"

/* Opaque PostgreSQL connection handle */
typedef struct scribe_pg_conn scribe_pg_conn_t;

/* Connection state */
typedef enum {
    SCRIBE_PG_DISCONNECTED = 0,
    SCRIBE_PG_CONNECTED,
    SCRIBE_PG_REPLICATION  /* In replication mode */
} scribe_pg_state_t;

/* Create a new PostgreSQL connection */
scribe_pg_conn_t *scribe_pg_conn_new(const char *connection_string);

/* Free connection */
void scribe_pg_conn_free(scribe_pg_conn_t *conn);

/* Connect to database */
scribe_error_t scribe_pg_connect(scribe_pg_conn_t *conn);

/* Disconnect */
void scribe_pg_disconnect(scribe_pg_conn_t *conn);

/* Get connection state */
scribe_pg_state_t scribe_pg_get_state(const scribe_pg_conn_t *conn);

/* Execute a query (returns JSON result or NULL on error) */
char *scribe_pg_query(scribe_pg_conn_t *conn, const char *sql);

/* Execute a command (no result expected) */
scribe_error_t scribe_pg_execute(scribe_pg_conn_t *conn, const char *sql);

/* Get last error message */
const char *scribe_pg_error(const scribe_pg_conn_t *conn);

/* Get underlying socket for select()/poll() */
int scribe_pg_socket(const scribe_pg_conn_t *conn);

/* Consume input (for async operations) */
scribe_error_t scribe_pg_consume_input(scribe_pg_conn_t *conn);

/* Check if connection is busy */
int scribe_pg_is_busy(const scribe_pg_conn_t *conn);

#endif /* SCRIBE_POSTGRES_PG_CLIENT_H */
