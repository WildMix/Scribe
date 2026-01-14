/*
 * Scribe - A protocol for Verifiable Data Lineage
 * util/log.c - Logging utilities
 */

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

/* Log levels */
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3
} scribe_log_level_t;

/* Current log level (can be set via environment or API) */
static scribe_log_level_t current_level = LOG_INFO;

/* ANSI color codes */
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_GRAY    "\033[90m"

static int use_colors = 1;

void scribe_log_set_level(scribe_log_level_t level)
{
    current_level = level;
}

void scribe_log_set_colors(int enable)
{
    use_colors = enable;
}

static const char *level_string(scribe_log_level_t level)
{
    switch (level) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        case LOG_ERROR: return "ERROR";
        default:        return "?????";
    }
}

static const char *level_color(scribe_log_level_t level)
{
    if (!use_colors) return "";
    switch (level) {
        case LOG_DEBUG: return COLOR_GRAY;
        case LOG_INFO:  return COLOR_GREEN;
        case LOG_WARN:  return COLOR_YELLOW;
        case LOG_ERROR: return COLOR_RED;
        default:        return "";
    }
}

static void scribe_log_impl(scribe_log_level_t level, const char *fmt, va_list args)
{
    if (level < current_level) return;

    FILE *out = (level >= LOG_WARN) ? stderr : stdout;

    /* Timestamp */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[20];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    /* Print log line */
    fprintf(out, "%s[%s]%s %s%s%s: ",
            use_colors ? COLOR_GRAY : "",
            time_buf,
            use_colors ? COLOR_RESET : "",
            level_color(level),
            level_string(level),
            use_colors ? COLOR_RESET : "");

    vfprintf(out, fmt, args);
    fprintf(out, "\n");
    fflush(out);
}

void scribe_log_debug(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    scribe_log_impl(LOG_DEBUG, fmt, args);
    va_end(args);
}

void scribe_log_info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    scribe_log_impl(LOG_INFO, fmt, args);
    va_end(args);
}

void scribe_log_warn(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    scribe_log_impl(LOG_WARN, fmt, args);
    va_end(args);
}

void scribe_log_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    scribe_log_impl(LOG_ERROR, fmt, args);
    va_end(args);
}

/* Simple output without timestamp (for CLI output) */
void scribe_print(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

void scribe_print_error(const char *fmt, ...)
{
    if (use_colors) fprintf(stderr, COLOR_RED);
    fprintf(stderr, "error: ");
    if (use_colors) fprintf(stderr, COLOR_RESET);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}
