#ifndef VIDEO_REDAC_ARENA_H
#define VIDEO_REDAC_ARENA_H

/*
 * arena.h — the bump allocator's API. The struct itself lives in types.h;
 * see the comment there for the rationale.
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Takes `capacity` bytes once, via malloc. false → allocation failed. */
bool arena_init(Arena *a, size_t capacity);

/* Wraps an existing buffer (e.g. on the stack) — no malloc involved. */
void arena_init_from_buffer(Arena *a, void *buffer, size_t capacity);

/*
 * Hands out `size` bytes aligned to `align` (which must be a power of two).
 * NULL → the arena is exhausted; the caller *must* check.
 */
void *arena_alloc(Arena *a, size_t size, size_t align);

/* Same, but zeroed — convenient for arrays of structs. */
void *arena_alloc_zero(Arena *a, size_t size, size_t align);

/* alignof is spelled differently in C11 and C++; nvcc compiles renderer.cu
 * as C++, so both spellings are covered here. */
#ifdef __cplusplus
#  define VR_ALIGNOF(T) alignof(T)
#else
#  define VR_ALIGNOF(T) _Alignof(T)
#endif

/* Helper for a typed array: ARENA_NEW(a, WidgetRuntime, n) */
#define ARENA_NEW(arena_ptr, T, count) \
    ((T *)arena_alloc_zero((arena_ptr), sizeof(T) * (size_t)(count), VR_ALIGNOF(T)))

/* Releases everything at once (O(1)). Memory is not returned to the OS. */
void arena_reset(Arena *a);

/* Scoped allocation: take a marker → work → roll back to it. */
ArenaMarker arena_mark(const Arena *a);
void        arena_release(Arena *a, ArenaMarker m);

/* Returns the malloc'd block to the OS (only if created by arena_init). */
void arena_destroy(Arena *a);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_REDAC_ARENA_H */
