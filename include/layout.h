#ifndef VIDEO_REDAC_LAYOUT_H
#define VIDEO_REDAC_LAYOUT_H

/*
 * layout.h — relative coordinates and 9-point anchoring.
 *
 * Positions used to be absolute pixels only, so centring an object or measuring
 * from the bottom edge had to be computed outside the JSON, in a generator
 * script. This module lets the JSON say it directly:
 *
 *     "x": "center"          →  horizontally centred
 *     "y": "bottom-160"      →  160 px above the bottom edge
 *     "x": "50%+40"          →  40 px right of the canvas centre
 *     "x": 540, "anchor": "center"
 *
 * Evaluation happens in two stages, because an object's size is only known
 * after rasterization: the expression is stored first and translated into
 * pixels later, once the size exists.
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LAYOUT_AXIS_X = 0,
    LAYOUT_AXIS_Y
} LayoutAxis;

/*
 * Evaluates an expression on the given axis.
 *
 * `canvas`     — the matching canvas dimension (width or height);
 * `out_pos`    — the anchor point's coordinate, in pixels;
 * `out_anchor` — the default anchor (0 = edge, 0.5 = centre, 1 = far edge)
 *                implied by the keyword ("center" → 0.5). Stays 0 when the
 *                expression contains no keyword.
 *
 * Returns false if the expression cannot be parsed (the caller warns).
 *
 * Grammar:  term (('+'|'-') term)*
 *   term := number | number'%' | keyword
 *   keyword: X — left|center|right ; Y — top|middle|center|bottom
 */
bool layout_eval(const char *expr, float canvas, LayoutAxis axis,
                 float *out_pos, float *out_anchor);

/* "center", "bottomright", "top"… → fractions in [0,1]. false → unknown name. */
bool layout_anchor_from_name(const char *name, float *out_ax, float *out_ay);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_REDAC_LAYOUT_H */
