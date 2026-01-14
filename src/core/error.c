/*
 * Scribe - A protocol for Verifiable Data Lineage
 * core/error.c - Error handling implementation
 */

#include "scribe/error.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* Thread-local error detail buffer */
static __thread char error_detail[1024] = {0};

const char *scribe_error_string(scribe_error_t err)
{
    switch (err) {
        case SCRIBE_OK:
            return "Success";
        case SCRIBE_ERR_NOMEM:
            return "Out of memory";
        case SCRIBE_ERR_INVALID_ARG:
            return "Invalid argument";
        case SCRIBE_ERR_NOT_FOUND:
            return "Resource not found";
        case SCRIBE_ERR_NOT_A_REPO:
            return "Not a scribe repository";
        case SCRIBE_ERR_REPO_EXISTS:
            return "Repository already exists";
        case SCRIBE_ERR_REPO_CORRUPT:
            return "Repository is corrupted";
        case SCRIBE_ERR_IO:
            return "I/O error";
        case SCRIBE_ERR_DB:
            return "Database error";
        case SCRIBE_ERR_OBJECT_MISSING:
            return "Object not found in store";
        case SCRIBE_ERR_HASH_MISMATCH:
            return "Hash verification failed";
        case SCRIBE_ERR_CRYPTO:
            return "Cryptographic operation failed";
        case SCRIBE_ERR_PG_CONNECT:
            return "PostgreSQL connection failed";
        case SCRIBE_ERR_PG_QUERY:
            return "PostgreSQL query failed";
        case SCRIBE_ERR_PG_REPLICATION:
            return "PostgreSQL replication error";
        case SCRIBE_ERR_JSON_PARSE:
            return "JSON parsing failed";
        case SCRIBE_ERR_JSON_SCHEMA:
            return "JSON schema validation failed";
        default:
            return "Unknown error";
    }
}

void scribe_set_error_detail(const char *fmt, ...)
{
    if (!fmt) {
        error_detail[0] = '\0';
        return;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(error_detail, sizeof(error_detail), fmt, args);
    va_end(args);
}

const char *scribe_get_error_detail(void)
{
    return error_detail[0] ? error_detail : NULL;
}

void scribe_clear_error_detail(void)
{
    error_detail[0] = '\0';
}
