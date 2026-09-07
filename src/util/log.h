#ifndef SCRIBE_UTIL_LOG_H
#define SCRIBE_UTIL_LOG_H

#include "scribe/scribe.h"

typedef enum {
    SCRIBE_LOG_DEBUG = 0,
    SCRIBE_LOG_INFO = 1,
    SCRIBE_LOG_WARN = 2,
    SCRIBE_LOG_ERROR = 3,
} scribe_log_level;

scribe_error_t scribe_log_configure_from_env(void);
scribe_error_t scribe_log_set_format(const char *format);
scribe_error_t scribe_log_open(scribe_ctx *ctx);
scribe_error_t scribe_log_use_file(scribe_ctx *ctx, const char *filename);
void scribe_log_close(scribe_ctx *ctx);
void scribe_log_msg(scribe_ctx *ctx, scribe_log_level level, const char *component, const char *fmt, ...);
void scribe_log_event(scribe_ctx *ctx, scribe_log_level level, const char *component, const char *event,
                      const char *message, const char *commit_hash, const char *operation, const char *path,
                      const char *correlation_id);
void scribe_log_plain(scribe_ctx *ctx, const char *fmt, ...);
void scribe_log_plain_event(scribe_ctx *ctx, const char *event, const char *message, const char *commit_hash,
                            const char *operation, const char *path, const char *correlation_id);
void scribe_log_flush(scribe_ctx *ctx);

#endif
