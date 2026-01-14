/*
 * Scribe - A protocol for Verifiable Data Lineage
 * main.c - CLI entry point
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <argp.h>

#include "scribe/scribe.h"

/* Version information */
const char *argp_program_version = "scribe " SCRIBE_VERSION_STRING;
const char *argp_program_bug_address = "https://github.com/scribe/scribe/issues";

/* Program documentation */
static char doc[] =
    "Scribe - A protocol for Verifiable Data Lineage\n\n"
    "Scribe brings Git-like version control to your data pipelines. "
    "It tracks who changed a record, what process they used, and where that data came from.";

static char args_doc[] = "COMMAND [OPTIONS]";

/* Global options */
static struct argp_option global_options[] = {
    {"verbose", 'v', 0, 0, "Produce verbose output", 0},
    {"quiet", 'q', 0, 0, "Suppress non-error output", 0},
    {"path", 'C', "PATH", 0, "Run as if scribe was started in PATH", 0},
    {0}
};

/* Command declarations */
extern int cmd_init(int argc, char **argv);
extern int cmd_commit(int argc, char **argv);
extern int cmd_log(int argc, char **argv);
extern int cmd_status(int argc, char **argv);
extern int cmd_verify(int argc, char **argv);
#ifdef SCRIBE_WITH_POSTGRES
extern int cmd_watch(int argc, char **argv);
#endif

/* Command structure */
typedef struct {
    const char *name;
    int (*handler)(int argc, char **argv);
    const char *description;
} command_t;

static command_t commands[] = {
    {"init",    cmd_init,    "Create an empty Scribe repository"},
    {"commit",  cmd_commit,  "Record changes to the repository"},
    {"log",     cmd_log,     "Show commit logs"},
    {"status",  cmd_status,  "Show the repository status"},
    {"verify",  cmd_verify,  "Verify repository integrity"},
#ifdef SCRIBE_WITH_POSTGRES
    {"watch",   cmd_watch,   "Monitor PostgreSQL for changes"},
#endif
    {NULL, NULL, NULL}
};

/* Global arguments */
struct arguments {
    int verbose;
    int quiet;
    char *path;
    char *command;
    int command_argc;
    char **command_argv;
};

static struct arguments global_args = {0};

/* Find command by name */
static command_t *find_command(const char *name)
{
    for (command_t *cmd = commands; cmd->name; cmd++) {
        if (strcmp(cmd->name, name) == 0) {
            return cmd;
        }
    }
    return NULL;
}

/* Print available commands */
static void print_commands(void)
{
    printf("\nAvailable commands:\n");
    for (command_t *cmd = commands; cmd->name; cmd++) {
        printf("  %-12s %s\n", cmd->name, cmd->description);
    }
    printf("\nRun 'scribe COMMAND --help' for more information on a command.\n");
}

/* Parse global options */
static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    struct arguments *args = state->input;

    switch (key) {
        case 'v':
            args->verbose = 1;
            break;
        case 'q':
            args->quiet = 1;
            break;
        case 'C':
            args->path = arg;
            break;
        case ARGP_KEY_ARG:
            /* First non-option argument is the command */
            if (!args->command) {
                args->command = arg;
                /* Remaining args go to the command */
                args->command_argc = state->argc - state->next + 1;
                args->command_argv = &state->argv[state->next - 1];
                state->next = state->argc;  /* Stop parsing */
            }
            break;
        case ARGP_KEY_END:
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = {global_options, parse_opt, args_doc, doc, 0, 0, 0};

int main(int argc, char **argv)
{
    /* Parse global options up to the command */
    argp_parse(&argp, argc, argv, ARGP_IN_ORDER, 0, &global_args);

    /* If no command, show help */
    if (!global_args.command) {
        printf("Usage: scribe [OPTIONS] COMMAND [COMMAND_OPTIONS]\n");
        printf("%s\n", doc);
        print_commands();
        return 0;
    }

    /* Change directory if -C specified */
    if (global_args.path) {
        if (chdir(global_args.path) != 0) {
            fprintf(stderr, "error: cannot change to directory '%s'\n", global_args.path);
            return 1;
        }
    }

    /* Find and execute command */
    command_t *cmd = find_command(global_args.command);
    if (!cmd) {
        fprintf(stderr, "error: '%s' is not a scribe command. See 'scribe --help'.\n",
                global_args.command);
        return 1;
    }

    return cmd->handler(global_args.command_argc, global_args.command_argv);
}
