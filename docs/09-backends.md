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

The exported per-pixel entry points are `vr_px_composite`, `vr_px_highlight`,
`vr_px_mesh`, `vr_px_nv12`, `vr_px_transition`, `vr_px_fx_point`,
`vr_px_fx_window`, `vr_px_fx_bloom_cut`, `vr_px_fx_bloom_add`, `vr_px_fx_blur`,
`vr_px_fx_pixelate`, `vr_px_fx_rgb_split` and `vr_px_fx_glitch`. Each CUDA
kernel is a five-line wrapper: map a thread to a pixel, bounds-check, call the
shared function.

`vr_px_mesh` is the one whose loop is inverted — triangles inside a pixel rather
than pixels inside a triangle — and that inversion exists precisely so this
header can serve both backends. A conventional rasterizer would need a shared
depth buffer and atomics, which have no host equivalent; here the depth test is
a local variable.

The material and lighting state travels as structs rather than widening argument
lists: `MeshTextures` for the four maps a surface wears, `MeshParams` for the
lighting environment. That is deliberate — the material is a set that grows, and
without it every new map would change a signature in four places.

### `render_common.c` — the per-frame decisions

Everything a frame's appearance depends on, none of it aware of a device:

| Function | Does |
|---|---|
| `vr_evaluate_scene` | timeline → per-widget runtime state |
| `vr_compute_reveal_cutoffs` | typewriter progress → one x threshold per line |
| `vr_select_scenes` | which scene, or which pair mid-transition |
| `vr_composite_setup` | inverse matrix, destination centre, clipped bounding box |
| `vr_highlight_setup` | the highlight band's line range, colour and stencil |
| `vr_effect_sample` | Tracks → the flat `EffectGPU` POD |
| `vr_any_windowed` / `vr_any_effect` | which optional buffers to allocate at init |
| `vr_scene_lights` | the scene's light tracks → `Light[]` for one instant |
| `vr_mesh_project` | model → view → screen triangles, near-plane clipped |
| `vr_camera_view` | the look-at matrix for a moving camera |
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

## Does the split actually pay off?

The `highlight` action was added *after* the split, as a test of the claim. It
needed a new per-pixel operation, new parameters and a new pass between the
panel and the glyphs — a realistic feature, not a trivial one.

What it cost:

| File | Added | What |
|---|---|---|
| `pixel_ops.h` | ~60 lines | `vr_px_highlight` + `HighlightParams` |
| `render_common.c` | ~35 lines | `vr_highlight_setup` |
| `types.h`, `parser.c` | ~40 lines | fields and JSON parsing |
| `renderer.cu` | **11 lines** | a kernel wrapper + a launch |
| `renderer_cpu.c` | **9 lines** | two nested loops |

Twenty lines of backend code for a feature that draws pixels, and the two
backends' output was **bit-identical** (155,520,000 bytes compared, 0 differing)
on the first run that compiled.

Two details fell out of the shared design rather than being implemented twice:

- The band reuses `CompositeParams`, so it is back-projected through the same
  inverse matrix as the text and inherits position, scale and rotation for free.
- It samples the code panel's alpha as a stencil, so it follows the panel's
  rounded corners — one texture read, written once.

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
| `anim/space3d.json` (perspective) | 921,600 / frame | 7 | 4 |
| `scene.json` | 93,312,000 | 0 | — |

Worst-case PSNR 106 dB. Two of the four projects are bit-identical across
backends.

**Why they are not always identical:** the GPU contracts a multiply and an add
into a single FMA where the CPU rounds twice. The results differ by one ulp,
which after the `×255 + 0.5` conversion occasionally lands on the other side of a
rounding boundary — hence a handful of ±1 bytes.

**Consequence for tests:** regression tests must compare GPU against GPU, or CPU
against CPU. A cross-backend test needs a tolerance, never `cmp`.

±1 covers the affine compositor. The **perspective** path divides, which
amplifies the same rounding: measured at 7 differing pixels a frame with a max
delta of 4. Still invisible, but a tolerance of ±1 would fail there.

**Meshes** divide twice — once to project and once more per pixel for
perspective-correct texturing — so they diverge the most, though still only at
the last bit. Measured over the first 0.4 s of three projects:

| Project | bytes | differing | share |
|---|---|---|---|
| a glTF import, untextured surfaces | 9,331,882 | 12 | 0.0001 % |
| `anim/world3d.json` (textured ground, 10 meshes) | 74,650,719 | 1,885 | 0.0025 % |
| two solids, depth-tested | 6,998,977 | 0 | — |

The textured project is the outlier, and for a reason worth knowing: a texture
turns a one-bit difference in the interpolated coordinate into a jump to the
*neighbouring texel*, which on a checker is a large colour step rather than a
small one. Sampling is nearest-neighbour, so there is nothing to smooth it. The
lesson is not that the mesh path is less accurate — it is that a discontinuous
function amplifies whatever rounding it is handed.

### Header dependencies must cover both compilers

`pixel_ops.h` is compiled twice, once by gcc and once by nvcc. For a while only
the gcc half generated `.d` files, so editing that header rebuilt
`renderer_cpu.o` and left `renderer.o` stale.

That failure mode is worth naming because of how it presents. The two backends
are supposed to agree bit-for-bit, so a stale object does not look like a build
problem — it looks like a bug in the code you just wrote, in whichever backend
happens to be the stale one. It cost a real debugging session: a fix to the
rasterizer's fill rule appeared to have no effect at all, and the next suspect
was the fix rather than the build.

`NVCCFLAGS` now carries `-MMD -MP` too, and `DEPS` covers every object rather
than only the C ones. If you add a third compilation path, give it the same
treatment.

### The two backends must not share a build directory

The same failure mode, one level up, and it hides better.

Both backends produce a binary called `video_redac`, and for a while both wrote
their objects into `build`. So `make CPU=1` immediately after a CUDA build found
the binary newer than every object it cared about, said "nothing to be done",
and left the CUDA binary sitting there. A CPU-versus-GPU comparison run that way
compares the GPU against itself.

That is worse than a stale object, because the answer it gives is the answer you
were hoping for: perfect agreement, zero differing pixels, on a code path
neither backend had actually exercised twice. It was caught only because the
result was *too* good — the 2D pipeline agrees exactly, but the mesh path never
has, and a sudden 0.000% on a freshly written rasterizer is not a success, it is
a measurement that failed to measure.

`BUILD` is now `build/gpu` or `build/cpu`, which fixes exactly half of it. Going
*back* to the CUDA build, its objects are all present and the binary the CPU
build has just written is newer than every one of them — so make says "nothing
to be done" a second time, in the other direction, and the first fix looks
complete because the case it repairs is the one you happen to test.

No timestamp can express *which* backend produced `./video_redac`, so the answer
is written down. `build/.backend` is a file whose content is the backend name,
rewritten only when that name changes; the binary depends on it, so switching
relinks and staying put remains a no-op. The link banner names the backend too,
which is the cheap part and the part that makes the mistake visible.

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
