/*
 * Scribe - A protocol for Verifiable Data Lineage
 * types.h - Core type definitions
 */

#ifndef SCRIBE_TYPES_H
#define SCRIBE_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* SHA-256 hash constants */
#define SCRIBE_HASH_SIZE 32
#define SCRIBE_HASH_HEX_SIZE 65  /* 64 hex chars + null terminator */

/* SHA-256 hash type */
typedef struct {
    uint8_t bytes[SCRIBE_HASH_SIZE];
} scribe_hash_t;

/* Object types in the Merkle-DAG */
typedef enum {
    SCRIBE_OBJ_BLOB,      /* Raw data blob */
    SCRIBE_OBJ_TREE,      /* Directory/collection of blobs */
    SCRIBE_OBJ_COMMIT     /* Commit envelope */
} scribe_object_type_t;

/* Generic object header */
typedef struct {
    scribe_object_type_t type;
    scribe_hash_t hash;
    size_t size;
} scribe_object_t;

/* Zero hash constant (for root commits with no parent) */
extern const scribe_hash_t SCRIBE_ZERO_HASH;

/* Check if a hash is the zero hash */
bool scribe_hash_is_zero(const scribe_hash_t *hash);

/* Compare two hashes for equality */
bool scribe_hash_equal(const scribe_hash_t *a, const scribe_hash_t *b);

/* Copy a hash */
void scribe_hash_copy(scribe_hash_t *dst, const scribe_hash_t *src);

#endif /* SCRIBE_TYPES_H */
