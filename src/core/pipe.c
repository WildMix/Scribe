#include "core/internal.h"

#include "util/error.h"
#include "util/hex.h"
#include "util/queue.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void strip_lf(char *line) {
    size_t len = strlen(line);
    if (len != 0 && line[len - 1u] == '\n') {
        line[len - 1u] = '\0';
    }
}

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

static scribe_error_t parse_size_field(const char *s, size_t *out) {
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0') {
        return scribe_set_error(SCRIBE_EMALFORMED, "invalid decimal length '%s'", s);
    }
    *out = (size_t)v;
    return SCRIBE_OK;
}

static scribe_error_t parse_i64_field(const char *s, int64_t *out) {
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end != '\0') {
        return scribe_set_error(SCRIBE_EMALFORMED, "invalid timestamp '%s'", s);
    }
    *out = (int64_t)v;
    return SCRIBE_OK;
}

static scribe_error_t read_line(FILE *in, char **line, size_t *cap) {
    ssize_t n = getline(line, cap, in);
    if (n < 0) {
        return feof(in) ? scribe_set_error(SCRIBE_EPROTOCOL, "unexpected EOF")
                        : scribe_set_error(SCRIBE_EIO, "failed to read pipe line");
    }
    strip_lf(*line);
    return SCRIBE_OK;
}

static scribe_error_t read_exact(FILE *in, uint8_t *buf, size_t len) {
    size_t got = fread(buf, 1, len, in);
    if (got != len) {
        return scribe_set_error(SCRIBE_EPROTOCOL, "truncated binary payload");
    }
    return SCRIBE_OK;
}

static void free_batch(scribe_change_batch *batch) {
    size_t i;

    if (batch == NULL) {
        return;
    }
    for (i = 0; i < batch->event_count; i++) {
        size_t j;
        scribe_change_event *ev = (scribe_change_event *)&batch->events[i];
        for (j = 0; j < ev->path_len; j++) {
            free((void *)ev->path[j]);
        }
        free((void *)ev->path);
        free((void *)ev->payload);
    }
    free((void *)batch->events);
    free((void *)batch->author.name);
    free((void *)batch->author.email);
    free((void *)batch->author.source);
    free((void *)batch->committer.name);
    free((void *)batch->committer.email);
    free((void *)batch->committer.source);
    free((void *)batch->process.name);
    free((void *)batch->process.version);
    free((void *)batch->process.params);
    free((void *)batch->process.correlation_id);
    free((void *)batch->message);
}

static scribe_error_t parse_identity(char **parts, scribe_identity *id) {
    id->name = strdup(parts[1]);
    id->email = strdup(parts[2]);
    id->source = strdup(parts[3]);
    if (id->name == NULL || id->email == NULL || id->source == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate identity");
    }
    return SCRIBE_OK;
}

static scribe_error_t parse_process(char **parts, scribe_process_info *process) {
    process->name = strdup(parts[1]);
    process->version = strdup(parts[2]);
    process->params = strdup(parts[3]);
    process->correlation_id = strdup(parts[4]);
    if (process->name == NULL || process->version == NULL || process->params == NULL ||
        process->correlation_id == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate process info");
    }
    return SCRIBE_OK;
}

static scribe_error_t parse_one_batch(FILE *in, char *first_line, scribe_change_batch *batch) {
    char *line = NULL;
    size_t cap = 0;
    char *parts[5] = {0};
    size_t event_count = 0;
    size_t message_len = 0;
    size_t i;
    scribe_change_event *events;
    scribe_error_t err;

    /*
     * The pipe protocol is a hybrid text/binary frame. Header lines and path
     * components are
     * newline-delimited text, but message and payload bytes are
     * read by exact byte count. That lets adapters
     * send arbitrary blob bytes
     * without escaping them, while keeping framing easy to debug with a terminal.
 */
    memset(batch, 0, sizeof(*batch));
    if (split_tabs(first_line, parts, 3u) != 3u || strcmp(parts[0], "BATCH") != 0) {
        return scribe_set_error(SCRIBE_EPROTOCOL, "expected BATCH line");
    }
    if (strcmp(parts[1], "1") != 0) {
        return scribe_set_error(SCRIBE_EPROTOCOL, "unsupported pipe protocol version '%s'", parts[1]);
    }
    if ((err = parse_size_field(parts[2], &event_count)) != SCRIBE_OK) {
        return err;
    }
    if (event_count == 0) {
        return scribe_set_error(SCRIBE_EMALFORMED, "event_count must be greater than zero");
    }
    events = (scribe_change_event *)calloc(event_count, sizeof(*events));
    if (events == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate events");
    }
    batch->events = events;
    batch->event_count = event_count;

    if ((err = read_line(in, &line, &cap)) != SCRIBE_OK) {
        goto done;
    }
    if (split_tabs(line, parts, 4u) != 4u || strcmp(parts[0], "AUTHOR") != 0) {
        err = scribe_set_error(SCRIBE_EMALFORMED, "expected AUTHOR line");
        goto done;
    }
    if ((err = parse_identity(parts, &batch->author)) != SCRIBE_OK) {
        goto done;
    }

    if ((err = read_line(in, &line, &cap)) != SCRIBE_OK) {
        goto done;
    }
    if (split_tabs(line, parts, 4u) != 4u || strcmp(parts[0], "COMMITTER") != 0) {
        err = scribe_set_error(SCRIBE_EMALFORMED, "expected COMMITTER line");
        goto done;
    }
    if ((err = parse_identity(parts, &batch->committer)) != SCRIBE_OK) {
        goto done;
    }

    if ((err = read_line(in, &line, &cap)) != SCRIBE_OK) {
        goto done;
    }
    if (split_tabs(line, parts, 5u) != 5u || strcmp(parts[0], "PROCESS") != 0) {
        err = scribe_set_error(SCRIBE_EMALFORMED, "expected PROCESS line");
        goto done;
    }
    if ((err = parse_process(parts, &batch->process)) != SCRIBE_OK) {
        goto done;
    }

    if ((err = read_line(in, &line, &cap)) != SCRIBE_OK) {
        goto done;
    }
    if (split_tabs(line, parts, 2u) != 2u || strcmp(parts[0], "TIMESTAMP") != 0) {
        err = scribe_set_error(SCRIBE_EMALFORMED, "expected TIMESTAMP line");
        goto done;
    }
    if ((err = parse_i64_field(parts[1], &batch->timestamp_unix_nanos)) != SCRIBE_OK) {
        goto done;
    }

    if ((err = read_line(in, &line, &cap)) != SCRIBE_OK) {
        goto done;
    }
    if (split_tabs(line, parts, 2u) != 2u || strcmp(parts[0], "MESSAGE") != 0) {
        err = scribe_set_error(SCRIBE_EMALFORMED, "expected MESSAGE line");
        goto done;
    }
    if ((err = parse_size_field(parts[1], &message_len)) != SCRIBE_OK) {
        goto done;
    }
    if (message_len != 0) {
        uint8_t *msg = (uint8_t *)malloc(message_len);
        if (msg == NULL) {
            err = scribe_set_error(SCRIBE_ENOMEM, "failed to allocate message");
            goto done;
        }
        if ((err = read_exact(in, msg, message_len)) != SCRIBE_OK) {
            free(msg);
            goto done;
        }
        batch->message = (const char *)msg;
        batch->message_len = message_len;
    }

    for (i = 0; i < event_count; i++) {
        size_t depth;
        size_t payload_len;
        size_t j;
        const char **path;
        if ((err = read_line(in, &line, &cap)) != SCRIBE_OK) {
            goto done;
        }
        if (split_tabs(line, parts, 3u) != 3u || strcmp(parts[0], "EVENT") != 0) {
            err = scribe_set_error(SCRIBE_EMALFORMED, "expected EVENT line");
            goto done;
        }
        if ((err = parse_size_field(parts[1], &depth)) != SCRIBE_OK ||
            (err = parse_size_field(parts[2], &payload_len)) != SCRIBE_OK) {
            goto done;
        }
        path = (const char **)calloc(depth, sizeof(char *));
        if (path == NULL) {
            err = scribe_set_error(SCRIBE_ENOMEM, "failed to allocate event path");
            goto done;
        }
        ((scribe_change_event *)&batch->events[i])->path = path;
        ((scribe_change_event *)&batch->events[i])->path_len = depth;
        for (j = 0; j < depth; j++) {
            if ((err = read_line(in, &line, &cap)) != SCRIBE_OK) {
                goto done;
            }
            ((const char **)batch->events[i].path)[j] = strdup(line);
            if (batch->events[i].path[j] == NULL) {
                err = scribe_set_error(SCRIBE_ENOMEM, "failed to allocate path component");
                goto done;
            }
        }
        /*
         * payload_len == 0 means tombstone/delete. Empty blobs are not
         * representable in v1 because the public change event API uses
         * payload == NULL and payload_len == 0 as the delete marker.
         */
        if (payload_len != 0) {
            uint8_t *payload = (uint8_t *)malloc(payload_len);
            if (payload == NULL) {
                err = scribe_set_error(SCRIBE_ENOMEM, "failed to allocate payload");
                goto done;
            }
            if ((err = read_exact(in, payload, payload_len)) != SCRIBE_OK) {
                free(payload);
                goto done;
            }
            ((scribe_change_event *)&batch->events[i])->payload = payload;
            ((scribe_change_event *)&batch->events[i])->payload_len = payload_len;
        }
    }
    if ((err = read_line(in, &line, &cap)) != SCRIBE_OK) {
        goto done;
    }
    if (strcmp(line, "END") != 0) {
        err = scribe_set_error(SCRIBE_EMALFORMED, "expected END line");
        goto done;
    }
    err = SCRIBE_OK;

done:
    free(line);
    if (err != SCRIBE_OK) {
        free_batch(batch);
        memset(batch, 0, sizeof(*batch));
    }
    return err;
}

static void write_error(FILE *out, scribe_error_t err) {
    const char *detail = scribe_last_error_detail();
    size_t len = strlen(detail);
    fprintf(out, "ERR\t%s\t%zu\n", scribe_error_symbol(err), len);
    if (len != 0) {
        fwrite(detail, 1, len, out);
    }
    fputc('\n', out);
    fflush(out);
}

static scribe_error_t commit_via_queue(scribe_ctx *ctx, scribe_change_batch *batch,
                                       uint8_t commit_hash[SCRIBE_HASH_SIZE]) {
    /*
     * The CLI is single-process, but this still routes the batch through the
     * same SPSC queue abstraction
     * used by library/adapter paths. That keeps the
     * pipe command exercising queue behavior instead of calling
     * the commit
     * builder directly.
     */
    scribe_spsc_queue q;
    void *dequeued = NULL;
    scribe_error_t err =
        scribe_spsc_queue_init(&q, ctx->config.event_queue_capacity, ctx->config.queue_stall_warn_seconds);
    if (err != SCRIBE_OK) {
        return err;
    }
    err = scribe_spsc_queue_push(&q, batch);
    if (err == SCRIBE_OK) {
        err = scribe_spsc_queue_pop(&q, &dequeued);
    }
    if (err == SCRIBE_OK) {
        err = scribe_commit_batch(ctx, (scribe_change_batch *)dequeued, commit_hash);
    }
    scribe_spsc_queue_destroy(&q);
    return err;
}

scribe_error_t scribe_pipe_commit_batch(scribe_ctx *ctx, FILE *in, FILE *out) {
    char *line = NULL;
    size_t cap = 0;

    /*
     * Accept multiple BATCH frames on one stdin stream. Each successful frame
     * commits independently and
     * emits its own OK line. The first malformed frame
     * emits ERR and stops; continuing after malformed framing
     * would risk reading
     * binary payload bytes as protocol lines.
     */
    while (getline(&line, &cap, in) >= 0) {
        scribe_change_batch batch;
        uint8_t commit_hash[SCRIBE_HASH_SIZE];
        char hex[SCRIBE_HEX_HASH_SIZE + 1];
        scribe_error_t err;

        strip_lf(line);
        if (line[0] == '\0') {
            continue;
        }
        err = parse_one_batch(in, line, &batch);
        if (err == SCRIBE_OK) {
            err = commit_via_queue(ctx, &batch, commit_hash);
        }
        if (err != SCRIBE_OK) {
            write_error(out, err);
            free(line);
            return err;
        }
        scribe_hash_to_hex(commit_hash, hex);
        fprintf(out, "OK\t%s\n", hex);
        fflush(out);
        free_batch(&batch);
    }
    free(line);
    return SCRIBE_OK;
}
