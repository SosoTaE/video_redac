#ifndef VIDEO_REDAC_PIXEL_OPS_H
#define VIDEO_REDAC_PIXEL_OPS_H

/*
 * pixel_ops.h — every per-pixel operation in the renderer, exactly once.
 *
 * This header is the *single source of truth* for the pixel maths. It compiles
 * two ways:
 *
 *   under nvcc  → each function becomes __device__ __forceinline__ and is
 *                 inlined into a CUDA kernel (src/renderer.cu)
 *   under cc    → each function becomes static inline and is called from a
 *                 plain loop over rows (src/renderer_cpu.c)
 *
 * The reason for the split is maintenance, not portability for its own sake.
 * The project has 16 effects and 17 transitions; writing them twice would mean
 * every new effect had to be implemented twice, and the two copies would
 * inevitably drift apart. Here the *driver* differs (grid launch versus an
 * OpenMP loop) while the arithmetic is shared verbatim.
 *
 * Rules for anything added to this file:
 *
 *   - No allocation, no I/O, no globals. One pixel in, one pixel out.
 *   - C-compatible: structs by pointer, no references, no templates. The CPU
 *     backend is C11 like the rest of the host code.
 *   - Use the vr_* helpers (vr_sat, vr_mini…) and never the CUDA intrinsics
 *     directly, so the CPU build has something to compile.
 *
 * On bit-identity: the GPU and CPU backends produce *visually* identical
 * frames, not byte-identical ones. The GPU contracts a multiply and an add
 * into a single FMA where the CPU performs two separately rounded operations,
 * so results can differ by one ulp. Regression tests must therefore compare
 * GPU against GPU, or CPU against CPU — never one against the other.
 */

#include "effects.h"
#include "types.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>   /* abs() in the blur's tap loop */

/* ------------------------------------------------------------------------- */
/* Compiling for the device or for the host                                   */
/* ------------------------------------------------------------------------- */

#ifdef __CUDACC__
#  define VR_PIX      __device__ __forceinline__
#  define VR_RESTRICT __restrict__
#else
#  define VR_PIX      static inline
#  define VR_RESTRICT restrict
#endif

#ifndef __CUDACC__
/*
 * The CUDA vector types, redeclared for the host build.
 *
 * The layouts match CUDA's (four packed components, natural alignment), so the
 * two backends can share buffers and structs without any conversion. Only the
 * host build sees these — under nvcc the real definitions from
 * vector_types.h win.
 */
typedef struct { unsigned char x, y, z, w; } uchar4;
typedef struct { float x, y, z;            } float3;
typedef struct { float x, y, z, w;         } float4;

static inline uchar4 make_uchar4(unsigned char x, unsigned char y,
                                 unsigned char z, unsigned char w)
{
    uchar4 r; r.x = x; r.y = y; r.z = z; r.w = w; return r;
}
static inline float3 make_float3(float x, float y, float z)
{
    float3 r; r.x = x; r.y = y; r.z = z; return r;
}
static inline float4 make_float4(float x, float y, float z, float w)
{
    float4 r; r.x = x; r.y = y; r.z = z; r.w = w; return r;
}
#endif /* !__CUDACC__ */

/* Integer min/max/clamp under our own names: `min` and `max` are device
 * builtins under nvcc but nothing at all in C, and defining them as macros
 * would collide with the standard library. */
VR_PIX int vr_mini(int a, int b) { return (a < b) ? a : b; }
VR_PIX int vr_maxi(int a, int b) { return (a > b) ? a : b; }
VR_PIX int vr_clampi(int v, int lo, int hi) { return vr_mini(vr_maxi(v, lo), hi); }

/*
 * Clamp to [0,1].
 *
 * On the GPU this is __saturatef — a single free instruction. The host version
 * is written so NaN maps to 0 rather than propagating, which is what the
 * hardware instruction does; a naive `v < 0 ? 0 : v > 1 ? 1 : v` would let a
 * NaN through and produce a garbage pixel on one backend only.
 */
VR_PIX float vr_sat(float v)
{
#ifdef __CUDACC__
    return __saturatef(v);
#else
    if (!(v > 0.0f)) {
        return 0.0f;
    }
    return (v > 1.0f) ? 1.0f : v;
#endif
}

/* float → 8-bit with round-to-nearest, the conversion used everywhere. */
VR_PIX unsigned char vr_u8(float v)
{
    return (unsigned char)(vr_sat(v) * 255.0f + 0.5f);
}

/* ------------------------------------------------------------------------- */
/* Sampling                                                                   */
/* ------------------------------------------------------------------------- */

/* Bilinear sampling from a premultiplied texture; u,v are continuous
 * coordinates in pixel units (a texel's centre sits at i+0.5). */
VR_PIX float4 vr_sample_bilinear(const uchar4 *VR_RESTRICT tex,
                                 int tw, int th, float u, float v)
{
    float fx = u - 0.5f;
    float fy = v - 0.5f;

    int   x0 = (int)floorf(fx);
    int   y0 = (int)floorf(fy);
    float tx = fx - (float)x0;
    float ty = fy - (float)y0;

    int x1 = x0 + 1;
    int y1 = y0 + 1;

    /* Clamp at the edges — wrapping would drag the far side of the glyph in. */
    x0 = vr_clampi(x0, 0, tw - 1);
    x1 = vr_clampi(x1, 0, tw - 1);
    y0 = vr_clampi(y0, 0, th - 1);
    y1 = vr_clampi(y1, 0, th - 1);

    uchar4 p00 = tex[(size_t)y0 * tw + x0];
    uchar4 p10 = tex[(size_t)y0 * tw + x1];
    uchar4 p01 = tex[(size_t)y1 * tw + x0];
    uchar4 p11 = tex[(size_t)y1 * tw + x1];

    const float inv = 1.0f / 255.0f;
    float w00 = (1.0f - tx) * (1.0f - ty);
    float w10 = tx * (1.0f - ty);
    float w01 = (1.0f - tx) * ty;
    float w11 = tx * ty;

    /* Interpolate in premultiplied space — the only correct option on
     * semi-transparent edges (straight alpha dirties the outlines). */
    float4 out;
    out.x = (p00.x * w00 + p10.x * w10 + p01.x * w01 + p11.x * w11) * inv;
    out.y = (p00.y * w00 + p10.y * w10 + p01.y * w01 + p11.y * w11) * inv;
    out.z = (p00.z * w00 + p10.z * w10 + p01.z * w01 + p11.z * w11) * inv;
    out.w = (p00.w * w00 + p10.w * w10 + p01.w * w01 + p11.w * w11) * inv;
    return out;
}

/* ------------------------------------------------------------------------- */
/* RGBA → NV12                                                                */
/* ------------------------------------------------------------------------- */

/*
 * Why this is the single most important optimisation in the pipeline:
 *
 *   A raw RGBA frame at 1080x1920 is 8.29 MB. The same frame as NV12 (4:2:0)
 *   is 3.11 MB — 2.67x less. That saving happens twice: once on the D2H
 *   transfer over PCIe, and again writing into the ffmpeg pipe.
 *
 * No quality is lost: the final file was yuv420p all along — the conversion
 * simply happens somewhere else.
 *
 * One call handles a 2x2 block: four Y samples and one averaged UV pair. The
 * coefficients are BT.709 limited range ("studio swing"), the HD video
 * standard.
 */
VR_PIX void vr_px_nv12(const uchar4 *VR_RESTRICT rgba,
                       uint8_t *VR_RESTRICT y_plane,
                       uint8_t *VR_RESTRICT uv_plane,
                       int width, int height, int bx, int by)
{
    int x = bx * 2;
    int y = by * 2;

    if (x >= width || y >= height) {
        return;
    }

    /* All four pixels; at an edge (odd size) the last one repeats. */
    int x1 = vr_mini(x + 1, width - 1);
    int y1 = vr_mini(y + 1, height - 1);

    uchar4 p00 = rgba[(size_t)y  * width + x];
    uchar4 p10 = rgba[(size_t)y  * width + x1];
    uchar4 p01 = rgba[(size_t)y1 * width + x];
    uchar4 p11 = rgba[(size_t)y1 * width + x1];

    /* --- Y (full resolution) --------------------------------------------- */
    #define VR_LUMA(p) (16.0f + 0.18259f * (p).x + 0.61423f * (p).y + 0.06201f * (p).z)

    y_plane[(size_t)y * width + x] = vr_u8(VR_LUMA(p00) / 255.0f);
    if (x + 1 < width) {
        y_plane[(size_t)y * width + x + 1] = vr_u8(VR_LUMA(p10) / 255.0f);
    }
    if (y + 1 < height) {
        y_plane[(size_t)(y + 1) * width + x] = vr_u8(VR_LUMA(p01) / 255.0f);
        if (x + 1 < width) {
            y_plane[(size_t)(y + 1) * width + x + 1] = vr_u8(VR_LUMA(p11) / 255.0f);
        }
    }
    #undef VR_LUMA

    /* --- UV (half resolution, the 2x2 block's average) -------------------- */
    float r = (p00.x + p10.x + p01.x + p11.x) * 0.25f;
    float g = (p00.y + p10.y + p01.y + p11.y) * 0.25f;
    float b = (p00.z + p10.z + p01.z + p11.z) * 0.25f;

    float u = 128.0f - 0.10064f * r - 0.33857f * g + 0.43922f * b;
    float v = 128.0f + 0.43922f * r - 0.39894f * g - 0.04027f * b;

    size_t uv_idx = ((size_t)by * ((width + 1) / 2) + bx) * 2;
    uv_plane[uv_idx    ] = vr_u8(u / 255.0f);
    uv_plane[uv_idx + 1] = vr_u8(v / 255.0f);
}

/* ------------------------------------------------------------------------- */
/* POST-PROCESSING effects                                                    */
/* ------------------------------------------------------------------------- */

/*
 * The packed form of an effect.
 *
 * Each frame the host samples the Tracks and packs the resulting numbers into
 * this POD struct — so the pixel code never has to walk keyframes.
 */
typedef struct {
    int   type;
    float p[FXP_MAX];
    float ca[4];   /* color_a 0..1 */
    float cb[4];   /* color_b 0..1 */
    int   lut_size;   /* FX_LUT: edge length of the cube; 0 = no table */

    /* Power window, in canvas fractions. shape 0 = the whole frame. */
    int   win_shape;
    float win_cx, win_cy, win_rx, win_ry, win_feather;
    int   win_invert;

    /* HSL qualifier. 0 = every colour passes. */
    int   qual_on, qual_invert;
} EffectGPU;

/* The four converters every effect starts and ends with. They sit here,
 * above the effects themselves, because the qualifier and the window both
 * need luma and a colour load before any effect has run. */

VR_PIX float3 vr_fx_load(uchar4 c)
{
    const float inv = 1.0f / 255.0f;
    return make_float3(c.x * inv, c.y * inv, c.z * inv);
}

VR_PIX uchar4 vr_fx_store(float3 c, unsigned char a)
{
    return make_uchar4(vr_u8(c.x), vr_u8(c.y), vr_u8(c.z), a);
}

/* BT.709 luma — the same weights as in the NV12 conversion. */
VR_PIX float vr_fx_luma(float3 c)
{
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

VR_PIX float3 vr_fx_mix(float3 a, float3 b, float t)
{
    return make_float3(a.x + (b.x - a.x) * t,
                       a.y + (b.y - a.y) * t,
                       a.z + (b.z - a.z) * t);
}

/*
 * How far `v` is inside the band [lo, hi], with a soft edge of width `soft`.
 *
 * 1 well inside, 0 well outside, and a smoothstep between. The softness is what
 * makes a qualifier usable rather than a party trick: a hard selection on a
 * photographic image chatters along every edge where the hue crosses the
 * threshold, and the chatter is far more visible than the correction.
 */
VR_PIX float vr_fx_band(float v, float lo, float hi, float soft)
{
    if (soft <= 1e-6f) {
        return (v >= lo && v <= hi) ? 1.0f : 0.0f;
    }
    float a = (v - (lo - soft)) / soft;      /* rising edge  */
    float b = ((hi + soft) - v) / soft;      /* falling edge */
    float t = fminf(a, b);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/*
 * The qualifier: how much this colour belongs to the selection, 0..1.
 *
 * Hue, saturation and luma are tested separately and multiplied, which is what
 * makes the control usable — "green, but only the saturated ones, and not in
 * the shadows" is three independent thoughts and a colourist adjusts them one
 * at a time.
 *
 * Hue wraps. A selection from 340 to 20 degrees is reds through magenta and has
 * to work; testing it as a plain interval selects everything except reds, which
 * is the exact opposite and looks like a sign error somewhere else entirely.
 */
VR_PIX float vr_fx_qualifier(float3 c, const EffectGPU *fx)
{
    float mx = fmaxf(c.x, fmaxf(c.y, c.z));
    float mn = fminf(c.x, fminf(c.y, c.z));
    float d  = mx - mn;

    float sat = (mx > 1e-6f) ? d / mx : 0.0f;
    float lum = vr_fx_luma(c);

    float hue = 0.0f;
    if (d > 1e-6f) {
        if (mx == c.x)      hue = 60.0f * fmodf((c.y - c.z) / d + 6.0f, 6.0f);
        else if (mx == c.y) hue = 60.0f * ((c.z - c.x) / d + 2.0f);
        else                hue = 60.0f * ((c.x - c.y) / d + 4.0f);
    }

    float soft = fx->p[FXP_Q_SOFT];
    float h0 = fx->p[FXP_Q_HUE0], h1 = fx->p[FXP_Q_HUE1];

    float ch;
    if (h1 >= h0) {
        ch = vr_fx_band(hue, h0, h1, soft * 360.0f);
    } else {
        /* Wrapped: the union of [h0, 360] and [0, h1]. */
        float a = vr_fx_band(hue, h0, 360.0f, soft * 360.0f);
        float b = vr_fx_band(hue, 0.0f, h1,   soft * 360.0f);
        ch = fmaxf(a, b);
    }

    float cs = vr_fx_band(sat, fx->p[FXP_Q_SAT0],  fx->p[FXP_Q_SAT1],  soft);
    float cl = vr_fx_band(lum, fx->p[FXP_Q_LUMA0], fx->p[FXP_Q_LUMA1], soft);

    float cov = ch * cs * cl;
    return fx->qual_invert ? (1.0f - cov) : cov;
}

/*
 * How much of this pixel the window covers, 0..1.
 *
 * A signed-distance formulation rather than an inside/outside test, because the
 * edge is the whole point: a correction that stops at a hard boundary announces
 * itself as a correction. `feather` is the width of the ramp as a fraction of
 * the canvas, and the ellipse's distance is normalised by its own radii so a
 * long thin window feathers evenly along both axes rather than mostly along the
 * short one.
 */
VR_PIX float vr_fx_window(const EffectGPU *fx, int x, int y, int w, int h)
{
    if (fx->win_shape == 0) {
        return 1.0f;
    }

    float nx = ((float)x + 0.5f) / (float)w;
    float ny = ((float)y + 0.5f) / (float)h;

    float rx = (fx->win_rx > 1e-5f) ? fx->win_rx : 1e-5f;
    float ry = (fx->win_ry > 1e-5f) ? fx->win_ry : 1e-5f;

    float dx = (nx - fx->win_cx) / rx;
    float dy = (ny - fx->win_cy) / ry;

    /*
     * `d` is 1 exactly on the boundary and grows outward, in units of "window
     * radii". Converting the feather into the same units is what keeps the ramp
     * the requested width in canvas terms on both axes.
     */
    float d;
    if (fx->win_shape == 1) {
        d = sqrtf(dx * dx + dy * dy);
    } else {
        d = fmaxf(fabsf(dx), fabsf(dy));
    }

    float cov;
    float fe = fx->win_feather;
    if (fe <= 1e-5f) {
        cov = (d <= 1.0f) ? 1.0f : 0.0f;
    } else {
        /* The ramp measured along whichever axis this pixel lies on, so a
         * feather of 0.1 is a tenth of the canvas either way round. */
        float scale = (fx->win_shape == 1)
            ? sqrtf((dx * rx) * (dx * rx) + (dy * ry) * (dy * ry))
            : fmaxf(fabsf(dx) * rx, fabsf(dy) * ry);
        float rad = (d > 1e-6f) ? scale / d : rx;   /* the radius in this direction */
        float t = (d - 1.0f) * rad / fe;            /* 0 at the edge, 1 a feather out */
        if (t <= 0.0f)      cov = 1.0f;
        else if (t >= 1.0f) cov = 0.0f;
        else                cov = 1.0f - t * t * (3.0f - 2.0f * t);
    }

    return fx->win_invert ? (1.0f - cov) : cov;
}

/*
 * Blends an effect's output back toward the untouched frame outside its window.
 *
 * A separate pass rather than a test inside every effect, and that is the whole
 * reason it works on all of them: blur, glitch and pixelate read neighbouring
 * pixels, so "skip this pixel" is not something they can honour — a blur whose
 * taps are half-skipped is not a narrower blur, it is a wrong one. Letting the
 * effect run over the frame and then choosing per pixel how much of it to keep
 * is correct for every effect there is, at the cost of one pass.
 */
VR_PIX void vr_px_fx_window(uchar4 *VR_RESTRICT dst, const uchar4 *VR_RESTRICT keep,
                            int w, int h, const EffectGPU *fx, int x, int y)
{
    size_t i = (size_t)y * w + x;

    float cov = vr_fx_window(fx, x, y, w, h);
    if (fx->qual_on) {
        /*
         * The qualifier is judged on the frame as it was, not as the effect
         * left it. Testing the output would make the selection chase its own
         * correction — push the greens toward cyan and they stop being green,
         * so the selection shrinks, so the push weakens.
         */
        cov *= vr_fx_qualifier(vr_fx_load(keep[i]), fx);
    }
    if (cov >= 1.0f) {
        return;
    }
    if (cov <= 0.0f) {
        dst[i] = keep[i];
        return;
    }
    uchar4 a = keep[i];
    uchar4 b = dst[i];
    dst[i] = make_uchar4(vr_u8((a.x + (b.x - a.x) * cov) * (1.0f / 255.0f)),
                         vr_u8((a.y + (b.y - a.y) * cov) * (1.0f / 255.0f)),
                         vr_u8((a.z + (b.z - a.z) * cov) * (1.0f / 255.0f)),
                         vr_u8((a.w + (b.w - a.w) * cov) * (1.0f / 255.0f)));
}

/*
 * Trilinear lookup in a 3D colour cube.
 *
 * The table maps a colour to a colour: it is the whole of a "look" — the film
 * emulation, the show LUT, the thing a colourist hands over — reduced to a
 * lattice that any program can read. Interpolating between its entries is what
 * makes a 33-cube enough for sixteen million colours.
 *
 * Trilinear rather than tetrahedral. Tetrahedral is the better interpolation
 * and what grading tools use, because it follows the cube's diagonal — the grey
 * axis — exactly, where trilinear drifts slightly off it and can tint a neutral.
 * The drift is well under a code value for the smooth tables that make up
 * nearly every real .cube, and trilinear is eight taps of straightforward
 * arithmetic against a barycentric case analysis. If a LUT ever visibly tints
 * greys here, that is the thing to change.
 *
 * The input is clamped, not wrapped: a colour outside 0..1 has no entry, and
 * the nearest edge of the cube is the only defensible answer.
 */
VR_PIX float3 vr_fx_lut3(const float *VR_RESTRICT lut, int n, float3 c)
{
    float fn = (float)(n - 1);
    float r = vr_sat(c.x) * fn;
    float g = vr_sat(c.y) * fn;
    float b = vr_sat(c.z) * fn;

    int r0 = (int)r, g0 = (int)g, b0 = (int)b;
    if (r0 > n - 2) r0 = (n > 1) ? n - 2 : 0;
    if (g0 > n - 2) g0 = (n > 1) ? n - 2 : 0;
    if (b0 > n - 2) b0 = (n > 1) ? n - 2 : 0;
    int r1 = r0 + 1, g1 = g0 + 1, b1 = b0 + 1;

    float fr = r - (float)r0, fg = g - (float)g0, fb = b - (float)b0;

    /* Red varies fastest — the order .cube stores and lut.c preserves. */
    #define VR_LUT_AT(ri, gi, bi) \
        (&lut[(((size_t)(bi) * n + (size_t)(gi)) * n + (size_t)(ri)) * 3])

    const float *c000 = VR_LUT_AT(r0, g0, b0);
    const float *c100 = VR_LUT_AT(r1, g0, b0);
    const float *c010 = VR_LUT_AT(r0, g1, b0);
    const float *c110 = VR_LUT_AT(r1, g1, b0);
    const float *c001 = VR_LUT_AT(r0, g0, b1);
    const float *c101 = VR_LUT_AT(r1, g0, b1);
    const float *c011 = VR_LUT_AT(r0, g1, b1);
    const float *c111 = VR_LUT_AT(r1, g1, b1);
    #undef VR_LUT_AT

    float3 o;
    /* Three nested lerps, unrolled over the channels. */
    o.x = ((c000[0] * (1 - fr) + c100[0] * fr) * (1 - fg)
         + (c010[0] * (1 - fr) + c110[0] * fr) * fg) * (1 - fb)
        + ((c001[0] * (1 - fr) + c101[0] * fr) * (1 - fg)
         + (c011[0] * (1 - fr) + c111[0] * fr) * fg) * fb;
    o.y = ((c000[1] * (1 - fr) + c100[1] * fr) * (1 - fg)
         + (c010[1] * (1 - fr) + c110[1] * fr) * fg) * (1 - fb)
        + ((c001[1] * (1 - fr) + c101[1] * fr) * (1 - fg)
         + (c011[1] * (1 - fr) + c111[1] * fr) * fg) * fb;
    o.z = ((c000[2] * (1 - fr) + c100[2] * fr) * (1 - fg)
         + (c010[2] * (1 - fr) + c110[2] * fr) * fg) * (1 - fb)
        + ((c001[2] * (1 - fr) + c101[2] * fr) * (1 - fg)
         + (c011[2] * (1 - fr) + c111[2] * fr) * fg) * fb;
    return o;
}

/*
 * Integer hash → a pseudo-random value in [0,1).
 *
 * No separate RNG state is needed: the seed comes from the pixel's coordinate
 * and the frame number, so the result is deterministic (a given frame always
 * gets the same grain) yet changes from frame to frame.
 */
VR_PIX float vr_fx_hash(unsigned int x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return (float)(x & 0x00FFFFFFU) / (float)0x01000000U;
}

/* Evaluates one effect for one pixel (no neighbour reads). */
VR_PIX float3 vr_fx_apply_point(float3 c, const EffectGPU *fx,
                                const float *VR_RESTRICT lut,
                                int x, int y, int w, int h, unsigned int seed)
{
    switch (fx->type) {
        case FX_LGG: {
            /*
             * out = (in + lift * (1 - in)) * gain, then gamma.
             *
             * Lift is scaled by (1 - in) rather than added flat, which is what
             * confines it to the shadows: at white it does nothing, at black it
             * is the whole of the change. A flat offset would move the
             * highlights just as far and there would be no difference between
             * lift and gain at all.
             */
            float3 o = c;
            const float *p = fx->p;
            float ch[3] = { o.x, o.y, o.z };
            for (int k = 0; k < 3; k++) {
                float lift = p[FXP_LIFT_R + k];
                float gain = p[FXP_GAIN_R + k];
                float gam  = p[FXP_GAMMA_R + k];

                float v = ch[k];
                v = v + lift * (1.0f - v);
                v = v * gain;
                if (v < 0.0f) v = 0.0f;
                if (gam > 1e-4f && gam != 1.0f) {
                    v = powf(v, 1.0f / gam);
                }
                ch[k] = v;
            }
            float3 lit = make_float3(ch[0], ch[1], ch[2]);
            return vr_fx_mix(c, lit, vr_sat(fx->p[FXP_AMOUNT]));
        }
        case FX_LUT: {
            if (lut == NULL || fx->lut_size < 2) {
                return c;
            }
            /* `amount` mixes back toward the original, which is how a look is
             * dialled down — the same control a grading tool calls opacity. */
            return vr_fx_mix(c, vr_fx_lut3(lut, fx->lut_size, c),
                             vr_sat(fx->p[FXP_AMOUNT]));
        }
        case FX_GRAYSCALE: {
            float a = vr_sat(fx->p[FXP_AMOUNT]);
            float l = vr_fx_luma(c);
            return vr_fx_mix(c, make_float3(l, l, l), a);
        }
        case FX_INVERT: {
            float a = vr_sat(fx->p[FXP_AMOUNT]);
            return vr_fx_mix(c, make_float3(1.0f - c.x, 1.0f - c.y, 1.0f - c.z), a);
        }
        case FX_SEPIA: {
            float a = vr_sat(fx->p[FXP_AMOUNT]);
            float3 s = make_float3(c.x * 0.393f + c.y * 0.769f + c.z * 0.189f,
                                   c.x * 0.349f + c.y * 0.686f + c.z * 0.168f,
                                   c.x * 0.272f + c.y * 0.534f + c.z * 0.131f);
            return vr_fx_mix(c, s, a);
        }
        case FX_POSTERIZE: {
            float n = fmaxf(2.0f, fx->p[FXP_LEVELS]);
            return make_float3(floorf(c.x * n) / (n - 1.0f),
                               floorf(c.y * n) / (n - 1.0f),
                               floorf(c.z * n) / (n - 1.0f));
        }
        case FX_THRESHOLD: {
            float t = fx->p[FXP_LEVEL];
            float v = (vr_fx_luma(c) >= t) ? 1.0f : 0.0f;
            return make_float3(v, v, v);
        }
        case FX_VIGNETTE: {
            /* Normalised distance from the centre, aspect-corrected. */
            float nx = ((float)x + 0.5f) / (float)w - 0.5f;
            float ny = ((float)y + 0.5f) / (float)h - 0.5f;
            float d  = sqrtf(nx * nx + ny * ny) * 1.41421356f;

            float r    = fmaxf(0.01f, fx->p[FXP_RADIUS]);
            float soft = fmaxf(0.01f, fx->p[FXP_SOFTNESS]);
            float t    = vr_sat((d - r) / soft);
            t = t * t * (3.0f - 2.0f * t);           /* smoothstep */

            float3 tint = make_float3(fx->ca[0], fx->ca[1], fx->ca[2]);
            return vr_fx_mix(c, tint, t * vr_sat(fx->p[FXP_AMOUNT]));
        }
        case FX_GRAIN: {
            float a = fx->p[FXP_AMOUNT];
            float n = vr_fx_hash((unsigned int)(y * w + x) * 2654435761U + seed) - 0.5f;
            return make_float3(c.x + n * a, c.y + n * a, c.z + n * a);
        }
        case FX_SCANLINES: {
            float a     = vr_sat(fx->p[FXP_AMOUNT]);
            float count = fmaxf(1.0f, fx->p[FXP_COUNT]);
            float s     = sinf((float)y / (float)h * count * 3.14159265f);
            float k     = 1.0f - a * 0.5f * (1.0f - s * s);
            return make_float3(c.x * k, c.y * k, c.z * k);
        }
        case FX_VIBRANCE: {
            /* Boosts less saturated colours more — skin tones are protected. */
            float a   = fx->p[FXP_AMOUNT];
            float mx  = fmaxf(c.x, fmaxf(c.y, c.z));
            float mn  = fminf(c.x, fminf(c.y, c.z));
            float sat = mx - mn;
            float l   = vr_fx_luma(c);
            return vr_fx_mix(make_float3(l, l, l), c, 1.0f + a * (1.0f - sat));
        }
        case FX_SPLIT_TONE: {
            /* One tone for the shadows, another for the highlights (teal/orange). */
            float l   = vr_fx_luma(c);
            float bal = fx->p[FXP_BALANCE];
            float t   = vr_sat((l - bal) * 2.0f + 0.5f);
            float3 sh = make_float3(fx->ca[0], fx->ca[1], fx->ca[2]);
            float3 hi = make_float3(fx->cb[0], fx->cb[1], fx->cb[2]);
            float3 tone = vr_fx_mix(sh, hi, t);
            /* a soft-light-ish blend, so luminance is preserved */
            float3 blended = make_float3(c.x * (0.5f + tone.x), c.y * (0.5f + tone.y),
                                         c.z * (0.5f + tone.z));
            return vr_fx_mix(c, blended, vr_sat(fx->p[FXP_AMOUNT]));
        }
        case FX_GRADIENT_MAP: {
            float l = vr_fx_luma(c);
            float3 sh = make_float3(fx->ca[0], fx->ca[1], fx->ca[2]);
            float3 hi = make_float3(fx->cb[0], fx->cb[1], fx->cb[2]);
            return vr_fx_mix(c, vr_fx_mix(sh, hi, l), vr_sat(fx->p[FXP_AMOUNT]));
        }
        case FX_COLOR_GRADE: {
            /* The order mirrors a colourist's workflow: exposure → contrast →
             * gamma → temperature → saturation → hue. */
            float e = fx->p[FXP_EXPOSURE];
            if (e != 0.0f) {
                float k = exp2f(e);
                c = make_float3(c.x * k, c.y * k, c.z * k);
            }
            float br = fx->p[FXP_BRIGHTNESS];
            if (br != 0.0f) {
                c = make_float3(c.x + br, c.y + br, c.z + br);
            }
            float ct = fx->p[FXP_CONTRAST];
            if (ct != 1.0f) {
                c = make_float3((c.x - 0.5f) * ct + 0.5f,
                                (c.y - 0.5f) * ct + 0.5f,
                                (c.z - 0.5f) * ct + 0.5f);
            }
            float gm = fx->p[FXP_GAMMA];
            if (gm > 0.0f && gm != 1.0f) {
                float ig = 1.0f / gm;
                c = make_float3(powf(fmaxf(c.x, 0.0f), ig),
                                powf(fmaxf(c.y, 0.0f), ig),
                                powf(fmaxf(c.z, 0.0f), ig));
            }
            float tmp = fx->p[FXP_TEMPERATURE];
            float tnt = fx->p[FXP_TINT];
            if (tmp != 0.0f || tnt != 0.0f) {
                c = make_float3(c.x + tmp * 0.2f, c.y + tnt * 0.2f, c.z - tmp * 0.2f);
            }
            float sa = fx->p[FXP_SATURATION];
            if (sa != 1.0f) {
                float l = vr_fx_luma(c);
                c = vr_fx_mix(make_float3(l, l, l), c, sa);
            }
            float vb = fx->p[FXP_VIBRANCE];
            if (vb != 0.0f) {
                float mx = fmaxf(c.x, fmaxf(c.y, c.z));
                float mn = fminf(c.x, fminf(c.y, c.z));
                float l  = vr_fx_luma(c);
                c = vr_fx_mix(make_float3(l, l, l), c, 1.0f + vb * (1.0f - (mx - mn)));
            }
            float hu = fx->p[FXP_HUE];
            if (hu != 0.0f) {
                /* Rotation about the hue axis (a YIQ approximation). */
                float a  = hu * 3.14159265f / 180.0f;
                float cs = cosf(a), sn = sinf(a);
                float3 o = c;
                c.x = o.x * (0.299f + 0.701f * cs + 0.168f * sn) +
                      o.y * (0.587f - 0.587f * cs + 0.330f * sn) +
                      o.z * (0.114f - 0.114f * cs - 0.497f * sn);
                c.y = o.x * (0.299f - 0.299f * cs - 0.328f * sn) +
                      o.y * (0.587f + 0.413f * cs + 0.035f * sn) +
                      o.z * (0.114f - 0.114f * cs + 0.292f * sn);
                c.z = o.x * (0.299f - 0.300f * cs + 1.250f * sn) +
                      o.y * (0.587f - 0.588f * cs - 1.050f * sn) +
                      o.z * (0.114f + 0.886f * cs - 0.203f * sn);
            }
            return c;
        }
        default:
            return c;
    }
}

/* One pixel of the pointwise pass — several effects share it. */
VR_PIX void vr_px_fx_point(uchar4 *VR_RESTRICT dst, const uchar4 *VR_RESTRICT src,
                           int w, int h, const EffectGPU *fx,
                           const float *VR_RESTRICT lut, unsigned int seed,
                           int x, int y)
{
    size_t i = (size_t)y * w + x;
    uchar4 s = src[i];
    dst[i]   = vr_fx_store(vr_fx_apply_point(vr_fx_load(s), fx, lut,
                                             x, y, w, h, seed), s.w);
}

/*
 * Bloom, first half: keep only what is brighter than the threshold.
 *
 * The excess is kept, not the pixel — a pixel at 0.8 against a threshold of
 * 0.75 contributes 0.05, not 0.8. That is what makes the effect scale with how
 * bright a thing actually is instead of turning everything above the line into
 * the same glow, and it is why a specular glint blooms and a white wall does
 * not.
 *
 * The ramp is softened over a small knee. A hard threshold makes the bloom
 * appear and disappear as a highlight drifts across it, which on moving footage
 * reads as flicker.
 */
VR_PIX void vr_px_fx_bloom_cut(uchar4 *VR_RESTRICT dst, const uchar4 *VR_RESTRICT src,
                               int w, int h, float threshold, int x, int y)
{
    (void)h;
    size_t i = (size_t)y * w + x;
    float3 c = vr_fx_load(src[i]);
    float  l = vr_fx_luma(c);

    const float knee = 0.10f;
    float t;
    if (l <= threshold)             t = 0.0f;
    else if (l >= threshold + knee) t = 1.0f;
    else {
        float u = (l - threshold) / knee;
        t = u * u * (3.0f - 2.0f * u);
    }

    float excess = (l > threshold) ? (l - threshold) * t : 0.0f;
    /* Scaled by the pixel's own colour, normalised by its luma, so the glow
     * takes the hue of whatever is glowing rather than coming out white. */
    float k = (l > 1e-4f) ? excess / l : 0.0f;
    dst[i] = vr_fx_store(make_float3(c.x * k, c.y * k, c.z * k), src[i].w);
}

/* Bloom, second half: the blurred excess added back on top. */
VR_PIX void vr_px_fx_bloom_add(uchar4 *VR_RESTRICT dst, const uchar4 *VR_RESTRICT src,
                               const uchar4 *VR_RESTRICT glow, int w, int h,
                               float intensity, int x, int y)
{
    (void)h;
    size_t i = (size_t)y * w + x;
    float3 c = vr_fx_load(src[i]);
    float3 g = vr_fx_load(glow[i]);
    dst[i] = vr_fx_store(make_float3(c.x + g.x * intensity,
                                     c.y + g.y * intensity,
                                     c.z + g.z * intensity), src[i].w);
}

/*
 * Separable blur.
 *
 * Two one-dimensional passes read O(2r) samples per pixel where a single
 * two-dimensional pass would read O(r²). At r=20 that is 40 versus 1600 —
 * forty times less.
 */
VR_PIX void vr_px_fx_blur(uchar4 *VR_RESTRICT dst, const uchar4 *VR_RESTRICT src,
                          int w, int h, int radius, int horizontal, int x, int y)
{
    float3 sum  = make_float3(0.0f, 0.0f, 0.0f);
    float  wsum = 0.0f;

    for (int k = -radius; k <= radius; k++) {
        int sx = horizontal ? vr_clampi(x + k, 0, w - 1) : x;
        int sy = horizontal ? y : vr_clampi(y + k, 0, h - 1);

        /* Triangular weights — a cheap Gaussian approximation; after two
         * passes the kernel is effectively Gaussian. */
        float wt = (float)(radius + 1 - abs(k));
        float3 c = vr_fx_load(src[(size_t)sy * w + sx]);
        sum.x += c.x * wt; sum.y += c.y * wt; sum.z += c.z * wt;
        wsum  += wt;
    }

    size_t i = (size_t)y * w + x;
    float inv = (wsum > 0.0f) ? 1.0f / wsum : 1.0f;
    dst[i] = vr_fx_store(make_float3(sum.x * inv, sum.y * inv, sum.z * inv), src[i].w);
}

VR_PIX void vr_px_fx_pixelate(uchar4 *VR_RESTRICT dst, const uchar4 *VR_RESTRICT src,
                              int w, int h, int size, int x, int y)
{
    if (size < 1) {
        size = 1;
    }

    /* Sample the block's centre — visually close to averaging the block, but
     * one read instead of size². */
    int bx = (x / size) * size + size / 2;
    int by = (y / size) * size + size / 2;
    bx = vr_mini(bx, w - 1);
    by = vr_mini(by, h - 1);

    dst[(size_t)y * w + x] = src[(size_t)by * w + bx];
}

/* Chromatic aberration — the channels drift apart. */
VR_PIX void vr_px_fx_rgb_split(uchar4 *VR_RESTRICT dst, const uchar4 *VR_RESTRICT src,
                               int w, int h, float amount, float angle_deg, int x, int y)
{
    float a  = angle_deg * 3.14159265f / 180.0f;
    float dx = cosf(a) * amount;
    float dy = sinf(a) * amount;

    int rx = vr_clampi((int)lrintf((float)x + dx), 0, w - 1);
    int ry = vr_clampi((int)lrintf((float)y + dy), 0, h - 1);
    int bx = vr_clampi((int)lrintf((float)x - dx), 0, w - 1);
    int by = vr_clampi((int)lrintf((float)y - dy), 0, h - 1);

    size_t i = (size_t)y * w + x;
    dst[i] = make_uchar4(src[(size_t)ry * w + rx].x,
                         src[i].y,
                         src[(size_t)by * w + bx].z,
                         src[i].w);
}

/* Random horizontal band displacement + channel separation. */
VR_PIX void vr_px_fx_glitch(uchar4 *VR_RESTRICT dst, const uchar4 *VR_RESTRICT src,
                            int w, int h, float amount, unsigned int seed, int x, int y)
{
    (void)h; /* glitch shifts along rows only — the height is never needed */

    int   band = y / 12;                                  /* 12-pixel bands */
    float r    = vr_fx_hash((unsigned int)band * 9781U + seed);

    int shift = 0;
    if (r > 0.75f) {                                      /* a quarter of the bands shift */
        shift = (int)((vr_fx_hash((unsigned int)band * 6151U + seed) - 0.5f) * amount * w);
    }

    int sx = vr_clampi(x + shift, 0, w - 1);
    size_t i = (size_t)y * w + x;

    uchar4 c = src[(size_t)y * w + sx];
    if (r > 0.9f) {
        int gx = vr_clampi(sx + (int)(amount * 12.0f), 0, w - 1);
        c.y = src[(size_t)y * w + gx].y;
    }
    dst[i] = c;
}

/* ------------------------------------------------------------------------- */
/* Transition compositor                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Two already-rendered scenes combined into one frame.
 *
 * Each side has an inverse transform (translation + scale + rotation) and an
 * optional mask. It works by backward mapping, just like the texture
 * compositor: for every *output* pixel it looks up where to read from.
 *
 * `from` is drawn first, `to` on top. That is why a crossfade is simply `to`
 * growing more opaque, and a slide is `to` being translated in.
 */
typedef struct {
    float opacity;
    float ia, ib, ic, id;   /* inverse 2x2 */
    float tx, ty;           /* translation in pixels */
    int   mask;             /* 0 = none, 1 = circle, 2 = rectangle */
    float m0, m1, m2, m3;   /* circle: cx,cy,r | rect: x,y,w,h (canvas fractions) */
} TransSide;

typedef struct {
    int       w, h;
    uchar4    bg;
    TransSide from, to;
} TransParams;

VR_PIX bool vr_trans_mask_ok(const TransSide *s, float nx, float ny)
{
    if (s->mask == 1) {
        float dx = nx - s->m0, dy = ny - s->m1;
        return (dx * dx + dy * dy) <= (s->m2 * s->m2);
    }
    if (s->mask == 2) {
        return nx >= s->m0 && nx <= s->m0 + s->m2 && ny >= s->m1 && ny <= s->m1 + s->m3;
    }
    return true;
}

/* Samples one side; returns false if this scene does not cover the pixel. */
VR_PIX bool vr_trans_sample(const uchar4 *VR_RESTRICT src,
                            const TransSide *s, int w, int h,
                            float px, float py, float4 *out)
{
    if (s->opacity <= 0.0f) {
        return false;
    }

    float nx = px / (float)w, ny = py / (float)h;
    if (!vr_trans_mask_ok(s, nx, ny)) {
        return false;
    }

    float cx = (float)w * 0.5f, cy = (float)h * 0.5f;
    float dx = px - cx - s->tx;
    float dy = py - cy - s->ty;

    float u = cx + (s->ia * dx + s->ib * dy);
    float v = cy + (s->ic * dx + s->id * dy);

    if (u < 0.0f || u >= (float)w || v < 0.0f || v >= (float)h) {
        return false;   /* the scene has slid off-canvas — the background shows */
    }

    *out = vr_sample_bilinear(src, w, h, u, v);
    return true;
}

VR_PIX void vr_px_transition(uchar4 *VR_RESTRICT dst,
                             const uchar4 *VR_RESTRICT from,
                             const uchar4 *VR_RESTRICT to,
                             const TransParams *p, int x, int y)
{
    float px = (float)x + 0.5f;
    float py = (float)y + 0.5f;

    const float inv = 1.0f / 255.0f;
    float3 acc = make_float3(p->bg.x * inv, p->bg.y * inv, p->bg.z * inv);

    float4 c;
    if (vr_trans_sample(from, &p->from, p->w, p->h, px, py, &c)) {
        float a = vr_sat(p->from.opacity);
        acc.x = c.x * a + acc.x * (1.0f - a);
        acc.y = c.y * a + acc.y * (1.0f - a);
        acc.z = c.z * a + acc.z * (1.0f - a);
    }
    if (vr_trans_sample(to, &p->to, p->w, p->h, px, py, &c)) {
        float a = vr_sat(p->to.opacity);
        acc.x = c.x * a + acc.x * (1.0f - a);
        acc.y = c.y * a + acc.y * (1.0f - a);
        acc.z = c.z * a + acc.z * (1.0f - a);
    }

    dst[(size_t)y * p->w + x] =
        make_uchar4(vr_u8(acc.x), vr_u8(acc.y), vr_u8(acc.z), 255);
}

/* ------------------------------------------------------------------------- */
/* Texture compositor                                                         */
/* ------------------------------------------------------------------------- */

/*
 * Compositing parameters. A struct so the argument list stops growing — CUDA
 * passes it through the constant bank anyway.
 */
typedef struct {
    int   fb_w, fb_h;
    int   tex_w, tex_h;

    /* Destination centre, in frame coordinates. */
    float cx, cy;

    /*
     * Perspective. When false the affine `inv_*` matrix below is used and this
     * struct behaves exactly as it always has; when true, `hinv` replaces it.
     *
     * Two paths rather than one general one, deliberately: the homography
     * reduces to the affine case algebraically but not bit-for-bit, and every
     * existing project would shift by a rounding step for no reason.
     */
    int   persp;
    float hinv[9];    /* inverse homography, row-major */

    /*
     * Angle shading: how square-on the quad is to the viewer, 1 = face on.
     *
     * A flat quad carries no lighting of its own, so a card turning in space
     * reads as a shape that merely narrows — the eye expects it to darken as it
     * turns away. One multiply per pixel buys that, and `shade` is 1 whenever
     * the feature is off, so the multiply costs nothing to skip.
     */
    float shade;

    /*
     * Inverse 2x2 matrix: from frame space into texture space.
     *
     * The forward transform is  d = R(θ) · S · t  (scale first, then rotate).
     * The work is done backwards — for every *destination* pixel it looks up
     * where to read from. That yields a result without holes (forward
     * projection would leave scattered pixels when rotating).
     */
    float inv_a, inv_b, inv_c, inv_d;

    /* Destination bounding box (already clipped to the screen). */
    int   bb_x, bb_y, bb_w, bb_h;

    float alpha;

    /*
     * Tint: blend the layer's colour toward (tint_r, tint_g, tint_b) by
     * `tint_amount`. 0 is the overwhelmingly common case and costs one
     * comparison.
     */
    float tint_amount;
    float tint_r, tint_g, tint_b;

    /*
     * How the layer is combined with what is already there.
     * 0 = source-over (normal), 1 = additive, 2 = screen.
     *
     * Additive is what makes a mass of particles or a glow read as *light*:
     * overlapping sprites brighten toward white instead of each hiding the one
     * behind it. Screen is its gentler cousin — it saturates rather than
     * clipping, so a bright background does not blow out.
     */
    int   blend;

    /*
     * Clip mask in texture space, as fractions of the texture.
     * 0 = none, 1 = circle (m[0],m[1] centre, m[2] radius), 2 = rect.
     */
    int   mask_shape;
    float mask[4];
    int   mask_invert;

    /* TYPEWRITE: line geometry in texture space. */
    float pad_y, line_height;
    int   line_count;

    /*
     * Chroma key: knock a background colour out of the layer.
     *
     * It lives in the compositor rather than in the effect stack because it is
     * a property of *one layer*, not of the finished frame. A green screen is
     * green in the clip and nowhere else; running it over the composite would
     * key the same green out of everything behind it too.
     *
     * `key_dom` is which channel the key colour is strongest in, worked out on
     * the host. Spill suppression needs it and it never changes per pixel.
     */
    int   key_on;
    float key_r, key_g, key_b;   /* the colour to remove, 0..1 */
    float key_tol;               /* chroma distance fully keyed out */
    float key_soft;              /* width of the ramp beyond it     */
    float key_spill;             /* 0 = leave the fringe, 1 = full  */
    int   key_dom;               /* 0 = r, 1 = g, 2 = b             */
} CompositeParams;

/*
 * A highlight band: a filled rectangle covering a range of text lines, drawn
 * behind the glyphs the way an editor marks the current line.
 *
 * It reuses `CompositeParams` verbatim rather than carrying its own geometry.
 * That is the whole point: the band is back-projected through exactly the same
 * inverse matrix as the text it sits behind, so it inherits the widget's
 * position, scale and rotation for free and can never drift out of alignment
 * with the lines it is marking.
 */
typedef struct {
    CompositeParams geom;     /* the owning widget's transform */
    float           r, g, b;  /* 0..1, premultiplied by `alpha` */
    float           alpha;    /* 0 → nothing to draw */
    int             first_line, last_line; /* inclusive, 0-based */

    /*
     * The code block's panel, used purely as a stencil.
     *
     * A band is a rectangle in texture space, but the panel behind it has
     * rounded corners — so on the first and last line the band's square corners
     * poke out past the panel and the whole thing looks broken. Multiplying the
     * band's coverage by the panel's own alpha clips it to whatever shape the
     * panel actually has, corners included, for the cost of one texture read.
     *
     * NULL for plain text, which has no panel and should fill its line box.
     */
    int plate_w, plate_h;
} HighlightParams;

/*
 * One pixel of the band. (i,j) indexes the widget's destination bounding box,
 * the same as vr_px_composite. `plate` may be NULL — see HighlightParams.
 */
VR_PIX void vr_px_highlight(uchar4 *VR_RESTRICT fb, const uchar4 *VR_RESTRICT plate,
                            const HighlightParams *p, int i, int j)
{
    const CompositeParams *g = &p->geom;

    int gx = g->bb_x + i;
    int gy = g->bb_y + j;

    if (gx < 0 || gx >= g->fb_w || gy < 0 || gy >= g->fb_h) {
        return;
    }

    /* Back-project into texture space — identical to the compositor. */
    float dx = (float)gx + 0.5f - g->cx;
    float dy = (float)gy + 0.5f - g->cy;

    float tx = g->inv_a * dx + g->inv_b * dy;
    float ty = g->inv_c * dx + g->inv_d * dy;

    float u = tx + (float)g->tex_w * 0.5f;
    float v = ty + (float)g->tex_h * 0.5f;

    if (u < 0.0f || u >= (float)g->tex_w || v < 0.0f || v >= (float)g->tex_h) {
        return;
    }

    /* Is this row inside the marked lines? */
    float top    = g->pad_y + (float)p->first_line * g->line_height;
    float bottom = g->pad_y + (float)(p->last_line + 1) * g->line_height;
    if (v < top || v >= bottom) {
        return;
    }

    float a = vr_sat(p->alpha);
    float r = p->r, gg = p->g, bb = p->b;

    /*
     * Clip to the panel's shape. Both textures are composited onto the same
     * destination rectangle, so normalised coordinates carry across even when
     * their pixel dimensions differ.
     */
    if (plate != NULL && p->plate_w > 0 && p->plate_h > 0) {
        float pu = u / (float)g->tex_w * (float)p->plate_w;
        float pv = v / (float)g->tex_h * (float)p->plate_h;

        float cov = vr_sample_bilinear(plate, p->plate_w, p->plate_h, pu, pv).w;
        a  *= cov;
        r  *= cov;   /* the colour is premultiplied, so it scales too */
        gg *= cov;
        bb *= cov;
    }

    if (a <= 0.0f) {
        return;
    }

    /*
     * Source-over with a premultiplied source: dst = src + dst*(1-a).
     *
     * `p->r/g/b` already carry the alpha (vr_highlight_setup premultiplies
     * them), so they must NOT be scaled by `a` a second time here — doing that
     * squares the alpha on the colour while leaving the coverage alone, and the
     * band comes out several times too dim to see.
     */
    size_t idx = (size_t)gy * g->fb_w + gx;
    uchar4 d   = fb[idx];

    const float inv = 1.0f / 255.0f;
    float       ia  = 1.0f - a;

    fb[idx] = make_uchar4(vr_u8(r  + (float)d.x * inv * ia),
                          vr_u8(gg + (float)d.y * inv * ia),
                          vr_u8(bb + (float)d.z * inv * ia),
                          vr_u8(a  + (float)d.w * inv * ia));
}

/*
 * Composites one texture pixel onto the frame, with scale and rotation.
 *
 * (i,j) indexes the *destination bounding box*, not the whole screen — for a
 * small title on a 1080x1920 canvas that is 50x fewer iterations.
 *
 * The blend (premultiplied source-over):
 *      dst = src * alpha + dst * (1 - src.a * alpha)
 * `alpha` is the widget's fade — multiplying a premultiplied colour by it
 * directly is correct, since it scales both the colour and the coverage.
 */
VR_PIX void vr_px_composite(uchar4 *VR_RESTRICT fb,
                            const uchar4 *VR_RESTRICT tex,
                            const float *VR_RESTRICT cutoff_x,
                            const CompositeParams *p, int i, int j)
{
    int gx = p->bb_x + i;
    int gy = p->bb_y + j;

    if (gx < 0 || gx >= p->fb_w || gy < 0 || gy >= p->fb_h) {
        return;
    }

    /* Pixel centre → a vector from the object's centre, in frame space. */
    float dx = (float)gx + 0.5f - p->cx;
    float dy = (float)gy + 0.5f - p->cy;

    float u, v;

    if (p->persp) {
        /*
         * Backward mapping through the inverse homography.
         *
         * A flat quad in space projects to the screen by a homography, so the
         * way back is another one — the same "for every destination pixel, ask
         * where it came from" as the affine case, with a divide added.
         */
        float hx = p->hinv[0] * dx + p->hinv[1] * dy + p->hinv[2];
        float hy = p->hinv[3] * dx + p->hinv[4] * dy + p->hinv[5];
        float hw = p->hinv[6] * dx + p->hinv[7] * dy + p->hinv[8];

        /* w <= 0 is the part of the plane at or behind the viewer; there is no
         * meaningful texel there, and dividing would fold it back into view. */
        if (hw <= 1e-6f) {
            return;
        }
        u = hx / hw;
        v = hy / hw;
    } else {
        /* Backward projection into texture space (from the centre). */
        float tx = p->inv_a * dx + p->inv_b * dy;
        float ty = p->inv_c * dx + p->inv_d * dy;

        /* Switch from centre-relative to top-left-relative coordinates. */
        u = tx + (float)p->tex_w * 0.5f;
        v = ty + (float)p->tex_h * 0.5f;
    }

    if (u < 0.0f || u >= (float)p->tex_w || v < 0.0f || v >= (float)p->tex_h) {
        return; /* the texture does not cover this pixel */
    }

    /*
     * Clip mask. Evaluated in normalised texture space, so it scales and
     * rotates with the object instead of staying stuck to the screen.
     */
    if (p->mask_shape != 0) {
        float nx = u / (float)p->tex_w;
        float ny = v / (float)p->tex_h;
        bool  in;

        if (p->mask_shape == 1) {
            float mdx = nx - p->mask[0], mdy = ny - p->mask[1];
            in = (mdx * mdx + mdy * mdy) <= (p->mask[2] * p->mask[2]);
        } else {
            in = nx >= p->mask[0] && nx <= p->mask[0] + p->mask[2] &&
                 ny >= p->mask[1] && ny <= p->mask[1] + p->mask[3];
        }
        if (p->mask_invert) {
            in = !in;
        }
        if (!in) {
            return;
        }
    }

    /*
     * TYPEWRITE — clipping exactly on a character boundary.
     *
     * The host has already computed an x threshold per line (up to which
     * character's right edge the text is visible). All that remains here is
     * working out which line this pixel belongs to.
     */
    if (cutoff_x != NULL && p->line_count > 0) {
        int line = (int)floorf((v - p->pad_y) / p->line_height);
        line = vr_clampi(line, 0, p->line_count - 1);
        if (u > cutoff_x[line]) {
            return;
        }
    }

    float4 src = vr_sample_bilinear(tex, p->tex_w, p->tex_h, u, v);

    /*
     * Chroma key.
     *
     * The comparison is in the chroma plane only — brightness is discarded —
     * because a lit backdrop is never one colour. The top of a green screen is
     * pale green and the bottom is dark green, and a test on RGB distance keys
     * one of them and leaves the other; the two differ almost entirely in
     * luma, and hardly at all in Cb/Cr.
     *
     * The result is a coverage multiplier, so a partly-keyed pixel comes out
     * partly transparent. That is what makes hair and motion blur survive: they
     * are genuine blends of subject and backdrop, and a hard in/out test can
     * only round them to one or the other.
     */
    if (p->key_on && src.w > 1e-4f) {
        /* The sample is premultiplied; chroma is a property of the colour
         * itself, so it has to be divided back out before measuring. */
        float ia = 1.0f / src.w;
        float cr = src.x * ia, cg = src.y * ia, cb = src.z * ia;

        float ly = vr_fx_luma(make_float3(cr, cg, cb));
        float lk = vr_fx_luma(make_float3(p->key_r, p->key_g, p->key_b));

        /* BT.709 chroma axes, the same primaries as everything else here. */
        float pcb = (cb - ly) * (1.0f / 1.8556f);
        float pcr = (cr - ly) * (1.0f / 1.5748f);
        float kcb = (p->key_b - lk) * (1.0f / 1.8556f);
        float kcr = (p->key_r - lk) * (1.0f / 1.5748f);

        float ddb = pcb - kcb, ddr = pcr - kcr;
        float dist = sqrtf(ddb * ddb + ddr * ddr);

        float cov;
        if (dist <= p->key_tol) {
            cov = 0.0f;
        } else if (dist >= p->key_tol + p->key_soft) {
            cov = 1.0f;
        } else {
            float t = (dist - p->key_tol) / p->key_soft;
            cov = t * t * (3.0f - 2.0f * t);      /* smoothstep */
        }

        if (cov <= 0.0f) {
            return;                                /* wholly background */
        }

        /*
         * Spill suppression, on the partly-keyed fringe.
         *
         * A subject in front of a green screen is lit by green bounced off it,
         * so its edges carry a green rim that no keying threshold removes — the
         * rim is the subject's colour, merely tinted. Clamping the key's own
         * channel to the average of the other two takes the tint out while
         * leaving the pixel's brightness, which is why it reads as a fixed edge
         * rather than a dark one.
         */
        if (p->key_spill > 0.0f) {
            float ch[3] = { cr, cg, cb };
            int   d0 = p->key_dom;
            float other = (ch[(d0 + 1) % 3] + ch[(d0 + 2) % 3]) * 0.5f;
            if (ch[d0] > other) {
                ch[d0] += (other - ch[d0]) * p->key_spill;
                cr = ch[0]; cg = ch[1]; cb = ch[2];
            }
        }

        /* Back to premultiplied, with the keyed coverage folded in. */
        src.w *= cov;
        src.x = cr * src.w;
        src.y = cg * src.w;
        src.z = cb * src.w;
    }

    /*
     * Tint, before the fade is applied.
     *
     * The sample is premultiplied, so the target colour has to be multiplied by
     * the sample's own alpha before mixing — otherwise a tint would light up
     * the transparent area around a glyph and the text would gain a coloured
     * box.
     */
    if (p->tint_amount > 0.0f) {
        float t = p->tint_amount;
        src.x += (p->tint_r * src.w - src.x) * t;
        src.y += (p->tint_g * src.w - src.y) * t;
        src.z += (p->tint_b * src.w - src.z) * t;
    }

    /* Angle shading, before the fade — it darkens the surface, not the
     * coverage, so alpha is deliberately left alone. */
    if (p->shade < 1.0f) {
        src.x *= p->shade;
        src.y *= p->shade;
        src.z *= p->shade;
    }

    src.x *= p->alpha;
    src.y *= p->alpha;
    src.z *= p->alpha;
    src.w *= p->alpha;

    if (src.w <= 0.0f) {
        return; /* fully transparent — skip the read-modify-write */
    }

    size_t idx = (size_t)gy * p->fb_w + gx;
    uchar4 d   = fb[idx];

    const float inv = 1.0f / 255.0f;
    float dr = (float)d.x * inv, dg = (float)d.y * inv;
    float db = (float)d.z * inv, da = (float)d.w * inv;

    float r, g, b, a;

    if (p->blend == 1) {
        /* Additive: the source is already premultiplied, so it adds directly. */
        r = src.x + dr;
        g = src.y + dg;
        b = src.z + db;
        a = src.w + da;
    } else if (p->blend == 2) {
        /* Screen: 1-(1-s)(1-d), which approaches 1 instead of exceeding it. */
        r = 1.0f - (1.0f - vr_sat(src.x)) * (1.0f - dr);
        g = 1.0f - (1.0f - vr_sat(src.y)) * (1.0f - dg);
        b = 1.0f - (1.0f - vr_sat(src.z)) * (1.0f - db);
        a = src.w + da * (1.0f - src.w);
    } else {
        float inv_src = 1.0f - src.w;
        r = src.x + dr * inv_src;
        g = src.y + dg * inv_src;
        b = src.z + db * inv_src;
        a = src.w + da * inv_src;
    }

    fb[idx] = make_uchar4(vr_u8(r), vr_u8(g), vr_u8(b), vr_u8(a));
}

/* ------------------------------------------------------------------------- */
/* Mesh rasterization                                                         */
/* ------------------------------------------------------------------------- */

/*
 * One triangle, already transformed and projected by the host.
 *
 * Screen x/y in pixels, `z` the camera-space depth used for the depth test, and
 * a flat colour the host derived from the face normal. Keeping the shading on
 * the host means the pixel loop does no lighting maths at all — it only decides
 * which triangle is nearest.
 */
typedef struct {
    float x0, y0, z0;
    float x1, y1, z1;
    float x2, y2, z2;

    /* Flat colour, used when the mesh has neither normals nor a texture. */
    float r, g, b;

    /*
     * Per-vertex normals, in view space and pointing outward.
     *
     * The host used to resolve the lighting here and hand down a scalar per
     * vertex, which is Gouraud shading: cheap, and wrong wherever the lighting
     * varies faster than the mesh is subdivided. A specular highlight smaller
     * than a triangle was the case that made it untenable — it landed on
     * whichever vertices happened to catch it and came out a faceted lozenge.
     *
     * Interpolating the normal instead and lighting each pixel costs the inner
     * loop a normalise and a short loop over the lights, and buys a highlight
     * that is round on a coarse mesh — and the one thing a normal map cannot do
     * without, since its whole purpose is to change the normal per pixel.
     *
     * Flat-shaded and normal-less meshes put the face normal in all three, so
     * the interpolation is constant and the result is exactly flat shading.
     */
    float n0x, n0y, n0z;
    float n1x, n1y, n1z;
    float n2x, n2y, n2z;

    /*
     * Per-vertex tangents, also in view space: the direction along the surface
     * in which the texture's u increases. With the normal they span the frame a
     * tangent-space normal map is written in, and without them the map is just
     * three unrelated numbers per texel.
     *
     * Only meaningful when the mesh carries a normal map; otherwise zero, and
     * the rasterizer never looks.
     */
    float t0x, t0y, t0z;
    float t1x, t1y, t1z;
    float t2x, t2y, t2z;

    /*
     * Handedness of the tangent frame, +1 or -1, taken per triangle rather than
     * per vertex.
     *
     * It cannot be interpolated: it is a sign, and halfway between +1 and -1 is
     * zero, which is a bitangent of no length. The only place the three
     * vertices disagree is a triangle straddling a mirrored UV seam, where the
     * frame is genuinely discontinuous and no interpolation would have helped
     * anyway.
     */
    float hand;

    /* Texture coordinates, already divided by view z so the interpolation is
     * perspective-correct; `wz` carries the reciprocals to undo it. */
    float u0, v0, u1, v1, u2, v2;
    float w0, w1, w2;

    /*
     * Reciprocal edge lengths, which turn an edge function into a distance in
     * pixels: e / |edge|. Computed once per triangle on the host, because the
     * three square roots are constant across the face and paying them per
     * pixel would cost millions of repeats of the same number.
     */
    float rlen0, rlen1, rlen2;
} ScreenTri;

typedef struct {
    int   fb_w, fb_h;
    int   bb_x, bb_y, bb_w, bb_h;   /* the mesh's screen bounding box */
    int   tri_count;
    float alpha;                    /* the widget's fade */
    int   blend;                    /* as in CompositeParams */

    int   tex_w, tex_h;             /* 0 = untextured */

    float wire;                     /* half stroke width in px; 0 = filled     */
    int   aa;                       /* feather the outer silhouette by a pixel */
    int   filter;                   /* 1 = bilinear, 0 = nearest texel         */

    int   ao_w, ao_h;               /* occlusion map; 0 = none                 */
    float ao_strength;              /* 0 = ignore the map, 1 = full            */

    int   nrm_w, nrm_h;             /* tangent-space normal map; 0 = none      */
    float normal_scale;             /* how far it may tilt the normal; 0 = off */

    int   emis_w, emis_h;           /* emissive map; 0 = none                  */
    float emis_r, emis_g, emis_b;   /* emissive colour, already premultiplied
                                     * by its strength; all zero = off         */
    /*
     * The lighting environment, resolved once per mesh.
     *
     * Positions are already in view space, so the pixel loop never touches the
     * camera matrix; and because the eye is the view origin, the direction to
     * the viewer is just the negated surface position.
     */
    Light lights[VR_MAX_LIGHTS];
    int   light_count;

    float ambient;                  /* floor for surfaces facing away          */
    float specular;                 /* 0 = matte                               */
    float shininess;                /* Blinn-Phong exponent                    */
    int   two_sided;                /* light both faces (cull off)             */

    float ccx, ccy, focal;          /* to undo the projection, per pixel       */
} MeshParams;

/*
 * Rasterizes one pixel of a mesh.
 *
 * The loop is over *triangles inside one pixel*, not pixels inside one
 * triangle. That inversion is what lets a single implementation serve both
 * backends: there is no shared depth buffer to race over, because the depth
 * test is a local variable — each pixel independently finds its own nearest
 * face. A z-buffer with atomics would be faster for heavy meshes but would need
 * two different implementations, and the whole architecture here is built on
 * not having those.
 *
 * The cost is O(triangles) per pixel, so this suits models of hundreds to a few
 * thousand faces rather than scanned assets.
 */
/*
 * Inverse-square falloff, softened so it never blows up at the source.
 *
 * `range` is where the light is down to half. Zero means no falloff, which is
 * what a sun wants and what every scene written before this existed assumes.
 */
VR_PIX float vr_falloff(float dist, float range)
{
    if (range <= 0.0f) {
        return 1.0f;
    }
    float t = dist / range;
    return 1.0f / (1.0f + t * t);
}

/*
 * The maps a mesh surface wears.
 *
 * A struct rather than a widening argument list because the material is a set
 * that grows together: base colour today, occlusion now, and the same UVs will
 * address a normal or emissive map next. Passing them as one value means the
 * rasterizer's signature stops changing every time the set does — the dimensions
 * ride along in MeshParams, and a null pointer is simply "this mesh has none".
 */
typedef struct {
    const uchar4 *VR_RESTRICT base;   /* diffuse / albedo   */
    const uchar4 *VR_RESTRICT ao;     /* ambient occlusion  */
    const uchar4 *VR_RESTRICT nrm;    /* tangent-space normal */
    const uchar4 *VR_RESTRICT emis;   /* emissive           */
} MeshTextures;

/*
 * Bilinear sample for a mesh surface: u wraps, v clamps.
 *
 * The two axes are treated differently on purpose, because on the maps meshes
 * actually wear they mean different things. An equirectangular texture's u is
 * longitude, which is a circle — the left and right edges are the same meridian,
 * so wrapping is what joins them and clamping would smear a seam down the globe.
 * Its v is latitude, which ends: the first and last rows are the poles, and
 * wrapping there would blend the arctic into the antarctic in a one-texel band
 * across the top and bottom of every planet.
 *
 * Nearest sampling was the previous behaviour and is still available, but it is
 * rarely what anyone wants on a mesh: a 2048-wide map on a 160-pixel planet
 * takes one texel in thirteen, and picks a different one each frame as the
 * globe turns, which crawls.
 */
VR_PIX float4 vr_sample_mesh(const uchar4 *VR_RESTRICT tex,
                             int tw, int th, float u, float v)
{
    float fx = u * (float)tw - 0.5f;
    float fy = v * (float)th - 0.5f;

    int   x0 = (int)floorf(fx);
    int   y0 = (int)floorf(fy);
    float ax = fx - (float)x0;
    float ay = fy - (float)y0;

    /* Wrap by modulus rather than by masking, so a non-power-of-two map works
     * and a negative coordinate lands on the far side instead of at zero. */
    int x0w = x0 % tw; if (x0w < 0) x0w += tw;
    int x1w = (x0w + 1 == tw) ? 0 : x0w + 1;

    int y0c = vr_clampi(y0,     0, th - 1);
    int y1c = vr_clampi(y0 + 1, 0, th - 1);

    uchar4 p00 = tex[(size_t)y0c * tw + x0w];
    uchar4 p10 = tex[(size_t)y0c * tw + x1w];
    uchar4 p01 = tex[(size_t)y1c * tw + x0w];
    uchar4 p11 = tex[(size_t)y1c * tw + x1w];

    const float inv = 1.0f / 255.0f;
    float w00 = (1.0f - ax) * (1.0f - ay);
    float w10 = ax * (1.0f - ay);
    float w01 = (1.0f - ax) * ay;
    float w11 = ax * ay;

    float4 out;
    out.x = (p00.x * w00 + p10.x * w10 + p01.x * w01 + p11.x * w11) * inv;
    out.y = (p00.y * w00 + p10.y * w10 + p01.y * w01 + p11.y * w11) * inv;
    out.z = (p00.z * w00 + p10.z * w10 + p01.z * w01 + p11.z * w11) * inv;
    out.w = (p00.w * w00 + p10.w * w10 + p01.w * w01 + p11.w * w11) * inv;
    return out;
}

/*
 * Squared distance from a point to a line *segment* — the projection clamped to
 * the segment's ends, so a point beyond an endpoint measures to that endpoint
 * rather than to the infinite line.
 *
 * `rlen` is the reciprocal of the segment's length, already to hand.
 */
VR_PIX float vr_seg_dist2(float px, float py,
                          float ax, float ay, float bx, float by, float rlen)
{
    float vx = bx - ax, vy = by - ay;
    float wx = px - ax, wy = py - ay;

    float t = (vx * wx + vy * wy) * rlen * rlen;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    float dx = wx - t * vx, dy = wy - t * vy;
    return dx * dx + dy * dy;
}

VR_PIX void vr_px_mesh(uchar4 *VR_RESTRICT fb, float *VR_RESTRICT depth,
                       const ScreenTri *VR_RESTRICT tris,
                       MeshTextures maps,
                       const MeshParams *p, int i, int j)
{
    int gx = p->bb_x + i;
    int gy = p->bb_y + j;

    if (gx < 0 || gx >= p->fb_w || gy < 0 || gy >= p->fb_h) {
        return;
    }

    size_t idx = (size_t)gy * p->fb_w + gx;

    float px = (float)gx + 0.5f;
    float py = (float)gy + 0.5f;

    /*
     * Start from whatever earlier meshes left in the depth buffer, so a nearer
     * surface from *another* mesh correctly hides this one. That is the whole
     * point of the shared buffer: without it each mesh only occludes itself and
     * two interpenetrating solids draw in the order they happen to be listed.
     *
     * No atomics are needed even on the GPU: one thread owns one pixel for the
     * whole of a mesh's rasterization, and mesh launches are ordered on the
     * stream — so nothing else can touch this entry while it is being decided.
     */
    float best_z = (depth != NULL) ? depth[idx] : 1e30f;
    float cr = 0.0f, cg = 0.0f, cb = 0.0f;
    float cov = 1.0f;          /* the texture's own alpha at the winning pixel */
    float cov_sum = 0.0f;      /* geometric coverage summed over the mesh      */
    bool  hit = false;

    for (int t = 0; t < p->tri_count; t++) {
        const ScreenTri *tr = &tris[t];

        /* Edge functions. Their signs together say whether the point is inside;
         * the sum is twice the signed area, which also normalises them. */
        float e0 = (tr->x1 - tr->x0) * (py - tr->y0) - (tr->y1 - tr->y0) * (px - tr->x0);
        float e1 = (tr->x2 - tr->x1) * (py - tr->y1) - (tr->y2 - tr->y1) * (px - tr->x1);
        float e2 = (tr->x0 - tr->x2) * (py - tr->y2) - (tr->y0 - tr->y2) * (px - tr->x2);

        /* The sum is twice the signed area — a constant for the triangle,
         * whatever the point — so it also normalises the barycentrics below. */
        float area = e0 + e1 + e2;
        if (area <= 1e-9f) {
            continue;
        }

        /*
         * The edge functions become distances in pixels, which is the form both
         * the wireframe and the anti-aliasing need: how far inside this
         * triangle the pixel centre sits, measured perpendicular to each edge.
         */
        float d0 = e0 * tr->rlen0;
        float d1 = e1 * tr->rlen1;
        float d2 = e2 * tr->rlen2;
        float din = (d0 < d1) ? d0 : d1;
        if (d2 < din) din = d2;

        /*
         * How far outside the triangle a pixel may still count.
         *
         * A hair of slack even in the hard-edged case, because two triangles
         * sharing a diagonal compute that edge from opposite ends and the two
         * expressions are exact negations only in real arithmetic: in floats
         * both can land a shade below zero, neither claims the pixel, and a
         * one-pixel crack opens down the middle of a flat face. With
         * anti-aliasing the slack widens to half a pixel, which is the width
         * the coverage ramp needs.
         */
        float slack = (p->aa || p->wire > 0.0f) ? 0.5f : 1e-3f;
        if (din < -slack) {
            continue;
        }

        /*
         * Geometric coverage: 0 just outside the edge, 1 half a pixel inside.
         *
         * A wireframe measures from the edge inwards instead, keeping the
         * band and discarding the interior — which is why the test sits here,
         * before the depth is committed: a hollow triangle must let whatever
         * is behind it through rather than occluding with an invisible face.
         */
        float ecov = 1.0f;
        if (p->aa || p->wire > 0.0f) {
            /*
             * Outside the triangle, distance is measured to the triangle as a
             * *shape* rather than to its edges' infinite lines.
             *
             * The difference only shows at a sharp vertex, and there it is the
             * whole story. Offsetting three lines outward by half a pixel and
             * keeping the intersection grows the triangle by half a pixel along
             * each edge — but past an acute corner the two offset lines run on
             * and meet far beyond it, by 0.5/sin(half the angle). A sphere's
             * poles are slivers of a couple of degrees, so that is tens of
             * pixels, and they threw faint spikes out into empty space that
             * flickered as the mesh turned.
             *
             * Clamping the projection to each segment removes the overshoot
             * exactly, and changes nothing along an edge — where the foot of the
             * perpendicular lies on the segment and the two measures agree — so
             * two triangles sharing an edge still contribute halves that sum to
             * a whole and no seam appears.
             */
            float sd = din;
            if (sd < 0.0f) {
                float q0 = vr_seg_dist2(px, py, tr->x0, tr->y0, tr->x1, tr->y1, tr->rlen0);
                float q1 = vr_seg_dist2(px, py, tr->x1, tr->y1, tr->x2, tr->y2, tr->rlen1);
                float q2 = vr_seg_dist2(px, py, tr->x2, tr->y2, tr->x0, tr->y0, tr->rlen2);
                float q  = (q0 < q1) ? q0 : q1;
                if (q2 < q) q = q2;
                sd = -sqrtf(q);
            }

            ecov = sd + 0.5f;
            if (ecov <= 0.0f) {
                continue;
            }
            if (ecov > 1.0f) ecov = 1.0f;

            if (p->wire > 0.0f) {
                /* Keep the band near an edge and drop the interior, gated by
                 * the coverage above so the stroke fades at the silhouette. */
                float band = p->wire - din + 0.5f;
                if (band <= 0.0f) {
                    continue;
                }
                if (band > 1.0f) band = 1.0f;
                if (band < ecov) ecov = band;
            }
        }

        /*
         * Coverage adds up across the whole mesh, and is banked here — before
         * the depth test, so a surface hidden behind another still counts.
         *
         * That is what makes the anti-aliasing correct rather than merely
         * blurry. Two triangles meeting on an interior edge each cover about
         * half the pixel; summed they fill it, so no seam appears along the
         * diagonal. Where a nearer surface only half-covers a pixel and a
         * farther part of the same mesh lies behind it, the two again sum to
         * one and nothing shows through — which is right, because the mesh
         * really is opaque there. Only on the outer silhouette, with nothing
         * behind, does the sum stay below one and let the background in.
         *
         * Counting triangles instead of summing coverage was the first attempt
         * and it failed exactly where meshes are hardest: at the limb of a
         * sphere, where triangles are foreshortened into slivers that do not
         * reliably both reach a shared edge, it cut thin dark slits into the
         * silhouette.
         */
        cov_sum += ecov;

        /* Barycentric depth. Interpolating camera-space z linearly in screen
         * space is not strictly correct under perspective, but across a single
         * triangle of a modest mesh the error is far below one depth step. */
        float inv = 1.0f / area;
        float z = (e1 * tr->z0 + e2 * tr->z1 + e0 * tr->z2) * inv;

        if (z >= best_z) {
            continue;
        }

        /* Barycentric weights. e1 belongs to vertex 0, e2 to vertex 1 and e0 to
         * vertex 2 — the edge opposite each. */
        float b0 = e1 * inv, b1 = e2 * inv, b2 = e0 * inv;

        /*
         * Clamped back onto the triangle before anything is interpolated.
         *
         * Anti-aliasing accepts a pixel whose centre lies just outside a
         * triangle, so it can measure how much of the pixel the triangle
         * covers. That is the right thing for coverage and the wrong thing for
         * colour: outside the triangle a barycentric goes negative, and a
         * shading term or texture coordinate read there is extrapolated past
         * the vertex that bounds it. Projecting back onto the triangle costs a
         * divide on the handful of fragments that need it and keeps every
         * interpolated value inside the range its vertices define.
         */
        if (b0 < 0.0f || b1 < 0.0f || b2 < 0.0f) {
            if (b0 < 0.0f) b0 = 0.0f;
            if (b1 < 0.0f) b1 = 0.0f;
            if (b2 < 0.0f) b2 = 0.0f;
            float bs = b0 + b1 + b2;
            if (bs > 1e-9f) {
                float rb = 1.0f / bs;
                b0 *= rb; b1 *= rb; b2 *= rb;
            }
        }

        /*
         * Lighting, per pixel.
         *
         * The normal interpolates perspective-correctly like the texture
         * coordinates — divided through by the same reciprocal depth — because
         * a normal is a surface attribute and suffers the same foreshortening.
         * That same reciprocal gives the view depth back, and from the pixel's
         * own screen position the rest of the view-space point follows: the
         * projection is x_screen = ccx + X*focal/Z, so X = (x - ccx)*Z/focal.
         * Nothing about the camera has to be passed in for that.
         */
        float wsum = b0 * tr->w0 + b1 * tr->w1 + b2 * tr->w2;
        if (wsum <= 1e-12f) {
            continue;
        }
        float invw = 1.0f / wsum;

        float nx = (b0 * tr->n0x * tr->w0 + b1 * tr->n1x * tr->w1
                  + b2 * tr->n2x * tr->w2) * invw;
        float ny = (b0 * tr->n0y * tr->w0 + b1 * tr->n1y * tr->w1
                  + b2 * tr->n2y * tr->w2) * invw;
        float nz = (b0 * tr->n0z * tr->w0 + b1 * tr->n1z * tr->w1
                  + b2 * tr->n2z * tr->w2) * invw;

        /*
         * The texture coordinate is resolved here, before the lighting, rather
         * than inside the colour lookup where it used to live. A normal map is
         * addressed by the same UVs and has to be read *before* anything is
         * lit — it decides what the normal is — so the two lookups now share
         * one perspective-correct divide instead of repeating it.
         */
        float uu = 0.0f, vv = 0.0f;
        bool  have_uv = false;
        if (p->tex_w > 0 || p->nrm_w > 0 || p->emis_w > 0) {
            /*
             * Perspective-correct texturing: u/z and 1/z interpolate linearly
             * in screen space, u does not. Interpolating u directly is the
             * classic swimming-texture artefact on a steeply angled face.
             */
            float w = b0 * tr->w0 + b1 * tr->w1 + b2 * tr->w2;
            if (w > 1e-9f) {
                uu = (b0 * tr->u0 + b1 * tr->u1 + b2 * tr->u2) / w;
                vv = (b0 * tr->v0 + b1 * tr->v1 + b2 * tr->v2) / w;
                have_uv = true;
            }
        }

        float nl = sqrtf(nx * nx + ny * ny + nz * nz);
        if (nl < 1e-12f) {
            nl = 1.0f;
        }

        /*
         * Normal mapping.
         *
         * The map stores a normal in tangent space — a frame that follows the
         * texture across the surface — so reading it means building that frame
         * here and rotating the texel into view space. T comes interpolated
         * from the vertices, N is the geometric normal just computed, and the
         * third axis is their cross product, signed by the handedness.
         *
         * T is re-orthogonalised against N first. Interpolation does not
         * preserve a right angle: two perpendicular frames averaged across a
         * curved face come out slightly skewed, and the skew shows as the
         * lighting sliding a little as a surface turns.
         *
         * What this buys is the whole reason the map exists: the geometry stays
         * at two triangles per plank while the lighting behaves as though the
         * grain, the knots and the nail heads were modelled.
         */
        if (p->nrm_w > 0 && maps.nrm != NULL && have_uv && p->normal_scale > 0.0f) {
            float4 nt = vr_sample_mesh(maps.nrm, p->nrm_w, p->nrm_h, uu, vv);

            /* 0..1 back to -1..1. The blue channel is not rescaled: it is the
             * component along the normal, and weakening the map means tilting
             * less, which is exactly "leave z alone and shrink x and y". */
            float mx_ = (nt.x * 2.0f - 1.0f) * p->normal_scale;
            float my_ = (nt.y * 2.0f - 1.0f) * p->normal_scale;
            float mz_ =  nt.z * 2.0f - 1.0f;

            float tx = (b0 * tr->t0x * tr->w0 + b1 * tr->t1x * tr->w1
                      + b2 * tr->t2x * tr->w2) * invw;
            float ty = (b0 * tr->t0y * tr->w0 + b1 * tr->t1y * tr->w1
                      + b2 * tr->t2y * tr->w2) * invw;
            float tz = (b0 * tr->t0z * tr->w0 + b1 * tr->t1z * tr->w1
                      + b2 * tr->t2z * tr->w2) * invw;

            /* Unit normal, needed for the Gram-Schmidt and for the cross. */
            float ux = nx / nl, uy = ny / nl, uz = nz / nl;

            float d = tx * ux + ty * uy + tz * uz;
            tx -= ux * d; ty -= uy * d; tz -= uz * d;

            float tl = sqrtf(tx * tx + ty * ty + tz * tz);
            if (tl > 1e-9f) {
                tx /= tl; ty /= tl; tz /= tl;

                float hsign = (tr->hand < 0.0f) ? -1.0f : 1.0f;
                float bx = (uy * tz - uz * ty) * hsign;
                float by = (uz * tx - ux * tz) * hsign;
                float bz = (ux * ty - uy * tx) * hsign;

                float rx = tx * mx_ + bx * my_ + ux * mz_;
                float ry = ty * mx_ + by * my_ + uy * mz_;
                float rz = tz * mx_ + bz * my_ + uz * mz_;

                float rl = sqrtf(rx * rx + ry * ry + rz * rz);
                if (rl > 1e-9f) {
                    nx = rx; ny = ry; nz = rz;
                    nl = rl;
                }
            }
        }

        float shade = 1.0f;
        float spec  = 0.0f;

        if (p->light_count > 0) {
            float Zv = invw;
            float Xv = (px - p->ccx) * Zv / p->focal;
            float Yv = (py - p->ccy) * Zv / p->focal;

            /* The eye is the view origin, so this is the direction to it. */
            float vlen = sqrtf(Xv * Xv + Yv * Yv + Zv * Zv);
            float vx = 0.0f, vy = 0.0f, vz = -1.0f;
            if (vlen > 1e-9f) {
                vx = -Xv / vlen; vy = -Yv / vlen; vz = -Zv / vlen;
            }

            float diff = 0.0f;
            for (int li = 0; li < p->light_count; li++) {
                const Light *L = &p->lights[li];

                /*
                 * The vector from the surface to the light, and how far away it
                 * is. A directional light has no position: every surface is lit
                 * from the same angle, so the vector is just the negated aim
                 * and the distance is meaningless — which is why `ll` is forced
                 * to 1 there rather than left to produce a falloff.
                 */
                float lx, ly, lz, ll;
                if (L->type == VR_LIGHT_DIR) {
                    lx = -L->dx; ly = -L->dy; lz = -L->dz;
                    ll = sqrtf(lx * lx + ly * ly + lz * lz);
                    if (ll < 1e-9f) {
                        continue;
                    }
                    lx /= ll; ly /= ll; lz /= ll;
                    ll = 1.0f;
                } else {
                    lx = L->x - Xv;
                    ly = L->y - Yv;
                    lz = L->z - Zv;
                    ll = sqrtf(lx * lx + ly * ly + lz * lz);
                    if (ll < 1e-9f) {
                        continue;
                    }
                }

                /*
                 * The spot cone. The test is on the angle between the light's
                 * aim and the direction to *this* surface point, smoothed
                 * across the penumbra — a hard cone edge is the one thing that
                 * makes a spotlight look like a stencil rather than a lamp.
                 */
                float cone = 1.0f;
                if (L->type == VR_LIGHT_SPOT) {
                    float sx = -lx / ll, sy = -ly / ll, sz = -lz / ll;
                    float cd = sx * L->dx + sy * L->dy + sz * L->dz;
                    if (cd <= L->cos_outer) {
                        continue;                 /* outside the beam entirely */
                    }
                    if (cd < L->cos_inner) {
                        float ct = (cd - L->cos_outer) / (L->cos_inner - L->cos_outer);
                        cone = ct * ct * (3.0f - 2.0f * ct);
                    }
                }

                float d = (nx * lx + ny * ly + nz * lz) / (ll * nl);
                if (p->two_sided) {
                    d = fabsf(d);
                }
                if (d <= 0.0f) {
                    continue;
                }

                float att = L->intensity * cone;
                if (L->type != VR_LIGHT_DIR) {
                    att *= vr_falloff(ll, L->range);
                }
                diff += d * att;

                if (p->specular > 0.0f) {
                    /* Blinn-Phong: the halfway vector between light and eye. */
                    float hx = lx / ll + vx;
                    float hy = ly / ll + vy;
                    float hz = lz / ll + vz;
                    float hl = sqrtf(hx * hx + hy * hy + hz * hz);
                    if (hl > 1e-9f) {
                        float nh = (nx * hx + ny * hy + nz * hz) / (hl * nl);
                        if (p->two_sided) {
                            nh = fabsf(nh);
                        }
                        if (nh > 0.0f) {
                            spec += att * powf(nh, p->shininess);
                        }
                    }
                }
            }
            if (diff > 1.0f) diff = 1.0f;
            shade = p->ambient + (1.0f - p->ambient) * diff;
            spec *= p->specular;
        } else {
            /*
             * No lights: the camera is the lamp, so the term is just how far
             * the surface has turned away from the viewer. Absolute, because
             * with nothing else to disambiguate it, either face is the lit one.
             */
            float lam = fabsf(nz) / nl;
            shade = p->ambient + (1.0f - p->ambient) * lam;
        }

        /*
         * The surface colour before texturing. Under flat shading the host has
         * already folded the lighting into tr->r and `shade` is 1; under smooth
         * shading tr->r is the raw colour and `shade` carries the light. Either
         * way this is "the lit mesh colour", which is what a texture modulates.
         */
        float tr_r = tr->r * shade;
        float tr_g = tr->g * shade;
        float tr_b = tr->b * shade;
        float ta = 1.0f;

        if (p->tex_w > 0 && maps.base != NULL) {
            if (have_uv) {
                float su = uu, sv = vv;
                float sr_, sg_, sb_, sa_;

                if (p->filter) {
                    float4 t4 = vr_sample_mesh(maps.base, p->tex_w, p->tex_h, su, sv);
                    sr_ = t4.x; sg_ = t4.y; sb_ = t4.z; sa_ = t4.w;
                } else {
                    /* Wrap, so tiled UVs outside 0..1 behave as expected. */
                    su = su - floorf(su);
                    sv = sv - floorf(sv);

                    int tx = vr_clampi((int)(su * (float)p->tex_w), 0, p->tex_w - 1);
                    int ty = vr_clampi((int)(sv * (float)p->tex_h), 0, p->tex_h - 1);
                    uchar4 tc = maps.base[(size_t)ty * p->tex_w + tx];

                    const float i255 = 1.0f / 255.0f;
                    sr_ = tc.x * i255; sg_ = tc.y * i255;
                    sb_ = tc.z * i255; sa_ = tc.w * i255;
                }

                /*
                 * Texture times mesh colour, so a white mesh shows the texture
                 * untouched and a coloured one tints it — and the lighting,
                 * already in cr/cg/cb, survives either way. Multiplying the
                 * texel by `shade` alone would light the flat-shaded case not
                 * at all, leaving a textured cube perfectly evenly lit.
                 */
                tr_r *= sr_;
                tr_g *= sg_;
                tr_b *= sb_;
                ta    = sa_;

                /*
                 * Ambient occlusion: how much of the sky a crevice cannot see.
                 *
                 * Stored in the red channel, which is where glTF puts it and
                 * also where the ORM/ARM maps that ship with scanned props keep
                 * it — occlusion, roughness and metalness packed into one image
                 * so it costs a single fetch.
                 *
                 * The spec says occlusion attenuates *indirect* light only. It
                 * is applied to the whole shade here instead, and the reason is
                 * practical rather than principled: this renderer's indirect
                 * term is one `ambient` constant, usually around 0.15, so the
                 * spec-correct version is very nearly invisible. Multiplying
                 * everything is what actually seats an object into its own
                 * shadowed corners, which is the entire reason to read the map.
                 */
                if (p->ao_w > 0 && maps.ao != NULL) {
                    /* Always filtered, even when the albedo is asked for
                     * unfiltered: `nearest` is a choice about how the colour
                     * should look, and nothing is gained by making the shading
                     * term blocky to match. */
                    float ao = vr_sample_mesh(maps.ao, p->ao_w, p->ao_h, uu, vv).x;
                    /* Dialled towards 1, so a map that already has the shading
                     * baked into its albedo does not darken twice. */
                    ao = 1.0f + (ao - 1.0f) * p->ao_strength;
                    tr_r *= ao; tr_g *= ao; tr_b *= ao;
                }
            }
        }

        /*
         * The highlight is added, not multiplied — after the texture, so it
         * sits *on* the surface rather than being tinted by it. Occlusion has
         * already been applied to the diffuse above and deliberately not to
         * this: a crevice still reflects a light that reaches it.
         */
        tr_r += spec;
        tr_g += spec;
        tr_b += spec;

        /*
         * Emissive: light the surface produces rather than reflects.
         *
         * Added last, after the shading and after the occlusion, and that
         * placement is the whole feature. Every other term here is a multiplier
         * on the albedo, so anything expressed through the albedo goes dark
         * exactly when the object does — which is the opposite of what a lit
         * screen or a glowing filament should do. A term that is added survives
         * the dark, and the object reads as switched on.
         *
         * No bloom and no light cast on anything else: this renderer has no
         * global illumination, so an emissive surface lights itself only.
         */
        if (p->emis_r > 0.0f || p->emis_g > 0.0f || p->emis_b > 0.0f) {
            float er = p->emis_r, eg = p->emis_g, eb = p->emis_b;
            if (p->emis_w > 0 && maps.emis != NULL && have_uv) {
                float4 e4 = vr_sample_mesh(maps.emis, p->emis_w, p->emis_h, uu, vv);
                er *= e4.x; eg *= e4.y; eb *= e4.z;
            }
            tr_r += er; tr_g += eg; tr_b += eb;
        }

        /*
         * A transparent texel is not a surface.
         *
         * The test has to sit here, after sampling and before the depth is
         * committed, or the hole in a planetary ring would still write depth
         * and hide whatever is behind it — an invisible disc that occludes.
         * Rejecting the fragment instead lets the triangle behind win the
         * pixel, which is the whole point of an alpha-cut surface.
         */
        if (ta <= (1.0f / 255.0f)) {
            continue;
        }

        best_z = z;
        hit = true;
        cov = ta;
        cr = tr_r;
        cg = tr_g;
        cb = tr_b;
    }

    if (!hit) {
        return;
    }

    if (depth != NULL) {
        depth[idx] = best_z;
    }

    float a = vr_sat(p->alpha);
    if (a <= 0.0f) {
        return;
    }

    /*
     * Coverage and colour part company once a texture has an alpha channel.
     * Textures here are premultiplied, so cr/cg/cb already carry the texel's
     * alpha and must not be scaled by it again; only the *coverage* — what the
     * fragment hides of the background — takes it. Multiplying both would
     * darken every semi-transparent pixel twice.
     */
    float edge = (cov_sum < 1.0f) ? cov_sum : 1.0f;
    float ea   = a * cov * edge;

    uchar4 d = fb[idx];

    const float inv255 = 1.0f / 255.0f;
    float dr = (float)d.x * inv255, dg = (float)d.y * inv255;
    float db = (float)d.z * inv255, da = (float)d.w * inv255;

    /* Premultiplied source, matching every other layer in the pipeline. The
     * colour is scaled by the same coverage as the alpha, or a feathered edge
     * would be the right shape and the wrong brightness. */
    float acol = a * edge;
    float sr = cr * acol, sg = cg * acol, sb = cb * acol;
    float r, g, b, outa;

    if (p->blend == 1) {
        r = sr + dr; g = sg + dg; b = sb + db; outa = ea + da;
    } else if (p->blend == 2) {
        r = 1.0f - (1.0f - vr_sat(sr)) * (1.0f - dr);
        g = 1.0f - (1.0f - vr_sat(sg)) * (1.0f - dg);
        b = 1.0f - (1.0f - vr_sat(sb)) * (1.0f - db);
        outa = ea + da * (1.0f - ea);
    } else {
        float ia = 1.0f - ea;
        r = sr + dr * ia; g = sg + dg * ia; b = sb + db * ia; outa = ea + da * ia;
    }

    fb[idx] = make_uchar4(vr_u8(r), vr_u8(g), vr_u8(b), vr_u8(outa));
}

#endif /* VIDEO_REDAC_PIXEL_OPS_H */
