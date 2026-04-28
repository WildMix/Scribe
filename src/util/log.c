/*
 * Operational logging implementation.
 *
 * Logs are written in the v1 format `<iso8601-utc> <LEVEL> <component>
 * <message>` to stderr and, when a repository context is open, to `.scribe/log`.
 * This file also owns SCRIBE_LOG_LEVEL parsing so all commands share the same
 * runtime log-level behavior.
 */
#include "util/log.h"

#include "core/internal.h"
#include "util/error.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static scribe_log_level g_min_level = SCRIBE_LOG_INFO;

/*
 * Maps the internal log-level enum to the exact uppercase token used in log
 * lines. Unknown values are treated as ERROR so malformed callers do not print
 * an empty or misleading level.
 */
static const char *level_name(scribe_log_level level) {
    switch (level) {
    case SCRIBE_LOG_DEBUG:
        return "DEBUG";
    case SCRIBE_LOG_INFO:
        return "INFO";
    case SCRIBE_LOG_WARN:
        return "WARN";
    case SCRIBE_LOG_ERROR:
        return "ERROR";
    default:
        return "ERROR";
    }
}

/*
 * Reads SCRIBE_LOG_LEVEL and updates the process-wide minimum level. Missing or
 * empty values default to INFO; unknown values fail with SCRIBE_ECONFIG because
 * a misspelled logging setting should be visible to the operator.
 */
scribe_error_t scribe_log_configure_from_env(void) {
    const char *level = getenv("SCRIBE_LOG_LEVEL");

    if (level == NULL || level[0] == '\0') {
        g_min_level = SCRIBE_LOG_INFO;
        return SCRIBE_OK;
    }
    if (strcmp(level, "DEBUG") == 0) {
        g_min_level = SCRIBE_LOG_DEBUG;
    } else if (strcmp(level, "INFO") == 0) {
        g_min_level = SCRIBE_LOG_INFO;
    } else if (strcmp(level, "WARN") == 0) {
        g_min_level = SCRIBE_LOG_WARN;
    } else if (strcmp(level, "ERROR") == 0) {
        g_min_level = SCRIBE_LOG_ERROR;
    } else {
        return scribe_set_error(SCRIBE_ECONFIG, "unknown SCRIBE_LOG_LEVEL '%s'", level);
    }
    return SCRIBE_OK;
}

/*
 * Opens the repository's append-only operational log. The context must already
 * know the repository path; read-only and writable commands both use this so
 * diagnostics are preserved across command invocations.
 */
scribe_error_t scribe_log_open(scribe_ctx *ctx) {
    char *path;

    if (ctx == NULL || ctx->repo_path == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid logger context");
    }
    path = scribe_path_join(ctx->repo_path, "log");
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    ctx->log_file = fopen(path, "ab");
    free(path);
    if (ctx->log_file == NULL) {
        return scribe_set_error(SCRIBE_EIO, "failed to open .scribe/log");
    }
    return SCRIBE_OK;
}

/*
 * Closes the log file owned by a context. The function tolerates NULL contexts
 * and unopened logs so cleanup paths can call it unconditionally.
 */
void scribe_log_close(scribe_ctx *ctx) {
    if (ctx != NULL && ctx->log_file != NULL) {
        fclose(ctx->log_file);
        ctx->log_file = NULL;
    }
}

/*
 * Writes one formatted log message if its level passes the current threshold.
 * The timestamp is generated in UTC and the same line is sent to stderr and the
 * repository log file so interactive and persisted diagnostics match exactly.
 */
void scribe_log_msg(scribe_ctx *ctx, scribe_log_level level, const char *component, const char *fmt, ...) {
    char ts[32];
    char message[768];
    time_t now;
    struct tm tm_utc;
    va_list ap;

    if (level < g_min_level) {
        return;
    }
    now = time(NULL);
    if (gmtime_r(&now, &tm_utc) == NULL || strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        strcpy(ts, "1970-01-01T00:00:00Z");
    }

    va_start(ap, fmt);
    (void)vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s %s %s %s\n", ts, level_name(level), component, message);
    if (ctx != NULL && ctx->log_file != NULL) {
        fprintf(ctx->log_file, "%s %s %s %s\n", ts, level_name(level), component, message);
    }
}

/*
 * Flushes the repository log stream when one is open. Long-running adapters use
 * this at important boundaries so recent diagnostics are not left only in libc
 * buffers during shutdown.
 */
void scribe_log_flush(scribe_ctx *ctx) {
    if (ctx != NULL && ctx->log_file != NULL) {
        fflush(ctx->log_file);
    }
}
