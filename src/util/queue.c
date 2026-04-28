#include "util/queue.h"

#include "util/error.h"

#include <errno.h>
#include <stdlib.h>
#include <time.h>

scribe_error_t scribe_spsc_queue_init(scribe_spsc_queue *q, size_t capacity, int stall_warn_seconds) {
    if (q == NULL || capacity == 0) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid queue capacity");
    }
    q->items = (void **)calloc(capacity, sizeof(void *));
    if (q->items == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate queue");
    }
    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->stall_warn_seconds = stall_warn_seconds;
    q->warned_full = 0;
    if (pthread_mutex_init(&q->mu, NULL) != 0 || pthread_cond_init(&q->not_empty, NULL) != 0 ||
        pthread_cond_init(&q->not_full, NULL) != 0) {
        free(q->items);
        q->items = NULL;
        return scribe_set_error(SCRIBE_ERR, "failed to initialize queue synchronization");
    }
    return SCRIBE_OK;
}

void scribe_spsc_queue_destroy(scribe_spsc_queue *q) {
    if (q != NULL) {
        pthread_cond_destroy(&q->not_full);
        pthread_cond_destroy(&q->not_empty);
        pthread_mutex_destroy(&q->mu);
        free(q->items);
        q->items = NULL;
    }
}

static void make_deadline(struct timespec *ts, int seconds) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += seconds;
}

scribe_error_t scribe_spsc_queue_push(scribe_spsc_queue *q, void *item) {
    if (q == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "queue is NULL");
    }
    /*
     * This is a bounded single-producer/single-consumer queue, but it still
     * uses a mutex/condition pair rather than lock-free atomics. The queue is
     * on the adapter/pipe boundary where clarity and predictable blocking matter
     * more than raw nanosecond latency.
     */
    pthread_mutex_lock(&q->mu);
    while (q->count == q->capacity) {
        if (q->stall_warn_seconds > 0) {
            struct timespec deadline;
            int rc;
            make_deadline(&deadline, q->stall_warn_seconds);
            rc = pthread_cond_timedwait(&q->not_full, &q->mu, &deadline);
            if (rc == ETIMEDOUT) {
                /*
                 * The queue does not log directly because util/queue has no
                 * repository context. It records that a full-queue wait crossed
                 * the configured threshold so callers can inspect the condition
                 * or extend this with contextual logging later.
                 */
                q->warned_full = 1;
            }
        } else {
            pthread_cond_wait(&q->not_full, &q->mu);
        }
    }
    q->items[q->tail] = item;
    q->tail = (q->tail + 1u) % q->capacity;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
    return SCRIBE_OK;
}

scribe_error_t scribe_spsc_queue_pop(scribe_spsc_queue *q, void **out) {
    if (q == NULL || out == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "invalid queue pop");
    }
    pthread_mutex_lock(&q->mu);
    while (q->count == 0) {
        pthread_cond_wait(&q->not_empty, &q->mu);
    }
    *out = q->items[q->head];
    q->head = (q->head + 1u) % q->capacity;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return SCRIBE_OK;
}

int scribe_spsc_queue_try_pop(scribe_spsc_queue *q, void **out) {
    int found = 0;

    if (q == NULL || out == NULL) {
        return 0;
    }
    pthread_mutex_lock(&q->mu);
    if (q->count != 0) {
        *out = q->items[q->head];
        q->head = (q->head + 1u) % q->capacity;
        q->count--;
        pthread_cond_signal(&q->not_full);
        found = 1;
    }
    pthread_mutex_unlock(&q->mu);
    return found;
}
