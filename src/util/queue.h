#ifndef SCRIBE_UTIL_QUEUE_H
#define SCRIBE_UTIL_QUEUE_H

#include "scribe/scribe.h"

#include <pthread.h>
#include <stddef.h>
#include <time.h>

typedef struct {
    void **items;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    int stall_warn_seconds;
    int warned_full;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} scribe_spsc_queue;

scribe_error_t scribe_spsc_queue_init(scribe_spsc_queue *q, size_t capacity, int stall_warn_seconds);
void scribe_spsc_queue_destroy(scribe_spsc_queue *q);
scribe_error_t scribe_spsc_queue_push(scribe_spsc_queue *q, void *item);
scribe_error_t scribe_spsc_queue_pop(scribe_spsc_queue *q, void **out);
int scribe_spsc_queue_try_pop(scribe_spsc_queue *q, void **out);

#endif
