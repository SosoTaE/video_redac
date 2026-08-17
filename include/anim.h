#ifndef VIDEO_REDAC_ANIM_H
#define VIDEO_REDAC_ANIM_H

/*
 * anim.h — the animation core: easing curves and keyframe tracks.
 *
 * This generalises what used to be hard-wired into the timeline actions. Every
 * animation once ran through the same smoothstep; now:
 *
 *   1. any event can pick its own curve — "ease": "backout",
 *   2. any numeric property can be a keyframe array instead of a constant —
 *      "opacity": [{"t":0,"v":0},{"t":1,"v":1}].
 *
 * A Track is sampled in seconds.
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Easing                                                                     */
/* ------------------------------------------------------------------------- */

/* EaseType, Keyframe and Track are defined in types.h (see the note there). */

/* "backout", "easeInOut", "cubic_out"… → EaseType. Unknown → EASE_LINEAR. */
EaseType easing_from_name(const char *name);
const char *easing_name(EaseType e);

/* p ∈ [0,1] → eased p. Some curves (back/elastic) leave the [0,1] range on
 * purpose — that overshoot is what makes the motion feel alive. */
float easing_apply(EaseType e, float p);

/* ------------------------------------------------------------------------- */
/* Keyframe track                                                             */
/* ------------------------------------------------------------------------- */

/*
 * Sampling rules:
 *   - before the first key → the first value,
 *   - after the last key   → the last value,
 *   - in between → interpolate using the *second* key's ease.
 * In other words, easing belongs to the key you are moving toward.
 */

/* Initialise as a constant. */
void track_set_constant(Track *tr, float value);

/* Whether the track actually animates. */
bool track_is_animated(const Track *tr);

/* Value at time `t` (seconds). */
float track_sample(const Track *tr, float t);

/* Frees the keys; safe on NULL and when called repeatedly. */
void track_free(Track *tr);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_REDAC_ANIM_H */
