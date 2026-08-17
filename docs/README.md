# video_redac — Documentation

From a JSON description to an MP4: layout and text rasterization on the CPU,
compositing and effects on the GPU, encoding on NVENC.

> **Note on language.** These documents are in English; the source comments are
> in Georgian. Both describe the same code — where they disagree, the code wins.

## Contents

| File | Covers |
|---|---|
| [01-architecture.md](01-architecture.md) | Pipeline, data flow, core design decisions |
| [02-data-model.md](02-data-model.md) | Every struct and field |
| [03-json-reference.md](03-json-reference.md) | Complete JSON schema: every key, type, default |
| [04-modules.md](04-modules.md) | Every host-side module and function |
| [05-cuda-kernels.md](05-cuda-kernels.md) | All nine kernels, device helpers, the maths |
| [06-algorithms.md](06-algorithms.md) | The algorithms in depth — and why they are what they are |
| [07-cli-and-build.md](07-cli-and-build.md) | CLI, Makefile, sanitizers, troubleshooting |
| [08-performance.md](08-performance.md) | Measurements and bottleneck analysis |

## Quick start

```bash
make
./video_redac showcase.json -o out/showcase.mp4
```

A minimal project:

```json
{
  "project": { "width": 1080, "height": 1920, "fps": 30, "duration_ms": 3000 },
  "objects": [
    { "id": "t", "type": "text", "content": "Hello",
      "size": 72, "x": "center", "y": "center", "anchor": "center" }
  ],
  "timeline": [
    { "time_ms": 0, "action": "fade_in", "target": "t", "duration_ms": 600 }
  ]
}
```

## Two modes

**Flat** — top-level `objects` and `timeline`. One implicit scene.

**Scenes** — top-level `scenes` (each with its own `objects`, `timeline`,
`duration`) plus `transitions`. Time inside a scene is local to that scene.

Both use the same code path: flat mode is simply a one-scene project. See
[01-architecture.md](01-architecture.md#the-scene-model).

## Core invariants

Three rules hold throughout the codebase. Breaking any of them is a bug source:

1. **No global variables.** State lives in `EditorContext` and is passed by
   pointer. The only exceptions are `const` lookup tables.
2. **Textures are premultiplied alpha.** Cairo produces them that way, and both
   bilinear interpolation and fading are only correct in that space.
3. **Frame evaluation is a pure function of time.** A frame depends only on its
   own timestamp, never on the previous frame — which is why `--range` works.
