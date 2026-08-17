/*
 * arena.c — bump-allocator-ის იმპლემენტაცია.
 *
 * მთელი ლოგიკა ორ ხაზში ჯდება: `used`-ს ვასწორებთ align-ზე და ვზრდით.
 * სირთულე მხოლოდ overflow-ის სწორად შემოწმებაშია.
 */

#include "arena.h"

#include <stdlib.h>
#include <string.h>

/* გავასწოროთ `n` ზემოთ `align`-ის ჯერადამდე. align — 2-ის ხარისხი. */
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
        /* ველებს მაინც ვასუფთავებთ, რომ arena_destroy() უსაფრთხო იყოს. */
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
    a->owns_base = false; /* ბუფერი სხვისია — არ გავათავისუფლოთ */
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
     * Overflow-ის ორმაგი შემოწმება:
     *   1. align_up-მა შეიძლება გადაატრიალოს, თუ used უზარმაზარია → offset < used;
     *   2. offset + size შეიძლება გადაატრიალოს → ამას იძლევა capacity - offset.
     * ორივე შემთხვევაში უსაფრთხოდ ვბრუნდებით NULL-ით.
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
        a->used = 0; /* სულ ესაა — არავითარი free() ციკლი */
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
    /* არასოდეს "გავზარდოთ" used უკან-დაბრუნებისას — ეს ლოგიკის შეცდომას ნიშნავს. */
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
