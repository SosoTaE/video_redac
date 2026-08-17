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
} EffectGPU;

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
                                int x, int y, int w, int h, unsigned int seed)
{
    switch (fx->type) {
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
                           int w, int h, const EffectGPU *fx, unsigned int seed,
                           int x, int y)
{
    size_t i = (size_t)y * w + x;
    uchar4 s = src[i];
    dst[i]   = vr_fx_store(vr_fx_apply_point(vr_fx_load(s), fx, x, y, w, h, seed), s.w);
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
     * Per-vertex shading terms, for smooth (Gouraud) shading. Interpolating a
     * scalar rather than a normal keeps the pixel loop to one multiply-add per
     * vertex — the lighting itself was already resolved on the host.
     */
    float l0, l1, l2;

    /* Texture coordinates, already divided by view z so the interpolation is
     * perspective-correct; `wz` carries the reciprocals to undo it. */
    float u0, v0, u1, v1, u2, v2;
    float w0, w1, w2;
} ScreenTri;

typedef struct {
    int   fb_w, fb_h;
    int   bb_x, bb_y, bb_w, bb_h;   /* the mesh's screen bounding box */
    int   tri_count;
    float alpha;                    /* the widget's fade */
    int   blend;                    /* as in CompositeParams */

    int   smooth;                   /* interpolate l0..l2 rather than use r,g,b flat */
    int   tex_w, tex_h;             /* 0 = untextured */
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
VR_PIX void vr_px_mesh(uchar4 *VR_RESTRICT fb, float *VR_RESTRICT depth,
                       const ScreenTri *VR_RESTRICT tris,
                       const uchar4 *VR_RESTRICT tex,
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
    bool  hit = false;

    for (int t = 0; t < p->tri_count; t++) {
        const ScreenTri *tr = &tris[t];

        /* Edge functions. Their signs together say whether the point is inside;
         * the sum is twice the signed area, which also normalises them. */
        float e0 = (tr->x1 - tr->x0) * (py - tr->y0) - (tr->y1 - tr->y0) * (px - tr->x0);
        float e1 = (tr->x2 - tr->x1) * (py - tr->y1) - (tr->y2 - tr->y1) * (px - tr->x1);
        float e2 = (tr->x0 - tr->x2) * (py - tr->y2) - (tr->y0 - tr->y2) * (px - tr->x2);

        /* The sum is twice the signed area — a constant for the triangle,
         * whatever the point — so it also sets the scale for the test below. */
        float area = e0 + e1 + e2;
        if (area <= 1e-9f) {
            continue;
        }

        /*
         * One winding only: the host has already discarded back faces when the
         * mesh asked for culling, so anything arriving here is front-facing.
         *
         * The tolerance is what closes the seam along a shared edge. Two
         * triangles meeting on a diagonal compute that edge from opposite ends,
         * and the two expressions are exact negations only in real arithmetic —
         * in floats both can land a hair below zero, so neither claims the
         * pixel and a one-pixel crack opens down the middle of a flat face. A
         * relative epsilon lets both claim it instead; the second then loses the
         * depth test to the first, which costs nothing because on a shared edge
         * they agree about depth and colour anyway.
         */
        float eps = 1e-6f * area;
        if (e0 < -eps || e1 < -eps || e2 < -eps) {
            continue;
        }

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

        float shade = 1.0f;
        if (p->smooth) {
            shade = b0 * tr->l0 + b1 * tr->l1 + b2 * tr->l2;
            if (shade < 0.0f) shade = 0.0f;
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

        if (p->tex_w > 0 && tex != NULL) {
            /*
             * Perspective-correct texturing: u/z and 1/z interpolate linearly
             * in screen space, u does not. Interpolating u directly is the
             * classic swimming-texture artefact on a steeply angled face.
             */
            float w = b0 * tr->w0 + b1 * tr->w1 + b2 * tr->w2;
            if (w > 1e-9f) {
                float uu = (b0 * tr->u0 + b1 * tr->u1 + b2 * tr->u2) / w;
                float vv = (b0 * tr->v0 + b1 * tr->v1 + b2 * tr->v2) / w;

                /* Wrap, so tiled UVs outside 0..1 behave as expected. */
                uu = uu - floorf(uu);
                vv = vv - floorf(vv);

                int tx = vr_clampi((int)(uu * (float)p->tex_w), 0, p->tex_w - 1);
                int ty = vr_clampi((int)(vv * (float)p->tex_h), 0, p->tex_h - 1);
                uchar4 tc = tex[(size_t)ty * p->tex_w + tx];

                /*
                 * Texture times mesh colour, so a white mesh shows the texture
                 * untouched and a coloured one tints it — and the lighting,
                 * already in cr/cg/cb, survives either way. Multiplying the
                 * texel by `shade` alone would light the flat-shaded case not
                 * at all, leaving a textured cube perfectly evenly lit.
                 */
                const float i255 = 1.0f / 255.0f;
                tr_r *= tc.x * i255;
                tr_g *= tc.y * i255;
                tr_b *= tc.z * i255;
                ta    = tc.w * i255;
            }
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
    float ea = a * cov;

    uchar4 d = fb[idx];

    const float inv255 = 1.0f / 255.0f;
    float dr = (float)d.x * inv255, dg = (float)d.y * inv255;
    float db = (float)d.z * inv255, da = (float)d.w * inv255;

    /* Premultiplied source, matching every other layer in the pipeline. */
    float sr = cr * a, sg = cg * a, sb = cb * a;
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
