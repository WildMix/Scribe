/*
 * Scribe - A protocol for Verifiable Data Lineage
 * postgres/pg_client.c - PostgreSQL connection management
 */

#include "scribe/postgres/pg_client.h"
#include "scribe/error.h"

#include <libpq-fe.h>
#include <stdlib.h>
#include <string.h>

struct scribe_pg_conn {
    PGconn *pg_conn;
    char *connection_string;
    scribe_pg_state_t state;
    char *last_error;
};

scribe_pg_conn_t *scribe_pg_conn_new(const char *connection_string)
{
    if (!connection_string) return NULL;

    scribe_pg_conn_t *conn = calloc(1, sizeof(scribe_pg_conn_t));
    if (!conn) return NULL;

    conn->connection_string = strdup(connection_string);
    if (!conn->connection_string) {
        free(conn);
        return NULL;
    }

    conn->state = SCRIBE_PG_DISCONNECTED;
    return conn;
}

void scribe_pg_conn_free(scribe_pg_conn_t *conn)
{
    if (!conn) return;

    scribe_pg_disconnect(conn);
    free(conn->connection_string);
    free(conn->last_error);
    free(conn);
}

static void set_error(scribe_pg_conn_t *conn, const char *msg)
{
    free(conn->last_error);
    conn->last_error = msg ? strdup(msg) : NULL;
}

scribe_error_t scribe_pg_connect(scribe_pg_conn_t *conn)
{
    if (!conn) return SCRIBE_ERR_INVALID_ARG;

    if (conn->pg_conn) {
        PQfinish(conn->pg_conn);
        conn->pg_conn = NULL;
    }

    conn->pg_conn = PQconnectdb(conn->connection_string);
    if (!conn->pg_conn) {
        set_error(conn, "Failed to allocate connection");
        return SCRIBE_ERR_PG_CONNECT;
    }

    if (PQstatus(conn->pg_conn) != CONNECTION_OK) {
        set_error(conn, PQerrorMessage(conn->pg_conn));
        PQfinish(conn->pg_conn);
        conn->pg_conn = NULL;
        return SCRIBE_ERR_PG_CONNECT;
    }

    conn->state = SCRIBE_PG_CONNECTED;
    set_error(conn, NULL);
    return SCRIBE_OK;
}

void scribe_pg_disconnect(scribe_pg_conn_t *conn)
{
    if (!conn) return;

    if (conn->pg_conn) {
        PQfinish(conn->pg_conn);
        conn->pg_conn = NULL;
    }

    conn->state = SCRIBE_PG_DISCONNECTED;
}

scribe_pg_state_t scribe_pg_get_state(const scribe_pg_conn_t *conn)
{
    return conn ? conn->state : SCRIBE_PG_DISCONNECTED;
}

char *scribe_pg_query(scribe_pg_conn_t *conn, const char *sql)
{
    if (!conn || !conn->pg_conn || !sql) return NULL;

    PGresult *result = PQexec(conn->pg_conn, sql);
    if (!result) {
        set_error(conn, PQerrorMessage(conn->pg_conn));
        return NULL;
    }

    ExecStatusType status = PQresultStatus(result);
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
        set_error(conn, PQresultErrorMessage(result));
        PQclear(result);
        return NULL;
    }

    /* Convert result to JSON */
    int nrows = PQntuples(result);
    int ncols = PQnfields(result);

    if (nrows == 0 || ncols == 0) {
        PQclear(result);
        return strdup("[]");
    }

    /* Build JSON array */
    size_t buf_size = 4096;
    char *json = malloc(buf_size);
    if (!json) {
        PQclear(result);
        return NULL;
    }

    size_t pos = 0;
    json[pos++] = '[';

    for (int row = 0; row < nrows; row++) {
        if (row > 0) json[pos++] = ',';
        json[pos++] = '{';

        for (int col = 0; col < ncols; col++) {
            if (col > 0) json[pos++] = ',';

            const char *name = PQfname(result, col);
            const char *value = PQgetvalue(result, row, col);
            int is_null = PQgetisnull(result, row, col);

            /* Ensure buffer space */
            size_t needed = strlen(name) + (value ? strlen(value) : 4) + 20;
            if (pos + needed >= buf_size) {
                buf_size *= 2;
                char *new_json = realloc(json, buf_size);
                if (!new_json) {
                    free(json);
                    PQclear(result);
                    return NULL;
                }
                json = new_json;
            }

            pos += sprintf(json + pos, "\"%s\":", name);
            if (is_null) {
                pos += sprintf(json + pos, "null");
            } else {
                /* Simple string escaping */
                json[pos++] = '"';
                for (const char *p = value; *p; p++) {
                    if (*p == '"' || *p == '\\') {
                        json[pos++] = '\\';
                    }
                    json[pos++] = *p;
                }
                json[pos++] = '"';
            }
        }

        json[pos++] = '}';
    }

    json[pos++] = ']';
    json[pos] = '\0';

    PQclear(result);
    return json;
}

scribe_error_t scribe_pg_execute(scribe_pg_conn_t *conn, const char *sql)
{
    if (!conn || !conn->pg_conn || !sql) return SCRIBE_ERR_INVALID_ARG;

    PGresult *result = PQexec(conn->pg_conn, sql);
    if (!result) {
        set_error(conn, PQerrorMessage(conn->pg_conn));
        return SCRIBE_ERR_PG_QUERY;
    }

    ExecStatusType status = PQresultStatus(result);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        set_error(conn, PQresultErrorMessage(result));
        PQclear(result);
        return SCRIBE_ERR_PG_QUERY;
    }

    PQclear(result);
    set_error(conn, NULL);
    return SCRIBE_OK;
}

const char *scribe_pg_error(const scribe_pg_conn_t *conn)
{
    return conn ? conn->last_error : NULL;
}

int scribe_pg_socket(const scribe_pg_conn_t *conn)
{
    if (!conn || !conn->pg_conn) return -1;
    return PQsocket(conn->pg_conn);
}

scribe_error_t scribe_pg_consume_input(scribe_pg_conn_t *conn)
{
    if (!conn || !conn->pg_conn) return SCRIBE_ERR_INVALID_ARG;

    if (!PQconsumeInput(conn->pg_conn)) {
        set_error(conn, PQerrorMessage(conn->pg_conn));
        return SCRIBE_ERR_PG_QUERY;
    }

    return SCRIBE_OK;
}

int scribe_pg_is_busy(const scribe_pg_conn_t *conn)
{
    if (!conn || !conn->pg_conn) return 0;
    return PQisBusy(conn->pg_conn);
}
