/*
 * Canonical tree object serialization and parsing.
 *
 * Tree objects map entry names to child object hashes. The serialized payload is
 * byte-sorted by entry name and rejects duplicates, which makes identical
 * logical directory trees hash identically and lets diff/log use deterministic
 * merge walks.
 */
#include "core/internal.h"

#include "util/error.h"
#include "util/leb128.h"

#include <stdlib.h>
#include <string.h>

/*
 * Compares two tree entries by raw name bytes, then by length when one name is
 * a prefix of the other. This defines the canonical storage order for trees.
 */
static int entry_cmp(const void *a, const void *b) {
    const scribe_tree_entry *ea = (const scribe_tree_entry *)a;
    const scribe_tree_entry *eb = (const scribe_tree_entry *)b;
    size_t min = ea->name_len < eb->name_len ? ea->name_len : eb->name_len;
    int cmp = memcmp(ea->name, eb->name, min);
    if (cmp != 0) {
        return cmp;
    }
    if (ea->name_len < eb->name_len) {
        return -1;
    }
    if (ea->name_len > eb->name_len) {
        return 1;
    }
    return 0;
}

/*
 * Serializes an entry array into the canonical tree payload. The caller may
 * provide entries in any order; this function sorts a copy, validates names and
 * types, rejects duplicates, and writes the compact binary entry sequence.
 */
scribe_error_t scribe_tree_serialize(const scribe_tree_entry *entries, size_t count, scribe_arena *arena, uint8_t **out,
                                     size_t *out_len) {
    scribe_tree_entry *sorted;
    size_t i;
    size_t len = 0;
    uint8_t *buf;
    size_t off = 0;

    if (out == NULL || out_len == NULL || arena == NULL || (count != 0 && entries == NULL)) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid tree serialization");
    }
    /*
     * Tree objects are canonical: callers may provide entries in any order, but
     * the serialized payload is sorted byte-for-byte by name and rejects
     * duplicates. This makes identical logical trees hash identically and keeps
     * diff/log walks deterministic.
     */
    sorted = (scribe_tree_entry *)scribe_arena_alloc(arena, sizeof(*sorted) * (count == 0 ? 1u : count),
                                                     _Alignof(scribe_tree_entry));
    if (sorted == NULL) {
        return SCRIBE_ENOMEM;
    }
    if (count != 0) {
        memcpy(sorted, entries, sizeof(*sorted) * count);
        qsort(sorted, count, sizeof(*sorted), entry_cmp);
    }
    for (i = 0; i < count; i++) {
        uint8_t leb[10];
        size_t leb_len;
        if ((sorted[i].type != SCRIBE_OBJECT_BLOB && sorted[i].type != SCRIBE_OBJECT_TREE) || sorted[i].name == NULL ||
            sorted[i].name_len == 0) {
            return scribe_set_error(SCRIBE_EINVAL, "invalid tree entry");
        }
        if (i > 0 && entry_cmp(&sorted[i - 1u], &sorted[i]) == 0) {
            return scribe_set_error(SCRIBE_EINVAL, "duplicate tree entry name");
        }
        /*
         * Entry payload format:
         *   type byte, child hash, unsigned LEB128 name length, name bytes.
         * Names are not NUL-terminated on disk; parse creates arena-owned
         * NUL-terminated copies for C convenience.
         */
        leb_len = scribe_leb128_encode((uint64_t)sorted[i].name_len, leb);
        len += 1u + SCRIBE_HASH_SIZE + leb_len + sorted[i].name_len;
    }
    buf = (uint8_t *)scribe_arena_alloc(arena, len == 0 ? 1u : len, _Alignof(uint8_t));
    if (buf == NULL) {
        return SCRIBE_ENOMEM;
    }
    for (i = 0; i < count; i++) {
        uint8_t leb[10];
        size_t leb_len = scribe_leb128_encode((uint64_t)sorted[i].name_len, leb);
        buf[off++] = sorted[i].type;
        memcpy(buf + off, sorted[i].hash, SCRIBE_HASH_SIZE);
        off += SCRIBE_HASH_SIZE;
        memcpy(buf + off, leb, leb_len);
        off += leb_len;
        memcpy(buf + off, sorted[i].name, sorted[i].name_len);
        off += sorted[i].name_len;
    }
    *out = buf;
    *out_len = len;
    return SCRIBE_OK;
}

/*
 * Returns a conservative arena capacity for parsing a tree payload. The parser
 * needs entry structs plus NUL-terminated name copies, so callers use this
 * helper to avoid under-sizing fixed arenas.
 */
scribe_error_t scribe_tree_parse_arena_capacity(size_t payload_len, size_t *out) {
    if (out == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid tree parse arena capacity output");
    }
    if (payload_len > (SIZE_MAX - 4096u) / 8u) {
        return scribe_set_error(SCRIBE_ENOMEM, "tree payload is too large");
    }
    /*
     * Parsing creates an array of entries and copies every name into the arena.
     * The exact count is not known without parsing, so callers use this
     * conservative bound to avoid the fixed-size arena exhaustion bugs that
     * large collections previously exposed.
     */
    *out = payload_len * 8u + 4096u;
    return SCRIBE_OK;
}

/*
 * Parses a canonical tree payload into arena-owned entry views. It verifies
 * entry framing, child types, nonempty names, and strict byte-sorted ordering so
 * corrupt or duplicate tree entries are rejected at the boundary.
 */
scribe_error_t scribe_tree_parse(const uint8_t *payload, size_t len, scribe_arena *arena,
                                 scribe_tree_entry **out_entries, size_t *out_count) {
    size_t off = 0;
    size_t cap = 8;
    size_t count = 0;
    scribe_tree_entry *entries;

    if (payload == NULL && len != 0) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid tree payload");
    }
    entries = (scribe_tree_entry *)scribe_arena_alloc(arena, sizeof(*entries) * cap, _Alignof(scribe_tree_entry));
    if (entries == NULL) {
        return SCRIBE_ENOMEM;
    }
    while (off < len) {
        uint64_t name_len64;
        size_t leb_used;
        char *name;
        if (len - off < 1u + SCRIBE_HASH_SIZE) {
            return scribe_set_error(SCRIBE_ECORRUPT, "truncated tree entry");
        }
        if (count == cap) {
            scribe_tree_entry *grown;
            cap *= 2u;
            grown = (scribe_tree_entry *)scribe_arena_alloc(arena, sizeof(*grown) * cap, _Alignof(scribe_tree_entry));
            if (grown == NULL) {
                return SCRIBE_ENOMEM;
            }
            memcpy(grown, entries, sizeof(*entries) * count);
            entries = grown;
        }
        entries[count].type = payload[off++];
        if (entries[count].type != SCRIBE_OBJECT_BLOB && entries[count].type != SCRIBE_OBJECT_TREE) {
            return scribe_set_error(SCRIBE_ECORRUPT, "invalid tree entry type");
        }
        memcpy(entries[count].hash, payload + off, SCRIBE_HASH_SIZE);
        off += SCRIBE_HASH_SIZE;
        if (scribe_leb128_decode(payload + off, len - off, &name_len64, &leb_used) != SCRIBE_OK) {
            return SCRIBE_ECORRUPT;
        }
        off += leb_used;
        if (name_len64 == 0 || name_len64 > SIZE_MAX || (size_t)name_len64 > len - off) {
            return scribe_set_error(SCRIBE_ECORRUPT, "invalid tree entry name length");
        }
        name = scribe_arena_strdup_len(arena, (const char *)(payload + off), (size_t)name_len64);
        if (name == NULL) {
            return SCRIBE_ENOMEM;
        }
        entries[count].name = name;
        entries[count].name_len = (size_t)name_len64;
        off += entries[count].name_len;
        /*
         * Because serialized trees must be strictly sorted, a repeated name or
         * out-of-order entry is corrupt. Enforcing this during parse lets later
         * code use simple merge walks without defensive duplicate resolution.
         */
        if (count > 0 && entry_cmp(&entries[count - 1u], &entries[count]) >= 0) {
            return scribe_set_error(SCRIBE_ECORRUPT, "tree entries are not strictly sorted");
        }
        count++;
    }
    *out_entries = entries;
    *out_count = count;
    return SCRIBE_OK;
}
