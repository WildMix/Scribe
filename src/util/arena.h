#ifndef SCRIBE_UTIL_ARENA_H
#define SCRIBE_UTIL_ARENA_H

#include "scribe/scribe.h"

#include <stddef.h>

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t used;
} scribe_arena;

scribe_error_t scribe_arena_init(scribe_arena *arena, size_t capacity);
void scribe_arena_destroy(scribe_arena *arena);
void scribe_arena_reset(scribe_arena *arena);
void *scribe_arena_alloc(scribe_arena *arena, size_t size, size_t align);
char *scribe_arena_strdup_len(scribe_arena *arena, const char *s, size_t len);
char *scribe_arena_strdup(scribe_arena *arena, const char *s);

#endif
