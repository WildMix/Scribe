#include "util/log.h"

#include "core/internal.h"
#include "util/error.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static scribe_log_level g_min_level = SCRIBE_LOG_INFO;

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

void scribe_log_close(scribe_ctx *ctx) {
    if (ctx != NULL && ctx->log_file != NULL) {
        fclose(ctx->log_file);
        ctx->log_file = NULL;
    }
}

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

void scribe_log_flush(scribe_ctx *ctx) {
    if (ctx != NULL && ctx->log_file != NULL) {
        fflush(ctx->log_file);
    }
}
