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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SCRIBE_HAVE_MONGO_ADAPTER
#include "adapter_mongo/mongo_adapter.h"
#endif

/*
 * Prints the command summary used for invalid arguments and --help. Keep this
 * text synchronized with the parser below: Scribe intentionally has one
 * top-level help page rather than per-command help in v1.
 */
static void usage(FILE *out) {
    fputs("Scribe - content-addressed history for adapter snapshots\n"
          "\n"
          "Usage:\n"
          "  scribe [--store <path>] <command> [args]\n"
          "  scribe -h | --help\n"
          "\n"
          "Global options:\n"
          "  --store <path>\n"
          "      Path to the .scribe repository. Defaults to ./.scribe. Most commands\n"
          "      accept this only before the command; mongo-watch also accepts it after\n"
          "      the URI for operational scripts.\n"
          "  -h, --help\n"
          "      Print this help text.\n"
          "\n"
          "Revision syntax:\n"
          "  Commands that accept commits resolve HEAD, HEAD~N, or a full 64-hex commit\n"
          "  hash. Object commands require full object hashes unless noted otherwise.\n"
          "\n"
          "Commands:\n"
          "\n",
          out);
    fputs("  init\n"
          "    Usage:   scribe [--store <path>] init [path]\n"
          "    Options: none\n"
          "    Does:    Create a repository skeleton: HEAD, config, log, object\n"
          "             directories, refs directory, adapter-state directory, and lock\n"
          "             path. If [path] is omitted, initializes --store or ./.scribe.\n"
          "\n"
          "  info\n"
          "    Usage:   scribe [--store <path>] info\n"
          "    Options: none\n"
          "    Does:    Print Scribe version, hash algorithm, pipe protocol, and selected\n"
          "             repository config. Works even when the store is not initialized.\n"
          "\n"
          "  commit-batch\n"
          "    Usage:   scribe [--store <path>] commit-batch < frame.stream\n"
          "    Options: none\n"
          "    Does:    Read pipe protocol v1 BATCH frames from stdin, validate them,\n"
          "             apply payload/tombstone events to the current tree, write new\n"
          "             objects, and advance refs/heads/main atomically.\n"
          "\n",
          out);
    fputs("  list-objects\n"
          "    Usage:   scribe [--store <path>] list-objects [options]\n"
          "    Options: --type=blob|tree|commit\n"
          "                 Filter by object type. May be repeated; repeated types are\n"
          "                 combined.\n"
          "             --reachable\n"
          "                 Print only objects reachable from HEAD through the full\n"
          "                 parent chain, each commit root tree, and all nested trees.\n"
          "                 This keeps the reachable set in memory.\n"
          "             --format=<spec>\n"
          "                 Output template. Placeholders: %H full hash, %T type,\n"
          "                 %S uncompressed payload size, %C compressed on-disk size.\n"
          "                 Default: %H %T %S. %C stats each loose object and is slower.\n"
          "    Does:    Iterate the object store and print matching objects. Default\n"
          "             output includes dangling loose objects and is intentionally\n"
          "             unsorted; pipe to sort when stable ordering is needed.\n"
          "\n"
          "  ls-tree\n"
          "    Usage:   scribe [--store <path>] ls-tree <hash>\n"
          "    Options: none\n"
          "    Does:    Recursively list a tree as: <type><tab><hash><tab><path>.\n"
          "             A commit hash resolves to its root tree. A blob hash fails.\n"
          "             Entries follow byte-sorted tree storage order.\n"
          "\n",
          out);
    fputs("  log\n"
          "    Usage:   scribe [--store <path>] log [--oneline] [--paths] [-n <N>]\n"
          "             scribe [--store <path>] log [options] [--] <path>\n"
          "    Options: --oneline\n"
          "                 Print one compact line per emitted commit.\n"
          "             --paths\n"
          "                 Also print changed leaf paths for each emitted commit. In\n"
          "                 oneline mode this appends [N changed].\n"
          "             -n <N>\n"
          "                 Limit the number of commits emitted. With a path filter,\n"
          "                 Scribe still scans history until N matching commits appear.\n"
          "             -- <path>\n"
          "                 End option parsing and treat the next argument as a path.\n"
          "    Does:    Walk the parent chain from HEAD. With <path>, emit only commits\n"
          "             where that path's blob or tree hash differs from the parent,\n"
          "             annotating first appearance as added and disappearance as deleted.\n"
          "\n"
          "  show\n"
          "    Usage:   scribe [--store <path>] show <commit>\n"
          "             scribe [--store <path>] show <commit>:<path>\n"
          "    Options: none\n"
          "    Does:    Without :<path>, print commit metadata, message, and touched\n"
          "             paths. With :<path>, resolve the path inside the commit root;\n"
          "             blob paths write raw bytes exactly, tree paths list recursively.\n"
          "             Quote the argument in the shell when Mongo _id JSON contains\n"
          "             braces, quotes, or other shell-special characters.\n"
          "\n",
          out);
    fputs("  cat-object\n"
          "    Usage:   scribe [--store <path>] cat-object (-p|-t|-s) <hash>\n"
          "    Options: -p  Print raw object payload bytes.\n"
          "             -t  Print object type: blob, tree, or commit.\n"
          "             -s  Print uncompressed payload size.\n"
          "    Does:    Read one object by full hash, verifying compression, envelope,\n"
          "             and BLAKE3 hash before printing the requested view.\n"
          "\n"
          "  diff\n"
          "    Usage:   scribe [--store <path>] diff <commit>\n"
          "             scribe [--store <path>] diff <commit1> <commit2>\n"
          "    Options: none\n"
          "    Does:    Compare commit root trees. With one commit, compare its parent to\n"
          "             it. Output is one changed leaf path per line prefixed by A, M, or D.\n"
          "\n"
          "  fsck\n"
          "    Usage:   scribe [--store <path>] fsck\n"
          "    Options: none\n"
          "    Does:    Verify reachable history from refs/heads/main, following parent\n"
          "             commits and tree edges, checking object envelopes and hashes on\n"
          "             read. Then scan loose objects and report unvisited ones as dangling.\n"
          "\n",
          out);
    fputs("  mongo-watch\n"
          "    Usage:   scribe [--store <path>] mongo-watch <uri>\n"
          "             scribe mongo-watch <uri> --store <path>\n"
          "    Options: <uri>\n"
          "                 MongoDB connection string. Include a database path for\n"
          "                 database-scoped bootstrap/watch; omit it for cluster scope.\n"
          "    Does:    Acquire the writer lock, bootstrap current MongoDB document state\n"
          "             when needed, resume from adapter-state when possible, consume\n"
          "             change streams, write document commits, print concise commit\n"
          "             summaries, and persist resume state only after commit success.\n",
          out);
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

/*
 * Implements `scribe info`. It prints binary-level facts even when the store is
 * missing, then prints repository settings only if the store opens successfully.
 */
static int cmd_info(const char *store) {
    scribe_ctx *ctx = NULL;
    scribe_error_t err = scribe_open(store, 0, &ctx);

    printf("scribe version %s\n", SCRIBE_VERSION);
    printf("hash_algorithm blake3-256\n");
    printf("pipe_protocol 1\n");
    if (err == SCRIBE_OK) {
        printf("store %s\n", store);
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

/*
 * Parses global options, dispatches subcommands, and owns all CLI context
 * lifetimes. Each command opens the repository in the narrowest mode it needs:
 * writable for commit-producing commands and read-only for inspection commands.
 */
int main(int argc, char **argv) {
    const char *store = ".scribe";
    const char *cmd;
    int argi = 1;
    scribe_ctx *ctx = NULL;
    scribe_error_t err;

    if (argc < 2) {
        usage(stderr);
        return (int)SCRIBE_EINVAL;
    }
    if (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(argv[argi], "--store") == 0) {
        if (argi + 2 >= argc) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        store = argv[argi + 1];
        argi += 2;
    }
    cmd = argv[argi++];

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
        return cmd_info(store);
    }
    if (strcmp(cmd, "commit-batch") == 0) {
        err = open_ctx(store, 1, &ctx);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        err = scribe_pipe_commit_batch(ctx, stdin, stdout);
        scribe_close(ctx);
        return err == SCRIBE_OK ? 0 : (int)err;
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
        if (argi != argc - 1) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        err = open_ctx(store, 0, &ctx);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        err = scribe_cli_ls_tree(ctx, argv[argi]);
        scribe_close(ctx);
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "log") == 0) {
        int oneline = 0;
        int show_paths = 0;
        size_t limit = 0;
        const char *path_filter = NULL;
        int after_separator = 0;
        /*
         * log accepts one optional positional path in addition to flags. "--"
         * is supported so paths beginning with '-' can still be passed without
         * being confused for options.
         */
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
            } else if (!after_separator && strcmp(argv[argi], "-n") == 0 && argi + 1 < argc) {
                limit = (size_t)strtoul(argv[argi + 1], NULL, 10);
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
        err = open_ctx(store, 0, &ctx);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        err = scribe_cli_log(ctx, oneline, limit, show_paths, path_filter);
        scribe_close(ctx);
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "show") == 0) {
        if (argi != argc - 1) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        err = open_ctx(store, 0, &ctx);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        err = scribe_cli_show(ctx, argv[argi]);
        scribe_close(ctx);
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "cat-object") == 0) {
        char mode;
        if (argi + 2 != argc || argv[argi][0] != '-' || strlen(argv[argi]) != 2u) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        mode = argv[argi][1];
        err = open_ctx(store, 0, &ctx);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        err = scribe_cli_cat_object(ctx, mode, argv[argi + 1]);
        scribe_close(ctx);
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "diff") == 0) {
        if (argi >= argc || argi + 2 < argc) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        err = open_ctx(store, 0, &ctx);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        err = scribe_cli_diff(ctx, argv[argi], argi + 1 < argc ? argv[argi + 1] : NULL);
        scribe_close(ctx);
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "fsck") == 0) {
        err = open_ctx(store, 0, &ctx);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        err = scribe_cli_fsck(ctx);
        scribe_close(ctx);
        return err == SCRIBE_OK ? 0 : fail(err);
    }
    if (strcmp(cmd, "mongo-watch") == 0) {
#ifdef SCRIBE_HAVE_MONGO_ADAPTER
        const char *uri;

        if (argi >= argc) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        /*
         * Most commands accept --store only before the subcommand. mongo-watch
         * additionally accepts the historical smoke-test form with --store
         * after the URI because long-running watcher invocations are often
         * copied from operational examples.
         */
        uri = argv[argi++];
        if (argi < argc) {
            if (argi + 2 != argc || strcmp(argv[argi], "--store") != 0) {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
            store = argv[argi + 1];
        }
        err = open_ctx(store, 1, &ctx);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        err = scribe_mongo_watch_bootstrap(ctx, uri);
        scribe_close(ctx);
        return err == SCRIBE_OK ? 0 : fail(err);
#else
        (void)argi;
        fprintf(stderr, "scribe: %s: mongo-watch is not implemented in Milestone 1\n",
                scribe_error_symbol(SCRIBE_ENOSYS));
        return (int)SCRIBE_ENOSYS;
#endif
    }

    usage(stderr);
    return (int)SCRIBE_EINVAL;
}
