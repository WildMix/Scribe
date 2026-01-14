/*
 * Scribe - A protocol for Verifiable Data Lineage
 * postgres/pg_monitor.c - Change monitoring orchestration
 */

#include "scribe/postgres/pg_monitor.h"
#include "scribe/postgres/pg_client.h"
#include "scribe/error.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* External trigger functions */
extern scribe_error_t scribe_pg_trigger_setup(scribe_pg_conn_t *conn,
                                              const char **tables,
                                              size_t table_count);
extern scribe_error_t scribe_pg_trigger_poll(scribe_pg_conn_t *conn,
                                             scribe_pg_change_t **out_changes,
                                             size_t *out_count);
extern scribe_error_t scribe_pg_trigger_cleanup(scribe_pg_conn_t *conn,
                                                const char **tables,
                                                size_t table_count);

/* External logical replication functions */
extern int scribe_pg_logical_available(scribe_pg_conn_t *conn);
extern scribe_error_t scribe_pg_logical_create_slot(scribe_pg_conn_t *conn,
                                                    const char *slot_name);
extern scribe_error_t scribe_pg_logical_create_publication(scribe_pg_conn_t *conn,
                                                           const char *publication_name,
                                                           const char **tables,
                                                           size_t table_count);
extern scribe_error_t scribe_pg_logical_drop_slot(scribe_pg_conn_t *conn,
                                                  const char *slot_name);
extern scribe_error_t scribe_pg_logical_get_changes(scribe_pg_conn_t *conn,
                                                    const char *slot_name,
                                                    scribe_pg_change_t **out_changes,
                                                    size_t *out_count);
extern scribe_error_t scribe_pg_logical_set_replica_identity(scribe_pg_conn_t *conn,
                                                             const char *table_name,
                                                             const char *identity);

struct scribe_pg_monitor {
    scribe_pg_monitor_config_t config;
    scribe_pg_conn_t *conn;
    int running;
    int64_t last_lsn;
};

static char *safe_strdup(const char *s)
{
    return s ? strdup(s) : NULL;
}

scribe_pg_monitor_t *scribe_pg_monitor_new(const scribe_pg_monitor_config_t *config)
{
    if (!config || !config->connection_string) return NULL;

    scribe_pg_monitor_t *monitor = calloc(1, sizeof(scribe_pg_monitor_t));
    if (!monitor) return NULL;

    /* Copy config */
    monitor->config.connection_string = safe_strdup(config->connection_string);
    monitor->config.mode = config->mode;
    monitor->config.poll_interval_ms = config->poll_interval_ms > 0 ? config->poll_interval_ms : 1000;
    monitor->config.slot_name = safe_strdup(config->slot_name ? config->slot_name : "scribe_slot");
    monitor->config.publication_name = safe_strdup(config->publication_name ? config->publication_name : "scribe_pub");

    /* Copy tables */
    if (config->tables && config->table_count > 0) {
        monitor->config.tables = calloc(config->table_count, sizeof(char *));
        if (monitor->config.tables) {
            for (size_t i = 0; i < config->table_count; i++) {
                monitor->config.tables[i] = safe_strdup(config->tables[i]);
            }
            monitor->config.table_count = config->table_count;
        }
    }

    /* Create connection */
    monitor->conn = scribe_pg_conn_new(config->connection_string);
    if (!monitor->conn) {
        scribe_pg_monitor_free(monitor);
        return NULL;
    }

    return monitor;
}

void scribe_pg_monitor_free(scribe_pg_monitor_t *monitor)
{
    if (!monitor) return;

    scribe_pg_conn_free(monitor->conn);

    free(monitor->config.connection_string);
    free(monitor->config.slot_name);
    free(monitor->config.publication_name);

    for (size_t i = 0; i < monitor->config.table_count; i++) {
        free(monitor->config.tables[i]);
    }
    free(monitor->config.tables);

    free(monitor);
}

scribe_error_t scribe_pg_monitor_setup(scribe_pg_monitor_t *monitor)
{
    if (!monitor) return SCRIBE_ERR_INVALID_ARG;

    /* Connect */
    scribe_error_t err = scribe_pg_connect(monitor->conn);
    if (err != SCRIBE_OK) {
        return err;
    }

    if (monitor->config.mode == SCRIBE_PG_MODE_LOGICAL) {
        /* Check if logical replication is available */
        if (!scribe_pg_logical_available(monitor->conn)) {
            scribe_set_error_detail("Logical replication not available. "
                                   "Set wal_level = logical in postgresql.conf");
            return SCRIBE_ERR_PG_REPLICATION;
        }

        /* Create replication slot */
        err = scribe_pg_logical_create_slot(monitor->conn, monitor->config.slot_name);
        if (err != SCRIBE_OK) return err;

        /* Create publication */
        err = scribe_pg_logical_create_publication(monitor->conn,
                                                   monitor->config.publication_name,
                                                   (const char **)monitor->config.tables,
                                                   monitor->config.table_count);
        if (err != SCRIBE_OK) return err;

        /* Set replica identity for watched tables */
        for (size_t i = 0; i < monitor->config.table_count; i++) {
            scribe_pg_logical_set_replica_identity(monitor->conn,
                                                   monitor->config.tables[i],
                                                   "FULL");
        }
    } else {
        /* Trigger-based setup */
        err = scribe_pg_trigger_setup(monitor->conn,
                                      (const char **)monitor->config.tables,
                                      monitor->config.table_count);
        if (err != SCRIBE_OK) return err;
    }

    return SCRIBE_OK;
}

scribe_error_t scribe_pg_monitor_cleanup(scribe_pg_monitor_t *monitor)
{
    if (!monitor) return SCRIBE_ERR_INVALID_ARG;

    if (scribe_pg_get_state(monitor->conn) != SCRIBE_PG_CONNECTED) {
        scribe_pg_connect(monitor->conn);
    }

    if (monitor->config.mode == SCRIBE_PG_MODE_LOGICAL) {
        scribe_pg_logical_drop_slot(monitor->conn, monitor->config.slot_name);
    } else {
        scribe_pg_trigger_cleanup(monitor->conn,
                                  (const char **)monitor->config.tables,
                                  monitor->config.table_count);
    }

    return SCRIBE_OK;
}

scribe_error_t scribe_pg_monitor_start(scribe_pg_monitor_t *monitor,
                                        scribe_pg_change_callback callback,
                                        void *user_data)
{
    if (!monitor || !callback) return SCRIBE_ERR_INVALID_ARG;

    monitor->running = 1;

    while (monitor->running) {
        scribe_pg_change_t *changes = NULL;
        size_t count = 0;

        scribe_error_t err = scribe_pg_monitor_poll(monitor, &changes, &count);
        if (err != SCRIBE_OK) {
            /* Try to reconnect */
            scribe_pg_disconnect(monitor->conn);
            sleep(1);
            scribe_pg_connect(monitor->conn);
            continue;
        }

        /* Process changes */
        for (size_t i = 0; i < count; i++) {
            callback(&changes[i], user_data);
        }

        scribe_pg_changes_free(changes, count);

        /* Sleep until next poll */
        usleep(monitor->config.poll_interval_ms * 1000);
    }

    return SCRIBE_OK;
}

scribe_error_t scribe_pg_monitor_stop(scribe_pg_monitor_t *monitor)
{
    if (!monitor) return SCRIBE_ERR_INVALID_ARG;
    monitor->running = 0;
    return SCRIBE_OK;
}

int scribe_pg_monitor_is_running(const scribe_pg_monitor_t *monitor)
{
    return monitor ? monitor->running : 0;
}

scribe_error_t scribe_pg_monitor_poll(scribe_pg_monitor_t *monitor,
                                       scribe_pg_change_t **out_changes,
                                       size_t *out_count)
{
    if (!monitor || !out_changes || !out_count) return SCRIBE_ERR_INVALID_ARG;

    *out_changes = NULL;
    *out_count = 0;

    /* Ensure connected */
    if (scribe_pg_get_state(monitor->conn) != SCRIBE_PG_CONNECTED) {
        scribe_error_t err = scribe_pg_connect(monitor->conn);
        if (err != SCRIBE_OK) return err;
    }

    if (monitor->config.mode == SCRIBE_PG_MODE_LOGICAL) {
        return scribe_pg_logical_get_changes(monitor->conn,
                                             monitor->config.slot_name,
                                             out_changes,
                                             out_count);
    } else {
        return scribe_pg_trigger_poll(monitor->conn, out_changes, out_count);
    }
}

void scribe_pg_changes_free(scribe_pg_change_t *changes, size_t count)
{
    if (!changes) return;

    for (size_t i = 0; i < count; i++) {
        scribe_pg_change_free(&changes[i]);
    }

    free(changes);
}

void scribe_pg_change_free(scribe_pg_change_t *change)
{
    if (!change) return;

    free(change->table_name);
    free(change->operation);
    free(change->primary_key_json);
    free(change->before_json);
    free(change->after_json);
}

int64_t scribe_pg_monitor_get_lsn(const scribe_pg_monitor_t *monitor)
{
    return monitor ? monitor->last_lsn : 0;
}
