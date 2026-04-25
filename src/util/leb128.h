#ifndef SCRIBE_UTIL_LEB128_H
#define SCRIBE_UTIL_LEB128_H

#include "scribe/scribe.h"

#include <stddef.h>
#include <stdint.h>

size_t scribe_leb128_encode(uint64_t value, uint8_t out[10]);
scribe_error_t scribe_leb128_decode(const uint8_t *bytes, size_t len, uint64_t *out_value, size_t *out_used);

#endif
