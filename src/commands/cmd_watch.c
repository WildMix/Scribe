/*
 * Scribe - A protocol for Verifiable Data Lineage
 * commands/cmd_watch.c - PostgreSQL monitoring daemon
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <argp.h>

#include "scribe/storage/repository.h"
#include "scribe/core/envelope.h"
#include "scribe/core/hash.h"
#include "scribe/postgres/pg_monitor.h"
#include "scribe/error.h"

static char doc[] = "Monitor PostgreSQL for changes and record them in Scribe";
static char args_doc[] = "";

static struct argp_option options[] = {
    {"connection", 'c', "CONN", 0, "PostgreSQL connection string", 0},
    {"tables", 't', "TABLES", 0, "Comma-separated list of tables to watch", 0},
    {"mode", 'm', "MODE", 0, "CDC mode: trigger or logical (default: logical)", 0},
    {"interval", 'i', "MS", 0, "Poll interval in milliseconds (default: 1000)", 0},
    {"slot", 's', "NAME", 0, "Replication slot name (default: scribe_slot)", 0},
    {"setup", 'S', 0, 0, "Setup CDC infrastructure and exit", 0},
    {"cleanup", 'C', 0, 0, "Cleanup CDC infrastructure and exit", 0},
    {0}
};

struct watch_args {
    char *connection;
    char *tables;
    char *mode;
    int interval;
    char *slot;
    int setup_only;
    int cleanup_only;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    struct watch_args *args = state->input;

    switch (key) {
        case 'c': args->connection = arg; break;
        case 't': args->tables = arg; break;
        case 'm': args->mode = arg; break;
        case 'i': args->interval = atoi(arg); break;
        case 's': args->slot = arg; break;
        case 'S': args->setup_only = 1; break;
        case 'C': args->cleanup_only = 1; break;
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = {options, parse_opt, args_doc, doc, 0, 0, 0};

/* Global for signal handling */
static scribe_pg_monitor_t *g_monitor = NULL;
static scribe_repo_t *g_repo = NULL;

static void signal_handler(int sig)
{
    (void)sig;
    printf("\nStopping monitor...\n");
    if (g_monitor) {
        scribe_pg_monitor_stop(g_monitor);
    }
}

/* Callback for changes */
static void on_change(const scribe_pg_change_t *change, void *user_data)
{
    scribe_repo_t *repo = (scribe_repo_t *)user_data;

    printf("[%s] %s on %s\n",
           change->operation ? change->operation : "?",
           change->table_name ? change->table_name : "?",
           change->primary_key_json ? change->primary_key_json : "?");

    /* Create commit for this change */
    scribe_envelope_t *env = scribe_envelope_new();
    if (!env) return;

    /* Load config for author info */
    scribe_config_t *config = scribe_config_load(repo);

    scribe_envelope_set_author(env,
                               config ? config->author_id : "service:scribe-watch",
                               config ? config->author_role : "automated");

    char process_name[256];
    snprintf(process_name, sizeof(process_name), "pg_txid:%ld", (long)change->transaction_id);
    scribe_envelope_set_process(env, process_name, "postgresql-cdc", NULL);

    char message[512];
    snprintf(message, sizeof(message), "%s on %s",
             change->operation ? change->operation : "change",
             change->table_name ? change->table_name : "unknown");
    scribe_envelope_set_message(env, message);

    /* Get parent */
    scribe_hash_t parent;
    if (scribe_repo_get_head(repo, &parent) == SCRIBE_OK && !scribe_hash_is_zero(&parent)) {
        scribe_envelope_set_parent(env, &parent);
    }

    /* Add change */
    scribe_hash_t before_hash = {0}, after_hash = {0};
    if (change->before_json) {
        scribe_hash_bytes(change->before_json, strlen(change->before_json), &before_hash);
    }
    if (change->after_json) {
        scribe_hash_bytes(change->after_json, strlen(change->after_json), &after_hash);
    }

    scribe_envelope_add_change(env,
                               change->table_name,
                               change->operation,
                               change->primary_key_json,
                               scribe_hash_is_zero(&before_hash) ? NULL : &before_hash,
                               scribe_hash_is_zero(&after_hash) ? NULL : &after_hash);

    /* Finalize and store */
    if (scribe_envelope_finalize(env) == SCRIBE_OK) {
        if (scribe_repo_store_commit(repo, env) == SCRIBE_OK) {
            scribe_repo_set_head(repo, &env->commit_id);

            char hex[SCRIBE_HASH_HEX_SIZE];
            scribe_hash_to_hex(&env->commit_id, hex);
            printf("  -> Committed: %.12s\n", hex);
        }
    }

    scribe_envelope_free(env);
    scribe_config_free(config);
}

/* Parse comma-separated table list */
static char **parse_tables(const char *tables_str, size_t *count)
{
    if (!tables_str || !count) return NULL;

    *count = 0;

    /* Count commas */
    size_t capacity = 1;
    for (const char *p = tables_str; *p; p++) {
        if (*p == ',') capacity++;
    }

    char **tables = calloc(capacity, sizeof(char *));
    if (!tables) return NULL;

    /* Parse */
    char *str = strdup(tables_str);
    if (!str) {
        free(tables);
        return NULL;
    }

    char *token = strtok(str, ",");
    while (token && *count < capacity) {
        /* Trim whitespace */
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') *end-- = '\0';

        if (*token) {
            tables[(*count)++] = strdup(token);
        }
        token = strtok(NULL, ",");
    }

    free(str);
    return tables;
}

int cmd_watch(int argc, char **argv)
{
    struct watch_args args = {0};
    args.interval = 1000;

    argp_parse(&argp, argc, argv, 0, 0, &args);

    /* Open repository */
    scribe_repo_t *repo = scribe_repo_open(NULL);
    if (!repo) {
        fprintf(stderr, "error: not a scribe repository (or any parent)\n");
        return 1;
    }
    g_repo = repo;

    /* Get connection string from args or config */
    const char *connection = args.connection;
    scribe_config_t *config = NULL;

    if (!connection) {
        config = scribe_config_load(repo);
        if (config && config->pg_connection_string) {
            connection = config->pg_connection_string;
        }
    }

    if (!connection) {
        fprintf(stderr, "error: no PostgreSQL connection string specified\n");
        fprintf(stderr, "Use --connection or set pg_connection_string in config\n");
        scribe_config_free(config);
        scribe_repo_close(repo);
        return 1;
    }

    /* Parse tables */
    char **tables = NULL;
    size_t table_count = 0;

    if (args.tables) {
        tables = parse_tables(args.tables, &table_count);
    } else if (config && config->watched_tables) {
        tables = config->watched_tables;
        table_count = config->watched_table_count;
        config->watched_tables = NULL;  /* Don't free these */
    }

    /* Determine mode */
    scribe_pg_mode_t mode = SCRIBE_PG_MODE_LOGICAL;  /* Default: logical */
    if (args.mode) {
        if (strcmp(args.mode, "trigger") == 0) {
            mode = SCRIBE_PG_MODE_TRIGGER;
        } else if (strcmp(args.mode, "logical") == 0) {
            mode = SCRIBE_PG_MODE_LOGICAL;
        } else {
            fprintf(stderr, "error: unknown mode '%s'. Use 'trigger' or 'logical'\n", args.mode);
            scribe_config_free(config);
            scribe_repo_close(repo);
            return 1;
        }
    }

    /* Create monitor config */
    scribe_pg_monitor_config_t mon_config = {
        .connection_string = (char *)connection,
        .mode = mode,
        .tables = tables,
        .table_count = table_count,
        .poll_interval_ms = args.interval,
        .slot_name = args.slot ? args.slot : "scribe_slot",
        .publication_name = "scribe_pub"
    };

    /* Create monitor */
    scribe_pg_monitor_t *monitor = scribe_pg_monitor_new(&mon_config);
    if (!monitor) {
        fprintf(stderr, "error: failed to create monitor\n");
        scribe_config_free(config);
        scribe_repo_close(repo);
        return 1;
    }
    g_monitor = monitor;

    /* Setup or cleanup only? */
    if (args.setup_only) {
        printf("Setting up %s CDC for %zu table(s)...\n",
               mode == SCRIBE_PG_MODE_LOGICAL ? "logical replication" : "trigger-based",
               table_count);

        scribe_error_t err = scribe_pg_monitor_setup(monitor);
        if (err != SCRIBE_OK) {
            const char *detail = scribe_get_error_detail();
            fprintf(stderr, "error: setup failed%s%s\n",
                    detail ? ": " : "", detail ? detail : "");
            scribe_pg_monitor_free(monitor);
            scribe_config_free(config);
            scribe_repo_close(repo);
            return 1;
        }

        printf("Setup complete!\n");
        scribe_pg_monitor_free(monitor);
        scribe_config_free(config);
        scribe_repo_close(repo);
        return 0;
    }

    if (args.cleanup_only) {
        printf("Cleaning up CDC infrastructure...\n");
        scribe_pg_monitor_cleanup(monitor);
        printf("Cleanup complete!\n");
        scribe_pg_monitor_free(monitor);
        scribe_config_free(config);
        scribe_repo_close(repo);
        return 0;
    }

    /* Setup signal handler */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Setup CDC */
    printf("Setting up %s CDC...\n",
           mode == SCRIBE_PG_MODE_LOGICAL ? "logical replication" : "trigger-based");

    scribe_error_t err = scribe_pg_monitor_setup(monitor);
    if (err != SCRIBE_OK) {
        const char *detail = scribe_get_error_detail();
        fprintf(stderr, "error: setup failed%s%s\n",
                detail ? ": " : "", detail ? detail : "");
        scribe_pg_monitor_free(monitor);
        scribe_config_free(config);
        scribe_repo_close(repo);
        return 1;
    }

    /* Start monitoring */
    printf("Monitoring %zu table(s) for changes (Ctrl+C to stop)...\n", table_count);
    for (size_t i = 0; i < table_count; i++) {
        printf("  - %s\n", tables[i]);
    }
    printf("\n");

    err = scribe_pg_monitor_start(monitor, on_change, repo);
    if (err != SCRIBE_OK && scribe_pg_monitor_is_running(monitor)) {
        fprintf(stderr, "error: monitoring failed\n");
    }

    /* Cleanup */
    printf("Shutting down...\n");
    scribe_pg_monitor_free(monitor);
    scribe_config_free(config);
    scribe_repo_close(repo);

    /* Free tables if we allocated them */
    if (args.tables && tables) {
        for (size_t i = 0; i < table_count; i++) {
            free(tables[i]);
        }
        free(tables);
    }

    return 0;
}
