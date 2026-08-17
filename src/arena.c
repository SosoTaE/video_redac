/*
 * arena.c — the bump allocator's implementation.
 *
 * The whole idea fits in two lines: align `used`, then bump it. The only
 * subtlety is checking for overflow correctly.
 */

#include "arena.h"

#include <stdlib.h>
#include <string.h>

/* Round `n` up to a multiple of `align`, which must be a power of two. */
static size_t align_up(size_t n, size_t align)
{
    return (n + (align - 1)) & ~(align - 1);
}

static bool is_power_of_two(size_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

bool arena_init(Arena *a, size_t capacity)
{
    if (a == NULL || capacity == 0) {
        return false;
    }

    a->base = (uint8_t *)malloc(capacity);
    if (a->base == NULL) {
        /* Still clear the fields so arena_destroy() stays safe. */
        a->capacity = a->used = a->peak = 0;
        a->owns_base = false;
        return false;
    }

    a->capacity  = capacity;
    a->used      = 0;
    a->peak      = 0;
    a->owns_base = true;
    return true;
}

void arena_init_from_buffer(Arena *a, void *buffer, size_t capacity)
{
    if (a == NULL) {
        return;
    }
    a->base      = (uint8_t *)buffer;
    a->capacity  = (buffer != NULL) ? capacity : 0;
    a->used      = 0;
    a->peak      = 0;
    a->owns_base = false; /* the buffer belongs to someone else — never free it */
}

void *arena_alloc(Arena *a, size_t size, size_t align)
{
    if (a == NULL || a->base == NULL || size == 0) {
        return NULL;
    }
    if (!is_power_of_two(align)) {
        return NULL;
    }

    size_t offset = align_up(a->used, align);

    /*
     * Two overflow checks, both necessary:
     *   1. align_up can wrap if `used` is enormous → offset < used;
     *   2. offset + size can wrap → hence the `capacity - offset` form.
     * Either way we return NULL safely.
     */
    if (offset < a->used || offset > a->capacity) {
        return NULL;
    }
    if (size > a->capacity - offset) {
        return NULL;
    }

    void *ptr = a->base + offset;
    a->used   = offset + size;
    if (a->used > a->peak) {
        a->peak = a->used;
    }
    return ptr;
}

void *arena_alloc_zero(Arena *a, size_t size, size_t align)
{
    void *ptr = arena_alloc(a, size, align);
    if (ptr != NULL) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void arena_reset(Arena *a)
{
    if (a != NULL) {
        a->used = 0; /* that is all — no free() loop */
    }
}

ArenaMarker arena_mark(const Arena *a)
{
    ArenaMarker m;
    m.used = (a != NULL) ? a->used : 0;
    return m;
}

void arena_release(Arena *a, ArenaMarker m)
{
    /* Never *grow* used while rolling back — that would mean a logic error. */
    if (a != NULL && m.used <= a->used) {
        a->used = m.used;
    }
}

void arena_destroy(Arena *a)
{
    if (a == NULL) {
        return;
    }
    if (a->owns_base) {
        free(a->base);
    }
    a->base      = NULL;
    a->capacity  = 0;
    a->used      = 0;
    a->owns_base = false;
}
