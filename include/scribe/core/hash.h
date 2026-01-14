/*
 * Scribe - A protocol for Verifiable Data Lineage
 * core/hash.h - SHA-256 hashing interface
 */

#ifndef SCRIBE_CORE_HASH_H
#define SCRIBE_CORE_HASH_H

#include "scribe/types.h"
#include "scribe/error.h"
#include <stdio.h>

/*
 * Hash raw bytes
 *
 * @param data      Input data to hash
 * @param len       Length of input data
 * @param out_hash  Output hash
 * @return          SCRIBE_OK on success, error code on failure
 */
scribe_error_t scribe_hash_bytes(const void *data, size_t len, scribe_hash_t *out_hash);

/*
 * Hash a file's contents
 *
 * @param path      Path to the file
 * @param out_hash  Output hash
 * @return          SCRIBE_OK on success, error code on failure
 */
scribe_error_t scribe_hash_file(const char *path, scribe_hash_t *out_hash);

/*
 * Hash a file handle's contents
 *
 * @param fp        File pointer (read from current position to EOF)
 * @param out_hash  Output hash
 * @return          SCRIBE_OK on success, error code on failure
 */
scribe_error_t scribe_hash_fp(FILE *fp, scribe_hash_t *out_hash);

/*
 * Convert hash to hex string
 *
 * @param hash      Input hash
 * @param out_hex   Output buffer (must be at least SCRIBE_HASH_HEX_SIZE bytes)
 */
void scribe_hash_to_hex(const scribe_hash_t *hash, char *out_hex);

/*
 * Parse hex string to hash
 *
 * @param hex       Input hex string (64 characters)
 * @param out_hash  Output hash
 * @return          SCRIBE_OK on success, SCRIBE_ERR_INVALID_ARG on invalid input
 */
scribe_error_t scribe_hash_from_hex(const char *hex, scribe_hash_t *out_hash);

/*
 * Combine two hashes (for Merkle tree internal nodes)
 * Result is: SHA256(0x01 || left || right)
 *
 * @param left      Left child hash
 * @param right     Right child hash
 * @param out_hash  Output combined hash
 * @return          SCRIBE_OK on success, error code on failure
 */
scribe_error_t scribe_hash_combine(const scribe_hash_t *left,
                                   const scribe_hash_t *right,
                                   scribe_hash_t *out_hash);

/*
 * Create a leaf hash (for Merkle tree leaf nodes)
 * Result is: SHA256(0x00 || data)
 *
 * @param data      Input data
 * @param len       Length of input data
 * @param out_hash  Output leaf hash
 * @return          SCRIBE_OK on success, error code on failure
 */
scribe_error_t scribe_hash_leaf(const void *data, size_t len, scribe_hash_t *out_hash);

#endif /* SCRIBE_CORE_HASH_H */
