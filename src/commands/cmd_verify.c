/*
 * Scribe - A protocol for Verifiable Data Lineage
 * commands/cmd_verify.c - Verify repository integrity
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <argp.h>

#include "scribe/storage/repository.h"
#include "scribe/core/envelope.h"
#include "scribe/core/hash.h"
#include "scribe/error.h"

static char doc[] = "Verify repository integrity";
static char args_doc[] = "[COMMIT]";

static struct argp_option options[] = {
    {"verbose", 'v', 0, 0, "Show detailed verification output", 0},
    {"full", 'f', 0, 0, "Verify full history (not just reachable from HEAD)", 0},
    {0}
};

struct verify_args {
    int verbose;
    int full;
    char *commit;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    struct verify_args *args = state->input;

    switch (key) {
        case 'v':
            args->verbose = 1;
            break;
        case 'f':
            args->full = 1;
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

static int verify_commit(scribe_repo_t *repo, const scribe_hash_t *hash, int verbose)
{
    char hex[SCRIBE_HASH_HEX_SIZE];
    scribe_hash_to_hex(hash, hex);

    if (verbose) {
        printf("Verifying commit %.12s... ", hex);
        fflush(stdout);
    }

    /* Load commit */
    scribe_envelope_t *env = scribe_repo_load_commit(repo, hash);
    if (!env) {
        if (verbose) printf("\033[31mFAILED\033[0m (not found)\n");
        return 0;
    }

    /* Verify envelope integrity */
    scribe_error_t err = scribe_envelope_verify(env);
    if (err != SCRIBE_OK) {
        if (verbose) printf("\033[31mFAILED\033[0m (hash mismatch)\n");
        scribe_envelope_free(env);
        return 0;
    }

    /* Verify parent exists (if not root) */
    if (!scribe_hash_is_zero(&env->parent_id)) {
        scribe_envelope_t *parent = scribe_repo_load_commit(repo, &env->parent_id);
        if (!parent) {
            if (verbose) printf("\033[31mFAILED\033[0m (missing parent)\n");
            scribe_envelope_free(env);
            return 0;
        }
        scribe_envelope_free(parent);
    }

    if (verbose) printf("\033[32mOK\033[0m\n");

    scribe_envelope_free(env);
    return 1;
}

int cmd_verify(int argc, char **argv)
{
    struct verify_args args = {0};

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
    } else {
        /* Start from HEAD */
        scribe_repo_get_head(repo, &start);
    }

    if (scribe_hash_is_zero(&start)) {
        printf("Repository is empty (no commits to verify)\n");
        scribe_repo_close(repo);
        return 0;
    }

    printf("Verifying repository integrity...\n");
    if (args.verbose) printf("\n");

    /* Get history and verify each commit */
    scribe_commit_list_t *history = scribe_repo_get_history(repo, &start, 1000);
    if (!history) {
        fprintf(stderr, "error: failed to get commit history\n");
        scribe_repo_close(repo);
        return 1;
    }

    int verified = 0;
    int failed = 0;

    for (size_t i = 0; i < history->count; i++) {
        if (verify_commit(repo, &history->hashes[i], args.verbose)) {
            verified++;
        } else {
            failed++;
        }
    }

    scribe_commit_list_free(history);

    printf("\n");
    if (failed == 0) {
        printf("\033[32mVerification successful!\033[0m\n");
        printf("  %d commit(s) verified\n", verified);
        printf("  All parent links valid\n");
        printf("  All commit hashes match\n");
    } else {
        printf("\033[31mVerification failed!\033[0m\n");
        printf("  %d commit(s) verified\n", verified);
        printf("  %d commit(s) failed\n", failed);
    }

    scribe_repo_close(repo);
    return (failed == 0) ? 0 : 1;
}
