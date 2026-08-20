#ifndef VIDEO_REDAC_EFFECTS_H
#define VIDEO_REDAC_EFFECTS_H

/*
 * effects.h — the post-processing stack.
 *
 * This is where the architecture pays off most: every effect is a single
 * per-pixel function over two megapixels — precisely the workload GPUs were
 * built for. On a CPU the same stack would cost tens of milliseconds per frame;
 * on the SMs it does not even show above the noise.
 *
 * Effects run in array order, after compositing and before the NV12
 * conversion. Every numeric parameter is a Track, so it can be animated over
 * time (a blur radius can ramp from 0 to 20, for instance).
 */

#include "anim.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FX_NONE = 0,

    /* --- pointwise (no neighbour reads) --- */
    FX_GRAYSCALE,     /* amount                                    */
    FX_INVERT,        /* amount                                    */
    FX_SEPIA,         /* amount                                    */
    FX_POSTERIZE,     /* levels                                    */
    FX_THRESHOLD,     /* level                                     */
    FX_VIGNETTE,      /* amount, radius, softness, color_a         */
    FX_GRAIN,         /* amount                                    */
    FX_SCANLINES,     /* amount, count                             */
    FX_COLOR_GRADE,   /* exposure…tint — the full corrector        */
    FX_VIBRANCE,      /* amount                                    */
    FX_SPLIT_TONE,    /* color_a=shadows, color_b=highlights, balance, amount */
    FX_GRADIENT_MAP,  /* color_a=shadow, color_b=highlight, amount */
    FX_LUT,           /* amount — a .cube table, see `lut` below   */
    FX_LGG,           /* lift / gamma / gain, per channel           */

    /* --- neighbourhood reads (needs a ping-pong buffer) --- */
    FX_BLOOM,         /* level=threshold, radius, amount=intensity */
    FX_BLUR,          /* radius — separable, two passes            */
    FX_PIXELATE,      /* size                                      */
    FX_RGB_SPLIT,     /* amount, angle                             */
    FX_GLITCH,        /* amount                                    */

    FX_TYPE_COUNT
} EffectType;

/* Parameter slots. One shared table for every effect; which slots are read
 * depends on the effect's type (see above). */
typedef enum {
    FXP_AMOUNT = 0,
    FXP_RADIUS,
    FXP_SOFTNESS,
    FXP_ANGLE,
    FXP_COUNT,
    FXP_SIZE,
    FXP_LEVEL,
    FXP_LEVELS,
    FXP_BALANCE,
    FXP_EXPOSURE,
    FXP_BRIGHTNESS,
    FXP_CONTRAST,
    FXP_GAMMA,
    FXP_SATURATION,
    FXP_VIBRANCE,
    FXP_HUE,
    FXP_TEMPERATURE,
    FXP_TINT,

    /*
     * Lift / gamma / gain, three channels each — the three-way corrector every
     * grading tool puts front and centre, because "the shadows are too blue"
     * and "the highlights are too warm" are different complaints and a single
     * brightness control answers neither.
     */
    FXP_LIFT_R, FXP_LIFT_G, FXP_LIFT_B,
    FXP_GAMMA_R, FXP_GAMMA_G, FXP_GAMMA_B,
    FXP_GAIN_R, FXP_GAIN_G, FXP_GAIN_B,

    /*
     * HSL qualifier: which colours an effect is allowed to touch.
     *
     * Structurally the same thing as a power window — a per-pixel coverage
     * mask — and multiplied with it, which is exactly how the two combine in a
     * grading tool: "this hue, but only inside this shape".
     */
    FXP_Q_HUE0, FXP_Q_HUE1,
    FXP_Q_SAT0, FXP_Q_SAT1,
    FXP_Q_LUMA0, FXP_Q_LUMA1,
    FXP_Q_SOFT,

    FXP_MAX
} EffectParam;

/* The tag ("struct Effect") is required: types.h forward-declares it so that
 * EditorContext can hold a pointer without including effects.h. */
typedef struct Effect {
    EffectType type;
    Track      param[FXP_MAX];
    Color      color_a;   /* vignette colour / shadow tone   */
    Color      color_b;   /* highlight tone                  */

    /*
     * FX_LUT only: the table, as size^3 RGB triples.
     *
     * It cannot ride in `param` like everything else, because every other
     * effect's parameters are a handful of scalars that fit in a struct passed
     * by value to the kernel, and a 33-cube is 108 KB. So the table is uploaded
     * once at init and the kernel takes a pointer — `d_lut` on the CUDA
     * backend, and `lut` itself on the CPU one, where they are the same memory.
     */
    char      *lut_path;
    float     *lut;
    int        lut_size;
    void      *d_lut;

    /*
     * Power window: confine this effect to a region of the frame.
     *
     * The one thing missing that separates "you can apply a look" from "you can
     * grade": a colourist's work is almost never global. Darkening a sky,
     * lifting a face, drawing the eye to one corner — all of them are the same
     * correction applied *here and not there*, and without a window the only
     * available answer is to apply it everywhere.
     *
     * Every parameter is a track, because a window that cannot follow its
     * subject is only useful on a locked-off shot.
     *
     * shape 0 = none (the whole frame), 1 = ellipse, 2 = rectangle. Geometry is
     * in canvas fractions, so a window survives a change of resolution.
     */
    int        win_shape;
    Track      win_cx, win_cy, win_rx, win_ry, win_feather;
    bool       win_invert;

    /* Whether an HSL qualifier was given at all — the slots above always hold
     * a full-range default, and a full range is indistinguishable from "no
     * qualifier" except that evaluating one costs something. */
    bool       qual_on;
    bool       qual_invert;
} Effect;

/* "vignette", "color_grade", "rgbSplit"… → EffectType. Unknown → FX_NONE. */
EffectType effect_from_name(const char *name);
const char *effect_name(EffectType t);

/* true if the effect reads neighbouring pixels (and so needs a separate destination). */
bool effect_needs_neighbors(EffectType t);

/* Frees all of the effect's Tracks. */
void effect_free(Effect *fx);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_REDAC_EFFECTS_H */
