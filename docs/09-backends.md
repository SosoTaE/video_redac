# Rendering backends

The renderer has two interchangeable backends. One is compiled into the binary;
`main.c` cannot tell which.

| | CUDA (default) | CPU (`make CPU=1`) |
|---|---|---|
| Backend file | `src/renderer.cu` | `src/renderer_cpu.c` |
| Compositing | CUDA kernels, 16×16 blocks | OpenMP loops over rows |
| Encoder | `h264_nvenc` | `libx264` |
| Requires | nvcc + an NVIDIA GPU | neither |
| Throughput* | 387 fps | 77 fps |
| Frames in flight | `VR_PIPELINE_DEPTH` (2) | 1 |

\* `showcase.json`, 1320 frames at 1080×1920. See [08-performance.md](08-performance.md).

---

## Why two backends, and why they are cheap

Before the split, the project could not be *built* without the CUDA toolkit, let
alone run without an NVIDIA GPU. That excluded most potential users of a public
repository, and made CI impossible without a GPU runner.

The naive way to fix that — write a second renderer — would have been a mistake.
The project has 16 effects and 17 transitions; two independent implementations
means every new effect gets written twice, and the two copies drift apart until
one of them is quietly wrong.

So the code is split by *what varies*, not by backend:

```
include/pixel_ops.h      what to compute for one pixel   shared, compiled twice
src/render_common.c      what to compute for one frame   shared, compiled once
src/renderer.cu          how to make the GPU do it       backend
src/renderer_cpu.c       how to make the CPU do it       backend
```

A backend owns allocation, scheduling and the loop. Nothing else. If a backend
ever computes a position, an opacity or a matrix, that code is in the wrong file.

### `pixel_ops.h` — the per-pixel maths

Compiles two ways off one macro:

```c
#ifdef __CUDACC__
#  define VR_PIX      __device__ __forceinline__
#  define VR_RESTRICT __restrict__
#else
#  define VR_PIX      static inline
#  define VR_RESTRICT restrict
#endif
```

Under nvcc the functions inline into kernels; under gcc they inline into loops.
The header is C-compatible on purpose (structs by pointer, no references, no
templates) so the CPU backend stays C11 like the rest of the host code.

For the host build it also redeclares `uchar4` / `float3` / `float4` with CUDA's
layout, and defines the intrinsics the device code uses:

| Device | Host equivalent | Note |
|---|---|---|
| `__saturatef(v)` | `vr_sat(v)` | Written so NaN → 0, matching the hardware instruction |
| `min` / `max` (int) | `vr_mini` / `vr_maxi` | Builtins on the device; nothing at all in C |

`vr_sat`'s NaN behaviour is not pedantry. The obvious
`v < 0 ? 0 : v > 1 ? 1 : v` lets a NaN through, which would produce a garbage
pixel on one backend and a black one on the other — a difference that only shows
up on the rare frame that generates a NaN.

The exported per-pixel entry points are `vr_px_composite`, `vr_px_nv12`,
`vr_px_transition`, `vr_px_fx_point`, `vr_px_fx_blur`, `vr_px_fx_pixelate`,
`vr_px_fx_rgb_split` and `vr_px_fx_glitch`. Each CUDA kernel is now a five-line
wrapper: map a thread to a pixel, bounds-check, call the shared function.

### `render_common.c` — the per-frame decisions

Everything a frame's appearance depends on, none of it aware of a device:

| Function | Does |
|---|---|
| `vr_evaluate_scene` | timeline → per-widget runtime state |
| `vr_compute_reveal_cutoffs` | typewriter progress → one x threshold per line |
| `vr_select_scenes` | which scene, or which pair mid-transition |
| `vr_composite_setup` | inverse matrix, destination centre, clipped bounding box |
| `vr_effect_sample` | Tracks → the flat `EffectGPU` POD |
| `vr_transition_preset` / `vr_transition_apply_inline` | the 17 presets, then the JSON's overrides |
| `vr_open_ffmpeg_pipe` | starts the encoder, with the colour tagging both backends need |
| `vr_frame_range` | `--range` → first/last frame |

---

## Differences that are deliberate

### No pipelining on the CPU

The GPU backend keeps two frames in flight so the CPU can write frame *i−1* into
the pipe while the GPU renders frame *i* — the two use different engines, so they
genuinely overlap.

On the CPU, rendering and encoding compete for the same cores. Pipelining would
add a buffer copy and a thread without making anything finish sooner, so the CPU
loop is plainly synchronous: render, write, repeat.

### One typewriter cutoff buffer instead of `VR_PIPELINE_DEPTH`

The GPU needs a separate cutoff slice per in-flight frame, or writing frame *i*
would pull data out from under frame *i−1*'s still-running kernels. The CPU
backend finishes compositing before anything else touches the buffer, so it
reuses one slice.

### Encoder fallback

A project file that says `"encoder": "h264_nvenc"` should still render on a
machine with no NVIDIA GPU. The CPU backend therefore maps hardware encoders to
their software equivalents:

| Requested | Used |
|---|---|
| `h264_nvenc` | `libx264` |
| `hevc_nvenc` | `libx265` |
| `av1_nvenc` | `libsvtav1` |

Two rules keep this honest:

1. **The swap is always announced** on stderr. Silently taking a different code
   path would be worse than the failure it prevents.
2. **`$VIDEO_REDAC_ENCODER` is never overridden.** Naming an encoder explicitly
   is taken as knowing what you are asking for.

The preset has to travel with the encoder. NVENC speaks `p1`..`p7`, x264 speaks
`ultrafast`..`placebo`, and handing one to the other is not a slow encode but a
hard failure (`x264 [error]: invalid preset 'p5'`). So a project's preset is
honoured only while the encoder stays in the same family; across a fallback it is
dropped, with a note.

---

## Verification

### The extraction was proved safe before the CPU backend existed

Moving the maths out of `renderer.cu` was done in two steps, each checked by
rendering to **rawvideo** (lossless, so byte comparison is meaningful) and
comparing against the same clips from the previous build:

```bash
VIDEO_REDAC_ENCODER=rawvideo ./video_redac showcase.json -o a.nut --range 0:3
```

| Step | showcase | binsearch | effects_demo |
|---|---|---|---|
| 1. `pixel_ops.h` extracted | identical | identical | identical |
| 2. `render_common.c` extracted | identical | identical | identical |

Bit-identical, not merely similar. Refactoring a renderer without that check is
guesswork.

### CPU versus GPU

The two backends agree far more closely than floating point requires:

| Project | Bytes compared | Differing | Max delta |
|---|---|---|---|
| `showcase.json` | 93,312,000 | 42 (0.00005 %) | 1 |
| `effects_demo.json` | 20,736,000 | 13 (0.0001 %) | 1 |
| `anim/binsearch.json` | 93,312,000 | 0 | — |
| `scene.json` | 93,312,000 | 0 | — |

Worst-case PSNR 106 dB. Two of the four projects are bit-identical across
backends.

**Why they are not always identical:** the GPU contracts a multiply and an add
into a single FMA where the CPU rounds twice. The results differ by one ulp,
which after the `×255 + 0.5` conversion occasionally lands on the other side of a
rounding boundary — hence a handful of ±1 bytes.

**Consequence for tests:** regression tests must compare GPU against GPU, or CPU
against CPU. A cross-backend test needs a tolerance of ±1, never `cmp`.

### Sanitizers now cover the whole pipeline

This is the unexpected payoff. ASAN and the CUDA runtime do not coexist — the
driver's own allocations trip the interceptors — so `make debug` could only ever
sanitize the *host* half of a GPU build. The compositor, the effect stack and the
NV12 conversion were reachable only through kernels, where ASAN cannot follow.

With `CPU=1` there is no CUDA runtime, so:

```bash
make sanitize
./video_redac showcase.json --range 0:1
```

runs ASAN + UBSAN + LSAN over *every* line of the pixel pipeline. Clean on
`showcase.json`, `effects_demo.json`, `scene.json`, `anim/transitions.json` and
`anim/binsearch.json`.

`compute-sanitizer` remains the tool for the CUDA build; the two are
complementary. It proves the kernels do not corrupt memory. The CPU backend
proves the arithmetic is right, because it is a second implementation of the
same maths that agrees to within one ulp.

---

## Build

```bash
make              # CUDA + NVENC
make CPU=1        # CPU + libx264, no CUDA required at any stage
make sanitize     # CPU build with ASAN/UBSAN
```

The CPU target links with `gcc` and never mentions `nvcc`, `-lcudart` or
`$(CUDA_HOME)` — that is what makes it work on a machine with no toolkit
installed. If it ever starts requiring nvcc, the target is broken.

OpenMP is optional (`make CPU=1 OPENMP=`): without it the pragmas are ignored and
the renderer runs single-threaded. Still correct, roughly 6× slower.

### Thread scaling

`showcase.json`, 300 frames, rawvideo (no encoder in the way):

| Threads | fps | Speed-up |
|---|---|---|
| 1 | 19.5 | 1.0× |
| 4 | 64.5 | 3.3× |
| 20 | 130.7 | 6.7× |

Scaling flattens well before 20 threads because at that rate the run is writing
about 400 MB/s of raw frames; the compositing itself parallelises better than
this table can show.
