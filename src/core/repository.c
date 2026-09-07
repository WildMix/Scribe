/*
 * Repository operations facade.
 *
 * The CLI and REPL use this layer for history inspection so local and remote
 * repositories share one dispatch surface. The current implementation delegates
 * to the existing local formatters or remote query client; future structured
 * event formatters can be added here without changing command parsers.
 */
#include "core/internal.h"

#include "util/error.h"

#include <string.h>

scribe_error_t scribe_repository_open(scribe_repository *repo, scribe_ctx *ctx, const char *remote) {
    if (repo == NULL || ctx == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid repository open arguments");
    }
    memset(repo, 0, sizeof(*repo));
    repo->ctx = ctx;
    repo->remote = remote;
    return SCRIBE_OK;
}

scribe_error_t scribe_repository_open_url(scribe_repository *repo, const char *url, const char *token) {
    if (repo == NULL || url == NULL || url[0] == '\0') {
        return scribe_set_error(SCRIBE_EINVAL, "invalid remote URL repository arguments");
    }
    memset(repo, 0, sizeof(*repo));
    repo->url = url;
    repo->token = token;
    return SCRIBE_OK;
}

void scribe_repository_close(scribe_repository *repo) {
    if (repo != NULL) {
        memset(repo, 0, sizeof(*repo));
    }
}

scribe_error_t scribe_repository_check(scribe_repository *repo) {
    if (repo == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid repository check arguments");
    }
    if (repo->url != NULL) {
        return scribe_cli_remote_check_url(repo->url, repo->token);
    }
    if (repo->remote != NULL) {
        return scribe_cli_remote_check(repo->ctx, repo->remote);
    }
    return SCRIBE_OK;
}

scribe_error_t scribe_repository_info(scribe_repository *repo) {
    if (repo->url != NULL) {
        return scribe_cli_remote_info_url(repo->url, repo->token);
    }
    return repo->remote == NULL ? scribe_cli_info(repo->ctx) : scribe_cli_remote_info(repo->ctx, repo->remote);
}

scribe_error_t scribe_repository_refs(scribe_repository *repo) {
    if (repo->url != NULL) {
        return scribe_cli_remote_refs_url(repo->url, repo->token);
    }
    return repo->remote == NULL ? scribe_cli_refs(repo->ctx) : scribe_cli_remote_refs(repo->ctx, repo->remote);
}

scribe_error_t scribe_repository_log(scribe_repository *repo, int oneline, size_t limit, int show_paths,
                                     const char *path_filter, const scribe_time_range *time_range) {
    if (repo->url != NULL) {
        return scribe_cli_remote_log_url(repo->url, repo->token, oneline, limit, show_paths, path_filter, time_range);
    }
    return repo->remote == NULL
               ? scribe_cli_log(repo->ctx, oneline, limit, show_paths, path_filter, time_range)
               : scribe_cli_remote_log(repo->ctx, repo->remote, oneline, limit, show_paths, path_filter, time_range);
}

scribe_error_t scribe_repository_show(scribe_repository *repo, const char *spec) {
    if (repo->url != NULL) {
        return scribe_cli_remote_show_url(repo->url, repo->token, spec);
    }
    return repo->remote == NULL ? scribe_cli_show(repo->ctx, spec)
                                : scribe_cli_remote_show(repo->ctx, repo->remote, spec);
}

scribe_error_t scribe_repository_cat_object(scribe_repository *repo, char mode, const char *hex) {
    if (repo->url != NULL) {
        return scribe_cli_remote_cat_object_url(repo->url, repo->token, mode, hex);
    }
    return repo->remote == NULL ? scribe_cli_cat_object(repo->ctx, mode, hex)
                                : scribe_cli_remote_cat_object(repo->ctx, repo->remote, mode, hex);
}

scribe_error_t scribe_repository_diff(scribe_repository *repo, const char *a, const char *b) {
    if (repo->url != NULL) {
        return scribe_cli_remote_diff_url(repo->url, repo->token, a, b);
    }
    return repo->remote == NULL ? scribe_cli_diff(repo->ctx, a, b)
                                : scribe_cli_remote_diff(repo->ctx, repo->remote, a, b);
}

scribe_error_t scribe_repository_ls_tree(scribe_repository *repo, const char *hex) {
    if (repo->url != NULL) {
        return scribe_cli_remote_ls_tree_url(repo->url, repo->token, hex);
    }
    return repo->remote == NULL ? scribe_cli_ls_tree(repo->ctx, hex)
                                : scribe_cli_remote_ls_tree(repo->ctx, repo->remote, hex);
}

scribe_error_t scribe_repository_ingest(scribe_repository *repo, const uint8_t *body, size_t body_len) {
    if (repo == NULL || body == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid ingest repository arguments");
    }
    if (repo->url != NULL) {
        return scribe_cli_remote_ingest_url(repo->url, repo->token, body, body_len);
    }
    if (repo->remote != NULL) {
        return scribe_cli_remote_ingest(repo->ctx, repo->remote, body, body_len);
    }
    return scribe_set_error(SCRIBE_EINVAL, "ingest requires a Scribe server; use --url or --remote");
}

scribe_error_t scribe_repository_ingest_stream(scribe_repository *repo, const uint8_t *body, size_t body_len) {
    if (repo == NULL || body == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid stream ingest repository arguments");
    }
    if (repo->url != NULL) {
        return scribe_cli_remote_ingest_stream_url(repo->url, repo->token, body, body_len);
    }
    if (repo->remote != NULL) {
        return scribe_cli_remote_ingest_stream(repo->ctx, repo->remote, body, body_len);
    }
    return scribe_set_error(SCRIBE_EINVAL, "stream ingest requires a Scribe server; use --url or --remote");
}
scribe_error_t scribe_repository_ingest_content(scribe_repository *repo, const char *path, const char *message,
                                                scribe_ingest_op op, const uint8_t *body, size_t body_len) {
    if (repo == NULL || path == NULL || (op == SCRIBE_INGEST_OP_PUT && body == NULL)) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid content ingest repository arguments");
    }
    if (op != SCRIBE_INGEST_OP_PUT && op != SCRIBE_INGEST_OP_DELETE) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid content ingest operation");
    }
    if (repo->url != NULL) {
        return scribe_cli_remote_ingest_content_url(repo->url, repo->token, path, message, op, body, body_len);
    }
    if (repo->remote != NULL) {
        return scribe_cli_remote_ingest_content(repo->ctx, repo->remote, path, message, op, body, body_len);
    }
    return scribe_set_error(SCRIBE_EINVAL, "content ingest requires a Scribe server; use --url or --remote");
}
