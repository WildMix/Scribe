/*
 * Scribe command-line frontend.
 *
 * This file parses top-level CLI arguments, opens repository contexts with the
 * correct read/write mode, dispatches to core command implementations, and turns
 * Scribe errors into the consistent `scribe: SYMBOL: detail` diagnostic format.
 */
#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"
#include "util/log.h"
#include "linenoise.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SCRIBE_DEFAULT_HOST_PORT "127.0.0.1:9323"
#define SCRIBE_DEFAULT_LISTEN SCRIBE_DEFAULT_HOST_PORT
#define SCRIBE_DEFAULT_URL "http://" SCRIBE_DEFAULT_HOST_PORT

/*
 * Prints the command summary used for invalid arguments and --help. Keep this
 * text synchronized with the parser below: each binary intentionally has one
 * top-level help page rather than per-command help.
 */
static void usage(FILE *out) {
#ifdef SCRIBE_CLIENT_ONLY
    fputs("Scribe CLI - query client for local or remote Scribe repositories\n"
          "\n"
          "Usage:\n"
          "  scribe-cli [query-command] [args]\n"
          "  scribe-cli --store <path> [--remote <name>] [query-command] [args]\n"
          "  scribe-cli --url <url> [--token <token>] [query-command] [args]\n"
          "  scribe-cli -h | --help\n"
          "\n"
          "No command starts the interactive REPL. With no connection option it opens\n"
          "the default local server at http://127.0.0.1:9323. With --store it reads\n"
          "named remote configuration only; with --remote it opens the named server\n"
          "configured in that store; with --url it opens a repository-free remote session.\n"
          "\n"
          "Global options:\n"
          "  --store <path>       Local .scribe repository used only for named\n"
          "                       remote configuration. Defaults to ./.scribe.\n"
          "  --remote <name>      Query the named remote from .scribe/remotes/<name>.\n"
          "                       With no command, starts a REPL against that remote.\n"
          "  --url <url>          Query a Scribe server directly without opening a local\n"
          "                       repository. Supports http:// and https://. Defaults\n"
          "                       to http://127.0.0.1:9323 when no connection option\n"
          "                       is supplied.\n"
          "  --token <token>      Bearer token sent with --url requests.\n"
          "  --log-format=text|json\n"
          "                       Format diagnostic logs written to stderr.\n"
          "  -h, --help           Print this help text.\n"
          "\n"
          "Client commands:\n"
          "  info\n"
          "      Print repository version, storage configuration, advertised query\n"
          "      capabilities, main ref, and last fsck health fields.\n"
          "  refs\n"
          "      List direct refs as '<ref> <hash>' records. Use this to discover\n"
          "      branches, tags, and operational refs without downloading objects.\n"
          "  log [--oneline] [--paths] [--since <time>] [--until <time>] [-n <N>] [--] [<path>]\n"
          "      Walk HEAD history newest-first. Optional path filtering emits only\n"
          "      commits where that path changed. --since/--until accept UTC ISO-8601\n"
          "      or raw Unix nanoseconds and filter by commit registration time.\n"
          "  show <commit>|<commit>:<path>|<path>\n"
          "      Show commit metadata and changed paths, print an exact blob/tree from\n"
          "      a commit, or show history for a path. Accepts HEAD and HEAD~N.\n"
          "  diff <commit1> [<commit2>]\n"
          "      Compare two commit root trees, or one commit against its parent. Output\n"
          "      is A/M/D plus the changed Scribe path.\n"
          "  cat-object (-p|-t|-s) <hash-or-revision>\n"
          "      Read one object and print its payload, type, or uncompressed payload\n"
          "      size. Commit revisions such as HEAD and HEAD~N resolve to commits.\n"
          "  ls-tree <hash-or-revision>\n"
          "      Recursively list a tree. Commit revisions resolve to their root tree.\n"
          "  ingest [--json] [<json-file>]\n"
          "      POST a JSON/base64 ingest batch to a Scribe server. Reads stdin when\n"
          "      no file, or when <json-file> is '-'. Prints the server JSON response.\n"
          "  ingest --path <path> [--message <message>] --file <file>\n"
          "      PUT the file's exact bytes at a Scribe path through /scribe/v1/content.\n"
          "      Use --file - to read the content from stdin. The message is optional.\n"
          "  ingest --path <path> [--message <message>] --delete\n"
          "      DELETE the Scribe path through /scribe/v1/content, creating a\n"
          "      tombstone. Empty files remain valid zero-byte blobs in --file mode.\n"
          "  ingest --pipe [<frame-file>]\n"
          "      Read pipe protocol v2 BATCH frames from stdin or a file, POST them\n"
          "      to /scribe/v1/ingest-stream, and print OK/ERR pipe responses.\n"
          "      Legacy pipe protocol v1 frames are also accepted.\n"
          "  remote add <name> <url> [--token <token>]\n"
          "      Save a named Scribe server endpoint under .scribe/remotes/<name>.\n"
          "      Supports http:// and https:// URLs.\n"
          "  remote list\n"
          "      List configured named remotes for the selected local store.\n"
          "  remote remove <name>\n"
          "      Delete one named remote configuration.\n"
          "  remote test <name>\n"
          "      Query the remote /scribe/v1/info endpoint and print the returned info.\n"
          "\n"
          "Use scribe with no command for the local server and for operator commands\n"
          "such as replicate, fsck, gc, bundle, and repository setup. Use\n"
          "scribe-cli ingest --pipe to send pipe protocol frames to the server.\n",
          out);
    return;
#endif
    fputs("Scribe - server/operator binary\n"
          "\n"
          "Usage:\n"
          "  scribe [--store <path>] [--listen <host:port>] [--cert <cert> --key <key>]\n"
          "  scribe [--store <path>] <operator-command> [args]\n"
          "  scribe -h | --help\n"
          "\n"
          "With no command, scribe starts the local repository server on\n"
          "127.0.0.1:9323. scribe also owns repository setup, maintenance,\n"
          "offline bundles, and server-side replication. Use scribe-cli for all\n"
          "history queries and adapter ingestion.\n"
          "\n"
          "Global options:\n"
          "  --store <path>          Local .scribe repository. Defaults to ./.scribe.\n"
          "  --listen <host:port>    Server listen address. Defaults to 127.0.0.1:9323.\n"
          "  --cert <cert> --key <key>\n"
          "                          Serve TLS with HTTP/2 instead of plain HTTP/1.1.\n"
          "  --log-format=text|json  Format operational logs. Text is the default.\n"
          "  -h, --help              Print this help text.\n"
          "\n"
          "Operator commands:\n"
          "  init [path]\n"
          "      Create a new repository directory with config, refs, object storage,\n"
          "      pack storage, logs, and adapter state directories.\n"
          "  branch <name> [<commit>]\n"
          "      Create or move refs/heads/<name> to a commit. Without <commit>, uses\n"
          "      HEAD. Branch moves are local operator markers and write reflog entries.\n"
          "  tag [--force] <name> <commit>\n"
          "      Create refs/tags/<name>. Existing tags are rejected unless --force is\n"
          "      supplied, in which case the reflog records the replacement.\n"
          "  tag -d <name> [--force]\n"
          "      Delete refs/tags/<name>. --force ignores a missing tag.\n"
          "  reflog <ref>\n"
          "      Print local ref movement history from .scribe/logs/<ref>. Accepts HEAD,\n"
          "      full refs, branch shorthand, and tag shorthand.\n"
          "  list-objects [options]\n"
          "      Inspect object storage entries. Supports --reachable, --type=blob|tree|commit,\n"
          "      and --format=<template> for operator inventory/debugging.\n"
          "  status\n"
          "      Print repository health and inventory: fsck status, main ref, commit\n"
          "      count, latest commit time, refs, object counts, loose objects, and packs.\n"
          "  fsck\n"
          "      Verify packs, refs, reachable commits/trees/blobs, object envelopes,\n"
          "      hashes, and dangling/duplicate storage entries. Records fsck health.\n"
          "  gc [--dry-run] [--prune-now] [--pack] [--consolidate]\n"
          "      Prune dangling loose objects, roll reachable loose objects into packs,\n"
          "      or consolidate packed records. Uses the repository writer lock.\n"
          "  bundle create <file>\n"
          "      Write a self-checking offline bundle containing refs, loose objects,\n"
          "      packs, indexes, and a manifest checksum.\n"
          "  bundle import <file> <target-store>\n"
          "      Verify and import a bundle into a target repository, initializing that\n"
          "      target store if needed.\n"
          "  replicate <remote> [--interval-ms <N>] [--max-cycles <N>]\n"
          "      Server-side mirror/archive operation. Pulls missing storage entries and\n"
          "      refs from a configured remote and advances refs fast-forward-only.\n"
          "  daemon --config <file>\n"
          "      Run the local supervisor described by a tab-delimited config file and\n"
          "      expose daemon status at GET /scribe/v1/status.\n"
          "\n"
          "Remote configuration and history queries live in scribe-cli. Use\n"
          "  scribe-cli --help\n",
          out);
    return;
}

/*
 * Prints the most recent Scribe error detail and returns the numeric error code
 * as a process exit status.
 */
static int fail(scribe_error_t err) {
    fprintf(stderr, "scribe: %s: %s\n", scribe_error_symbol(err), scribe_last_error_detail());
    return (int)err;
}

/*
 * Thin wrapper around scribe_open(). Keeping command dispatch through one helper
 * makes it easy to adjust context-opening policy later.
 */
static scribe_error_t open_ctx(const char *store, int writable, scribe_ctx **ctx) {
    return scribe_open(store, writable, ctx);
}

static int run_server(const char *store, const char *listen_addr, const char *cert, const char *key) {
    scribe_ctx *ctx = NULL;
    scribe_error_t err = open_ctx(store, 0, &ctx);

    if (err != SCRIBE_OK) {
        return fail(err);
    }
    err = scribe_log_use_file(ctx, "scribe_server.log");
    if (err == SCRIBE_OK) {
        err = scribe_cli_serve(ctx, listen_addr == NULL ? SCRIBE_DEFAULT_LISTEN : listen_addr, cert, key);
    }
    scribe_close(ctx);
    return err == SCRIBE_OK ? 0 : fail(err);
}

static int is_remote_url(const char *value) {
    return value != NULL && (strncmp(value, "http://", 7u) == 0 || strncmp(value, "https://", 8u) == 0);
}

static int executable_is_client(const char *argv0) {
    const char *base;

    if (argv0 == NULL) {
        return 0;
    }
    base = strrchr(argv0, '/');
    if (base == NULL) {
        base = strrchr(argv0, '\\');
    }
    base = base == NULL ? argv0 : base + 1;
    return strcmp(base, "scribe-cli") == 0 || strcmp(base, "scribe-cli.exe") == 0;
}

static int is_query_command(const char *cmd) {
    return strcmp(cmd, "info") == 0 || strcmp(cmd, "refs") == 0 || strcmp(cmd, "log") == 0 ||
           strcmp(cmd, "show") == 0 || strcmp(cmd, "diff") == 0 || strcmp(cmd, "cat-object") == 0 ||
           strcmp(cmd, "ls-tree") == 0;
}

static int is_remote_repository_command(const char *cmd) { return is_query_command(cmd) || strcmp(cmd, "ingest") == 0; }

static int is_client_command(const char *cmd) {
    return is_remote_repository_command(cmd) || strcmp(cmd, "remote") == 0;
}

static scribe_error_t parse_ulong_option(const char *option, const char *value, unsigned long max_value,
                                         unsigned long *out) {
    char *end = NULL;
    unsigned long parsed;

    if (option == NULL || value == NULL || value[0] == '\0' || value[0] == '-' || value[0] == '+') {
        return scribe_set_error(SCRIBE_EINVAL, "invalid %s value '%s'", option == NULL ? "numeric option" : option,
                                value == NULL ? "" : value);
    }
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || errno == ERANGE || parsed > max_value) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid %s value '%s'", option, value);
    }
    *out = parsed;
    return SCRIBE_OK;
}

static scribe_error_t parse_size_option(const char *option, const char *value, size_t *out) {
    unsigned long parsed;
    scribe_error_t err = parse_ulong_option(option, value, ULONG_MAX, &parsed);

    if (err != SCRIBE_OK) {
        return err;
    }
    if (parsed > (unsigned long)SIZE_MAX) {
        return scribe_set_error(SCRIBE_EINVAL, "%s value is too large", option);
    }
    *out = (size_t)parsed;
    return SCRIBE_OK;
}

static scribe_error_t read_stream_all(FILE *in, const char *label, uint8_t **out, size_t *out_len) {
    uint8_t *data = NULL;
    size_t len = 0;
    size_t cap = 0;
    uint8_t tmp[8192];

    if (in == NULL || out == NULL || out_len == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid input reader arguments");
    }
    *out = NULL;
    *out_len = 0;
    for (;;) {
        size_t n = fread(tmp, 1, sizeof(tmp), in);

        if (n != 0u) {
            if (len > SIZE_MAX - n) {
                free(data);
                return scribe_set_error(SCRIBE_ENOMEM, "%s is too large", label == NULL ? "input" : label);
            }
            if (len + n > cap) {
                size_t needed = len + n;
                size_t next = cap == 0u ? 8192u : cap;
                uint8_t *grown;

                while (next < needed) {
                    if (next > SIZE_MAX / 2u) {
                        next = needed;
                        break;
                    }
                    next *= 2u;
                }
                grown = (uint8_t *)realloc(data, next);
                if (grown == NULL) {
                    free(data);
                    return scribe_set_error(SCRIBE_ENOMEM, "failed to read %s", label == NULL ? "input" : label);
                }
                data = grown;
                cap = next;
            }
            memcpy(data + len, tmp, n);
            len += n;
        }
        if (n < sizeof(tmp)) {
            if (ferror(in)) {
                free(data);
                return scribe_set_error(SCRIBE_EIO, "failed to read %s", label == NULL ? "input" : label);
            }
            break;
        }
    }
    if (data == NULL) {
        data = (uint8_t *)malloc(1u);
        if (data == NULL) {
            return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate input buffer");
        }
    }
    *out = data;
    *out_len = len;
    return SCRIBE_OK;
}

static scribe_error_t read_ingest_body(const char *path, uint8_t **out, size_t *out_len) {
    FILE *in = stdin;
    scribe_error_t err;

    if (path != NULL && strcmp(path, "-") != 0) {
        in = fopen(path, "rb");
        if (in == NULL) {
            return scribe_set_error(SCRIBE_EIO, "failed to open ingest file '%s'", path);
        }
    }
    err = read_stream_all(in, path == NULL ? "stdin" : path, out, out_len);
    if (in != stdin) {
        fclose(in);
    }
    return err;
}

static scribe_error_t parse_log_time_option(const char *option, const char *value, scribe_time_range *time_range) {
    int64_t nanos;
    scribe_error_t err;

    if (value == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "missing value for %s", option);
    }
    err = scribe_parse_time_arg(value, &nanos);
    if (err != SCRIBE_OK) {
        return err;
    }
    if (strcmp(option, "--since") == 0) {
        time_range->has_since = 1;
        time_range->since_nanos = nanos;
    } else {
        time_range->has_until = 1;
        time_range->until_nanos = nanos;
    }
    if (time_range->has_since && time_range->has_until && time_range->since_nanos > time_range->until_nanos) {
        return scribe_set_error(SCRIBE_EINVAL, "--since must be less than or equal to --until");
    }
    return SCRIBE_OK;
}

static scribe_error_t open_query_repository(const char *store, const char *remote, const char *url, const char *token,
                                            int client_only, scribe_ctx **ctx, scribe_repository *repo) {
    const char *direct_url = url;
    const char *named_remote = remote;
    scribe_error_t err;

    *ctx = NULL;
    if (named_remote != NULL && is_remote_url(named_remote)) {
        return scribe_set_error(SCRIBE_EINVAL, "--remote expects a named remote; use --url for direct server URLs");
    }
    if (direct_url != NULL && named_remote != NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "--url and --remote <name> cannot be combined");
    }
    if (direct_url != NULL) {
        return scribe_repository_open_url(repo, direct_url, token);
    }
    if (client_only && named_remote == NULL) {
        return scribe_repository_open_url(repo, SCRIBE_DEFAULT_URL, NULL);
    }
    err = open_ctx(store, 0, ctx);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_repository_open(repo, *ctx, named_remote);
    if (err != SCRIBE_OK) {
        scribe_close(*ctx);
        *ctx = NULL;
    }
    return err;
}

static void close_query_repository(scribe_ctx *ctx, scribe_repository *repo) {
    scribe_repository_close(repo);
    scribe_close(ctx);
}

static char *client_history_path(void) {
    const char *home = getenv("HOME");
    char *path;
    size_t len;

    if (home == NULL || home[0] == '\0') {
        return strdup(".scribe-cli-history");
    }
    len = strlen(home) + strlen("/.scribe-cli-history") + 1u;
    path = (char *)malloc(len);
    if (path == NULL) {
        return NULL;
    }
    snprintf(path, len, "%s/.scribe-cli-history", home);
    return path;
}

static char *repl_local_location(scribe_ctx *ctx) {
    char resolved[PATH_MAX];

    if (ctx == NULL || ctx->repo_path == NULL) {
        return strdup("unknown");
    }
    if (realpath(ctx->repo_path, resolved) != NULL) {
        return strdup(resolved);
    }
    return strdup(ctx->repo_path);
}

static scribe_error_t repl_location(scribe_ctx *ctx, const scribe_repository *repo, char **out) {
    if (out == NULL || repo == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid repl location arguments");
    }
    *out = NULL;
    if (repo->url != NULL) {
        *out = strdup(repo->url);
    } else if (repo->remote != NULL) {
        return scribe_cli_remote_get_url(ctx, repo->remote, out);
    } else {
        *out = repl_local_location(ctx);
    }
    return *out == NULL ? scribe_set_error(SCRIBE_ENOMEM, "failed to allocate repl location") : SCRIBE_OK;
}

static char *repl_prompt(const char *location) {
    size_t len;
    char *prompt;

    if (location == NULL || location[0] == '\0') {
        return strdup("scribe-cli> ");
    }
    len = strlen("scribe-cli []> ") + strlen(location) + 1u;
    prompt = (char *)malloc(len);
    if (prompt == NULL) {
        return NULL;
    }
    snprintf(prompt, len, "scribe-cli [%s]> ", location);
    return prompt;
}

/*
 * Implements `scribe info`. It prints binary-level facts even when the store is
 * missing, then prints repository settings only if the store opens successfully.
 */
static int cmd_info(const char *store) {
    scribe_ctx *ctx = NULL;
    scribe_error_t err = scribe_open(store, 0, &ctx);

    printf("scribe version %s\n", SCRIBE_VERSION);
    printf("hash_algorithm blake3-256\n");
    printf("pipe_protocol 2\n");
    if (err == SCRIBE_OK) {
        printf("store %s\n", ctx->repo_path);
        printf("compression zstd level %d\n", ctx->config.compression_level);
        printf("worker_threads %d\n", ctx->config.worker_threads);
        scribe_close(ctx);
        return 0;
    }
    if (err == SCRIBE_ENOT_FOUND) {
        printf("store %s (not initialized)\n", store);
        return 0;
    }
    return fail(err);
}

static void repl_help(void) {
    fputs("commands:\n"
          "  info\n"
          "  refs\n"
          "  log [--oneline] [--paths] [--since <time>] [--until <time>] [-n <N>] [--] [<path>]\n"
          "  show [<commit>|<commit>:<path>|<path>]\n"
          "  diff [<commit1>] [<commit2>]\n"
          "  cat-object (-p|-t|-s) <hash-or-revision>\n"
          "  ls-tree <hash-or-revision>\n"
          "  use <commit>\n"
          "  more\n"
          "  help\n"
          "  quit\n",
          stdout);
}

static scribe_error_t repl_split(char *line, char **argv, int max_args, int *out_argc) {
    int argc = 0;
    char *src = line;

    while (*src != '\0') {
        char *dst;
        char quote = '\0';

        while (isspace((unsigned char)*src)) {
            src++;
        }
        if (*src == '\0') {
            break;
        }
        if (argc >= max_args) {
            return scribe_set_error(SCRIBE_EINVAL, "too many repl arguments");
        }
        argv[argc++] = src;
        dst = src;
        while (*src != '\0' && (quote != '\0' || !isspace((unsigned char)*src))) {
            if (quote != '\0') {
                if (*src == quote) {
                    quote = '\0';
                    src++;
                } else if (quote == '"' && *src == '\\' && src[1] != '\0') {
                    src++;
                    *dst++ = *src++;
                } else {
                    *dst++ = *src++;
                }
            } else if (*src == '\'' || *src == '"') {
                quote = *src++;
            } else if (*src == '\\' && src[1] != '\0') {
                src++;
                *dst++ = *src++;
            } else {
                *dst++ = *src++;
            }
        }
        if (quote != '\0') {
            return scribe_set_error(SCRIBE_EINVAL, "unterminated quote in repl command");
        }
        if (*src != '\0') {
            src++;
        }
        *dst = '\0';
    }
    *out_argc = argc;
    return SCRIBE_OK;
}

static void repl_completion(const char *buf, linenoiseCompletions *lc) {
    static const char *commands[] = {"info",    "refs", "log",  "show", "diff", "cat-object",
                                     "ls-tree", "use",  "more", "help", "quit"};
    size_t i;
    size_t len = strlen(buf);

    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strncmp(commands[i], buf, len) == 0) {
            linenoiseAddCompletion(lc, commands[i]);
        }
    }
}

static scribe_error_t repl_run_log(scribe_repository *repo, int argc, char **argv) {
    int oneline = 0;
    int show_paths = 0;
    size_t limit = 0;
    scribe_time_range time_range;
    const char *path_filter = NULL;
    int after_separator = 0;
    int i = 1;

    memset(&time_range, 0, sizeof(time_range));
    while (i < argc) {
        if (!after_separator && strcmp(argv[i], "--") == 0) {
            after_separator = 1;
            i++;
        } else if (!after_separator && strcmp(argv[i], "--oneline") == 0) {
            oneline = 1;
            i++;
        } else if (!after_separator && strcmp(argv[i], "--paths") == 0) {
            show_paths = 1;
            i++;
        } else if (!after_separator && (strcmp(argv[i], "--since") == 0 || strcmp(argv[i], "--until") == 0)) {
            scribe_error_t err;

            if (i + 1 >= argc) {
                return scribe_set_error(SCRIBE_EINVAL, "missing value for %s", argv[i]);
            }
            err = parse_log_time_option(argv[i], argv[i + 1], &time_range);
            if (err != SCRIBE_OK) {
                return err;
            }
            i += 2;
        } else if (!after_separator && strcmp(argv[i], "-n") == 0) {
            scribe_error_t err;

            if (i + 1 >= argc) {
                return scribe_set_error(SCRIBE_EINVAL, "missing value for -n");
            }
            err = parse_size_option("-n", argv[i + 1], &limit);
            if (err != SCRIBE_OK) {
                return err;
            }
            i += 2;
        } else if (!after_separator && argv[i][0] == '-') {
            return scribe_set_error(SCRIBE_EINVAL, "unknown repl log option '%s'", argv[i]);
        } else {
            if (path_filter != NULL) {
                return scribe_set_error(SCRIBE_EINVAL, "log accepts one optional path");
            }
            path_filter = argv[i++];
        }
    }
    return scribe_repository_log(repo, oneline, limit, show_paths, path_filter, &time_range);
}

static scribe_error_t repl_run_command(scribe_repository *repo, int argc, char **argv, char *current_commit,
                                       size_t current_commit_size) {
    if (argc == 0) {
        return SCRIBE_OK;
    }
    if (strcmp(argv[0], "help") == 0) {
        repl_help();
        return SCRIBE_OK;
    }
    if (strcmp(argv[0], "more") == 0) {
        fputs("more: no paginated query is pending\n", stdout);
        return SCRIBE_OK;
    }
    if (strcmp(argv[0], "use") == 0) {
        if (argc != 2) {
            return scribe_set_error(SCRIBE_EINVAL, "usage: use <commit>");
        }
        snprintf(current_commit, current_commit_size, "%s", argv[1]);
        printf("current %s\n", current_commit);
        return SCRIBE_OK;
    }
    if (strcmp(argv[0], "info") == 0) {
        if (argc != 1) {
            return scribe_set_error(SCRIBE_EINVAL, "usage: info");
        }
        return scribe_repository_info(repo);
    }
    if (strcmp(argv[0], "refs") == 0) {
        if (argc != 1) {
            return scribe_set_error(SCRIBE_EINVAL, "usage: refs");
        }
        return scribe_repository_refs(repo);
    }
    if (strcmp(argv[0], "log") == 0) {
        return repl_run_log(repo, argc, argv);
    }
    if (strcmp(argv[0], "show") == 0) {
        const char *spec = argc == 1 ? current_commit : argv[1];

        if (argc > 2) {
            return scribe_set_error(SCRIBE_EINVAL, "usage: show [spec]");
        }
        return scribe_repository_show(repo, spec);
    }
    if (strcmp(argv[0], "diff") == 0) {
        const char *a = argc == 1 ? current_commit : argv[1];
        const char *b = argc > 2 ? argv[2] : NULL;

        if (argc > 3) {
            return scribe_set_error(SCRIBE_EINVAL, "usage: diff [commit1] [commit2]");
        }
        return scribe_repository_diff(repo, a, b);
    }
    if (strcmp(argv[0], "cat-object") == 0) {
        char mode;

        if (argc != 3 || argv[1][0] != '-' || strlen(argv[1]) != 2u) {
            return scribe_set_error(SCRIBE_EINVAL, "usage: cat-object (-p|-t|-s) <hash-or-revision>");
        }
        mode = argv[1][1];
        return scribe_repository_cat_object(repo, mode, argv[2]);
    }
    if (strcmp(argv[0], "ls-tree") == 0) {
        if (argc != 2) {
            return scribe_set_error(SCRIBE_EINVAL, "usage: ls-tree <hash-or-revision>");
        }
        return scribe_repository_ls_tree(repo, argv[1]);
    }
    return scribe_set_error(SCRIBE_EINVAL, "unknown repl command '%s'", argv[0]);
}

static int cmd_repl(const char *store, const char *remote, const char *url, const char *token, int client_only) {
    scribe_ctx *ctx = NULL;
    scribe_repository repo;
    char current_commit[256] = "HEAD";
    char *history_path = NULL;
    char *location = NULL;
    char *prompt = NULL;
    int interactive = isatty(fileno(stdin));
    scribe_error_t err = open_query_repository(store, remote, url, token, client_only, &ctx, &repo);

    if (err != SCRIBE_OK) {
        return fail(err);
    }
    err = scribe_repository_check(&repo);
    if (err != SCRIBE_OK) {
        close_query_repository(ctx, &repo);
        return fail(err);
    }
    err = repl_location(ctx, &repo, &location);
    if (err == SCRIBE_OK) {
        prompt = repl_prompt(location);
        if (prompt == NULL) {
            err = scribe_set_error(SCRIBE_ENOMEM, "failed to allocate repl prompt");
        }
    }
    if (err != SCRIBE_OK) {
        free(location);
        close_query_repository(ctx, &repo);
        return fail(err);
    }
    linenoiseSetCompletionCallback(repl_completion);
    (void)linenoiseHistorySetMaxLen(256);
    history_path = ctx != NULL ? scribe_path_join(ctx->repo_path, "repl-history") : client_history_path();
    if (history_path != NULL) {
        (void)linenoiseHistoryLoad(history_path);
    }
    for (;;) {
        char *line = linenoise(interactive ? prompt : "");
        char *argv[32];
        int argc = 0;

        if (line == NULL) {
            break;
        }
        if (line[0] != '\0') {
            (void)linenoiseHistoryAdd(line);
        }
        err = repl_split(line, argv, 32, &argc);
        if (err != SCRIBE_OK) {
            fprintf(stderr, "scribe: %s: %s\n", scribe_error_symbol(err), scribe_last_error_detail());
            linenoiseFree(line);
            continue;
        }

        if (argc != 0 && (strcmp(argv[0], "quit") == 0 || strcmp(argv[0], "exit") == 0)) {
            linenoiseFree(line);
            break;
        }
        err = repl_run_command(&repo, argc, argv, current_commit, sizeof(current_commit));
        if (err != SCRIBE_OK) {
            fprintf(stderr, "scribe: %s: %s\n", scribe_error_symbol(err), scribe_last_error_detail());
        }
        linenoiseFree(line);
    }
    free(prompt);
    free(location);
    if (history_path != NULL) {
        (void)linenoiseHistorySave(history_path);
    }
    free(history_path);
    close_query_repository(ctx, &repo);
    return 0;
}

/*
 * Parses global options, dispatches subcommands, and owns all CLI context
 * lifetimes. Each command opens the repository in the narrowest mode it needs:
 * writable for commit-producing commands and read-only for inspection commands.
 */
int main(int argc, char **argv) {
    const char *store = ".scribe";
    const char *global_remote = NULL;
    const char *remote_url = NULL;
    const char *remote_token = NULL;
    const char *listen_addr = NULL;
    const char *cert = NULL;
    const char *key = NULL;
    const char *cmd;
    int argi = 1;
    int client_only = executable_is_client(argv[0]);
    scribe_ctx *ctx = NULL;
    scribe_error_t err;

#ifdef SCRIBE_CLIENT_ONLY
    client_only = 1;
#endif

    while (argi < argc) {
        if (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[argi], "--store") == 0) {
            if (argi + 1 >= argc) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            store = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "--url") == 0) {
            if (!client_only) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            if (argi + 1 >= argc) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            remote_url = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "--remote") == 0) {
            if (!client_only) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            if (argi + 1 >= argc) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            global_remote = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "--token") == 0) {
            if (!client_only) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            if (argi + 1 >= argc) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            remote_token = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "--log-format") == 0) {
            if (argi + 1 >= argc) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            err = scribe_log_set_format(argv[argi + 1]);
            if (err != SCRIBE_OK) {
                return fail(err);
            }
            argi += 2;
        } else if (strncmp(argv[argi], "--log-format=", 13u) == 0) {
            err = scribe_log_set_format(argv[argi] + 13u);
            if (err != SCRIBE_OK) {
                return fail(err);
            }
            argi++;
        } else if (!client_only && strcmp(argv[argi], "--listen") == 0) {
            if (argi + 1 >= argc) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            listen_addr = argv[argi + 1];
            argi += 2;
        } else if (!client_only && strcmp(argv[argi], "--cert") == 0) {
            if (argi + 1 >= argc) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            cert = argv[argi + 1];
            argi += 2;
        } else if (!client_only && strcmp(argv[argi], "--key") == 0) {
            if (argi + 1 >= argc) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            key = argv[argi + 1];
            argi += 2;
        } else {
            break;
        }
    }
    if (client_only && remote_token != NULL && remote_url == NULL) {
        return fail(scribe_set_error(SCRIBE_EINVAL, "--token requires --url"));
    }
    if (argi >= argc) {
        if (client_only) {
            if (remote_url == NULL && global_remote == NULL) {
                remote_url = SCRIBE_DEFAULT_URL;
            }
            return cmd_repl(store, global_remote, remote_url, remote_token, client_only);
        }
        return run_server(store, listen_addr, cert, key);
    }
    cmd = argv[argi++];

    if (!client_only && (listen_addr != NULL || cert != NULL || key != NULL)) {
        return fail(scribe_set_error(SCRIBE_EINVAL, "--listen, --cert, and --key apply only when starting the server"));
    }

    if (strcmp(cmd, "repl") == 0) {
        return fail(scribe_set_error(
            SCRIBE_EINVAL, "the repl subcommand was removed; run 'scribe-cli [--store <path>] [--remote <name>]' or "
                           "'scribe-cli --url <url>' with no command"));
    }
    if (strcmp(cmd, "commit-batch") == 0) {
        return fail(scribe_set_error(SCRIBE_EINVAL, "commit-batch moved to 'scribe-cli ingest --pipe'"));
    }
    if (client_only && !is_client_command(cmd)) {
        return fail(
            scribe_set_error(SCRIBE_EINVAL, "scribe-cli supports client query commands and remote config only"));
    }
    if (!client_only && is_remote_repository_command(cmd)) {
        return fail(scribe_set_error(SCRIBE_EINVAL, "client remote commands moved to scribe-cli"));
    }
    if (!client_only && strcmp(cmd, "remote") == 0) {
        return fail(scribe_set_error(SCRIBE_EINVAL, "remote configuration moved to scribe-cli"));
    }
    if (remote_url != NULL && !is_remote_repository_command(cmd)) {
        return fail(scribe_set_error(SCRIBE_EINVAL, "--url applies only to scribe-cli remote commands"));
    }
    if (global_remote != NULL && strcmp(cmd, "remote") == 0) {
        return fail(scribe_set_error(SCRIBE_EINVAL, "--remote cannot be combined with remote configuration commands"));
    }
    if (client_only && remote_url == NULL && global_remote == NULL && is_remote_repository_command(cmd)) {
        remote_url = SCRIBE_DEFAULT_URL;
    }

    if (strcmp(cmd, "init") == 0) {
        const char *init_path = store;
        if (argi + 1 < argc) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        if (argi < argc) {
            init_path = argv[argi];
        }
        err = scribe_init_repository(init_path);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        printf("initialized %s\n", init_path);
        return 0;
    }
    if (strcmp(cmd, "info") == 0) {
        const char *remote = global_remote;

        if (argi != argc) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        if (remote != NULL || remote_url != NULL || client_only) {
            scribe_repository repo;

            err = open_query_repository(store, remote, remote_url, remote_token, client_only, &ctx, &repo);
            if (err == SCRIBE_OK) {
                err = scribe_repository_info(&repo);
            }
            close_query_repository(ctx, &repo);
            return err == SCRIBE_OK ? 0 : fail(err);
        }
        return cmd_info(store);
    }
    if (strcmp(cmd, "refs") == 0) {
        const char *remote = global_remote;

        if (argi != argc) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        {
            scribe_repository repo;

            err = open_query_repository(store, remote, remote_url, remote_token, client_only, &ctx, &repo);
            if (err == SCRIBE_OK) {
                err = scribe_repository_refs(&repo);
            }
            close_query_repository(ctx, &repo);
        }
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "ingest") == 0) {
        const char *remote = global_remote;
        const char *batch_file = NULL;
        const char *content_path = NULL;
        const char *content_file = NULL;
        const char *message = NULL;
        int json_mode = 0;
        int pipe_mode = 0;
        int delete_mode = 0;
        uint8_t *body = NULL;
        size_t body_len = 0;
        scribe_repository repo = {0};

        while (argi < argc) {
            if (strcmp(argv[argi], "--json") == 0) {
                json_mode = 1;
                argi++;
            } else if (strcmp(argv[argi], "--pipe") == 0) {
                pipe_mode = 1;
                argi++;
            } else if (strcmp(argv[argi], "--path") == 0) {
                if (content_path != NULL || argi + 1 >= argc) {
                    return fail(scribe_set_error(SCRIBE_EINVAL, "ingest --path requires exactly one value"));
                }
                content_path = argv[argi + 1];
                argi += 2;
            } else if (strcmp(argv[argi], "--message") == 0) {
                if (message != NULL || argi + 1 >= argc) {
                    return fail(scribe_set_error(SCRIBE_EINVAL, "ingest --message requires exactly one value"));
                }
                message = argv[argi + 1];
                argi += 2;
            } else if (strcmp(argv[argi], "--file") == 0) {
                if (content_file != NULL || argi + 1 >= argc) {
                    return fail(scribe_set_error(SCRIBE_EINVAL, "ingest --file requires exactly one value"));
                }
                content_file = argv[argi + 1];
                argi += 2;
            } else if (strcmp(argv[argi], "--delete") == 0) {
                if (delete_mode) {
                    return fail(scribe_set_error(SCRIBE_EINVAL, "ingest --delete may be specified only once"));
                }
                delete_mode = 1;
                argi++;
            } else if (argv[argi][0] == '-' && strcmp(argv[argi], "-") != 0) {
                return fail(scribe_set_error(SCRIBE_EINVAL, "unknown ingest option '%s'", argv[argi]));
            } else if (batch_file == NULL) {
                batch_file = argv[argi++];
            } else {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
        }
        if (json_mode && pipe_mode) {
            return fail(scribe_set_error(SCRIBE_EINVAL, "ingest accepts only one of --json or --pipe"));
        }
        if (content_path != NULL || content_file != NULL || message != NULL || delete_mode) {
            scribe_ingest_op op = delete_mode ? SCRIBE_INGEST_OP_DELETE : SCRIBE_INGEST_OP_PUT;

            if (json_mode || pipe_mode) {
                return fail(scribe_set_error(SCRIBE_EINVAL, "content ingest cannot be combined with --json or --pipe"));
            }
            if (content_path == NULL) {
                return fail(scribe_set_error(SCRIBE_EINVAL, "content ingest requires --path <path>"));
            }
            if (batch_file != NULL) {
                return fail(scribe_set_error(SCRIBE_EINVAL, "content ingest file must be provided with --file <file>"));
            }
            if (delete_mode && content_file != NULL) {
                return fail(scribe_set_error(SCRIBE_EINVAL, "content ingest --delete cannot be combined with --file"));
            }
            if (!delete_mode && content_file == NULL) {
                return fail(scribe_set_error(SCRIBE_EINVAL, "content ingest requires --file <file>"));
            }
            if (!delete_mode) {
                err = read_ingest_body(content_file, &body, &body_len);
            } else {
                err = SCRIBE_OK;
            }
            if (err == SCRIBE_OK) {
                err = open_query_repository(store, remote, remote_url, remote_token, client_only, &ctx, &repo);
            }
            if (err == SCRIBE_OK) {
                err = scribe_repository_ingest_content(&repo, content_path, message, op, body, body_len);
            }
            close_query_repository(ctx, &repo);
            free(body);
            return err == SCRIBE_OK ? 0 : fail(err);
        }
        if (pipe_mode) {
            err = read_ingest_body(batch_file, &body, &body_len);
            if (err == SCRIBE_OK) {
                err = open_query_repository(store, remote, remote_url, remote_token, client_only, &ctx, &repo);
            }
            if (err == SCRIBE_OK) {
                err = scribe_repository_ingest_stream(&repo, body, body_len);
            }
            close_query_repository(ctx, &repo);
            free(body);
            return err == SCRIBE_OK ? 0 : fail(err);
        }

        err = read_ingest_body(batch_file, &body, &body_len);
        if (err == SCRIBE_OK) {
            err = open_query_repository(store, remote, remote_url, remote_token, client_only, &ctx, &repo);
        }
        if (err == SCRIBE_OK) {
            err = scribe_repository_ingest(&repo, body, body_len);
        }
        close_query_repository(ctx, &repo);
        free(body);
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "list-objects") == 0) {
        int type_mask = 0;
        int reachable = 0;
        const char *format = "%H %T %S";
        while (argi < argc) {
            if (strcmp(argv[argi], "--reachable") == 0) {
                reachable = 1;
                argi++;
            } else if (strncmp(argv[argi], "--type=", 7) == 0) {
                const char *type = argv[argi] + 7;
                if (strcmp(type, "blob") == 0) {
                    type_mask |= SCRIBE_LIST_TYPE_BLOB;
                } else if (strcmp(type, "tree") == 0) {
                    type_mask |= SCRIBE_LIST_TYPE_TREE;
                } else if (strcmp(type, "commit") == 0) {
                    type_mask |= SCRIBE_LIST_TYPE_COMMIT;
                } else {
                    return fail(scribe_set_error(SCRIBE_EINVAL, "invalid object type '%s'", type));
                }
                argi++;
            } else if (strncmp(argv[argi], "--format=", 9) == 0) {
                format = argv[argi] + 9;
                argi++;
            } else {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
        }
        err = open_ctx(store, 0, &ctx);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        err = scribe_cli_list_objects(ctx, type_mask, reachable, format);
        scribe_close(ctx);
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "ls-tree") == 0) {
        const char *remote = global_remote;
        const char *hash = NULL;

        while (argi < argc) {
            if (argv[argi][0] == '-') {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            } else if (hash == NULL) {
                hash = argv[argi++];
            } else {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
        }
        if (hash == NULL) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        {
            scribe_repository repo;

            err = open_query_repository(store, remote, remote_url, remote_token, client_only, &ctx, &repo);
            if (err == SCRIBE_OK) {
                err = scribe_repository_ls_tree(&repo, hash);
            }
            close_query_repository(ctx, &repo);
        }
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "log") == 0) {
        int oneline = 0;
        int show_paths = 0;
        size_t limit = 0;
        scribe_time_range time_range;
        const char *path_filter = NULL;
        const char *remote = global_remote;
        int after_separator = 0;
        /*
         * log accepts one optional positional path in addition to flags. "--"
         * is supported so paths beginning with '-' can still be passed without
         * being confused for options.
         */
        memset(&time_range, 0, sizeof(time_range));
        while (argi < argc) {
            if (!after_separator && strcmp(argv[argi], "--") == 0) {
                after_separator = 1;
                argi++;
            } else if (!after_separator && strcmp(argv[argi], "--oneline") == 0) {
                oneline = 1;
                argi++;
            } else if (!after_separator && strcmp(argv[argi], "--paths") == 0) {
                show_paths = 1;
                argi++;
            } else if (!after_separator && (strcmp(argv[argi], "--since") == 0 || strcmp(argv[argi], "--until") == 0)) {
                if (argi + 1 >= argc) {
                    return fail(scribe_set_error(SCRIBE_EINVAL, "missing value for %s", argv[argi]));
                }
                err = parse_log_time_option(argv[argi], argv[argi + 1], &time_range);
                if (err != SCRIBE_OK) {
                    return fail(err);
                }
                argi += 2;
            } else if (!after_separator && strcmp(argv[argi], "-n") == 0) {
                if (argi + 1 >= argc) {
                    return fail(scribe_set_error(SCRIBE_EINVAL, "missing value for -n"));
                }
                err = parse_size_option("-n", argv[argi + 1], &limit);
                if (err != SCRIBE_OK) {
                    return fail(err);
                }
                argi += 2;
            } else if (!after_separator && argv[argi][0] == '-') {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            } else {
                if (path_filter != NULL) {
                    usage(stderr);
                    return (int)SCRIBE_EINVAL;
                }
                path_filter = argv[argi++];
            }
        }
        if (path_filter != NULL && path_filter[0] == '\0') {
            return fail(scribe_set_error(SCRIBE_EINVAL, "log path filter must not be empty"));
        }
        {
            scribe_repository repo;

            err = open_query_repository(store, remote, remote_url, remote_token, client_only, &ctx, &repo);
            if (err == SCRIBE_OK) {
                err = scribe_repository_log(&repo, oneline, limit, show_paths, path_filter, &time_range);
            }
            close_query_repository(ctx, &repo);
        }
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "show") == 0) {
        const char *remote = global_remote;
        const char *spec = NULL;

        while (argi < argc) {
            if (spec == NULL) {
                spec = argv[argi++];
            } else {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
        }
        if (spec == NULL) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        {
            scribe_repository repo;

            err = open_query_repository(store, remote, remote_url, remote_token, client_only, &ctx, &repo);
            if (err == SCRIBE_OK) {
                err = scribe_repository_show(&repo, spec);
            }
            close_query_repository(ctx, &repo);
        }
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "cat-object") == 0) {
        char mode = '\0';
        const char *hash = NULL;
        const char *remote = global_remote;

        while (argi < argc) {
            if (argv[argi][0] == '-' && strlen(argv[argi]) == 2u && mode == '\0') {
                mode = argv[argi][1];
                argi++;
            } else if (hash == NULL) {
                hash = argv[argi++];
            } else {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
        }
        if (mode == '\0' || hash == NULL) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        {
            scribe_repository repo;

            err = open_query_repository(store, remote, remote_url, remote_token, client_only, &ctx, &repo);
            if (err == SCRIBE_OK) {
                err = scribe_repository_cat_object(&repo, mode, hash);
            }
            close_query_repository(ctx, &repo);
        }
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "diff") == 0) {
        const char *remote = global_remote;
        const char *a = NULL;
        const char *b = NULL;

        while (argi < argc) {
            if (a == NULL) {
                a = argv[argi++];
            } else if (b == NULL) {
                b = argv[argi++];
            } else {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
        }
        if (a == NULL) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        {
            scribe_repository repo;

            err = open_query_repository(store, remote, remote_url, remote_token, client_only, &ctx, &repo);
            if (err == SCRIBE_OK) {
                err = scribe_repository_diff(&repo, a, b);
            }
            close_query_repository(ctx, &repo);
        }
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "daemon") == 0) {
        const char *config_path = NULL;

        while (argi < argc) {
            if (strcmp(argv[argi], "--config") == 0 && argi + 1 < argc) {
                config_path = argv[argi + 1];
                argi += 2;
            } else {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
        }
        if (config_path == NULL) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        err = scribe_cli_daemon(config_path, argv[0]);
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "status") == 0) {
        if (argi != argc) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        err = open_ctx(store, 0, &ctx);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        err = scribe_cli_status(ctx);
        scribe_close(ctx);
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "fsck") == 0) {
        if (argi != argc) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        err = open_ctx(store, 1, &ctx);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        err = scribe_cli_fsck(ctx);
        scribe_close(ctx);
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "gc") == 0) {
        int dry_run = 0;
        int prune_now = 0;
        int pack = 0;
        int consolidate = 0;

        while (argi < argc) {
            if (strcmp(argv[argi], "--dry-run") == 0) {
                dry_run = 1;
            } else if (strcmp(argv[argi], "--prune-now") == 0) {
                prune_now = 1;
            } else if (strcmp(argv[argi], "--pack") == 0) {
                pack = 1;
            } else if (strcmp(argv[argi], "--consolidate") == 0) {
                consolidate = 1;
            } else {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            argi++;
        }
        err = open_ctx(store, dry_run ? 0 : 1, &ctx);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        err = scribe_cli_gc(ctx, dry_run, prune_now, pack, consolidate);
        scribe_close(ctx);
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "branch") == 0) {
        const char *name;
        const char *commit = NULL;

        if (argi >= argc || argi + 2 < argc) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        name = argv[argi++];
        if (argi < argc) {
            commit = argv[argi++];
        }
        err = open_ctx(store, 1, &ctx);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        err = scribe_cli_branch(ctx, name, commit);
        scribe_close(ctx);
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "tag") == 0) {
        int delete_mode = 0;
        int force = 0;
        const char *name = NULL;
        const char *commit = NULL;

        while (argi < argc) {
            if (strcmp(argv[argi], "-d") == 0) {
                delete_mode = 1;
            } else if (strcmp(argv[argi], "--force") == 0) {
                force = 1;
            } else if (name == NULL) {
                name = argv[argi];
            } else if (commit == NULL) {
                commit = argv[argi];
            } else {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            argi++;
        }
        if (name == NULL || (!delete_mode && commit == NULL) || (delete_mode && commit != NULL)) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        err = open_ctx(store, 1, &ctx);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        err = scribe_cli_tag(ctx, delete_mode, force, name, commit);
        scribe_close(ctx);
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "reflog") == 0) {
        if (argi != argc - 1) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        err = open_ctx(store, 0, &ctx);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        err = scribe_cli_reflog(ctx, argv[argi]);
        scribe_close(ctx);
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "remote") == 0) {
        const char *subcmd;

        if (argi >= argc) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        subcmd = argv[argi++];
        if (strcmp(subcmd, "add") == 0) {
            const char *name;
            const char *url;
            const char *token = NULL;

            if (argi + 2 > argc) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            name = argv[argi++];
            url = argv[argi++];
            if (argi < argc) {
                if (argi + 2 != argc || strcmp(argv[argi], "--token") != 0) {
                    usage(stderr);
                    return (int)SCRIBE_EINVAL;
                }
                token = argv[argi + 1];
            }
            err = open_ctx(store, 1, &ctx);
            if (err != SCRIBE_OK) {
                return fail(err);
            }
            err = scribe_cli_remote_add(ctx, name, url, token);
            scribe_close(ctx);
            return err == SCRIBE_OK ? 0 : fail(err);
        }
        if (strcmp(subcmd, "list") == 0) {
            if (argi != argc) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            err = open_ctx(store, 0, &ctx);
            if (err != SCRIBE_OK) {
                return fail(err);
            }
            err = scribe_cli_remote_list(ctx);
            scribe_close(ctx);
            return err == SCRIBE_OK ? 0 : fail(err);
        }
        if (strcmp(subcmd, "remove") == 0) {
            if (argi != argc - 1) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            err = open_ctx(store, 1, &ctx);
            if (err != SCRIBE_OK) {
                return fail(err);
            }
            err = scribe_cli_remote_remove(ctx, argv[argi]);
            scribe_close(ctx);
            return err == SCRIBE_OK ? 0 : fail(err);
        }
        if (strcmp(subcmd, "test") == 0) {
            if (argi != argc - 1) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            err = open_ctx(store, 0, &ctx);
            if (err != SCRIBE_OK) {
                return fail(err);
            }
            err = scribe_cli_remote_test(ctx, argv[argi]);
            scribe_close(ctx);
            return err == SCRIBE_OK ? 0 : fail(err);
        }
        usage(stderr);
        return (int)SCRIBE_EINVAL;
    }
    if (strcmp(cmd, "bundle") == 0) {
        const char *subcmd;

        if (argi >= argc) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        subcmd = argv[argi++];
        if (strcmp(subcmd, "create") == 0) {
            if (argi != argc - 1) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            err = open_ctx(store, 0, &ctx);
            if (err != SCRIBE_OK) {
                return fail(err);
            }
            err = scribe_cli_bundle_create(ctx, argv[argi]);
            scribe_close(ctx);
            return err == SCRIBE_OK ? 0 : fail(err);
        }
        if (strcmp(subcmd, "import") == 0) {
            if (argi + 2 != argc) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            err = scribe_cli_bundle_import(argv[argi], argv[argi + 1]);
            return err == SCRIBE_OK ? 0 : fail(err);
        }
        usage(stderr);
        return (int)SCRIBE_EINVAL;
    }
    if (strcmp(cmd, "serve") == 0) {
        (void)argi;
        return fail(scribe_set_error(SCRIBE_EINVAL, "the serve subcommand was removed; run 'scribe' with no command"));
    }
    if (strcmp(cmd, "replicate") == 0) {
        const char *remote = NULL;
        unsigned long interval_ms = 1000;
        unsigned long max_cycles = 1;
        unsigned long cycle = 0;

        while (argi < argc) {
            if (strcmp(argv[argi], "--interval-ms") == 0) {
                if (argi + 1 >= argc) {
                    return fail(scribe_set_error(SCRIBE_EINVAL, "missing value for --interval-ms"));
                }
                err = parse_ulong_option("--interval-ms", argv[argi + 1], ULONG_MAX, &interval_ms);
                if (err != SCRIBE_OK) {
                    return fail(err);
                }
                argi += 2;
            } else if (strcmp(argv[argi], "--max-cycles") == 0) {
                if (argi + 1 >= argc) {
                    return fail(scribe_set_error(SCRIBE_EINVAL, "missing value for --max-cycles"));
                }
                err = parse_ulong_option("--max-cycles", argv[argi + 1], ULONG_MAX, &max_cycles);
                if (err != SCRIBE_OK) {
                    return fail(err);
                }
                argi += 2;
            } else if (remote == NULL) {
                remote = argv[argi++];
            } else {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
        }
        if (remote == NULL) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        err = open_ctx(store, 1, &ctx);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        do {
            err = scribe_cli_replicate(ctx, remote);
            if (err != SCRIBE_OK) {
                break;
            }
            cycle++;
            if (max_cycles != 0 && cycle >= max_cycles) {
                break;
            }
            if (interval_ms > 3600000ul) {
                interval_ms = 3600000ul;
            }
            usleep((useconds_t)(interval_ms * 1000ul));
        } while (1);
        scribe_close(ctx);
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    usage(stderr);
    return (int)SCRIBE_EINVAL;
}
