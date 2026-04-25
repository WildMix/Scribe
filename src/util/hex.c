#include "util/hex.h"

#include "util/error.h"

#include <string.h>

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

void scribe_hash_to_hex(const uint8_t hash[SCRIBE_HASH_SIZE], char out[SCRIBE_HEX_HASH_SIZE + 1]) {
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < SCRIBE_HASH_SIZE; i++) {
        out[i * 2u] = digits[hash[i] >> 4u];
        out[i * 2u + 1u] = digits[hash[i] & 0x0fu];
    }
    out[SCRIBE_HEX_HASH_SIZE] = '\0';
}

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

int scribe_hash_cmp(const uint8_t a[SCRIBE_HASH_SIZE], const uint8_t b[SCRIBE_HASH_SIZE]) {
    return memcmp(a, b, SCRIBE_HASH_SIZE);
}

void scribe_hash_copy(uint8_t dst[SCRIBE_HASH_SIZE], const uint8_t src[SCRIBE_HASH_SIZE]) {
    memcpy(dst, src, SCRIBE_HASH_SIZE);
}

int scribe_hash_is_zero(const uint8_t hash[SCRIBE_HASH_SIZE]) {
    uint8_t acc = 0;
    size_t i;

    for (i = 0; i < SCRIBE_HASH_SIZE; i++) {
        acc |= hash[i];
    }
    return acc == 0;
}
