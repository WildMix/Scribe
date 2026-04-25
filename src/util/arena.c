#include "util/arena.h"

#include "util/error.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

scribe_error_t scribe_arena_init(scribe_arena *arena, size_t capacity) {
    if (arena == NULL) {
        return scribe_set_error(SCRIBE_EINVAL, "arena is NULL");
    }
    arena->data = NULL;
    arena->capacity = 0;
    arena->used = 0;
    if (capacity == 0) {
        return SCRIBE_OK;
    }
    arena->data = (uint8_t *)malloc(capacity);
    if (arena->data == NULL) {
        return scribe_set_error(SCRIBE_ENOMEM, "failed to allocate arena");
    }
    arena->capacity = capacity;
    return SCRIBE_OK;
}

void scribe_arena_destroy(scribe_arena *arena) {
    if (arena != NULL) {
        free(arena->data);
        arena->data = NULL;
        arena->capacity = 0;
        arena->used = 0;
    }
}

void scribe_arena_reset(scribe_arena *arena) {
    if (arena != NULL) {
        arena->used = 0;
    }
}

static int is_power_of_two(size_t value) { return value != 0 && (value & (value - 1)) == 0; }

void *scribe_arena_alloc(scribe_arena *arena, size_t size, size_t align) {
    uintptr_t base;
    uintptr_t aligned;
    size_t padding;

    if (arena == NULL || !is_power_of_two(align)) {
        (void)scribe_set_error(SCRIBE_EINVAL, "invalid arena allocation");
        return NULL;
    }
    if (size == 0) {
        size = 1;
    }
    base = (uintptr_t)(arena->data + arena->used);
    aligned = (base + (uintptr_t)align - 1u) & ~((uintptr_t)align - 1u);
    padding = (size_t)(aligned - base);
    if (padding > arena->capacity - arena->used || size > arena->capacity - arena->used - padding) {
        (void)scribe_set_error(SCRIBE_ENOMEM, "arena exhausted");
        return NULL;
    }
    arena->used += padding + size;
    return (void *)aligned;
}

char *scribe_arena_strdup_len(scribe_arena *arena, const char *s, size_t len) {
    char *copy;

    if (s == NULL) {
        (void)scribe_set_error(SCRIBE_EINVAL, "string is NULL");
        return NULL;
    }
    copy = (char *)scribe_arena_alloc(arena, len + 1u, _Alignof(char));
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

char *scribe_arena_strdup(scribe_arena *arena, const char *s) { return scribe_arena_strdup_len(arena, s, strlen(s)); }
