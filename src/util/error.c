#include "util/error.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static _Thread_local char g_error_detail[512];

const char *scribe_last_error_detail(void) { return g_error_detail[0] == '\0' ? "" : g_error_detail; }

void scribe_clear_error(void) { g_error_detail[0] = '\0'; }

scribe_error_t scribe_set_error(scribe_error_t err, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    if (fmt != NULL) {
        (void)vsnprintf(g_error_detail, sizeof(g_error_detail), fmt, ap);
    } else {
        g_error_detail[0] = '\0';
    }
    va_end(ap);
    return err;
}

const char *scribe_error_symbol(scribe_error_t err) {
    switch (err) {
    case SCRIBE_OK:
        return "SCRIBE_OK";
    case SCRIBE_ERR:
        return "SCRIBE_ERR";
    case SCRIBE_ENOT_FOUND:
        return "SCRIBE_ENOT_FOUND";
    case SCRIBE_EEXISTS:
        return "SCRIBE_EEXISTS";
    case SCRIBE_ELOCKED:
        return "SCRIBE_ELOCKED";
    case SCRIBE_EREF_STALE:
        return "SCRIBE_EREF_STALE";
    case SCRIBE_EIO:
        return "SCRIBE_EIO";
    case SCRIBE_ECORRUPT:
        return "SCRIBE_ECORRUPT";
    case SCRIBE_EINVAL:
        return "SCRIBE_EINVAL";
    case SCRIBE_EMALFORMED:
        return "SCRIBE_EMALFORMED";
    case SCRIBE_EPROTOCOL:
        return "SCRIBE_EPROTOCOL";
    case SCRIBE_ECONFIG:
        return "SCRIBE_ECONFIG";
    case SCRIBE_EADAPTER:
        return "SCRIBE_EADAPTER";
    case SCRIBE_ENOMEM:
        return "SCRIBE_ENOMEM";
    case SCRIBE_ENOSYS:
        return "SCRIBE_ENOSYS";
    case SCRIBE_EINTERRUPTED:
        return "SCRIBE_EINTERRUPTED";
    case SCRIBE_EPATH:
        return "SCRIBE_EPATH";
    case SCRIBE_EHASH:
        return "SCRIBE_EHASH";
    case SCRIBE_ECONCURRENT:
        return "SCRIBE_ECONCURRENT";
    case SCRIBE_ESHUTDOWN:
        return "SCRIBE_ESHUTDOWN";
    default:
        return "SCRIBE_UNKNOWN";
    }
}
