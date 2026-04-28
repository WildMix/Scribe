/*
 * Bounded single-producer/single-consumer queue.
 *
 * The queue is used at Scribe's pipe/library boundary where producer and
 * consumer work should be decoupled without unbounded memory growth. It uses
 * pthread mutexes and condition variables for clarity and portability rather
 * than a lock-free implementation.
 */
#include "util/queue.h"

#include "util/error.h"

#include <errno.h>
#include <stdlib.h>
#include <time.h>

/*
 * Allocates the ring buffer and synchronization primitives for a queue. The
 * stall warning threshold is stored here so push operations can notice when a
 * full queue has blocked longer than expected.
 */
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

/*
 * Destroys queue synchronization primitives and frees the ring buffer. The
 * queue does not own queued item pointers, so callers must drain/free items.
 */
void scribe_spsc_queue_destroy(scribe_spsc_queue *q) {
    if (q != NULL) {
        pthread_cond_destroy(&q->not_full);
        pthread_cond_destroy(&q->not_empty);
        pthread_mutex_destroy(&q->mu);
        free(q->items);
        q->items = NULL;
    }
}

/*
 * Builds an absolute CLOCK_REALTIME deadline seconds in the future for
 * pthread_cond_timedwait().
 */
static void make_deadline(struct timespec *ts, int seconds) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += seconds;
}

/*
 * Pushes one item, blocking while the ring is full. If the configured stall
 * threshold expires during a full wait, the queue records warned_full so callers
 * can surface a contextual warning.
 */
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

/*
 * Pops one item, blocking until an item is available. This is the normal
 * consumer path when the caller knows the producer will eventually provide
 * work.
 */
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

/*
 * Attempts to pop one item without blocking. The return value is boolean: 1
 * when an item was written to out, 0 when the queue was empty or invalid.
 */
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
