/*
 * Commit payload serialization and parsing.
 *
 * Scribe commit objects are human-readable text headers plus raw message bytes.
 * This file validates commit batch metadata, serializes commit payloads, and
 * parses commit objects into arena-backed views used by log, diff, fsck, and
 * adapter code.
 */
#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Checks whether a string field is unusable in a line-oriented commit header.
 * Required fields must be present and nonempty; all fields reject tabs/newlines
 * because those bytes are structural separators in the commit payload.
 */
static int invalid_field(const char *s, int required) {
    if (s == NULL) {
        return required;
    }
    if (required && s[0] == '\0') {
        return 1;
    }
    return strchr(s, '\n') != NULL || strchr(s, '\t') != NULL;
}

/*
 * Validates the semantic shape of a change batch before it can become a commit.
 * The allow_empty flag is used only for bootstrap commits, where the snapshot
 * root already contains the state and no individual change events are recorded.
 */
static scribe_error_t validate_batch(const scribe_change_batch *batch, int allow_empty) {
    size_t i;

    /*
     * The commit payload is line-oriented text, so fields that become headers
     * cannot contain tabs or newlines. Event paths also reject tabs/newlines so
     * tree listing and diff output remain unambiguous. A NULL payload with
     * length zero is the only tombstone representation; a non-NULL payload must
     * have nonzero length.
     */
    if (batch == NULL || (!allow_empty && (batch->events == NULL || batch->event_count == 0)) ||
        (batch->events == NULL && batch->event_count != 0) || (batch->events != NULL && batch->event_count == 0)) {
        return scribe_set_error(SCRIBE_EMALFORMED, "batch must contain at least one event");
    }
    if (invalid_field(batch->author.name, 1) || invalid_field(batch->author.email, 0) ||
        invalid_field(batch->author.source, 1) || invalid_field(batch->committer.name, 1) ||
        invalid_field(batch->committer.email, 0) || invalid_field(batch->committer.source, 1) ||
        invalid_field(batch->process.name, 1) || invalid_field(batch->process.version, 1) ||
        invalid_field(batch->process.params, 0) || invalid_field(batch->process.correlation_id, 0)) {
        return scribe_set_error(SCRIBE_EMALFORMED, "required string field is invalid");
    }
    if (batch->message == NULL && batch->message_len != 0) {
        return scribe_set_error(SCRIBE_EMALFORMED, "message length requires message bytes");
    }
    for (i = 0; i < batch->event_count; i++) {
        const scribe_change_event *ev = &batch->events[i];
        size_t j;
        if (ev->path == NULL || ev->path_len == 0) {
            return scribe_set_error(SCRIBE_EMALFORMED, "event path is empty");
        }
        if ((ev->payload == NULL && ev->payload_len != 0) || (ev->payload != NULL && ev->payload_len == 0)) {
            return scribe_set_error(SCRIBE_EMALFORMED, "invalid tombstone/payload pair");
        }
        for (j = 0; j < ev->path_len; j++) {
            if (ev->path[j] == NULL || ev->path[j][0] == '\0' || strchr(ev->path[j], '\n') != NULL ||
                strchr(ev->path[j], '\t') != NULL) {
                return scribe_set_error(SCRIBE_EMALFORMED, "invalid path component");
            }
        }
    }
    return SCRIBE_OK;
}

/*
 * Appends formatted text into an existing fixed-size buffer. The pointer and
 * remaining byte count are updated together so commit serialization can build
 * one contiguous payload without repeated strlen() scans.
 */
static int appendf(char **p, size_t *remaining, const char *fmt, ...) {
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(*p, *remaining, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= *remaining) {
        return -1;
    }
    *p += n;
    *remaining -= (size_t)n;
    return 0;
}

/*
 * Serializes a root tree, optional parent, batch metadata, and message into the
 * commit object payload. The helper is shared by normal commits and bootstrap
 * commits so the only behavioral difference is whether empty event lists are allowed.
 */
static scribe_error_t commit_serialize_ex(const uint8_t root_tree[SCRIBE_HASH_SIZE], const uint8_t *parent,
                                          const scribe_change_batch *batch, scribe_arena *arena, uint8_t **out,
                                          size_t *out_len, int allow_empty) {
    char root_hex[SCRIBE_HEX_HASH_SIZE + 1];
    char parent_hex[SCRIBE_HEX_HASH_SIZE + 1];
    size_t cap;
    char *buf;
    char *p;
    size_t rem;

    if (validate_batch(batch, allow_empty) != SCRIBE_OK) {
        return SCRIBE_EMALFORMED;
    }
    /*
     * Commit payloads intentionally stay human-readable. Header lines are
     * separated from the message by a blank line. The message is copied as raw
     * bytes after that blank line and may contain arbitrary content; parsers do
     * not attempt to interpret it.
     */
    cap = 512u + batch->message_len + strlen(batch->author.name) + strlen(batch->author.email) +
          strlen(batch->author.source) + strlen(batch->committer.name) + strlen(batch->committer.email) +
          strlen(batch->committer.source) + strlen(batch->process.name) + strlen(batch->process.version) +
          strlen(batch->process.params) + strlen(batch->process.correlation_id);
    buf = (char *)scribe_arena_alloc(arena, cap, _Alignof(char));
    if (buf == NULL) {
        return SCRIBE_ENOMEM;
    }
    p = buf;
    rem = cap;
    scribe_hash_to_hex(root_tree, root_hex);
    if (appendf(&p, &rem, "tree %s\n", root_hex) != 0) {
        return scribe_set_error(SCRIBE_ENOMEM, "commit buffer too small");
    }
    if (parent != NULL) {
        scribe_hash_to_hex(parent, parent_hex);
        if (appendf(&p, &rem, "parent %s\n", parent_hex) != 0) {
            return scribe_set_error(SCRIBE_ENOMEM, "commit buffer too small");
        }
    }
    if (appendf(&p, &rem, "author\t%s\t%s\t%s\t%lld\n", batch->author.name,
                batch->author.email == NULL ? "" : batch->author.email, batch->author.source,
                (long long)batch->timestamp_unix_nanos) != 0 ||
        appendf(&p, &rem, "committer\t%s\t%s\t%s\t%lld\n", batch->committer.name,
                batch->committer.email == NULL ? "" : batch->committer.email, batch->committer.source,
                (long long)batch->timestamp_unix_nanos) != 0 ||
        appendf(&p, &rem, "process\t%s\t%s\t%s\t%s\n\n", batch->process.name, batch->process.version,
                batch->process.params == NULL ? "" : batch->process.params,
                batch->process.correlation_id == NULL ? "" : batch->process.correlation_id) != 0) {
        return scribe_set_error(SCRIBE_ENOMEM, "commit buffer too small");
    }
    if (batch->message_len != 0) {
        memcpy(p, batch->message, batch->message_len);
        p += batch->message_len;
    }
    *out = (uint8_t *)buf;
    *out_len = (size_t)(p - buf);
    return SCRIBE_OK;
}

/*
 * Serializes a normal commit payload. Normal commits must have at least one
 * change event because they represent a concrete change batch.
 */
scribe_error_t scribe_commit_serialize(const uint8_t root_tree[SCRIBE_HASH_SIZE], const uint8_t *parent,
                                       const scribe_change_batch *batch, scribe_arena *arena, uint8_t **out,
                                       size_t *out_len) {
    return commit_serialize_ex(root_tree, parent, batch, arena, out, out_len, 0);
}

/*
 * Serializes a commit payload while permitting an empty event list. Bootstrap
 * uses this form because its root tree is already the complete baseline snapshot.
 */
scribe_error_t scribe_commit_serialize_allow_empty(const uint8_t root_tree[SCRIBE_HASH_SIZE], const uint8_t *parent,
                                                   const scribe_change_batch *batch, scribe_arena *arena, uint8_t **out,
                                                   size_t *out_len) {
    return commit_serialize_ex(root_tree, parent, batch, arena, out, out_len, 1);
}

/*
 * Returns the next newline-terminated line from a mutable buffer and replaces
 * the newline with NUL. The cursor advances past the newline for the next call.
 */
static char *next_line(char **cursor, char *end) {
    char *start;
    char *nl;

    if (*cursor >= end) {
        return NULL;
    }
    start = *cursor;
    nl = memchr(start, '\n', (size_t)(end - start));
    if (nl == NULL) {
        return NULL;
    }
    *nl = '\0';
    *cursor = nl + 1;
    return start;
}

/*
 * Splits one mutable line into at most max_parts tab-separated fields. Tabs are
 * replaced with NUL bytes so the returned pointers are ordinary C strings.
 */
static size_t split_tabs(char *line, char **parts, size_t max_parts) {
    size_t count = 0;
    char *p = line;

    while (count < max_parts) {
        char *tab;
        parts[count++] = p;
        tab = strchr(p, '\t');
        if (tab == NULL) {
            break;
        }
        *tab = '\0';
        p = tab + 1;
    }
    return count;
}

/*
 * Parses a commit payload into a view whose string fields point into arena
 * memory. The caller must keep the arena alive while using the returned view.
 */
scribe_error_t scribe_commit_parse(const uint8_t *payload, size_t len, scribe_arena *arena, scribe_commit_view *out) {
    char *copy;
    char *cursor;
    char *end;
    char *line;
    int saw_tree = 0;
    int saw_author = 0;
    int saw_committer = 0;
    int saw_process = 0;

    memset(out, 0, sizeof(*out));
    /*
     * Parsing works on an arena-owned mutable copy because split_tabs() and
     * next_line() replace separators with NUL bytes. The returned view points
     * into that arena copy, so callers must keep the arena alive while using the
     * commit fields.
     */
    copy = scribe_arena_strdup_len(arena, (const char *)payload, len);
    if (copy == NULL) {
        return SCRIBE_ENOMEM;
    }
    cursor = copy;
    end = copy + len;
    line = next_line(&cursor, end);
    if (line == NULL || strncmp(line, "tree ", 5) != 0 || scribe_hash_from_hex(line + 5, out->root_tree) != SCRIBE_OK) {
        return scribe_set_error(SCRIBE_ECORRUPT, "commit missing tree header");
    }
    saw_tree = 1;
    line = next_line(&cursor, end);
    if (line != NULL && strncmp(line, "parent ", 7) == 0) {
        if (scribe_hash_from_hex(line + 7, out->parent) != SCRIBE_OK) {
            return SCRIBE_ECORRUPT;
        }
        out->has_parent = true;
        line = next_line(&cursor, end);
    }
    while (line != NULL && line[0] != '\0') {
        char *parts[5] = {0};
        size_t part_count = split_tabs(line, parts, 5u);
        if (part_count == 5u && strcmp(parts[0], "author") == 0) {
            out->author_name = parts[1];
            out->author_email = parts[2];
            out->author_source = parts[3];
            out->author_time = strtoll(parts[4], NULL, 10);
            saw_author = 1;
        } else if (part_count == 5u && strcmp(parts[0], "committer") == 0) {
            out->committer_name = parts[1];
            out->committer_email = parts[2];
            out->committer_source = parts[3];
            out->committer_time = strtoll(parts[4], NULL, 10);
            saw_committer = 1;
        } else if (part_count == 5u && strcmp(parts[0], "process") == 0) {
            out->process_name = parts[1];
            out->process_version = parts[2];
            out->process_params = parts[3];
            out->process_correlation_id = parts[4];
            saw_process = 1;
        } else {
            return scribe_set_error(SCRIBE_ECORRUPT, "malformed commit header");
        }
        line = next_line(&cursor, end);
    }
    if (!saw_tree || !saw_author || !saw_committer || !saw_process || line == NULL) {
        return scribe_set_error(SCRIBE_ECORRUPT, "commit missing required headers");
    }
    out->message = (uint8_t *)cursor;
    out->message_len = (size_t)(end - cursor);
    return SCRIBE_OK;
}

/* Commit construction is implemented in blob.c to keep the tree editing helpers private. */
/*
 * Public commit entry point for callers with a writable context and a validated
 * change batch. The actual tree editing implementation lives in blob.c so its
 * private mutable-tree helpers do not need to be exposed.
 */
scribe_error_t scribe_commit_batch(scribe_ctx *ctx, const scribe_change_batch *batch,
                                   uint8_t out_commit_hash[SCRIBE_HASH_SIZE]) {
    return scribe_commit_batch_internal(ctx, batch, out_commit_hash);
}
