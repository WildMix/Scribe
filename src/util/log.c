/*
 * Operational logging implementation.
 *
 * Logs are written in the v1 format `<iso8601-utc> <LEVEL> <component>
 * <message>` to stderr and, when a repository context is open, to the context's
 * selected log file. This file also owns SCRIBE_LOG_LEVEL parsing so all
 * commands share the same runtime log-level behavior.
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
static int g_json_logs = 0;

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

static scribe_error_t parse_log_format(const char *format, int *out_json) {
    if (format == NULL || format[0] == '\0' || strcmp(format, "text") == 0) {
        *out_json = 0;
        return SCRIBE_OK;
    }
    if (strcmp(format, "json") == 0) {
        *out_json = 1;
        return SCRIBE_OK;
    }
    return scribe_set_error(SCRIBE_ECONFIG, "unknown log format '%s'", format);
}

scribe_error_t scribe_log_set_format(const char *format) { return parse_log_format(format, &g_json_logs); }

/*
 * Reads SCRIBE_LOG_LEVEL and updates the process-wide minimum level. Missing or
 * empty values default to INFO; unknown values fail with SCRIBE_ECONFIG because
 * a misspelled logging setting should be visible to the operator.
 */
scribe_error_t scribe_log_configure_from_env(void) {
    const char *level = getenv("SCRIBE_LOG_LEVEL");
    const char *format = getenv("SCRIBE_LOG_FORMAT");
    int json_logs = 0;
    scribe_error_t err;

    if (level == NULL || level[0] == '\0') {
        g_min_level = SCRIBE_LOG_INFO;
    } else if (strcmp(level, "DEBUG") == 0) {
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
    if (format != NULL && format[0] != '\0') {
        err = parse_log_format(format, &json_logs);
        if (err != SCRIBE_OK) {
            return err;
        }
        g_json_logs = json_logs;
    }
    return SCRIBE_OK;
}

static void timestamp_utc(char ts[32]) {
    time_t now;
    struct tm tm_utc;

    now = time(NULL);
    if (gmtime_r(&now, &tm_utc) == NULL || strftime(ts, 32, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        strcpy(ts, "1970-01-01T00:00:00Z");
    }
}

static void json_write_string(FILE *out, const char *s) {
    const unsigned char *p = (const unsigned char *)(s == NULL ? "" : s);

    fputc('"', out);
    while (*p != '\0') {
        switch (*p) {
        case '"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '\b':
            fputs("\\b", out);
            break;
        case '\f':
            fputs("\\f", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if (*p < 0x20u) {
                fprintf(out, "\\u%04x", (unsigned)*p);
            } else {
                fputc((int)*p, out);
            }
            break;
        }
        p++;
    }
    fputc('"', out);
}

static void write_json_log_line(FILE *out, const char *ts, const char *level, const char *component, const char *event,
                                const char *message, const char *commit_hash, const char *operation, const char *path,
                                const char *correlation_id) {
    fputs("{\"timestamp\":", out);
    json_write_string(out, ts);
    fputs(",\"level\":", out);
    json_write_string(out, level);
    fputs(",\"component\":", out);
    json_write_string(out, component);
    fputs(",\"event\":", out);
    json_write_string(out, event == NULL || event[0] == '\0' ? "log" : event);
    fputs(",\"message\":", out);
    json_write_string(out, message);
    if (commit_hash != NULL && commit_hash[0] != '\0') {
        fputs(",\"commit_hash\":", out);
        json_write_string(out, commit_hash);
    }
    if (operation != NULL && operation[0] != '\0') {
        fputs(",\"mongo_operation\":", out);
        json_write_string(out, operation);
    }
    if (path != NULL && path[0] != '\0') {
        fputs(",\"path\":", out);
        json_write_string(out, path);
    }
    if (correlation_id != NULL && correlation_id[0] != '\0') {
        fputs(",\"correlation_id\":", out);
        json_write_string(out, correlation_id);
    }
    fputs("}\n", out);
}

static void emit_log_line(scribe_ctx *ctx, scribe_log_level level, const char *component, const char *event,
                          const char *message, const char *commit_hash, const char *operation, const char *path,
                          const char *correlation_id) {
    char ts[32];
    const char *level_text = level_name(level);
    const char *component_text = component == NULL || component[0] == '\0' ? "scribe" : component;

    timestamp_utc(ts);
    if (g_json_logs) {
        write_json_log_line(stderr, ts, level_text, component_text, event, message, commit_hash, operation, path,
                            correlation_id);
        if (ctx != NULL && ctx->log_file != NULL) {
            write_json_log_line(ctx->log_file, ts, level_text, component_text, event, message, commit_hash, operation,
                                path, correlation_id);
        }
    } else {
        fprintf(stderr, "%s %s %s %s\n", ts, level_text, component_text, message == NULL ? "" : message);
        if (ctx != NULL && ctx->log_file != NULL) {
            fprintf(ctx->log_file, "%s %s %s %s\n", ts, level_text, component_text, message == NULL ? "" : message);
        }
    }
}

static int log_filename_is_safe(const char *filename) {
    const char *p;

    if (filename == NULL || filename[0] == '\0' || filename[0] == '.') {
        return 0;
    }
    for (p = filename; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\' || *p == '\n' || *p == '\r' || *p == '\t') {
            return 0;
        }
    }
    return 1;
}

/*
 * Selects the repository-local append-only log file for a context. Server and
 * adapter processes call this after opening the store so their operational
 * streams stay separate while reusing the same stderr/JSON formatting path.
 */
scribe_error_t scribe_log_use_file(scribe_ctx *ctx, const char *filename) {
    char *path;
    FILE *file;

    if (ctx == NULL || ctx->repo_path == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid logger context");
    }
    if (!log_filename_is_safe(filename)) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid log filename");
    }
    path = scribe_path_join(ctx->repo_path, filename);
    if (path == NULL) {
        return SCRIBE_ENOMEM;
    }
    file = fopen(path, "ab");
    free(path);
    if (file == NULL) {
        return scribe_set_error(SCRIBE_EIO, "failed to open log file '%s'", filename);
    }
    if (ctx->log_file != NULL) {
        fclose(ctx->log_file);
    }
    ctx->log_file = file;
    return SCRIBE_OK;
}

/*
 * Opens the default repository log. Short-lived generic commands use this file;
 * long-running server and adapter processes switch to role-specific logs.
 */
scribe_error_t scribe_log_open(scribe_ctx *ctx) { return scribe_log_use_file(ctx, "log"); }

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
    char message[768];
    va_list ap;

    if (level < g_min_level) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    emit_log_line(ctx, level, component, "log", message, NULL, NULL, NULL, NULL);
}

void scribe_log_event(scribe_ctx *ctx, scribe_log_level level, const char *component, const char *event,
                      const char *message, const char *commit_hash, const char *operation, const char *path,
                      const char *correlation_id) {
    if (level < g_min_level) {
        return;
    }
    emit_log_line(ctx, level, component, event, message, commit_hash, operation, path, correlation_id);
}

/*
 * Writes an operator-facing event line without the timestamp/level/component
 * prefix. MongoDB change-stream commit summaries use this path because they are
 * closer to command output than diagnostics: the important data is the commit
 * hash, operation, and affected document path, and repeating INFO metadata on
 * every commit makes a busy watch stream harder to scan.
 */
void scribe_log_plain(scribe_ctx *ctx, const char *fmt, ...) {
    char message[768];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    if (g_json_logs) {
        emit_log_line(ctx, SCRIBE_LOG_INFO, "scribe", "plain", message, NULL, NULL, NULL, NULL);
    } else {
        fprintf(stderr, "%s\n", message);
        if (ctx != NULL && ctx->log_file != NULL) {
            fprintf(ctx->log_file, "%s\n", message);
            fflush(ctx->log_file);
        }
    }
}

void scribe_log_plain_event(scribe_ctx *ctx, const char *event, const char *message, const char *commit_hash,
                            const char *operation, const char *path, const char *correlation_id) {
    if (g_json_logs) {
        emit_log_line(ctx, SCRIBE_LOG_INFO, "mongo", event == NULL ? "plain" : event, message, commit_hash, operation,
                      path, correlation_id);
    } else {
        fprintf(stderr, "%s\n", message == NULL ? "" : message);
        if (ctx != NULL && ctx->log_file != NULL) {
            fprintf(ctx->log_file, "%s\n", message == NULL ? "" : message);
            fflush(ctx->log_file);
        }
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
