#ifndef VIDEO_REDAC_LUT_H
#define VIDEO_REDAC_LUT_H

/*
 * lut.h — Adobe/Iridas `.cube` colour lookup tables.
 *
 * A LUT is how a "look" travels between programs. Every grading tool exports
 * one and every other tool reads it, which makes it the one colour format worth
 * supporting: it is the difference between "you can grade here" and "you can
 * bring the grade you already made".
 *
 * Both 1D and 3D tables are read and both are returned in the same shape — a
 * cube of `size^3` RGB triples — because a 1D table is a 3D one whose three
 * axes happen to be independent, and expanding it on load means the pixel loop
 * has a single case instead of two. The expansion costs memory only for the
 * small sizes 1D tables actually use.
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reads `path` into a freshly allocated cube of size^3 RGB triples (3 floats
 * each, red varying fastest — the order .cube itself stores).
 *
 * Returns false and leaves the outputs untouched on any failure, having said
 * why. The caller owns `*out` and frees it.
 */
bool lut_load_cube(const char *path, float **out, int *size);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_REDAC_LUT_H */
