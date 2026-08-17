# CUDA kernels

Nine `__global__` kernels in `src/renderer.cu`.

Launch geometry is 16×16 threads (256 per block) throughout: a multiple of the
warp size, and neighbouring threads touch neighbouring pixels, so writes
coalesce into full rows.

> **Where the maths actually lives.** The kernels are thin wrappers. Every
> per-pixel function they call is in `include/pixel_ops.h`, which compiles both
> as `__device__` code here and as ordinary C in the CPU backend — one
> implementation, two drivers. A kernel does only what is genuinely CUDA's job:
> map a thread to a pixel, bounds-check it, and call the shared function. See
> [09-backends.md](09-backends.md).
>
> So `k_fx_blur` below *is* the launch geometry; the tap loop it describes is
> `vr_px_fx_blur`.

---

## Error checking

```c
#define gpuErrchk(ans)      gpu_assert((ans), __FILE__, __LINE__, true)
#define CUDA_TRY(ans)       /* logs, returns false */
#define CUDA_CHECK_KERNEL() gpuErrchk(cudaGetLastError())
```

Two levels on purpose — see [01-architecture.md](01-architecture.md#error-handling).

---

## `k_clear_background`

```c
__global__ void k_clear_background(uchar4 *fb, int width, int height, uchar4 bg);
```

One thread per pixel; edge blocks return early. Fills the scene background.

---

## `sample_bilinear` *(device)*

```c
__device__ float4 sample_bilinear(const uchar4 *tex, int tw, int th, float u, float v);
```

`u`,`v` are continuous texel coordinates where texel *i*'s centre is at `i+0.5`,
so the function works on `u-0.5`. The four neighbours are **clamped** to the
edge — wrapping would drag the opposite side of a glyph into view.

Interpolation happens in **premultiplied** space. That is not a detail: in
straight alpha, blending a transparent black neighbour into a semi-transparent
edge darkens it, producing fringes around every glyph.

---

## `k_composite_texture`

```c
struct CompositeParams {
    int   fb_w, fb_h, tex_w, tex_h;
    float cx, cy;                    /* destination centre */
    float inv_a, inv_b, inv_c, inv_d;/* inverse 2×2 */
    int   bb_x, bb_y, bb_w, bb_h;    /* destination bbox, already screen-clipped */
    float alpha;
    float pad_y, line_height;        /* typewriter geometry */
    int   line_count;
};

__global__ void k_composite_texture(uchar4 *fb, const uchar4 *tex,
                                    const float *cutoff_x, CompositeParams p);
```

Draws one layer with scale and rotation.

**Backward mapping.** The forward transform is `d = R(θ)·S·t` (scale, then
rotate). The kernel inverts it: for every *destination* pixel it computes where
to read from. Forward projection would leave scattered holes when rotating.

```
dx = px - cx        u = inv_a·dx + inv_b·dy + tex_w/2
dy = py - cy        v = inv_c·dx + inv_d·dy + tex_h/2
```

with (host side, `composite_layer`):

```
sx = dst_w / tex_w              inv_a =  cos/sx    inv_b =  sin/sx
sy = dst_h / tex_h              inv_c = -sin/sy    inv_d =  cos/sy
```

**Grid covers only the destination bbox**, not the screen. For a small title on
a 1080×1920 canvas that is ~50× fewer threads. The rotated bounding box is

```
half_w = (|cos|·dst_w + |sin|·dst_h) / 2
half_h = (|sin|·dst_w + |cos|·dst_h) / 2
```

clipped to the framebuffer before launch.

**Typewriter clipping.** If `cutoff_x` is non-NULL the kernel derives the line
from `v` and discards anything past that line's cutoff:

```c
int line = floorf((v - p.pad_y) / p.line_height);
line = clamp(line, 0, p.line_count - 1);
if (u > cutoff_x[line]) return;
```

**Blend** (premultiplied source-over):

```
src *= alpha
dst  = src + dst·(1 − src.a)
```

`alpha` multiplies all four channels, which is exactly right in premultiplied
space. A fully transparent sample returns early, skipping a read-modify-write.

---

## `k_rgba_to_nv12`

```c
__global__ void k_rgba_to_nv12(const uchar4 *rgba, uint8_t *y_plane,
                               uint8_t *uv_plane, int width, int height);
```

One thread per 2×2 block: writes four Y samples and one averaged UV pair.

BT.709 limited range ("studio swing"):

```
Y  =  16 + 0.18259·R + 0.61423·G + 0.06201·B
U  = 128 − 0.10064·R − 0.33857·G + 0.43922·B
V  = 128 + 0.43922·R − 0.39894·G − 0.04027·B
```

Chroma is the average of the 2×2 block. Odd dimensions clamp to the last row and
column, though the parser rounds them down to even anyway.

**Why it matters:** 8.29 MB → 3.11 MB per frame at 1080×1920, saved on both the
device-to-host copy and the pipe write, and the RGB→YUV conversion moves off the
CPU.

> The pipeline must tag the raw stream as BT.709/limited on **input** as well as
> output. Without that, ffmpeg assumes a different matrix and silently inserts
> `swscale` — which corrupted colours by up to 15/255. See
> [06-algorithms.md](06-algorithms.md#the-colour-tagging-bug).

---

## Effects

```c
struct EffectGPU { int type; float p[FXP_MAX]; float ca[4], cb[4]; };
```

The host samples every `Track` once per frame and hands the kernel plain
numbers, so no kernel ever walks keyframes.

### Device helpers

```c
__device__ float3 fx_load(const uchar4&);          /* 0..1 */
__device__ uchar4 fx_store(float3, unsigned char a);
__device__ float  fx_luma(float3);                 /* BT.709 weights */
__device__ float3 fx_mix(float3 a, float3 b, float t);
__device__ float  fx_hash(unsigned int x);         /* integer hash → [0,1) */
__device__ float3 fx_apply_point(float3 c, const EffectGPU&, int x, int y,
                                 int w, int h, unsigned int seed);
```

`fx_hash` avoids RNG state: the seed comes from the pixel coordinate and the
frame number, so grain is deterministic per frame yet changes between frames.

### `k_fx_point`

```c
__global__ void k_fx_point(uchar4 *dst, const uchar4 *src, int w, int h,
                           EffectGPU fx, unsigned int seed);
```

All twelve pointwise effects in one kernel, branching on `fx.type` (uniform
across the grid, so no real divergence).

| Effect | Maths |
|---|---|
| `grayscale` | `mix(c, luma, amount)` |
| `invert` | `mix(c, 1−c, amount)` |
| `sepia` | fixed 3×3 matrix, mixed by `amount` |
| `posterize` | `floor(c·n)/(n−1)` |
| `threshold` | `luma ≥ level ? 1 : 0` |
| `vignette` | smoothstep on normalised radius, tinted toward `color_a` |
| `grain` | `c + (hash(pixel, seed) − 0.5)·amount` |
| `scanlines` | `1 − amount·0.5·(1 − sin²(y/h·count·π))` |
| `vibrance` | `mix(luma, c, 1 + amount·(1 − saturation))` — boosts muted colours, protects skin |
| `split_tone` | tone from shadows→highlights by luma, soft-light-ish blend |
| `gradient_map` | `mix(c, mix(shadow, highlight, luma), amount)` |
| `color_grade` | exposure → brightness → contrast → gamma → temp/tint → saturation → vibrance → hue |

`color_grade`'s order mirrors a real grading session; the hue rotation uses the
standard YIQ-approximation matrix.

### `k_fx_blur`

```c
__global__ void k_fx_blur(uchar4 *dst, const uchar4 *src, int w, int h,
                          int radius, int horizontal);
```

**Separable.** Two 1-D passes read `O(2r)` samples per pixel where one 2-D pass
would read `O(r²)` — at r=20 that is 40 versus 1600, a 40× difference.

Weights are triangular (`radius + 1 − |k|`); two triangular passes approximate a
Gaussian well. Radius is capped at 128 on the host.

### `k_fx_pixelate`

Samples the centre of each `size × size` block — visually equivalent to
averaging it, at one read instead of `size²`.

### `k_fx_rgb_split`

Offsets the red and blue channels along `angle` by `±amount`, keeping green
fixed. Chromatic aberration.

### `k_fx_glitch`

Bands of 12 rows; `fx_hash(band, seed)` decides whether a band shifts
horizontally, and the top decile also gets its green channel displaced.

### Ping-pong

Effects cannot run in place: `blur` and `glitch` read neighbours, so writing
into the source would make the result depend on execution order. Each effect
reads `src`, writes `dst`, then the pointers swap. `apply_effect_list` returns
whichever buffer holds the result.

---

## `k_transition`

```c
struct TransSide {
    float opacity;
    float ia, ib, ic, id;   /* inverse 2×2 */
    float tx, ty;           /* translation in pixels */
    int   mask;             /* 0 none, 1 circle, 2 rect */
    float m0, m1, m2, m3;   /* circle: cx,cy,r | rect: x,y,w,h — canvas fractions */
};

struct TransParams { int w, h; uchar4 bg; TransSide from, to; };

__global__ void k_transition(uchar4 *dst, const uchar4 *from,
                             const uchar4 *to, TransParams p);
```

Composites two already-rendered scenes. Same backward-mapping idea as
`k_composite_texture`, one inverse transform per side:

```
start from the project background
  draw `from`, blended by its opacity and mask
  draw `to`   on top, likewise
```

Because `to` is drawn last, a crossfade is simply `to.opacity = p`, and a slide
is `to` translated. A sample landing outside its scene's bounds is skipped, so
the background shows through during slides and zooms.

`trans_mask_ok` tests the pixel in normalised coordinates against a circle or
rectangle. Preset masks and JSON masks share this path; a JSON mask overrides
the preset's.

Preset formulas are in [06-algorithms.md](06-algorithms.md#transition-presets).

---

## Kernel inventory

| Kernel | Grid over | Reads neighbours |
|---|---|---|
| `k_clear_background` | whole frame | no |
| `k_composite_texture` | destination bbox | yes (bilinear) |
| `k_rgba_to_nv12` | half-resolution blocks | 2×2 |
| `k_fx_point` | whole frame | no |
| `k_fx_blur` | whole frame | yes, 1-D |
| `k_fx_pixelate` | whole frame | yes |
| `k_fx_rgb_split` | whole frame | yes |
| `k_fx_glitch` | whole frame | yes |
| `k_transition` | whole frame | yes (bilinear ×2) |

All nine are verified with `compute-sanitizer` `memcheck` and `initcheck`: zero
errors, zero leaked bytes. See [07-cli-and-build.md](07-cli-and-build.md#sanitizers).
