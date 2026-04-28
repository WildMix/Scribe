/*
 * Hex and hash byte helpers.
 *
 * Scribe stores BLAKE3 hashes as 32 raw bytes internally and as 64 lowercase
 * hexadecimal characters in refs, command output, and object paths. This file
 * keeps that conversion strict so callers never accidentally accept abbreviated,
 * uppercase, or malformed hashes.
 */
#include "util/hex.h"

#include "util/error.h"

#include <string.h>

/*
 * Converts one lowercase hexadecimal character into its numeric nibble value.
 * Returning -1 lets the parser give one consistent SCRIBE_EHASH error for any
 * non-lowercase-hex input.
 */
static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/*
 * Renders a 32-byte hash into the canonical 64-character lowercase hex form.
 * The caller supplies the output buffer, including room for the terminating
 * NUL, so this function does not allocate and can be used in hot paths.
 */
void scribe_hash_to_hex(const uint8_t hash[SCRIBE_HASH_SIZE], char out[SCRIBE_HEX_HASH_SIZE + 1]) {
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < SCRIBE_HASH_SIZE; i++) {
        out[i * 2u] = digits[hash[i] >> 4u];
        out[i * 2u + 1u] = digits[hash[i] & 0x0fu];
    }
    out[SCRIBE_HEX_HASH_SIZE] = '\0';
}

/*
 * Parses the canonical 64-character lowercase hex form back into 32 hash bytes.
 * The length and character set checks are intentionally strict because refs and
 * CLI arguments should never accept ambiguous or shortened object names in v1.
 */
scribe_error_t scribe_hash_from_hex(const char *hex, uint8_t out[SCRIBE_HASH_SIZE]) {
    size_t i;

    if (hex == NULL || out == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid hash argument");
    }
    if (strlen(hex) != SCRIBE_HEX_HASH_SIZE) {
        return scribe_set_error(SCRIBE_EHASH, "hash must be exactly 64 lowercase hex characters");
    }
    for (i = 0; i < SCRIBE_HASH_SIZE; i++) {
        int hi = hex_value(hex[i * 2u]);
        int lo = hex_value(hex[i * 2u + 1u]);
        if (hi < 0 || lo < 0) {
            return scribe_set_error(SCRIBE_EHASH, "hash must be lowercase hex");
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return SCRIBE_OK;
}

/*
 * Compares two raw hashes byte-for-byte using memcmp semantics. A zero return
 * means equality; callers that only need equality can compare the result to 0.
 */
int scribe_hash_cmp(const uint8_t a[SCRIBE_HASH_SIZE], const uint8_t b[SCRIBE_HASH_SIZE]) {
    return memcmp(a, b, SCRIBE_HASH_SIZE);
}

/*
 * Copies a raw hash value between fixed-size buffers. Keeping this helper avoids
 * repeating SCRIBE_HASH_SIZE in code that is otherwise about object semantics.
 */
void scribe_hash_copy(uint8_t dst[SCRIBE_HASH_SIZE], const uint8_t src[SCRIBE_HASH_SIZE]) {
    memcpy(dst, src, SCRIBE_HASH_SIZE);
}

/*
 * Returns whether every byte in a hash buffer is zero. This is mainly useful
 * for defensive checks around optional hashes, not for normal object identity.
 */
int scribe_hash_is_zero(const uint8_t hash[SCRIBE_HASH_SIZE]) {
    uint8_t acc = 0;
    size_t i;

    for (i = 0; i < SCRIBE_HASH_SIZE; i++) {
        acc |= hash[i];
    }
    return acc == 0;
}
