/*
 * Unsigned LEB128 integer encoding helpers.
 *
 * Scribe uses LEB128 inside object envelopes and tree payloads to store lengths
 * compactly while still allowing deterministic binary parsing. The decoder is
 * intentionally strict because corrupted length fields can otherwise make later
 * parsers read past the end of an object payload.
 */
#include "util/leb128.h"

#include "util/error.h"

/*
 * Encodes a uint64_t into unsigned LEB128 form. The caller provides the
 * 10-byte maximum buffer, and the function returns the number of bytes used.
 */
size_t scribe_leb128_encode(uint64_t value, uint8_t out[10]) {
    size_t n = 0;

    while (value > 0x7fu) {
        out[n++] = (uint8_t)((value & 0x7fu) | 0x80u);
        value >>= 7u;
    }
    out[n++] = (uint8_t)(value & 0x7fu);
    return n;
}

/*
 * Decodes one unsigned LEB128 integer from a byte buffer. It reports both the
 * decoded value and the number of bytes consumed so callers can continue
 * parsing the remaining payload exactly where the length field ended.
 */
scribe_error_t scribe_leb128_decode(const uint8_t *bytes, size_t len, uint64_t *out_value, size_t *out_used) {
    uint64_t value = 0;
    unsigned shift = 0;
    size_t i;

    if (bytes == NULL || out_value == NULL || out_used == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid LEB128 argument");
    }
    for (i = 0; i < len && i < 10u; i++) {
        uint8_t b = bytes[i];
        value |= ((uint64_t)(b & 0x7fu)) << shift;
        if ((b & 0x80u) == 0) {
            *out_value = value;
            *out_used = i + 1u;
            return SCRIBE_OK;
        }
        shift += 7u;
    }
    if (i >= 10u) {
        return scribe_set_error(SCRIBE_ECORRUPT, "LEB128 encoding exceeds 10 bytes");
    }
    return scribe_set_error(SCRIBE_ECORRUPT, "truncated LEB128 encoding");
}
