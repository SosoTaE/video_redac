# CLI, build and tooling

## Command line

```
video_redac [project.json] [options]
```

| Option | Meaning |
|---|---|
| `-o`, `--output FILE` | output file (default `output.mp4`) |
| `-r`, `--range A:B` | render only seconds A..B |
| `-c`, `--check` | validate and exit; 0 = clean, 1 = problems found |
| `-s`, `--set KEY=VALUE` | set a `${variable}`; repeatable |
| `-d`, `--dump` | print the parsed project and continue |
| `-n`, `--dry-run` | parse and rasterize, never touch the GPU |
| `-h`, `--help` | usage |

Environment: `VIDEO_REDAC_ENCODER` overrides `output.encoder`.

Exit status is 0 on success, 1 on any failure — usable from scripts and CI.

### `--range`

```bash
./video_redac anim/binsearch.json --range 13:17 -o preview.mp4
# frames 780..839 (240 frames, 13.00–17.00 s) @ 60 fps
```

Verified bit-identical to the corresponding frames of a full render. It works
because frame evaluation depends only on its own timestamp.

Ranges past the end are clamped; `B <= A` or a malformed value is rejected.

Two things it makes practical: iterating on one moment of a long video, and
running `compute-sanitizer`, which is 10–100× slower than a normal run.

### `--check`

Reports, per scene where relevant:

- zero duration; no objects at all
- duplicate ids **within one scene** (across scenes is legitimate)
- zero-size objects; textures that failed to rasterize
- objects entirely off-canvas
- **text** clipped by the canvas edge (for images this is only a note — cover
  images are meant to be cropped)
- unknown actions; unresolved timeline targets; events past the scene's end
- audio starting after the video ends

```bash
./video_redac listing/listing_60.json --check && echo OK
```

---

## Build

```bash
make            # build (CUDA + NVENC)
make CPU=1      # build the CPU backend — no CUDA toolkit or GPU required
make run        # build + render showcase.json into out/
make debug      # ASAN/UBSAN build with -G device code
make sanitize   # CPU build under ASAN/UBSAN — covers the whole pixel pipeline
make info       # environment diagnostics
make clean      # remove objects and binary
make distclean  # also remove out/
```

`.c` files go through `gcc` (C11), `.cu` through `nvcc`, and linking is done by
`nvcc` so it can add the CUDA runtime and perform device linking. Header
dependencies are tracked with `-MMD -MP`.

### Choosing a backend

`src/renderer.cu` and `src/renderer_cpu.c` define the same three symbols, so
exactly one is compiled — the Makefile filters the other out of the wildcard.
`CPU=1` also switches the linker to plain `gcc`, drops `-L$(CUDA_HOME)/lib64`
and adds `-fopenmp`; nothing in that path refers to CUDA, which is the point.

Each backend gets its own object directory — `build/gpu` and `build/cpu` — so
switching between them without a `make clean` works. They used to share one, and
since the two also share the binary's name, `make CPU=1` after a CUDA build
found the binary newer than every object it cared about and reported "nothing to
be done", leaving the CUDA binary in place. See
[09-backends.md](09-backends.md) for why that particular silence is expensive.

Details and the shared-code layout: [09-backends.md](09-backends.md).

### GPU architecture

```make
GENCODE ?= -gencode arch=compute_89,code=sm_89 \
           -gencode arch=compute_120,code=sm_120 \
           -gencode arch=compute_120,code=compute_120
```

A fat binary for Ada (sm_89) and Blackwell (sm_120) plus PTX for future
architectures. A cubin built only for sm_89 will **not** run on Blackwell — it
fails with *"no kernel image is available for execution on the device"*.

Restrict it with `make GENCODE="-arch=sm_120"`.

### Host compiler

```make
NVCC_CCBIN ?= $(shell command -v g++-15 || command -v g++-14 || command -v g++)
```

CUDA 13.x refuses host compilers newer than gcc 15. On distributions where the
default is gcc 16 this pin is what makes the build work at all. Override with
`make NVCC_CCBIN=g++-14`.

### Other knobs

| Variable | Purpose |
|---|---|
| `CPPFLAGS_EXTRA` | extra defines, e.g. `-DVR_PIPELINE_DEPTH=1` |
| `CUDA_HOME` | CUDA location (default `/opt/cuda`) |

---

## Sanitizers

### Host — ASAN / UBSAN / LSAN

```bash
make debug GENCODE="-arch=sm_120"
ASAN_OPTIONS=detect_leaks=1 ./video_redac project.json --check
```

> **ASAN cannot be combined with a real render — in the CUDA build.** The CUDA
> runtime fails at the very first call (`cudaGetDeviceCount` → out of memory)
> because ASAN's shadow mapping collides with CUDA's address-space
> requirements. This is a documented incompatibility, not a defect in this
> project — NVIDIA recommends `compute-sanitizer` for device code.
>
> Use `--check` or `--dry-run`, which never touch CUDA.

### Whole-pipeline — `make sanitize`

The restriction above is a property of the CUDA runtime, not of the renderer. The
CPU backend has no CUDA runtime, so it can be sanitized while actually rendering:

```bash
make sanitize
ASAN_OPTIONS=detect_leaks=1 ./video_redac showcase.json --range 0:1
```

This is the only way to get ASAN/UBSAN over the compositor, the effect stack and
the NV12 conversion — code that in the CUDA build is reachable only through
kernels. Since both backends share `pixel_ops.h`, a bug found here is a bug in
the kernels too.

`media_shutdown()` releases Cairo/fontconfig's global font caches so LSAN's
report stays empty and genuine leaks remain visible.

### Device — compute-sanitizer

```bash
compute-sanitizer --tool memcheck --leak-check full \
    ./video_redac anim/transitions.json --range 1.2:1.8 -o /tmp/t.mp4

compute-sanitizer --tool initcheck \
    ./video_redac effects_demo.json --range 0:0.4 -o /tmp/t.mp4
```

`memcheck` finds out-of-bounds accesses and VRAM leaks. `initcheck` finds reads
of uninitialised device memory — worth running separately, because it is the
tool that would catch a ping-pong or scene buffer being read before it is
written.

All nine kernels pass both with zero errors and zero leaked bytes.

---

## Regression testing

Because rendering is deterministic, output is byte-comparable:

```bash
md5sum out/showcase.mp4
./video_redac showcase.json -o /tmp/new.mp4
md5sum /tmp/new.mp4          # must match
```

This is a strict test and it has earned its keep: it caught per-scene effects
being applied twice in flat mode, which an "does it still render?" check would
have missed entirely.

When comparing renders of *different lengths*, compare before the encoder:

```bash
VIDEO_REDAC_ENCODER=rawvideo ./video_redac p.json --range 13:17 -o /tmp/a.nut
```

Two H.264 encodes of different lengths compress the same frame differently even
when the pixels are identical.

---

## Troubleshooting

**`no kernel image is available for execution on the device`**
The binary lacks your GPU's architecture. Check `make info` and set `GENCODE`.

**`nvcc fatal: unsupported GNU version`**
Host compiler too new; set `NVCC_CCBIN` to gcc ≤ 15.

**`error: could not start ffmpeg (is it on PATH?)`**
`ffmpeg` is not on `PATH`, or the chosen encoder is unavailable. Check with
`ffmpeg -hide_banner -encoders | grep nvenc`.

**`error: no CUDA-capable GPU found`**
The CUDA build needs an NVIDIA GPU. Either build the CPU backend
(`make CPU=1`), or use `--check` / `--dry-run`, which never touch CUDA.

**`x264 [error]: invalid preset 'p5'`**
An NVENC preset reached a software encoder. `vr_open_ffmpeg_pipe` drops a
project's preset across an encoder-family fallback, so this means one was forced
— check `output.preset` against `$VIDEO_REDAC_ENCODER`.

**Text renders as empty boxes**
The font lacks those glyphs. Cairo's toy API does no per-glyph fallback, so a
string is drawn with one family. Notably no monospace font ships Georgian, so
non-Latin text belongs in sans layers rather than code blocks.

**Colours look shifted**
Check that both input and output are tagged BT.709/limited; untagged raw input
makes ffmpeg insert a conversion. See
[06-algorithms.md](06-algorithms.md#the-colour-tagging-bug).

**Everything piles up in the top-left corner**
A position expression is being treated as a track. Compare `--dump` (which
prints `base->x`) against the render — if they disagree, that is the symptom.

**Object refuses to hide**
A constant `"opacity": 0` being ignored. See
[06-algorithms.md](06-algorithms.md#the-constant-property-trap).
