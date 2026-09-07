/*
 * Open-addressed hash set for 32-byte object ids.
 *
 * Maintenance and inspection paths use this for reachability and duplicate
 * tracking. It keeps insertion and membership expected O(1) instead of scanning
 * linear arrays as repositories grow.
 */
#include "core/internal.h"

#include "util/error.h"

#include <stdlib.h>
#include <string.h>

static uint64_t hash_key(const uint8_t hash[SCRIBE_HASH_SIZE]) {
    uint64_t h = UINT64_C(1469598103934665603);
    size_t i;

    for (i = 0; i < SCRIBE_HASH_SIZE; i++) {
        h ^= (uint64_t)hash[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static scribe_error_t hash_set_insert_rehashed(scribe_hash_set *set, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    size_t mask = set->cap - 1u;
    size_t pos = (size_t)(hash_key(hash) & (uint64_t)mask);

    while (set->used[pos]) {
        pos = (pos + 1u) & mask;
    }
    memcpy(set->hashes + pos * SCRIBE_HASH_SIZE, hash, SCRIBE_HASH_SIZE);
    set->used[pos] = 1u;
    set->count++;
    return SCRIBE_OK;
}

static scribe_error_t hash_set_grow(scribe_hash_set *set) {
    scribe_hash_set grown;
    size_t new_cap;
    size_t i;

    new_cap = set->cap == 0u ? 256u : set->cap * 2u;
    if (new_cap < set->cap || new_cap > SIZE_MAX / SCRIBE_HASH_SIZE) {
        return scribe_set_error(SCRIBE_ENOMEM, "object hash set is too large");
    }
    memset(&grown, 0, sizeof(grown));
    grown.hashes = (uint8_t *)calloc(new_cap, SCRIBE_HASH_SIZE);
    grown.used = (uint8_t *)calloc(new_cap, 1u);
    if (grown.hashes == NULL || grown.used == NULL) {
        free(grown.hashes);
        free(grown.used);
        return scribe_set_error(SCRIBE_ENOMEM, "failed to grow object hash set");
    }
    grown.cap = new_cap;
    for (i = 0; i < set->cap; i++) {
        if (set->used[i]) {
            scribe_error_t err = hash_set_insert_rehashed(&grown, set->hashes + i * SCRIBE_HASH_SIZE);
            if (err != SCRIBE_OK) {
                scribe_hash_set_destroy(&grown);
                return err;
            }
        }
    }
    free(set->hashes);
    free(set->used);
    *set = grown;
    return SCRIBE_OK;
}

int scribe_hash_set_has(const scribe_hash_set *set, const uint8_t hash[SCRIBE_HASH_SIZE]) {
    size_t mask;
    size_t pos;

    if (set == NULL || set->cap == 0u) {
        return 0;
    }
    mask = set->cap - 1u;
    pos = (size_t)(hash_key(hash) & (uint64_t)mask);
    while (set->used[pos]) {
        if (memcmp(set->hashes + pos * SCRIBE_HASH_SIZE, hash, SCRIBE_HASH_SIZE) == 0) {
            return 1;
        }
        pos = (pos + 1u) & mask;
    }
    return 0;
}

scribe_error_t scribe_hash_set_add(scribe_hash_set *set, const uint8_t hash[SCRIBE_HASH_SIZE], int *already) {
    scribe_error_t err;

    if (set == NULL || hash == NULL || already == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid object hash set arguments");
    }
    *already = scribe_hash_set_has(set, hash);
    if (*already) {
        return SCRIBE_OK;
    }
    if (set->cap == 0u || (set->count + 1u) * 10u >= set->cap * 7u) {
        err = hash_set_grow(set);
        if (err != SCRIBE_OK) {
            return err;
        }
    }
    return hash_set_insert_rehashed(set, hash);
}

void scribe_hash_set_destroy(scribe_hash_set *set) {
    if (set != NULL) {
        free(set->hashes);
        free(set->used);
        memset(set, 0, sizeof(*set));
    }
}
