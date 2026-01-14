/*
 * Scribe - A protocol for Verifiable Data Lineage
 * commands/cmd_status.c - Show repository status
 */

#include <stdio.h>
#include <stdlib.h>
#include <argp.h>

#include "scribe/storage/repository.h"
#include "scribe/storage/database.h"
#include "scribe/core/hash.h"
#include "scribe/error.h"

static char doc[] = "Show the working repository status";
static char args_doc[] = "";

static struct argp_option options[] = {
    {"porcelain", 'p', 0, 0, "Machine-readable output", 0},
    {0}
};

struct status_args {
    int porcelain;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    struct status_args *args = state->input;
    (void)arg;

    switch (key) {
        case 'p':
            args->porcelain = 1;
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = {options, parse_opt, args_doc, doc, 0, 0, 0};

int cmd_status(int argc, char **argv)
{
    struct status_args args = {0};

    argp_parse(&argp, argc, argv, 0, 0, &args);

    /* Open repository */
    scribe_repo_t *repo = scribe_repo_open(NULL);
    if (!repo) {
        fprintf(stderr, "error: not a scribe repository (or any parent)\n");
        return 1;
    }

    /* Get HEAD */
    scribe_hash_t head;
    scribe_error_t err = scribe_repo_get_head(repo, &head);

    if (args.porcelain) {
        /* Machine-readable output */
        if (err == SCRIBE_OK && !scribe_hash_is_zero(&head)) {
            char hex[SCRIBE_HASH_HEX_SIZE];
            scribe_hash_to_hex(&head, hex);
            printf("head %s\n", hex);
        } else {
            printf("head (none)\n");
        }
    } else {
        /* Human-readable output */
        printf("On repository: %s\n", scribe_repo_get_root(repo));

        if (err == SCRIBE_OK && !scribe_hash_is_zero(&head)) {
            char hex[SCRIBE_HASH_HEX_SIZE];
            scribe_hash_to_hex(&head, hex);
            printf("\nHEAD: %.12s...\n", hex);

            /* Load and show latest commit info */
            scribe_envelope_t *env = scribe_repo_load_commit(repo, &head);
            if (env) {
                printf("\nLatest commit:\n");
                printf("  Author:  %s", env->author.id ? env->author.id : "(unknown)");
                if (env->author.role) printf(" (%s)", env->author.role);
                printf("\n");
                printf("  Process: %s", env->process.name ? env->process.name : "(unknown)");
                if (env->process.version) printf(" %s", env->process.version);
                printf("\n");
                if (env->message) {
                    printf("  Message: %s\n", env->message);
                }
                printf("  Changes: %zu\n", env->change_count);
                scribe_envelope_free(env);
            }
        } else {
            printf("\nNo commits yet\n");
        }

        /* Load and show config */
        scribe_config_t *config = scribe_config_load(repo);
        if (config) {
            printf("\nConfiguration:\n");
            printf("  Default author: %s", config->author_id ? config->author_id : "(not set)");
            if (config->author_role) printf(" (%s)", config->author_role);
            printf("\n");

            if (config->pg_connection_string) {
                printf("  PostgreSQL: connected\n");
                if (config->watched_table_count > 0) {
                    printf("  Watched tables: ");
                    for (size_t i = 0; i < config->watched_table_count; i++) {
                        if (i > 0) printf(", ");
                        printf("%s", config->watched_tables[i]);
                    }
                    printf("\n");
                }
            }

            scribe_config_free(config);
        }
    }

    scribe_repo_close(repo);
    return 0;
}
