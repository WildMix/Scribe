/*
 * Scribe - A protocol for Verifiable Data Lineage
 * core/hash.c - SHA-256 hashing implementation using OpenSSL
 */

#include "scribe/core/hash.h"
#include "scribe/types.h"
#include "scribe/error.h"

#include <openssl/sha.h>
#include <openssl/evp.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Zero hash constant */
const scribe_hash_t SCRIBE_ZERO_HASH = {{0}};

bool scribe_hash_is_zero(const scribe_hash_t *hash)
{
    if (!hash) return true;
    return memcmp(hash->bytes, SCRIBE_ZERO_HASH.bytes, SCRIBE_HASH_SIZE) == 0;
}

bool scribe_hash_equal(const scribe_hash_t *a, const scribe_hash_t *b)
{
    if (!a || !b) return false;
    return memcmp(a->bytes, b->bytes, SCRIBE_HASH_SIZE) == 0;
}

void scribe_hash_copy(scribe_hash_t *dst, const scribe_hash_t *src)
{
    if (dst && src) {
        memcpy(dst->bytes, src->bytes, SCRIBE_HASH_SIZE);
    }
}

scribe_error_t scribe_hash_bytes(const void *data, size_t len, scribe_hash_t *out_hash)
{
    if (!out_hash) {
        return SCRIBE_ERR_INVALID_ARG;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return SCRIBE_ERR_CRYPTO;
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        return SCRIBE_ERR_CRYPTO;
    }

    if (data && len > 0) {
        if (EVP_DigestUpdate(ctx, data, len) != 1) {
            EVP_MD_CTX_free(ctx);
            return SCRIBE_ERR_CRYPTO;
        }
    }

    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(ctx, out_hash->bytes, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return SCRIBE_ERR_CRYPTO;
    }

    EVP_MD_CTX_free(ctx);
    return SCRIBE_OK;
}

scribe_error_t scribe_hash_file(const char *path, scribe_hash_t *out_hash)
{
    if (!path || !out_hash) {
        return SCRIBE_ERR_INVALID_ARG;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        scribe_set_error_detail("Failed to open file: %s", path);
        return SCRIBE_ERR_IO;
    }

    scribe_error_t err = scribe_hash_fp(fp, out_hash);
    fclose(fp);
    return err;
}

scribe_error_t scribe_hash_fp(FILE *fp, scribe_hash_t *out_hash)
{
    if (!fp || !out_hash) {
        return SCRIBE_ERR_INVALID_ARG;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return SCRIBE_ERR_CRYPTO;
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        return SCRIBE_ERR_CRYPTO;
    }

    unsigned char buffer[8192];
    size_t bytes_read;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        if (EVP_DigestUpdate(ctx, buffer, bytes_read) != 1) {
            EVP_MD_CTX_free(ctx);
            return SCRIBE_ERR_CRYPTO;
        }
    }

    if (ferror(fp)) {
        EVP_MD_CTX_free(ctx);
        return SCRIBE_ERR_IO;
    }

    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(ctx, out_hash->bytes, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return SCRIBE_ERR_CRYPTO;
    }

    EVP_MD_CTX_free(ctx);
    return SCRIBE_OK;
}

void scribe_hash_to_hex(const scribe_hash_t *hash, char *out_hex)
{
    if (!hash || !out_hex) return;

    static const char hex_chars[] = "0123456789abcdef";

    for (size_t i = 0; i < SCRIBE_HASH_SIZE; i++) {
        out_hex[i * 2]     = hex_chars[(hash->bytes[i] >> 4) & 0x0F];
        out_hex[i * 2 + 1] = hex_chars[hash->bytes[i] & 0x0F];
    }
    out_hex[SCRIBE_HASH_HEX_SIZE - 1] = '\0';
}

scribe_error_t scribe_hash_from_hex(const char *hex, scribe_hash_t *out_hash)
{
    if (!hex || !out_hash) {
        return SCRIBE_ERR_INVALID_ARG;
    }

    size_t len = strlen(hex);
    if (len != SCRIBE_HASH_HEX_SIZE - 1) {
        return SCRIBE_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < SCRIBE_HASH_SIZE; i++) {
        char high = hex[i * 2];
        char low = hex[i * 2 + 1];

        if (!isxdigit(high) || !isxdigit(low)) {
            return SCRIBE_ERR_INVALID_ARG;
        }

        uint8_t high_val = (high >= 'a') ? (high - 'a' + 10) :
                          (high >= 'A') ? (high - 'A' + 10) : (high - '0');
        uint8_t low_val = (low >= 'a') ? (low - 'a' + 10) :
                         (low >= 'A') ? (low - 'A' + 10) : (low - '0');

        out_hash->bytes[i] = (high_val << 4) | low_val;
    }

    return SCRIBE_OK;
}

scribe_error_t scribe_hash_combine(const scribe_hash_t *left,
                                   const scribe_hash_t *right,
                                   scribe_hash_t *out_hash)
{
    if (!left || !right || !out_hash) {
        return SCRIBE_ERR_INVALID_ARG;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return SCRIBE_ERR_CRYPTO;
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        return SCRIBE_ERR_CRYPTO;
    }

    /* Prefix with 0x01 for internal nodes (prevents second-preimage attacks) */
    unsigned char prefix = 0x01;
    if (EVP_DigestUpdate(ctx, &prefix, 1) != 1) {
        EVP_MD_CTX_free(ctx);
        return SCRIBE_ERR_CRYPTO;
    }

    if (EVP_DigestUpdate(ctx, left->bytes, SCRIBE_HASH_SIZE) != 1) {
        EVP_MD_CTX_free(ctx);
        return SCRIBE_ERR_CRYPTO;
    }

    if (EVP_DigestUpdate(ctx, right->bytes, SCRIBE_HASH_SIZE) != 1) {
        EVP_MD_CTX_free(ctx);
        return SCRIBE_ERR_CRYPTO;
    }

    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(ctx, out_hash->bytes, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return SCRIBE_ERR_CRYPTO;
    }

    EVP_MD_CTX_free(ctx);
    return SCRIBE_OK;
}

scribe_error_t scribe_hash_leaf(const void *data, size_t len, scribe_hash_t *out_hash)
{
    if (!out_hash) {
        return SCRIBE_ERR_INVALID_ARG;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return SCRIBE_ERR_CRYPTO;
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        return SCRIBE_ERR_CRYPTO;
    }

    /* Prefix with 0x00 for leaf nodes */
    unsigned char prefix = 0x00;
    if (EVP_DigestUpdate(ctx, &prefix, 1) != 1) {
        EVP_MD_CTX_free(ctx);
        return SCRIBE_ERR_CRYPTO;
    }

    if (data && len > 0) {
        if (EVP_DigestUpdate(ctx, data, len) != 1) {
            EVP_MD_CTX_free(ctx);
            return SCRIBE_ERR_CRYPTO;
        }
    }

    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(ctx, out_hash->bytes, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return SCRIBE_ERR_CRYPTO;
    }

    EVP_MD_CTX_free(ctx);
    return SCRIBE_OK;
}
