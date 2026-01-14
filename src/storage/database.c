/*
 * Scribe - A protocol for Verifiable Data Lineage
 * storage/database.c - SQLite database operations
 */

#include "scribe/storage/database.h"
#include "scribe/core/hash.h"
#include "scribe/error.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Embedded schema SQL */
static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS objects ("
    "    hash TEXT PRIMARY KEY,"
    "    type TEXT NOT NULL CHECK(type IN ('blob', 'tree', 'commit')),"
    "    content BLOB NOT NULL,"
    "    size INTEGER NOT NULL,"
    "    created_at TEXT DEFAULT (datetime('now'))"
    ");"
    "CREATE TABLE IF NOT EXISTS commits ("
    "    hash TEXT PRIMARY KEY,"
    "    parent_hash TEXT,"
    "    tree_hash TEXT NOT NULL,"
    "    author_id TEXT NOT NULL,"
    "    author_role TEXT,"
    "    author_email TEXT,"
    "    process_name TEXT NOT NULL,"
    "    process_version TEXT,"
    "    process_params TEXT,"
    "    process_source TEXT,"
    "    message TEXT,"
    "    timestamp INTEGER NOT NULL,"
    "    created_at TEXT DEFAULT (datetime('now'))"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_commits_parent ON commits(parent_hash);"
    "CREATE INDEX IF NOT EXISTS idx_commits_author ON commits(author_id);"
    "CREATE INDEX IF NOT EXISTS idx_commits_process ON commits(process_name);"
    "CREATE INDEX IF NOT EXISTS idx_commits_timestamp ON commits(timestamp);"
    "CREATE TABLE IF NOT EXISTS changes ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    commit_hash TEXT NOT NULL,"
    "    table_name TEXT NOT NULL,"
    "    operation TEXT NOT NULL CHECK(operation IN ('INSERT', 'UPDATE', 'DELETE')),"
    "    primary_key TEXT NOT NULL,"
    "    before_hash TEXT,"
    "    after_hash TEXT,"
    "    created_at TEXT DEFAULT (datetime('now')),"
    "    FOREIGN KEY (commit_hash) REFERENCES commits(hash)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_changes_commit ON changes(commit_hash);"
    "CREATE INDEX IF NOT EXISTS idx_changes_table ON changes(table_name);"
    "CREATE TABLE IF NOT EXISTS refs ("
    "    name TEXT PRIMARY KEY,"
    "    hash TEXT NOT NULL,"
    "    updated_at TEXT DEFAULT (datetime('now'))"
    ");"
    "INSERT OR IGNORE INTO refs (name, hash) VALUES ('HEAD', '');"
    "CREATE TABLE IF NOT EXISTS config ("
    "    key TEXT PRIMARY KEY,"
    "    value TEXT NOT NULL"
    ");"
    "INSERT OR IGNORE INTO config (key, value) VALUES ('schema_version', '1');";

struct scribe_db {
    sqlite3 *handle;
    char *path;
};

scribe_db_t *scribe_db_open(const char *path)
{
    if (!path) return NULL;

    scribe_db_t *db = calloc(1, sizeof(scribe_db_t));
    if (!db) return NULL;

    db->path = strdup(path);
    if (!db->path) {
        free(db);
        return NULL;
    }

    int rc = sqlite3_open(path, &db->handle);
    if (rc != SQLITE_OK) {
        scribe_set_error_detail("SQLite error: %s", sqlite3_errmsg(db->handle));
        sqlite3_close(db->handle);
        free(db->path);
        free(db);
        return NULL;
    }

    /* Enable foreign keys */
    sqlite3_exec(db->handle, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);

    return db;
}

void scribe_db_close(scribe_db_t *db)
{
    if (!db) return;
    if (db->handle) sqlite3_close(db->handle);
    free(db->path);
    free(db);
}

scribe_error_t scribe_db_init_schema(scribe_db_t *db)
{
    if (!db || !db->handle) return SCRIBE_ERR_INVALID_ARG;

    char *err_msg = NULL;
    int rc = sqlite3_exec(db->handle, SCHEMA_SQL, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        scribe_set_error_detail("Schema init failed: %s", err_msg);
        sqlite3_free(err_msg);
        return SCRIBE_ERR_DB;
    }

    return SCRIBE_OK;
}

scribe_error_t scribe_db_begin(scribe_db_t *db)
{
    if (!db || !db->handle) return SCRIBE_ERR_INVALID_ARG;
    int rc = sqlite3_exec(db->handle, "BEGIN TRANSACTION;", NULL, NULL, NULL);
    return (rc == SQLITE_OK) ? SCRIBE_OK : SCRIBE_ERR_DB;
}

scribe_error_t scribe_db_commit(scribe_db_t *db)
{
    if (!db || !db->handle) return SCRIBE_ERR_INVALID_ARG;
    int rc = sqlite3_exec(db->handle, "COMMIT;", NULL, NULL, NULL);
    return (rc == SQLITE_OK) ? SCRIBE_OK : SCRIBE_ERR_DB;
}

scribe_error_t scribe_db_rollback(scribe_db_t *db)
{
    if (!db || !db->handle) return SCRIBE_ERR_INVALID_ARG;
    int rc = sqlite3_exec(db->handle, "ROLLBACK;", NULL, NULL, NULL);
    return (rc == SQLITE_OK) ? SCRIBE_OK : SCRIBE_ERR_DB;
}

scribe_error_t scribe_db_store_commit(scribe_db_t *db, const scribe_envelope_t *env)
{
    if (!db || !db->handle || !env) return SCRIBE_ERR_INVALID_ARG;

    const char *sql =
        "INSERT INTO commits (hash, parent_hash, tree_hash, author_id, author_role, "
        "author_email, process_name, process_version, process_params, process_source, "
        "message, timestamp) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        scribe_set_error_detail("Prepare failed: %s", sqlite3_errmsg(db->handle));
        return SCRIBE_ERR_DB;
    }

    char commit_hex[SCRIBE_HASH_HEX_SIZE];
    char parent_hex[SCRIBE_HASH_HEX_SIZE];
    char tree_hex[SCRIBE_HASH_HEX_SIZE];

    scribe_hash_to_hex(&env->commit_id, commit_hex);
    scribe_hash_to_hex(&env->parent_id, parent_hex);
    scribe_hash_to_hex(&env->tree_hash, tree_hex);

    sqlite3_bind_text(stmt, 1, commit_hex, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, scribe_hash_is_zero(&env->parent_id) ? NULL : parent_hex, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, tree_hex, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, env->author.id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, env->author.role, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, env->author.email, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, env->process.name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, env->process.version, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, env->process.params, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, env->process.source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, env->message, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 12, (int64_t)env->timestamp);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        scribe_set_error_detail("Insert failed: %s", sqlite3_errmsg(db->handle));
        return SCRIBE_ERR_DB;
    }

    /* Store changes */
    const char *change_sql =
        "INSERT INTO changes (commit_hash, table_name, operation, primary_key, "
        "before_hash, after_hash) VALUES (?, ?, ?, ?, ?, ?);";

    for (size_t i = 0; i < env->change_count; i++) {
        rc = sqlite3_prepare_v2(db->handle, change_sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) continue;

        char before_hex[SCRIBE_HASH_HEX_SIZE];
        char after_hex[SCRIBE_HASH_HEX_SIZE];

        scribe_hash_to_hex(&env->changes[i].before_hash, before_hex);
        scribe_hash_to_hex(&env->changes[i].after_hash, after_hex);

        sqlite3_bind_text(stmt, 1, commit_hex, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, env->changes[i].table_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, env->changes[i].operation, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, env->changes[i].primary_key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, scribe_hash_is_zero(&env->changes[i].before_hash) ? NULL : before_hex, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, scribe_hash_is_zero(&env->changes[i].after_hash) ? NULL : after_hex, -1, SQLITE_TRANSIENT);

        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    return SCRIBE_OK;
}

scribe_envelope_t *scribe_db_load_commit(scribe_db_t *db, const scribe_hash_t *hash)
{
    if (!db || !db->handle || !hash) return NULL;

    const char *sql =
        "SELECT hash, parent_hash, tree_hash, author_id, author_role, author_email, "
        "process_name, process_version, process_params, process_source, message, timestamp "
        "FROM commits WHERE hash = ?;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;

    char hash_hex[SCRIBE_HASH_HEX_SIZE];
    scribe_hash_to_hex(hash, hash_hex);
    sqlite3_bind_text(stmt, 1, hash_hex, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    scribe_envelope_t *env = scribe_envelope_new();
    if (!env) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    /* Parse commit_id */
    const char *col_hash = (const char *)sqlite3_column_text(stmt, 0);
    if (col_hash) scribe_hash_from_hex(col_hash, &env->commit_id);

    /* Parse parent_id */
    const char *col_parent = (const char *)sqlite3_column_text(stmt, 1);
    if (col_parent) scribe_hash_from_hex(col_parent, &env->parent_id);

    /* Parse tree_hash */
    const char *col_tree = (const char *)sqlite3_column_text(stmt, 2);
    if (col_tree) scribe_hash_from_hex(col_tree, &env->tree_hash);

    /* Author */
    const char *col_author_id = (const char *)sqlite3_column_text(stmt, 3);
    const char *col_author_role = (const char *)sqlite3_column_text(stmt, 4);
    scribe_envelope_set_author(env, col_author_id, col_author_role);

    const char *col_author_email = (const char *)sqlite3_column_text(stmt, 5);
    if (col_author_email) scribe_envelope_set_author_email(env, col_author_email);

    /* Process */
    const char *col_proc_name = (const char *)sqlite3_column_text(stmt, 6);
    const char *col_proc_ver = (const char *)sqlite3_column_text(stmt, 7);
    const char *col_proc_params = (const char *)sqlite3_column_text(stmt, 8);
    scribe_envelope_set_process(env, col_proc_name, col_proc_ver, col_proc_params);

    const char *col_proc_source = (const char *)sqlite3_column_text(stmt, 9);
    if (col_proc_source) scribe_envelope_set_process_source(env, col_proc_source);

    /* Message and timestamp */
    const char *col_message = (const char *)sqlite3_column_text(stmt, 10);
    if (col_message) scribe_envelope_set_message(env, col_message);

    env->timestamp = (time_t)sqlite3_column_int64(stmt, 11);

    sqlite3_finalize(stmt);

    /* Load changes */
    const char *change_sql =
        "SELECT table_name, operation, primary_key, before_hash, after_hash "
        "FROM changes WHERE commit_hash = ?;";

    rc = sqlite3_prepare_v2(db->handle, change_sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, hash_hex, -1, SQLITE_TRANSIENT);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *table = (const char *)sqlite3_column_text(stmt, 0);
            const char *op = (const char *)sqlite3_column_text(stmt, 1);
            const char *pk = (const char *)sqlite3_column_text(stmt, 2);
            const char *before = (const char *)sqlite3_column_text(stmt, 3);
            const char *after = (const char *)sqlite3_column_text(stmt, 4);

            scribe_hash_t before_hash = {0}, after_hash = {0};
            if (before) scribe_hash_from_hex(before, &before_hash);
            if (after) scribe_hash_from_hex(after, &after_hash);

            scribe_envelope_add_change(env, table, op, pk, &before_hash, &after_hash);
        }
        sqlite3_finalize(stmt);
    }

    return env;
}

int scribe_db_commit_exists(scribe_db_t *db, const scribe_hash_t *hash)
{
    if (!db || !db->handle || !hash) return 0;

    const char *sql = "SELECT 1 FROM commits WHERE hash = ? LIMIT 1;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;

    char hash_hex[SCRIBE_HASH_HEX_SIZE];
    scribe_hash_to_hex(hash, hash_hex);
    sqlite3_bind_text(stmt, 1, hash_hex, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    int exists = (rc == SQLITE_ROW);
    sqlite3_finalize(stmt);

    return exists;
}

scribe_db_commit_list_t *scribe_db_get_history(scribe_db_t *db,
                                                const scribe_hash_t *from,
                                                size_t limit)
{
    if (!db || !db->handle) return NULL;
    if (limit == 0) limit = 100;

    scribe_db_commit_list_t *list = calloc(1, sizeof(scribe_db_commit_list_t));
    if (!list) return NULL;

    list->hashes = calloc(limit, sizeof(scribe_hash_t));
    if (!list->hashes) {
        free(list);
        return NULL;
    }

    scribe_hash_t current;
    if (from && !scribe_hash_is_zero(from)) {
        scribe_hash_copy(&current, from);
    } else {
        /* Start from HEAD */
        scribe_error_t err = scribe_db_get_ref(db, "HEAD", &current);
        if (err != SCRIBE_OK || scribe_hash_is_zero(&current)) {
            free(list->hashes);
            free(list);
            return NULL;
        }
    }

    /* Walk the parent chain */
    while (list->count < limit && !scribe_hash_is_zero(&current)) {
        scribe_hash_copy(&list->hashes[list->count], &current);
        list->count++;

        /* Get parent */
        const char *sql = "SELECT parent_hash FROM commits WHERE hash = ?;";
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) break;

        char hash_hex[SCRIBE_HASH_HEX_SIZE];
        scribe_hash_to_hex(&current, hash_hex);
        sqlite3_bind_text(stmt, 1, hash_hex, -1, SQLITE_TRANSIENT);

        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            const char *parent = (const char *)sqlite3_column_text(stmt, 0);
            if (parent) {
                scribe_hash_from_hex(parent, &current);
            } else {
                memset(&current, 0, sizeof(current));
            }
        } else {
            memset(&current, 0, sizeof(current));
        }
        sqlite3_finalize(stmt);
    }

    return list;
}

void scribe_db_commit_list_free(scribe_db_commit_list_t *list)
{
    if (!list) return;
    free(list->hashes);
    free(list);
}

scribe_error_t scribe_db_get_ref(scribe_db_t *db, const char *name, scribe_hash_t *out_hash)
{
    if (!db || !db->handle || !name || !out_hash) return SCRIBE_ERR_INVALID_ARG;

    const char *sql = "SELECT hash FROM refs WHERE name = ?;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return SCRIBE_ERR_DB;

    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return SCRIBE_ERR_NOT_FOUND;
    }

    const char *hash_str = (const char *)sqlite3_column_text(stmt, 0);
    if (hash_str && strlen(hash_str) == SCRIBE_HASH_HEX_SIZE - 1) {
        scribe_hash_from_hex(hash_str, out_hash);
    } else {
        memset(out_hash, 0, sizeof(scribe_hash_t));
    }

    sqlite3_finalize(stmt);
    return SCRIBE_OK;
}

scribe_error_t scribe_db_set_ref(scribe_db_t *db, const char *name, const scribe_hash_t *hash)
{
    if (!db || !db->handle || !name || !hash) return SCRIBE_ERR_INVALID_ARG;

    const char *sql = "INSERT OR REPLACE INTO refs (name, hash, updated_at) VALUES (?, ?, datetime('now'));";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return SCRIBE_ERR_DB;

    char hash_hex[SCRIBE_HASH_HEX_SIZE];
    scribe_hash_to_hex(hash, hash_hex);

    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, hash_hex, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? SCRIBE_OK : SCRIBE_ERR_DB;
}

size_t scribe_db_commit_count(scribe_db_t *db)
{
    if (!db || !db->handle) return 0;

    const char *sql = "SELECT COUNT(*) FROM commits;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;

    rc = sqlite3_step(stmt);
    size_t count = 0;
    if (rc == SQLITE_ROW) {
        count = (size_t)sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);

    return count;
}

scribe_db_commit_list_t *scribe_db_find_by_author(scribe_db_t *db, const char *author_id)
{
    if (!db || !db->handle || !author_id) return NULL;

    const char *sql = "SELECT hash FROM commits WHERE author_id = ? ORDER BY timestamp DESC;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;

    sqlite3_bind_text(stmt, 1, author_id, -1, SQLITE_TRANSIENT);

    scribe_db_commit_list_t *list = calloc(1, sizeof(scribe_db_commit_list_t));
    if (!list) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    size_t capacity = 64;
    list->hashes = calloc(capacity, sizeof(scribe_hash_t));
    if (!list->hashes) {
        free(list);
        sqlite3_finalize(stmt);
        return NULL;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (list->count >= capacity) {
            capacity *= 2;
            scribe_hash_t *new_hashes = realloc(list->hashes, capacity * sizeof(scribe_hash_t));
            if (!new_hashes) break;
            list->hashes = new_hashes;
        }

        const char *hash_str = (const char *)sqlite3_column_text(stmt, 0);
        if (hash_str) {
            scribe_hash_from_hex(hash_str, &list->hashes[list->count]);
            list->count++;
        }
    }

    sqlite3_finalize(stmt);
    return list;
}

scribe_db_commit_list_t *scribe_db_find_by_process(scribe_db_t *db, const char *process_name)
{
    if (!db || !db->handle || !process_name) return NULL;

    const char *sql = "SELECT hash FROM commits WHERE process_name = ? ORDER BY timestamp DESC;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;

    sqlite3_bind_text(stmt, 1, process_name, -1, SQLITE_TRANSIENT);

    scribe_db_commit_list_t *list = calloc(1, sizeof(scribe_db_commit_list_t));
    if (!list) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    size_t capacity = 64;
    list->hashes = calloc(capacity, sizeof(scribe_hash_t));
    if (!list->hashes) {
        free(list);
        sqlite3_finalize(stmt);
        return NULL;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (list->count >= capacity) {
            capacity *= 2;
            scribe_hash_t *new_hashes = realloc(list->hashes, capacity * sizeof(scribe_hash_t));
            if (!new_hashes) break;
            list->hashes = new_hashes;
        }

        const char *hash_str = (const char *)sqlite3_column_text(stmt, 0);
        if (hash_str) {
            scribe_hash_from_hex(hash_str, &list->hashes[list->count]);
            list->count++;
        }
    }

    sqlite3_finalize(stmt);
    return list;
}
