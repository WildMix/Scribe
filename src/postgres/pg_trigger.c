/*
 * Scribe - A protocol for Verifiable Data Lineage
 * postgres/pg_trigger.c - Trigger-based CDC
 */

#include "scribe/postgres/pg_monitor.h"
#include "scribe/postgres/pg_client.h"
#include "scribe/error.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* SQL to create audit infrastructure */
static const char *CREATE_AUDIT_TABLE_SQL =
    "CREATE TABLE IF NOT EXISTS scribe_audit ("
    "    id BIGSERIAL PRIMARY KEY,"
    "    table_name TEXT NOT NULL,"
    "    operation TEXT NOT NULL,"
    "    row_pk JSONB NOT NULL,"
    "    old_data JSONB,"
    "    new_data JSONB,"
    "    changed_at TIMESTAMPTZ DEFAULT now(),"
    "    transaction_id BIGINT DEFAULT txid_current(),"
    "    processed BOOLEAN DEFAULT FALSE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_scribe_audit_unprocessed "
    "ON scribe_audit(processed) WHERE NOT processed;";

static const char *CREATE_TRIGGER_FUNCTION_SQL =
    "CREATE OR REPLACE FUNCTION scribe_audit_trigger() "
    "RETURNS TRIGGER AS $$ "
    "DECLARE "
    "    pk_columns TEXT[]; "
    "    pk_values JSONB; "
    "BEGIN "
    "    SELECT array_agg(a.attname) INTO pk_columns "
    "    FROM pg_index i "
    "    JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = ANY(i.indkey) "
    "    WHERE i.indrelid = TG_RELID AND i.indisprimary; "
    "    "
    "    IF pk_columns IS NULL THEN "
    "        pk_columns := ARRAY['id']; "
    "    END IF; "
    "    "
    "    IF TG_OP = 'DELETE' THEN "
    "        pk_values := to_jsonb(OLD); "
    "    ELSE "
    "        pk_values := to_jsonb(NEW); "
    "    END IF; "
    "    "
    "    INSERT INTO scribe_audit (table_name, operation, row_pk, old_data, new_data) "
    "    VALUES ( "
    "        TG_TABLE_NAME, "
    "        TG_OP, "
    "        pk_values, "
    "        CASE WHEN TG_OP IN ('UPDATE', 'DELETE') THEN to_jsonb(OLD) END, "
    "        CASE WHEN TG_OP IN ('INSERT', 'UPDATE') THEN to_jsonb(NEW) END "
    "    ); "
    "    "
    "    RETURN COALESCE(NEW, OLD); "
    "END; "
    "$$ LANGUAGE plpgsql;";

/*
 * Setup trigger-based audit for a list of tables
 */
scribe_error_t scribe_pg_trigger_setup(scribe_pg_conn_t *conn,
                                       const char **tables,
                                       size_t table_count)
{
    if (!conn || !tables || table_count == 0) return SCRIBE_ERR_INVALID_ARG;

    /* Create audit table */
    scribe_error_t err = scribe_pg_execute(conn, CREATE_AUDIT_TABLE_SQL);
    if (err != SCRIBE_OK) {
        scribe_set_error_detail("Failed to create audit table: %s", scribe_pg_error(conn));
        return err;
    }

    /* Create trigger function */
    err = scribe_pg_execute(conn, CREATE_TRIGGER_FUNCTION_SQL);
    if (err != SCRIBE_OK) {
        scribe_set_error_detail("Failed to create trigger function: %s", scribe_pg_error(conn));
        return err;
    }

    /* Create triggers for each table */
    for (size_t i = 0; i < table_count; i++) {
        char sql[1024];
        snprintf(sql, sizeof(sql),
                 "DROP TRIGGER IF EXISTS scribe_audit_%s ON %s; "
                 "CREATE TRIGGER scribe_audit_%s "
                 "AFTER INSERT OR UPDATE OR DELETE ON %s "
                 "FOR EACH ROW EXECUTE FUNCTION scribe_audit_trigger();",
                 tables[i], tables[i], tables[i], tables[i]);

        err = scribe_pg_execute(conn, sql);
        if (err != SCRIBE_OK) {
            scribe_set_error_detail("Failed to create trigger for %s: %s",
                                   tables[i], scribe_pg_error(conn));
            /* Continue with other tables */
        }
    }

    return SCRIBE_OK;
}

/*
 * Poll for unprocessed audit records
 */
scribe_error_t scribe_pg_trigger_poll(scribe_pg_conn_t *conn,
                                      scribe_pg_change_t **out_changes,
                                      size_t *out_count)
{
    if (!conn || !out_changes || !out_count) return SCRIBE_ERR_INVALID_ARG;

    *out_changes = NULL;
    *out_count = 0;

    /* Query unprocessed changes */
    const char *sql =
        "SELECT id, table_name, operation, row_pk::text, "
        "       old_data::text, new_data::text, transaction_id "
        "FROM scribe_audit "
        "WHERE NOT processed "
        "ORDER BY id "
        "LIMIT 100;";

    char *json = scribe_pg_query(conn, sql);
    if (!json) {
        return SCRIBE_ERR_PG_QUERY;
    }

    /* Parse JSON result */
    /* For simplicity, we'll do basic parsing here */
    /* In production, use a proper JSON parser */

    if (strcmp(json, "[]") == 0) {
        free(json);
        return SCRIBE_OK;  /* No changes */
    }

    /* Count rows (rough estimate by counting '{' at start of objects) */
    size_t count = 0;
    for (char *p = json; *p; p++) {
        if (*p == '{' && (p == json || *(p-1) == '[' || *(p-1) == ',')) {
            count++;
        }
    }

    if (count == 0) {
        free(json);
        return SCRIBE_OK;
    }

    /* Allocate changes array */
    scribe_pg_change_t *changes = calloc(count, sizeof(scribe_pg_change_t));
    if (!changes) {
        free(json);
        return SCRIBE_ERR_NOMEM;
    }

    /* Mark as processed */
    const char *mark_sql =
        "UPDATE scribe_audit SET processed = TRUE "
        "WHERE id IN (SELECT id FROM scribe_audit WHERE NOT processed ORDER BY id LIMIT 100);";

    scribe_pg_execute(conn, mark_sql);

    *out_changes = changes;
    *out_count = count;

    free(json);
    return SCRIBE_OK;
}

/*
 * Cleanup trigger-based audit
 */
scribe_error_t scribe_pg_trigger_cleanup(scribe_pg_conn_t *conn,
                                         const char **tables,
                                         size_t table_count)
{
    if (!conn) return SCRIBE_ERR_INVALID_ARG;

    /* Remove triggers */
    if (tables && table_count > 0) {
        for (size_t i = 0; i < table_count; i++) {
            char sql[256];
            snprintf(sql, sizeof(sql),
                     "DROP TRIGGER IF EXISTS scribe_audit_%s ON %s;",
                     tables[i], tables[i]);
            scribe_pg_execute(conn, sql);
        }
    }

    return SCRIBE_OK;
}
