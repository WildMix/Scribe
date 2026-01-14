/*
 * Scribe - A protocol for Verifiable Data Lineage
 * commands/cmd_log.c - Show commit history
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

static char doc[] = "Show commit logs";
static char args_doc[] = "[COMMIT]";

static struct argp_option options[] = {
    {"oneline", '1', 0, 0, "Show each commit on one line", 0},
    {"limit", 'n', "NUM", 0, "Limit number of commits shown", 0},
    {"author", 'a', "ID", 0, "Filter by author ID", 0},
    {"process", 'p', "NAME", 0, "Filter by process name", 0},
    {"json", 'j', 0, 0, "Output as JSON", 0},
    {0}
};

struct log_args {
    int oneline;
    int limit;
    char *author;
    char *process;
    char *commit;
    int json;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    struct log_args *args = state->input;

    switch (key) {
        case '1':
            args->oneline = 1;
            break;
        case 'n':
            args->limit = atoi(arg);
            break;
        case 'a':
            args->author = arg;
            break;
        case 'p':
            args->process = arg;
            break;
        case 'j':
            args->json = 1;
            break;
        case ARGP_KEY_ARG:
            if (!args->commit) {
                args->commit = arg;
            }
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = {options, parse_opt, args_doc, doc, 0, 0, 0};

static void print_commit_oneline(const scribe_envelope_t *env)
{
    char hex[SCRIBE_HASH_HEX_SIZE];
    scribe_hash_to_hex(&env->commit_id, hex);

    printf("%.12s ", hex);

    if (env->author.id) {
        printf("(%s) ", env->author.id);
    }

    if (env->message) {
        printf("%s", env->message);
    } else {
        printf("(no message)");
    }

    printf("\n");
}

static void print_commit_full(const scribe_envelope_t *env)
{
    char hex[SCRIBE_HASH_HEX_SIZE];
    scribe_hash_to_hex(&env->commit_id, hex);

    printf("\033[33mcommit %s\033[0m\n", hex);

    if (!scribe_hash_is_zero(&env->parent_id)) {
        char parent_hex[SCRIBE_HASH_HEX_SIZE];
        scribe_hash_to_hex(&env->parent_id, parent_hex);
        printf("Parent: %s\n", parent_hex);
    }

    printf("Author: %s", env->author.id ? env->author.id : "(unknown)");
    if (env->author.role) printf(" <%s>", env->author.role);
    if (env->author.email) printf(" (%s)", env->author.email);
    printf("\n");

    printf("Process: %s", env->process.name ? env->process.name : "(unknown)");
    if (env->process.version) printf(" %s", env->process.version);
    if (env->process.params) printf(" %s", env->process.params);
    printf("\n");

    /* Format timestamp */
    char time_buf[64];
    struct tm *tm_info = localtime(&env->timestamp);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    printf("Date:   %s\n", time_buf);

    if (env->message) {
        printf("\n    %s\n", env->message);
    }

    if (env->change_count > 0) {
        printf("\n    Changes (%zu):\n", env->change_count);
        for (size_t i = 0; i < env->change_count && i < 5; i++) {
            printf("      - %s %s",
                   env->changes[i].operation ? env->changes[i].operation : "?",
                   env->changes[i].table_name ? env->changes[i].table_name : "?");
            if (env->changes[i].primary_key) {
                printf(" %s", env->changes[i].primary_key);
            }
            printf("\n");
        }
        if (env->change_count > 5) {
            printf("      ... and %zu more\n", env->change_count - 5);
        }
    }

    printf("\n");
}

static void print_commit_json(const scribe_envelope_t *env)
{
    char *json = scribe_envelope_to_json(env);
    if (json) {
        printf("%s\n", json);
        free(json);
    }
}

int cmd_log(int argc, char **argv)
{
    struct log_args args = {0};
    args.limit = 10;  /* Default limit */

    argp_parse(&argp, argc, argv, 0, 0, &args);

    /* Open repository */
    scribe_repo_t *repo = scribe_repo_open(NULL);
    if (!repo) {
        fprintf(stderr, "error: not a scribe repository (or any parent)\n");
        return 1;
    }

    /* Get starting commit */
    scribe_hash_t start = {0};
    if (args.commit) {
        scribe_error_t err = scribe_hash_from_hex(args.commit, &start);
        if (err != SCRIBE_OK) {
            fprintf(stderr, "error: invalid commit hash '%s'\n", args.commit);
            scribe_repo_close(repo);
            return 1;
        }
    }

    /* Get history */
    scribe_commit_list_t *history = scribe_repo_get_history(repo,
                                                            scribe_hash_is_zero(&start) ? NULL : &start,
                                                            args.limit > 0 ? (size_t)args.limit : 100);

    if (!history || history->count == 0) {
        if (!args.json) {
            printf("No commits found\n");
        }
        scribe_commit_list_free(history);
        scribe_repo_close(repo);
        return 0;
    }

    /* Print commits */
    if (args.json) printf("[\n");

    int first = 1;
    for (size_t i = 0; i < history->count; i++) {
        scribe_envelope_t *env = scribe_repo_load_commit(repo, &history->hashes[i]);
        if (!env) continue;

        /* Apply filters */
        if (args.author && env->author.id) {
            if (strstr(env->author.id, args.author) == NULL) {
                scribe_envelope_free(env);
                continue;
            }
        }

        if (args.process && env->process.name) {
            if (strstr(env->process.name, args.process) == NULL) {
                scribe_envelope_free(env);
                continue;
            }
        }

        /* Print */
        if (args.json) {
            if (!first) printf(",\n");
            first = 0;
            print_commit_json(env);
        } else if (args.oneline) {
            print_commit_oneline(env);
        } else {
            print_commit_full(env);
        }

        scribe_envelope_free(env);
    }

    if (args.json) printf("]\n");

    scribe_commit_list_free(history);
    scribe_repo_close(repo);

    return 0;
}
