#ifndef SCRIBE_ADAPTER_MONGO_H
#define SCRIBE_ADAPTER_MONGO_H

#include "scribe/scribe.h"

scribe_error_t scribe_mongo_watch_bootstrap(scribe_ctx *ctx, const char *uri);

#endif
