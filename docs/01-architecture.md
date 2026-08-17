# Architecture

## The pipeline

```
video.json
    │
    ▼  parse_video_project_ex()            [parser.c]
EditorContext ──── ${vars} → styles → scenes → objects
    │                → timeline → effects → audio → transitions
    │
    ▼  media_prepare_textures()            [media_loader.c]
Textures (host RAM) ─── Cairo draws text/shapes once
    │                   stb_image loads pictures
    │                   "center" / "bottom-160" / anchors are resolved here
    │
    ▼  renderer_init()                     [renderer.cu]
VRAM ─── textures uploaded once; frame buffers, effect ping-pong,
    │    scene buffers, pinned host staging
    │
    ▼  render_video() — the frame loop
    │
    ├─ [CPU] evaluate_scene()          → WidgetRuntime[] in the arena
    ├─ [GPU] k_clear_background        → scene background
    ├─ [GPU] k_composite_texture × N   → layers, via inverse matrix
    ├─ [GPU] scene effects             → ping-pong
    ├─ [GPU] k_transition              → two scenes into one frame (if needed)
    ├─ [GPU] global effects
    ├─ [GPU] k_rgba_to_nv12            → 8.29 MB → 3.11 MB
    ├─ [D2H] cudaMemcpyAsync           → pinned host RAM
    └─ [PIPE] fwrite → ffmpeg/NVENC    → .mp4
                                            │
                                            ▼  audio_mux() [audio.c]
                                        second pass: audio + `-c:v copy`
```

## Why text is rasterized on the CPU

Glyph rasterization — hinting, kerning, ligatures, antialiasing — is
sequential, branch-heavy work that maps badly onto a GPU's SIMT model. In
exchange it happens **once**: in a 3,600-frame video the title is drawn once and
*composited* 3,600 times.

This is the central design decision; almost everything else follows from it —
the texture cache, storing `base_w`/`base_h` separately from texture size, and
the glyph metrics used by the typewriter effect.

## Why NV12 conversion happens on the GPU

A raw RGBA frame at 1080×1920 is 8.29 MB; the same frame as NV12 is 3.11 MB.
That 2.67× reduction is saved twice — once over PCIe on the device-to-host copy,
and again writing into the ffmpeg pipe. On top of that, the RGB→YUV conversion
used to be done *by ffmpeg on the CPU*, every frame.

Measured: 306 → 361 fps (≈17%). See [08-performance.md](08-performance.md).

## The scene model

A scene is a clip with its own duration, objects, timeline, background and
effects. Objects live in **shared arrays** (`ctx->texts`, `ctx->codes`, …); a
scene stores only a contiguous span of `ctx->widgets`:

```c
sc->first_widget   /* index into ctx->widgets */
sc->widget_count
```

Contiguity is guaranteed because `z_order` increases **globally** across all
scenes, and `ctx->widgets` is sorted by `z_order`. A scene's objects are
therefore always adjacent in the index.

### Time

Time inside a scene is **local**: `t = 0` means the start of that scene. Moving
a scene along the timeline therefore requires no recomputation of its contents.

Start times, accounting for transition overlap:

```
start[0] = 0
start[i] = start[i-1] + duration[i-1] - transition[i-1].duration
total    = start[n-1] + duration[n-1]
```

### Which scene is visible

```c
/* the first scene that has not finished yet */
for (i = 0; i < scene_count; i++)
    if (time_ms < scenes[i].start_ms + scenes[i].duration_ms) { si = i; break; }
```

**It must be "first unfinished", not "last started".** During the overlap window
both neighbours are active and `from` is the *earlier* one. Picking the last
started scene selects the following pair instead, and the transition never gets
drawn — this was a real bug, fixed exactly this way.

### Flat mode

If the JSON has no `scenes`, `parse_scene()` is called on the root itself and
one implicit scene is created, leaving the renderer with a single code path.

> **Trap:** in flat mode `node == root`, so the scene must **not** read the
> root's `effects` — that is the global stack and is parsed separately. Reading
> it twice caused every effect to be applied twice. Guarded by
> `parse_scene(..., bool is_root)`.

## Memory model

| Resource | Lives in | Freed by |
|---|---|---|
| Strings, arrays | host heap | `editor_context_free()` |
| `Texture.pixels` | host heap | `texture_free()` |
| `Texture.d_pixels` | VRAM | `renderer_shutdown()` |
| `GlyphMetrics.d_cutoff` | VRAM | `renderer_shutdown()` |
| Frame / scene / effect buffers | VRAM | `renderer_shutdown()` |
| `WidgetRuntime[]` | arena | `arena_reset()` each frame |

The **arena** holds one frame's worth of scratch memory. At 60 fps, calling
`malloc`/`free` per frame would mean fragmentation and unpredictable latency.
Instead one 4 MiB block is taken once, `used` is bumped during the frame, and
the whole thing is released with a single assignment — O(1).

## Double buffering

`VR_PIPELINE_DEPTH = 2` slots, each with its own stream, event, frame buffer and
pinned host buffer. The loop is:

```
iteration i:  first write frame i-1 to the pipe (its GPU work is done),
              then submit frame i.
```

Safety argument: before slot `s` is overwritten, frame `i-2` is guaranteed
complete, because its event was awaited on the previous iteration.

> **Measured:** double buffering produced **no speedup** in this project — the
> GPU work was already fully hidden. It is architecturally correct and free, but
> the bottleneck was elsewhere. See [08-performance.md](08-performance.md).

## Alpha convention

Every texture is **premultiplied** RGBA8.

- Cairo's `CAIRO_FORMAT_ARGB32` is premultiplied already;
- un-premultiplying loses precision on antialiased edges (dark fringing around
  glyphs);
- bilinear interpolation and fading are **only** correct in premultiplied space.

`stb_image` returns straight alpha, so images are premultiplied at load time.

The blend used in kernels:

```
dst = src·α + dst·(1 − src.a·α)
```

## Error handling

Two levels on the CUDA side:

- `gpuErrchk(call)` — a programmer error (bad launch config, corrupt pointer)
  → exit immediately. "Recovering" from these would only hide the real defect.
- `CUDA_TRY(call)` — an environment-dependent failure (out of VRAM) → return
  `false` so `renderer_shutdown()` can clean up.

On the host side a missing JSON field is never an error — every field has a
default. Only genuine failures return `NULL`.
