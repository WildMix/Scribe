/*
 * Scribe - A protocol for Verifiable Data Lineage
 * commands/cmd_commit.c - Create a commit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <argp.h>
#include <time.h>

#include "scribe/storage/repository.h"
#include "scribe/core/envelope.h"
#include "scribe/core/hash.h"
#include "scribe/error.h"

static char doc[] = "Record changes to the repository";
static char args_doc[] = "";

static struct argp_option options[] = {
    {"message", 'm', "MSG", 0, "Commit message", 0},
    {"author", 'a', "ID", 0, "Author ID (overrides config)", 0},
    {"role", 'r', "ROLE", 0, "Author role (overrides config)", 0},
    {"process", 'p', "NAME", 0, "Process name", 0},
    {"version", 'V', "VERSION", 0, "Process version", 0},
    {"table", 't', "TABLE", 0, "Table name for change", 0},
    {"operation", 'o', "OP", 0, "Operation (INSERT/UPDATE/DELETE)", 0},
    {"data", 'd', "JSON", 0, "Change data (JSON)", 0},
    {0}
};

struct commit_args {
    char *message;
    char *author;
    char *role;
    char *process;
    char *version;
    char *table;
    char *operation;
    char *data;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    struct commit_args *args = state->input;

    switch (key) {
        case 'm': args->message = arg; break;
        case 'a': args->author = arg; break;
        case 'r': args->role = arg; break;
        case 'p': args->process = arg; break;
        case 'V': args->version = arg; break;
        case 't': args->table = arg; break;
        case 'o': args->operation = arg; break;
        case 'd': args->data = arg; break;
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = {options, parse_opt, args_doc, doc, 0, 0, 0};

int cmd_commit(int argc, char **argv)
{
    struct commit_args args = {0};

    argp_parse(&argp, argc, argv, 0, 0, &args);

    /* Open repository */
    scribe_repo_t *repo = scribe_repo_open(NULL);
    if (!repo) {
        fprintf(stderr, "error: not a scribe repository (or any parent)\n");
        return 1;
    }

    /* Load config for defaults */
    scribe_config_t *config = scribe_config_load(repo);

    /* Create envelope */
    scribe_envelope_t *env = scribe_envelope_new();
    if (!env) {
        fprintf(stderr, "error: out of memory\n");
        scribe_config_free(config);
        scribe_repo_close(repo);
        return 1;
    }

    /* Set author (from args or config) */
    const char *author_id = args.author;
    const char *author_role = args.role;

    if (!author_id && config) author_id = config->author_id;
    if (!author_role && config) author_role = config->author_role;

    if (!author_id) author_id = "user:anonymous";
    if (!author_role) author_role = "unknown";

    scribe_envelope_set_author(env, author_id, author_role);

    /* Set process */
    const char *process_name = args.process ? args.process : "manual";
    scribe_envelope_set_process(env, process_name, args.version, NULL);

    /* Set message */
    if (args.message) {
        scribe_envelope_set_message(env, args.message);
    }

    /* Get parent (current HEAD) */
    scribe_hash_t parent;
    scribe_error_t err = scribe_repo_get_head(repo, &parent);
    if (err == SCRIBE_OK && !scribe_hash_is_zero(&parent)) {
        scribe_envelope_set_parent(env, &parent);
    }

    /* Add change if specified */
    if (args.table && args.operation) {
        scribe_hash_t data_hash = {0};
        if (args.data) {
            scribe_hash_bytes(args.data, strlen(args.data), &data_hash);
        }

        const char *op = args.operation;
        scribe_hash_t *before = NULL;
        scribe_hash_t *after = NULL;

        if (strcmp(op, "INSERT") == 0) {
            after = &data_hash;
        } else if (strcmp(op, "DELETE") == 0) {
            before = &data_hash;
        } else {
            /* UPDATE: use data as after, empty as before */
            after = &data_hash;
        }

        scribe_envelope_add_change(env, args.table, op, args.data ? args.data : "{}", before, after);
    }

    /* Finalize (compute hashes) */
    err = scribe_envelope_finalize(env);
    if (err != SCRIBE_OK) {
        fprintf(stderr, "error: failed to finalize commit: %s\n", scribe_error_string(err));
        scribe_envelope_free(env);
        scribe_config_free(config);
        scribe_repo_close(repo);
        return 1;
    }

    /* Store commit */
    err = scribe_repo_store_commit(repo, env);
    if (err != SCRIBE_OK) {
        fprintf(stderr, "error: failed to store commit: %s\n", scribe_error_string(err));
        scribe_envelope_free(env);
        scribe_config_free(config);
        scribe_repo_close(repo);
        return 1;
    }

    /* Update HEAD */
    err = scribe_repo_set_head(repo, &env->commit_id);
    if (err != SCRIBE_OK) {
        fprintf(stderr, "error: failed to update HEAD: %s\n", scribe_error_string(err));
        scribe_envelope_free(env);
        scribe_config_free(config);
        scribe_repo_close(repo);
        return 1;
    }

    /* Print result */
    char hex[SCRIBE_HASH_HEX_SIZE];
    scribe_hash_to_hex(&env->commit_id, hex);
    printf("[%.12s] %s\n", hex, args.message ? args.message : "(no message)");
    printf(" Author: %s (%s)\n", author_id, author_role);
    printf(" Process: %s\n", process_name);
    if (env->change_count > 0) {
        printf(" %zu change(s) recorded\n", env->change_count);
    }

    scribe_envelope_free(env);
    scribe_config_free(config);
    scribe_repo_close(repo);

    return 0;
}
