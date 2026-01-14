/*
 * Scribe - A protocol for Verifiable Data Lineage
 * postgres/pg_logical.c - Logical replication CDC
 */

#include "scribe/postgres/pg_monitor.h"
#include "scribe/postgres/pg_client.h"
#include "scribe/error.h"

#include <libpq-fe.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Check if logical replication is available
 */
int scribe_pg_logical_available(scribe_pg_conn_t *conn)
{
    if (!conn) return 0;

    char *result = scribe_pg_query(conn, "SHOW wal_level;");
    if (!result) return 0;

    int available = (strstr(result, "logical") != NULL);
    free(result);

    return available;
}

/*
 * Create a logical replication slot
 */
scribe_error_t scribe_pg_logical_create_slot(scribe_pg_conn_t *conn,
                                              const char *slot_name)
{
    if (!conn || !slot_name) return SCRIBE_ERR_INVALID_ARG;

    /* Check if slot exists */
    char check_sql[256];
    snprintf(check_sql, sizeof(check_sql),
             "SELECT 1 FROM pg_replication_slots WHERE slot_name = '%s';",
             slot_name);

    char *result = scribe_pg_query(conn, check_sql);
    if (result && strcmp(result, "[]") != 0) {
        free(result);
        return SCRIBE_OK;  /* Already exists */
    }
    free(result);

    /* Create slot */
    char create_sql[256];
    snprintf(create_sql, sizeof(create_sql),
             "SELECT pg_create_logical_replication_slot('%s', 'pgoutput');",
             slot_name);

    scribe_error_t err = scribe_pg_execute(conn, create_sql);
    if (err != SCRIBE_OK) {
        scribe_set_error_detail("Failed to create replication slot: %s",
                               scribe_pg_error(conn));
        return err;
    }

    return SCRIBE_OK;
}

/*
 * Create a publication for tables
 */
scribe_error_t scribe_pg_logical_create_publication(scribe_pg_conn_t *conn,
                                                     const char *publication_name,
                                                     const char **tables,
                                                     size_t table_count)
{
    if (!conn || !publication_name) return SCRIBE_ERR_INVALID_ARG;

    /* Drop existing publication */
    char drop_sql[256];
    snprintf(drop_sql, sizeof(drop_sql),
             "DROP PUBLICATION IF EXISTS %s;",
             publication_name);
    scribe_pg_execute(conn, drop_sql);

    /* Build table list */
    char table_list[4096] = "";
    if (tables && table_count > 0) {
        for (size_t i = 0; i < table_count; i++) {
            if (i > 0) strcat(table_list, ", ");
            strcat(table_list, tables[i]);
        }
    }

    /* Create publication */
    char create_sql[4096];
    if (table_count > 0) {
        snprintf(create_sql, sizeof(create_sql),
                 "CREATE PUBLICATION %s FOR TABLE %s;",
                 publication_name, table_list);
    } else {
        snprintf(create_sql, sizeof(create_sql),
                 "CREATE PUBLICATION %s FOR ALL TABLES;",
                 publication_name);
    }

    scribe_error_t err = scribe_pg_execute(conn, create_sql);
    if (err != SCRIBE_OK) {
        scribe_set_error_detail("Failed to create publication: %s",
                               scribe_pg_error(conn));
        return err;
    }

    return SCRIBE_OK;
}

/*
 * Drop a logical replication slot
 */
scribe_error_t scribe_pg_logical_drop_slot(scribe_pg_conn_t *conn,
                                            const char *slot_name)
{
    if (!conn || !slot_name) return SCRIBE_ERR_INVALID_ARG;

    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT pg_drop_replication_slot('%s');",
             slot_name);

    return scribe_pg_execute(conn, sql);
}

/*
 * Get changes from logical replication
 * This is a simplified version - production would use streaming protocol
 */
scribe_error_t scribe_pg_logical_get_changes(scribe_pg_conn_t *conn,
                                              const char *slot_name,
                                              scribe_pg_change_t **out_changes,
                                              size_t *out_count)
{
    if (!conn || !slot_name || !out_changes || !out_count) {
        return SCRIBE_ERR_INVALID_ARG;
    }

    *out_changes = NULL;
    *out_count = 0;

    /* Use pg_logical_slot_get_changes to peek at changes */
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT lsn, xid, data FROM pg_logical_slot_peek_changes('%s', NULL, 100);",
             slot_name);

    char *result = scribe_pg_query(conn, sql);
    if (!result) {
        return SCRIBE_ERR_PG_QUERY;
    }

    if (strcmp(result, "[]") == 0) {
        free(result);
        return SCRIBE_OK;  /* No changes */
    }

    /* For now, we just acknowledge we got data */
    /* Production implementation would parse the pgoutput format */

    /* Advance the slot */
    snprintf(sql, sizeof(sql),
             "SELECT pg_logical_slot_get_changes('%s', NULL, 100);",
             slot_name);
    scribe_pg_execute(conn, sql);

    free(result);
    return SCRIBE_OK;
}

/*
 * Set replica identity for a table (needed for UPDATE/DELETE to get old values)
 */
scribe_error_t scribe_pg_logical_set_replica_identity(scribe_pg_conn_t *conn,
                                                       const char *table_name,
                                                       const char *identity)
{
    if (!conn || !table_name) return SCRIBE_ERR_INVALID_ARG;

    const char *id = identity ? identity : "FULL";

    char sql[256];
    snprintf(sql, sizeof(sql),
             "ALTER TABLE %s REPLICA IDENTITY %s;",
             table_name, id);

    return scribe_pg_execute(conn, sql);
}
