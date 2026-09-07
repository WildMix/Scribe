/*
 * Minimal NDJSON line-event writer/parser.
 *
 * Query streams use one object per line:
 *   {"line":"text without trailing newline"}
 * The helper is deliberately small and strict; Scribe does not need a general
 * JSON library for this protocol surface.
 */
#include "core/internal.h"

#include "util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} ndjson_buf;

static scribe_error_t ndjson_buf_append(ndjson_buf *buf, const char *data, size_t len) {
    char *grown;
    size_t new_cap;

    if (len == 0u) {
        return SCRIBE_OK;
    }
    if (buf->len > SIZE_MAX - len - 1u) {
        return scribe_set_error(SCRIBE_ENOMEM, "NDJSON buffer is too large");
    }
    if (buf->len + len + 1u <= buf->cap) {
        memcpy(buf->data + buf->len, data, len);
        buf->len += len;
        buf->data[buf->len] = '\0';
        return SCRIBE_OK;
    }
    new_cap = buf->cap == 0u ? 128u : buf->cap;
    while (new_cap < buf->len + len + 1u) {
        if (new_cap > SIZE_MAX / 2u) {
            return scribe_set_error(SCRIBE_ENOMEM, "NDJSON buffer is too large");
        }
        new_cap *= 2u;
    }
    grown = (char *)realloc(buf->data, new_cap);
    if (grown == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to grow NDJSON buffer");
    }
    buf->data = grown;
    buf->cap = new_cap;
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return SCRIBE_OK;
}

static scribe_error_t ndjson_buf_appendf(ndjson_buf *buf, const char *fmt, unsigned int value) {
    char tmp[16];
    int n = snprintf(tmp, sizeof(tmp), fmt, value);

    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return scribe_set_error(SCRIBE_EIO, "failed to format NDJSON escape");
    }
    return ndjson_buf_append(buf, tmp, (size_t)n);
}

scribe_error_t scribe_ndjson_line_event(const char *text, size_t len, char **out, size_t *out_len) {
    ndjson_buf buf = {0};
    size_t i;
    scribe_error_t err;

    if (out == NULL || out_len == NULL || (text == NULL && len != 0u)) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid NDJSON line event arguments");
    }
    *out = NULL;
    *out_len = 0;
    err = ndjson_buf_append(&buf, "{\"line\":\"", 9u);
    for (i = 0; err == SCRIBE_OK && i < len; i++) {
        unsigned char ch = (unsigned char)text[i];

        if (ch == '"' || ch == '\\') {
            char esc[2];

            esc[0] = '\\';
            esc[1] = (char)ch;
            err = ndjson_buf_append(&buf, esc, sizeof(esc));
        } else if (ch == '\n') {
            err = ndjson_buf_append(&buf, "\\n", 2u);
        } else if (ch == '\r') {
            err = ndjson_buf_append(&buf, "\\r", 2u);
        } else if (ch == '\t') {
            err = ndjson_buf_append(&buf, "\\t", 2u);
        } else if (ch < 0x20u) {
            err = ndjson_buf_appendf(&buf, "\\u%04x", (unsigned int)ch);
        } else {
            char c = (char)ch;

            err = ndjson_buf_append(&buf, &c, 1u);
        }
    }
    if (err == SCRIBE_OK) {
        err = ndjson_buf_append(&buf, "\"}\n", 3u);
    }
    if (err == SCRIBE_OK) {
        *out = buf.data;
        *out_len = buf.len;
        return SCRIBE_OK;
    }
    free(buf.data);
    return err;
}

static int ndjson_hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static scribe_error_t ndjson_append_utf8_codepoint(ndjson_buf *buf, unsigned int cp) {
    char bytes[4];

    if (cp <= 0x7fu) {
        bytes[0] = (char)cp;
        return ndjson_buf_append(buf, bytes, 1u);
    }
    if (cp <= 0x7ffu) {
        bytes[0] = (char)(0xc0u | (cp >> 6u));
        bytes[1] = (char)(0x80u | (cp & 0x3fu));
        return ndjson_buf_append(buf, bytes, 2u);
    }
    bytes[0] = (char)(0xe0u | (cp >> 12u));
    bytes[1] = (char)(0x80u | ((cp >> 6u) & 0x3fu));
    bytes[2] = (char)(0x80u | (cp & 0x3fu));
    return ndjson_buf_append(buf, bytes, 3u);
}

scribe_error_t scribe_ndjson_parse_line_event(const char *line, size_t len, char **out, size_t *out_len) {
    static const char prefix[] = "{\"line\":\"";
    ndjson_buf buf = {0};
    size_t pos = sizeof(prefix) - 1u;
    scribe_error_t err = SCRIBE_OK;

    if (line == NULL || out == NULL || out_len == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid NDJSON parse arguments");
    }
    *out = NULL;
    *out_len = 0;
    if (len != 0u && line[len - 1u] == '\n') {
        len--;
    }
    if (len < sizeof(prefix) || memcmp(line, prefix, sizeof(prefix) - 1u) != 0) {
        return scribe_set_error(SCRIBE_EPROTOCOL, "malformed NDJSON line event");
    }
    while (pos < len) {
        char ch = line[pos++];

        if (ch == '"') {
            if (pos + 1u != len || line[pos] != '}') {
                err = scribe_set_error(SCRIBE_EPROTOCOL, "malformed NDJSON line event trailer");
                break;
            }
            *out = buf.data;
            *out_len = buf.len;
            return SCRIBE_OK;
        }
        if (ch != '\\') {
            err = ndjson_buf_append(&buf, &ch, 1u);
        } else {
            char esc;

            if (pos >= len) {
                err = scribe_set_error(SCRIBE_EPROTOCOL, "truncated NDJSON escape");
                break;
            }
            esc = line[pos++];
            if (esc == '"' || esc == '\\') {
                err = ndjson_buf_append(&buf, &esc, 1u);
            } else if (esc == 'n') {
                err = ndjson_buf_append(&buf, "\n", 1u);
            } else if (esc == 'r') {
                err = ndjson_buf_append(&buf, "\r", 1u);
            } else if (esc == 't') {
                err = ndjson_buf_append(&buf, "\t", 1u);
            } else if (esc == 'u') {
                int h0;
                int h1;
                int h2;
                int h3;
                unsigned int cp;

                if (pos + 4u > len) {
                    err = scribe_set_error(SCRIBE_EPROTOCOL, "truncated NDJSON unicode escape");
                    break;
                }
                h0 = ndjson_hex_value(line[pos]);
                h1 = ndjson_hex_value(line[pos + 1u]);
                h2 = ndjson_hex_value(line[pos + 2u]);
                h3 = ndjson_hex_value(line[pos + 3u]);
                if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
                    err = scribe_set_error(SCRIBE_EPROTOCOL, "invalid NDJSON unicode escape");
                    break;
                }
                cp = (unsigned int)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                err = ndjson_append_utf8_codepoint(&buf, cp);
                pos += 4u;
            } else {
                err = scribe_set_error(SCRIBE_EPROTOCOL, "invalid NDJSON escape");
            }
        }
        if (err != SCRIBE_OK) {
            break;
        }
    }
    free(buf.data);
    return err == SCRIBE_OK ? scribe_set_error(SCRIBE_EPROTOCOL, "unterminated NDJSON line event") : err;
}
