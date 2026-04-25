#ifndef SCRIBE_UTIL_ERROR_H
#define SCRIBE_UTIL_ERROR_H

#include "scribe/scribe.h"

scribe_error_t scribe_set_error(scribe_error_t err, const char *fmt, ...);
void scribe_clear_error(void);

#endif
