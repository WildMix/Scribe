/*
 * Scribe - A protocol for Verifiable Data Lineage
 * scribe.h - Main API header
 */

#ifndef SCRIBE_H
#define SCRIBE_H

#include "scribe/types.h"
#include "scribe/error.h"
#include "scribe/core/hash.h"
#include "scribe/core/merkle.h"
#include "scribe/core/envelope.h"
#include "scribe/storage/repository.h"
#include "scribe/storage/database.h"

/* Version information */
#define SCRIBE_VERSION_MAJOR 0
#define SCRIBE_VERSION_MINOR 1
#define SCRIBE_VERSION_PATCH 0
#define SCRIBE_VERSION_STRING "0.1.0"

/* Initialize scribe library */
scribe_error_t scribe_init(void);

/* Cleanup scribe library */
void scribe_cleanup(void);

#endif /* SCRIBE_H */
