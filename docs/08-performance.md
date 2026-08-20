# Performance

All figures measured on an RTX 5070 (Blackwell, sm_120), CUDA 13.3,
ffmpeg 8.1.2, rendering to H.264 via `h264_nvenc` at `-preset p5 -cq 21`.

## Headline

| Project | Resolution | Frames | Time | fps |
|---|---|---|---|---|
| `showcase.json` | 1080×1920 @ 60 | 1320 | 3.41 s | 387 |
| `listing_60.json` | 1080×1920 @ 30 | 1800 | 4.62 s | 390 |
| `binsearch.json` | 1080×1920 @ 60 | 1440 | 3.70 s | 389 |
| `transitions.json` | 1080×1080 @ 60 | 636 | 1.08 s | 589 |
| `effects_demo.json` | 1280×720 @ 30 | 180 | 0.42 s | 433 |

Roughly 390 fps at 1080×1920 — about 6.5× real time at 60 fps.

## Where the time goes

The renderer is **encoder-bound**, not compute-bound. Measured by feeding
ffmpeg the same volume of data with no rendering at all:

| Configuration | 600 frames @ 1080×1920 |
|---|---|
| Raw RGBA pipe + NVENC, **zero rendering** | 2.10 s |
| Full renderer, RGBA | 1.94 s |
| NV12 pipe + NVENC, zero rendering | 1.78 s |
| **Full renderer, NV12** | **1.66 s** |
| Pipe transport only (ffmpeg discards) | 0.57 s |

The renderer runs *at* the ceiling. Of the remaining 1.66 s, roughly 0.57 s is
pipe transport and ~1.1 s is NVENC itself.

Independently confirmed by sampling the hardware during a render:

```
SM (CUDA cores) :  ~24%
NVENC (encoder) :  100%
```

> A monitoring widget that **adds** these engines will show >100% — they are
> independent hardware blocks, each reporting its own load. Nothing is wrong.
> Almost no other workload saturates NVENC while also running CUDA kernels,
> which is why this program in particular triggers that display artifact.

## What actually helped: NV12 on the GPU

| | fps |
|---|---|
| RGBA pipe | 306–315 |
| NV12 pipe | 355–361 |

A ~17% gain, from two effects: 2.67× fewer bytes crossing PCIe and the pipe
(8.29 MB → 3.11 MB per frame), and the RGB→YUV conversion moving off the CPU,
where ffmpeg had been doing it every frame.

Fixing the colour tagging afterwards removed a hidden `swscale` RGB round-trip
as well — that was a correctness fix that also happened to be free performance.

## What did not help: double buffering

| | fps |
|---|---|
| `VR_PIPELINE_DEPTH=1` (serial) | 306, 315 |
| `VR_PIPELINE_DEPTH=2` (overlapped) | 308, 298 |

Within noise. The GPU work was already fully hidden behind the pipe write, so
there was nothing left to overlap. The mechanism is architecturally correct and
costs nothing, but it was aimed at the wrong bottleneck.

This is why the A/B was worth running: the intuition ("overlap render and
encode") was reasonable and simply wrong for this workload.

## Effects are nearly free

1320 frames at 1080×1920, `showcase.json`:

| | fps |
|---|---|
| No effects | 391.7 |
| 7 effects (`color_grade`, `blur r=6`, `rgb_split`, `split_tone`, `vignette`, `grain`, `scanlines`) | 387.0 |

**1.2%.** The SMs had ~76% headroom, so a full post-processing stack fits inside
it. This is where the architecture pays off relative to a CPU compositor, where
the same stack would cost tens of milliseconds per frame.

**What that figure does and does not say.** `showcase.json` is encoder-bound at
1080×1920, so the measurement says the effect stack fits inside headroom that
already existed — which is the useful claim — but it is not a measurement of the
effects' own cost. On a render-bound project they will not be free in the same
way; see the motion-blur numbers below for what that looks like.

## Memory

At 1080×1920, per pipeline slot:

| Buffer | Size | Allocated when |
|---|---|---|
| `d_frame` (RGBA) | 7.91 MB | always |
| `d_nv12` | 2.97 MB | always |
| `h_frame` (pinned) | 2.97 MB | always — 7.91 MB when `output.alpha` is set |
| `d_depth` | 7.91 MB | a scene contains meshes |
| `d_fx` | 7.91 MB | any effect exists |
| `d_keep` | 7.91 MB | an effect has a power window or a qualifier |
| `d_bloom` | 7.91 MB | a `bloom` effect exists |
| `d_accum` (float4) | 31.6 MB | `motion_blur` with more than one sample |
| `d_scene[2]` | 15.82 MB | more than one scene |

With two slots that is 21.8 MB of frame buffers for a simple project, up to
~69 MB with effects and scenes, and ~200 MB for a project using motion blur,
bloom and windows at once.

`d_accum` is four times a frame because it is `float4`. That is deliberate:
rounding each sub-frame back to a byte before summing throws away exactly the
fractional detail the averaging exists to recover, and a slow pan comes out
stepped instead of smooth.

## Motion blur

Cost is linear in the sample count, because each sample is a full render of the
scene stage. Measured on `anim/materials.json` (3D, 1600×900, seconds 7–10 —
imported props with normal, occlusion and emissive maps):

| Samples | Time | fps | vs off |
|---|---|---|---|
| off | 6.47 s | 27.8 | — |
| 8 | 39.80 s | 4.5 | 6.2× |
| 16 | 89.48 s | 2.0 | 13.9× |

Slightly better than N× because the film-wide effects, the NV12 conversion and
the encode still happen once per output frame.

**On a 2D project it is free, and that is not a good thing.** `showcase.json`
with 8 samples measures the same as without, because the pipeline there is
encoder-bound rather than render-bound — and the frame is also nearly identical
(0.06% of pixels differ, max delta 2), since almost nothing in it moves fast
enough to smear. Motion blur is worth its cost only where something is actually
travelling: the blur length follows the subject's speed, so if you cannot see
it, you did not need it.


Textures dominate for image-heavy projects — `listing_60.json` uses 164 MB
because photos are stored at full resolution (2048×1365) even when displayed
smaller. Downscaling at load time is an obvious future saving.

Pinned host memory matters: a pageable buffer would make the driver stage the
copy through its own pinned buffer first, doubling the work and making the
transfer effectively synchronous.

## The CPU backend

Same machine (20 threads), `showcase.json`, 1320 frames at 1080×1920:

| Backend | Encoder | Time | fps | vs GPU |
|---|---|---|---|---|
| CUDA | `h264_nvenc` | 3.41 s | 387 | 1.0× |
| CPU | `libx264 -preset medium` | 17.09 s | 77 | 5.0× slower |

Five times slower — and still 1.3× real time at 60 fps, which makes it usable
rather than merely correct.

Isolating the compositing by rendering to rawvideo (300 frames, no encoder in the
way):

| Threads | fps |
|---|---|
| 1 | 19.5 |
| 4 | 64.5 |
| 20 | 130.7 |

At 130 fps the run is writing ~400 MB/s of raw frames, so this understates how
well the compositing itself parallelises. The useful conclusion is the same as on
the GPU: **the encoder is the bottleneck on both backends.** `libx264` is simply
a much lower ceiling than NVENC.

Details: [09-backends.md](09-backends.md).

## Scaling notes

- **Object count is cheap.** `binsearch.json` composites 86 objects per frame at
  389 fps. Each layer's grid covers only its destination bounding box, so small
  objects cost almost nothing.
- **Resolution is the main lever**, since the bottleneck is bytes and encoder
  throughput. 1280×720 renders at 430–590 fps.
- **`hevc_nvenc`** has more headroom than H.264 on Blackwell if the encoder is
  the limit.
- **Faster presets** (`p4`, `p1`) trade file size for speed. On the CPU backend
  the equivalent lever is `libx264`'s `-preset` (`veryfast` over `medium` is
  worth several times the encoder's throughput).
- **Threads** only help the CPU backend, and only up to the encoder's ceiling.

## Reproducing

```bash
# headline
./video_redac showcase.json -o /tmp/t.mp4

# encoder ceiling, no rendering
dd if=/dev/zero bs=3110400 count=600 2>/dev/null | \
  ffmpeg -hide_banner -loglevel error -y -f rawvideo -pixel_format nv12 \
    -video_size 1080x1920 -framerate 60 -i - -c:v h264_nvenc \
    -preset p5 -cq 21 /tmp/ceiling.mp4

# double-buffering A/B
make clean && make CPPFLAGS_EXTRA=-DVR_PIPELINE_DEPTH=1

# engine utilisation during a render
nvidia-smi --query-gpu=utilization.gpu,utilization.encoder --format=csv -l 1

# CPU backend, and its thread scaling
make clean && make CPU=1
./video_redac showcase.json -o /tmp/cpu.mp4
OMP_NUM_THREADS=1 VIDEO_REDAC_ENCODER=rawvideo \
  ./video_redac showcase.json -o /tmp/t.nut --range 0:5

# CPU vs GPU agreement (expect max delta 1, not 0 — see 09-backends.md)
ffmpeg -v error -i cpu.nut -i gpu.nut -lavfi psnr -f null -
```
