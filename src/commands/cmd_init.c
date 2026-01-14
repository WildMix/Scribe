/*
 * Scribe - A protocol for Verifiable Data Lineage
 * commands/cmd_init.c - Initialize repository
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <argp.h>

#include "scribe/storage/repository.h"
#include "scribe/error.h"

static char doc[] = "Create an empty Scribe repository";
static char args_doc[] = "[PATH]";

static struct argp_option options[] = {
    {"author", 'a', "ID", 0, "Set default author ID", 0},
    {"role", 'r', "ROLE", 0, "Set default author role", 0},
    {0}
};

struct init_args {
    char *path;
    char *author;
    char *role;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    struct init_args *args = state->input;

    switch (key) {
        case 'a':
            args->author = arg;
            break;
        case 'r':
            args->role = arg;
            break;
        case ARGP_KEY_ARG:
            if (!args->path) {
                args->path = arg;
            } else {
                argp_usage(state);
            }
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = {options, parse_opt, args_doc, doc, 0, 0, 0};

int cmd_init(int argc, char **argv)
{
    struct init_args args = {0};

    argp_parse(&argp, argc, argv, 0, 0, &args);

    /* Check if already a repository */
    if (scribe_repo_exists(args.path)) {
        const char *detail = scribe_get_error_detail();
        if (detail) {
            fprintf(stderr, "error: %s\n", detail);
        } else {
            fprintf(stderr, "error: repository already exists\n");
        }
        return 1;
    }

    /* Initialize repository */
    scribe_repo_t *repo = scribe_repo_init(args.path);
    if (!repo) {
        const char *detail = scribe_get_error_detail();
        fprintf(stderr, "error: failed to initialize repository%s%s\n",
                detail ? ": " : "", detail ? detail : "");
        return 1;
    }

    /* Update config if author specified */
    if (args.author || args.role) {
        scribe_config_t *config = scribe_config_load(repo);
        if (config) {
            if (args.author) {
                free(config->author_id);
                config->author_id = strdup(args.author);
            }
            if (args.role) {
                free(config->author_role);
                config->author_role = strdup(args.role);
            }
            scribe_config_save(repo, config);
            scribe_config_free(config);
        }
    }

    const char *path = scribe_repo_get_root(repo);
    printf("Initialized empty Scribe repository in %s\n", path);

    scribe_repo_close(repo);
    return 0;
}
