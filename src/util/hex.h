#ifndef SCRIBE_UTIL_HEX_H
#define SCRIBE_UTIL_HEX_H

#include "scribe/scribe.h"

void scribe_hash_to_hex(const uint8_t hash[SCRIBE_HASH_SIZE], char out[SCRIBE_HEX_HASH_SIZE + 1]);
scribe_error_t scribe_hash_from_hex(const char *hex, uint8_t out[SCRIBE_HASH_SIZE]);
int scribe_hash_cmp(const uint8_t a[SCRIBE_HASH_SIZE], const uint8_t b[SCRIBE_HASH_SIZE]);
void scribe_hash_copy(uint8_t dst[SCRIBE_HASH_SIZE], const uint8_t src[SCRIBE_HASH_SIZE]);
int scribe_hash_is_zero(const uint8_t hash[SCRIBE_HASH_SIZE]);

#endif
