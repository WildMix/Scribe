#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SCRIBE_HAVE_MONGO_ADAPTER
#include "adapter_mongo/mongo_adapter.h"
#endif

static void usage(FILE *out) {
    fprintf(out, "usage: scribe [--store <path>] <command> [args]\n"
                 "\n"
                 "commands:\n"
                 "  init <path>\n"
                 "  info\n"
                 "  log [--oneline] [-n <N>]\n"
                 "  show <commit>\n"
                 "  cat-object (-p|-t|-s) <hash>\n"
                 "  diff <commit1> [<commit2>]\n"
                 "  commit-batch\n"
                 "  fsck\n"
                 "  mongo-watch <uri>\n");
}

static int fail(scribe_error_t err) {
    fprintf(stderr, "scribe: %s: %s\n", scribe_error_symbol(err), scribe_last_error_detail());
    return (int)err;
}

static scribe_error_t open_ctx(const char *store, int writable, scribe_ctx **ctx) {
    return scribe_open(store, writable, ctx);
}

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
        if (argi != argc - 1) {
            usage(stderr);
            return (int)SCRIBE_EINVAL;
        }
        err = scribe_init_repository(argv[argi]);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        printf("initialized %s\n", argv[argi]);
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
    if (strcmp(cmd, "log") == 0) {
        int oneline = 0;
        size_t limit = 0;
        while (argi < argc) {
            if (strcmp(argv[argi], "--oneline") == 0) {
                oneline = 1;
                argi++;
            } else if (strcmp(argv[argi], "-n") == 0 && argi + 1 < argc) {
                limit = (size_t)strtoul(argv[argi + 1], NULL, 10);
                argi += 2;
            } else {
                usage(stderr);
                return (int)SCRIBE_EINVAL;
            }
        }
        err = open_ctx(store, 0, &ctx);
        if (err != SCRIBE_OK) {
            return fail(err);
        }
        err = scribe_cli_log(ctx, oneline, limit);
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
