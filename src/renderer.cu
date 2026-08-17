/*
 * renderer.cu — CUDA compositing + the FFmpeg/NVENC pipe.
 *
 * A frame's journey (with double buffering):
 *
 *   [CPU]  timeline evaluation (in the arena) →  per-widget runtime state
 *     ↓                                          (opacity, position, angle, reveal)
 *   [H2D]  typewriter cutoffs → VRAM          →  one float per line
 *     ↓
 *   [GPU]  k_clear_background                →  the whole frame in one colour
 *     ↓
 *   [GPU]  k_composite_texture × N           →  layers placed via an inverse
 *     ↓                                          matrix (scale + rotation)
 *   [GPU]  k_rgba_to_nv12                    →  8.29 MB → 3.11 MB (1080x1920)
 *     ↓
 *   [D2H]  cudaMemcpyAsync → pinned host RAM
 *     ↓
 *   [PIPE] fwrite → ffmpeg -c:v h264_nvenc   →  .mp4
 *
 * Double buffering: while the CPU writes frame i-1 into the pipe (and NVENC
 * encodes it), the GPU is already rendering frame i.
 *
 * Measured result: the bottleneck here is *neither* compositing *nor* encoding,
 * but moving the frame to the host and writing it into the pipe. That is why
 * the NV12 conversion — not double buffering — is what actually wins: it
 * directly reduces the bytes that have to travel.
 */

#include <cuda_runtime.h>

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "renderer.h"
#include "arena.h"
#include "pixel_ops.h"
#include "render_common.h"
#include "audio.h"
#include "effects.h"

/* ------------------------------------------------------------------------- */
/* Strict CUDA error checking                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Two levels, deliberately:
 *
 *   gpuErrchk(call)      — a programmer error (bad launch config, corrupt
 *                          pointer): abort immediately.
 *
 *   CUDA_TRY(call)       — an expected, environment-dependent failure (out of
 *                          VRAM): return false so renderer_shutdown() can
 *                          clean everything up.
 */
static void gpu_assert(cudaError_t code, const char *file, int line, bool abort_on_error)
{
    if (code == cudaSuccess) {
        return;
    }
    fprintf(stderr, "\nCUDA error: %s (%s)\n  → %s:%d\n",
            cudaGetErrorString(code), cudaGetErrorName(code), file, line);
    if (abort_on_error) {
        exit(EXIT_FAILURE);
    }
}

#define gpuErrchk(ans) gpu_assert((ans), __FILE__, __LINE__, true)

#define CUDA_TRY(ans)                                          \
    do {                                                       \
        cudaError_t err_ = (ans);                              \
        if (err_ != cudaSuccess) {                             \
            gpu_assert(err_, __FILE__, __LINE__, false);       \
            return false;                                      \
        }                                                      \
    } while (0)

#define CUDA_CHECK_KERNEL() gpuErrchk(cudaGetLastError())

/* ------------------------------------------------------------------------- */
/* Device resources (hidden behind the opaque EditorContext::gpu)             */
/* ------------------------------------------------------------------------- */

/* One "slot" = one frame in flight. */
struct FrameSlot {
    uchar4      *d_frame;   /* RGBA frame buffer in VRAM (the compositing target) */
    float       *d_depth;   /* shared per-pixel depth for meshes; NULL if none    */
    ScreenTri   *d_tris;    /* projected mesh triangles, re-uploaded each frame   */
    ScreenTri   *h_tris;    /* pinned staging for that upload                     */
    size_t       tri_cap;   /* how many fit                                       */
    uchar4      *d_fx;      /* ping-pong buffer for the effects                   */
    uchar4      *d_scene[2];/* two scenes during a transition; NULL if only one  */
    uint8_t     *d_nv12;    /* the same frame as NV12 — only this goes to the host */
    uint8_t     *h_frame;   /* pinned host staging — for fast DMA                 */
    cudaStream_t stream;
    cudaEvent_t  done;      /* recorded once the D2H copy has completed           */
};

struct RenderResources {
    FrameSlot slot[VR_PIPELINE_DEPTH];
    size_t    frame_bytes;  /* size of the RGBA framebuffer (in VRAM)            */
    size_t    nv12_bytes;   /* w*h*3/2 — what actually crosses the bus           */
    int       width;
    int       height;
    size_t    texture_bytes;
    int       texture_count;
};

/* ------------------------------------------------------------------------- */
/* KERNELS                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * The kernels below are deliberately thin.
 *
 * All the pixel arithmetic lives in include/pixel_ops.h, which compiles both
 * here (as __device__ code) and in src/renderer_cpu.c (as ordinary C). A kernel
 * therefore does only what is genuinely CUDA's job: map a thread to a pixel,
 * bounds-check it, and call the shared function.
 */

/*
 * Filling the background.
 *
 * One thread = one pixel. A 16x16 block (256 threads) is a clean multiple of
 * the warp size and gives neighbouring threads coalesced memory access — a
 * whole row is written in a single transaction.
 */
__global__ void k_clear_background(uchar4 *__restrict__ fb, int width, int height, uchar4 bg)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= width || y >= height) {
        return; /* edge blocks are always partly "empty" */
    }
    fb[(size_t)y * width + x] = bg;
}

/* RGBA → NV12; one thread per 2x2 block. See vr_px_nv12() for why this matters. */
__global__ void k_rgba_to_nv12(const uchar4 *__restrict__ rgba,
                               uint8_t *__restrict__ y_plane,
                               uint8_t *__restrict__ uv_plane,
                               int width, int height)
{
    int bx = blockIdx.x * blockDim.x + threadIdx.x; /* index of the 2x2 block */
    int by = blockIdx.y * blockDim.y + threadIdx.y;

    vr_px_nv12(rgba, y_plane, uv_plane, width, height, bx, by);
}

/* ========================================================================= */
/* POST-PROCESSING effects                                                   */
/* ========================================================================= */

/* Kernel for the pointwise effects — several of them share this one pass. */
__global__ void k_fx_point(uchar4 *__restrict__ dst, const uchar4 *__restrict__ src,
                           int w, int h, EffectGPU fx, unsigned int seed)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) {
        return;
    }
    vr_px_fx_point(dst, src, w, h, &fx, seed, x, y);
}

__global__ void k_fx_blur(uchar4 *__restrict__ dst, const uchar4 *__restrict__ src,
                          int w, int h, int radius, int horizontal)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) {
        return;
    }
    vr_px_fx_blur(dst, src, w, h, radius, horizontal, x, y);
}

__global__ void k_fx_pixelate(uchar4 *__restrict__ dst, const uchar4 *__restrict__ src,
                              int w, int h, int size)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) {
        return;
    }
    vr_px_fx_pixelate(dst, src, w, h, size, x, y);
}

__global__ void k_fx_rgb_split(uchar4 *__restrict__ dst, const uchar4 *__restrict__ src,
                               int w, int h, float amount, float angle_deg)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) {
        return;
    }
    vr_px_fx_rgb_split(dst, src, w, h, amount, angle_deg, x, y);
}

__global__ void k_fx_glitch(uchar4 *__restrict__ dst, const uchar4 *__restrict__ src,
                            int w, int h, float amount, unsigned int seed)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) {
        return;
    }
    vr_px_fx_glitch(dst, src, w, h, amount, seed, x, y);
}

/* ------------------------------------------------------------------------- */
/* Transition compositor                                                      */
/* ------------------------------------------------------------------------- */

__global__ void k_transition(uchar4 *__restrict__ dst,
                             const uchar4 *__restrict__ from,
                             const uchar4 *__restrict__ to,
                             TransParams p)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= p.w || y >= p.h) {
        return;
    }
    vr_px_transition(dst, from, to, &p, x, y);
}

/* ------------------------------------------------------------------------- */
/* Highlight band                                                             */
/* ------------------------------------------------------------------------- */

__global__ void k_highlight(uchar4 *__restrict__ fb, const uchar4 *__restrict__ plate,
                            HighlightParams p)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    int j = blockIdx.y * blockDim.y + threadIdx.y;

    if (i >= p.geom.bb_w || j >= p.geom.bb_h) {
        return;
    }
    vr_px_highlight(fb, plate, &p, i, j);
}

/* ------------------------------------------------------------------------- */
/* Mesh rasterizer                                                            */
/* ------------------------------------------------------------------------- */

__global__ void k_mesh(uchar4 *__restrict__ fb, float *__restrict__ depth,
                       const ScreenTri *__restrict__ tris,
                       const uchar4 *__restrict__ tex, MeshParams p)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    int j = blockIdx.y * blockDim.y + threadIdx.y;

    if (i >= p.bb_w || j >= p.bb_h) {
        return;
    }
    vr_px_mesh(fb, depth, tris, tex, &p, i, j);
}

/* Resets the shared depth buffer at the start of a scene. */
__global__ void k_depth_clear(float *__restrict__ depth, int n)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        depth[i] = 1e30f;
    }
}

/* ------------------------------------------------------------------------- */
/* Texture compositor                                                         */
/* ------------------------------------------------------------------------- */

/*
 * The grid covers only the *destination* rectangle, not the whole screen —
 * for a small title on a 1080x1920 canvas that is 50x fewer threads.
 */
__global__ void k_composite_texture(uchar4 *__restrict__ fb,
                                    const uchar4 *__restrict__ tex,
                                    const float *__restrict__ cutoff_x,
                                    CompositeParams p)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    int j = blockIdx.y * blockDim.y + threadIdx.y;

    if (i >= p.bb_w || j >= p.bb_h) {
        return;
    }
    vr_px_composite(fb, tex, cutoff_x, &p, i, j);
}



/* ------------------------------------------------------------------------- */
/* Resource setup and teardown                                                */
/* ------------------------------------------------------------------------- */

/* Uploads one texture to VRAM (if it is not there already). */
static bool upload_one_texture(Texture *t, RenderResources *res)
{
    if (t->pixels == NULL || t->width <= 0 || t->height <= 0 || t->d_pixels != NULL) {
        return true; /* empty, or already uploaded */
    }

    size_t bytes = (size_t)t->width * (size_t)t->height * sizeof(uchar4);

    void *d_ptr = NULL;
    CUDA_TRY(cudaMalloc(&d_ptr, bytes));
    CUDA_TRY(cudaMemcpy(d_ptr, t->pixels, bytes, cudaMemcpyHostToDevice));

    t->d_pixels = d_ptr;
    res->texture_bytes += bytes;
    res->texture_count++;
    return true;
}

/* Uploads every (already rasterized) texture to VRAM — once. */
static bool upload_textures(EditorContext *ctx, RenderResources *res)
{
    for (size_t i = 0; i < ctx->widget_count; i++) {
        WidgetBase *b = ctx->widgets[i];

        if (!upload_one_texture(&b->tex, res)) {
            return false;
        }

        /* A code block's panel is a separate layer → a separate texture. */
        if (b->kind == WIDGET_CODE) {
            CodeWidget *cw = (CodeWidget *)b;
            if (!upload_one_texture(&cw->plate, res)) {
                return false;
            }
        }

        /* A mesh's surface texture is its own, not base.tex — that one stays
         * empty because a mesh is rasterized rather than composited. */
        if (b->kind == WIDGET_MESH) {
            MeshWidget *mw = (MeshWidget *)b;
            if (!upload_one_texture(&mw->tex, res)) {
                return false;
            }
        }

        /*
         * The typewriter cutoff buffer: VR_PIPELINE_DEPTH sets in a single
         * allocation. Each slot owns its own share, so writing frame i cannot
         * pull the data out from under frame i-1's still-running kernels.
         */
        GlyphMetrics *g = &b->glyphs;
        if (g->line_count > 0 && g->d_cutoff == NULL) {
            size_t bytes = (size_t)g->line_count * VR_PIPELINE_DEPTH * sizeof(float);
            void  *d_ptr = NULL;
            CUDA_TRY(cudaMalloc(&d_ptr, bytes));
            g->d_cutoff = d_ptr;
            res->texture_bytes += bytes;
        }
    }
    return true;
}

bool renderer_init(EditorContext *ctx)
{
    if (ctx == NULL) {
        return false;
    }
    if (ctx->gpu != NULL) {
        return true; /* idempotent */
    }

    RenderResources *res = (RenderResources *)calloc(1, sizeof(RenderResources));
    if (res == NULL) {
        return false;
    }
    ctx->gpu = res;

    int device_count = 0;
    CUDA_TRY(cudaGetDeviceCount(&device_count));
    if (device_count == 0) {
        fprintf(stderr, "error: no CUDA-capable GPU found.\n");
        return false;
    }
    CUDA_TRY(cudaSetDevice(0));

    cudaDeviceProp prop;
    CUDA_TRY(cudaGetDeviceProperties(&prop, 0));
    fprintf(stderr, "GPU: %s (sm_%d%d, %.1f GiB VRAM)\n", prop.name, prop.major, prop.minor,
            (double)prop.totalGlobalMem / (1024.0 * 1024.0 * 1024.0));

    res->width       = ctx->config.width;
    res->height      = ctx->config.height;
    res->frame_bytes = (size_t)res->width * (size_t)res->height * sizeof(uchar4);

    /* NV12: a full Y plane + half-resolution UV → w*h*3/2. */
    res->nv12_bytes = (size_t)res->width * (size_t)res->height +
                      (size_t)((res->width + 1) / 2) * (size_t)((res->height + 1) / 2) * 2;

    for (int s = 0; s < VR_PIPELINE_DEPTH; s++) {
        FrameSlot *slot = &res->slot[s];

        CUDA_TRY(cudaMalloc((void **)&slot->d_frame, res->frame_bytes));
        CUDA_TRY(cudaMalloc((void **)&slot->d_nv12, res->nv12_bytes));
        /*
         * Mesh staging, sized to the largest mesh in the project. One
         * allocation per slot rather than per frame: the triangle data changes
         * every frame but its size never does.
         */
        size_t max_tris = 0;
        for (size_t mi = 0; mi < ctx->mesh_count; mi++) {
            ctx->meshes[mi].tri_base = max_tris;
            max_tris += ctx->meshes[mi].tri_count;
        }
        if (max_tris > 0) {
            /* One depth buffer per slot, shared by every mesh in the scene —
             * that sharing is what makes two solids interpenetrate correctly. */
            CUDA_TRY(cudaMalloc(&slot->d_depth,
                                (size_t)ctx->config.width * ctx->config.height * sizeof(float)));
            CUDA_TRY(cudaMalloc(&slot->d_tris, max_tris * sizeof(ScreenTri)));
            CUDA_TRY(cudaMallocHost(&slot->h_tris, max_tris * sizeof(ScreenTri)));
            slot->tri_cap = max_tris;
        }

        /* The ping-pong buffer only if any effects exist at all. */
        bool any_fx = ctx->effect_count > 0;
        for (size_t si = 0; si < ctx->scene_count && !any_fx; si++) {
            any_fx = ctx->scenes[si].effect_count > 0;
        }
        if (any_fx) {
            CUDA_TRY(cudaMalloc((void **)&slot->d_fx, res->frame_bytes));
        }
        /* Scene buffers only when a transition is even possible. */
        if (ctx->scene_count > 1) {
            CUDA_TRY(cudaMalloc((void **)&slot->d_scene[0], res->frame_bytes));
            CUDA_TRY(cudaMalloc((void **)&slot->d_scene[1], res->frame_bytes));
        }

        /*
         * Pinned (page-locked) host memory.
         * For an ordinary malloc'd buffer the driver would first copy into its
         * own pinned staging buffer and only then start the DMA — twice the
         * work. Here the DMA writes straight into our buffer, which roughly
         * doubles D2H throughput and, more importantly, makes it genuinely
         * asynchronous.
         */
        CUDA_TRY(cudaHostAlloc((void **)&slot->h_frame, res->nv12_bytes, cudaHostAllocDefault));
        CUDA_TRY(cudaStreamCreate(&slot->stream));
        /* DisableTiming — the event is only for synchronisation, so this is cheaper. */
        CUDA_TRY(cudaEventCreateWithFlags(&slot->done, cudaEventDisableTiming));
    }

    if (!upload_textures(ctx, res)) {
        return false;
    }

    fprintf(stderr, "VRAM: %d frame buffers %.1f MiB + %d textures %.1f MiB\n",
            VR_PIPELINE_DEPTH,
            (double)(res->frame_bytes + res->nv12_bytes) * VR_PIPELINE_DEPTH / (1024.0 * 1024.0),
            res->texture_count, (double)res->texture_bytes / (1024.0 * 1024.0));
    fprintf(stderr, "frame: %.2f MiB RGBA (VRAM) → %.2f MiB NV12 (pipe)\n",
            (double)res->frame_bytes / (1024.0 * 1024.0),
            (double)res->nv12_bytes / (1024.0 * 1024.0));
    return true;
}

void renderer_shutdown(EditorContext *ctx)
{
    if (ctx == NULL) {
        return;
    }

    /* The textures' VRAM copies — even if RenderResources was never created. */
    for (size_t i = 0; i < ctx->widget_count; i++) {
        WidgetBase *b = ctx->widgets[i];

        if (b->tex.d_pixels != NULL) {
            cudaFree(b->tex.d_pixels);
            b->tex.d_pixels = NULL;
        }
        if (b->glyphs.d_cutoff != NULL) {
            cudaFree(b->glyphs.d_cutoff);
            b->glyphs.d_cutoff = NULL;
        }
        if (b->kind == WIDGET_CODE) {
            CodeWidget *cw = (CodeWidget *)b;
            if (cw->plate.d_pixels != NULL) {
                cudaFree(cw->plate.d_pixels);
                cw->plate.d_pixels = NULL;
            }
        }

        if (b->kind == WIDGET_MESH) {
            MeshWidget *mw = (MeshWidget *)b;
            if (mw->tex.d_pixels != NULL) {
                cudaFree(mw->tex.d_pixels);
                mw->tex.d_pixels = NULL;
            }
        }
    }

    RenderResources *res = (RenderResources *)ctx->gpu;
    if (res == NULL) {
        return;
    }

    for (int s = 0; s < VR_PIPELINE_DEPTH; s++) {
        FrameSlot *slot = &res->slot[s];
        if (slot->d_frame != NULL) cudaFree(slot->d_frame);
        if (slot->d_nv12  != NULL) cudaFree(slot->d_nv12);
        if (slot->d_fx    != NULL) cudaFree(slot->d_fx);
        if (slot->d_scene[0] != NULL) cudaFree(slot->d_scene[0]);
        if (slot->d_scene[1] != NULL) cudaFree(slot->d_scene[1]);
        if (slot->h_frame != NULL) cudaFreeHost(slot->h_frame);
        if (slot->stream  != NULL) cudaStreamDestroy(slot->stream);
        if (slot->done    != NULL) cudaEventDestroy(slot->done);
    }

    free(res);
    ctx->gpu = NULL; /* → calling this again is safe */
}

/* ------------------------------------------------------------------------- */
/* The render loop                                                            */
/* ------------------------------------------------------------------------- */


/*
 * Composites one layer: computes the transform and launches the kernel.
 *
 * `tex` may be the glyph texture or the panel — the geometry is identical for
 * both; the only difference is `cutoff` (a panel is never clipped).
 */
static void composite_layer(const RenderResources *res, cudaStream_t stream, uchar4 *fb,
                            const Texture *tex, const WidgetBase *b, const WidgetRuntime *rt,
                            const float *d_cutoff)
{
    if (tex->d_pixels == NULL) {
        return;
    }

    CompositeParams p;
    if (!vr_composite_setup(res->width, res->height, tex->width, tex->height, b, rt, &p)) {
        return; /* degenerate size, or entirely off-screen */
    }

    const dim3 block(16, 16);
    dim3       grid((p.bb_w + block.x - 1) / block.x, (p.bb_h + block.y - 1) / block.y);

    k_composite_texture<<<grid, block, 0, stream>>>(fb, (const uchar4 *)tex->d_pixels,
                                                    d_cutoff, p);
    CUDA_CHECK_KERNEL();
}


/* ------------------------------------------------------------------------- */
/* Running the effect stack                                                   */
/* ------------------------------------------------------------------------- */


/*
 * Runs the whole stack and returns the buffer the final frame ended up in.
 *
 * Ping-pong: every effect reads from `src` and writes into `dst`, then the two
 * swap. Working in place is not allowed, because effects that read neighbours
 * (blur, glitch) would then read already-modified pixels and the result would
 * depend on the traversal direction.
 */
static uchar4 *apply_effect_list(const Effect *list, size_t count, RenderResources *res,
                                 int slot_idx, uchar4 *source, float t_sec, long long frame)
{
    FrameSlot *slot = &res->slot[slot_idx];
    uchar4    *src  = source;

    if (count == 0 || slot->d_fx == NULL) {
        return src;
    }

    uchar4 *dst = slot->d_fx;

    const dim3 block(16, 16);
    dim3       grid((res->width + block.x - 1) / block.x,
                    (res->height + block.y - 1) / block.y);

    unsigned int seed = vr_effect_seed(frame);

    for (size_t i = 0; i < count; i++) {
        const Effect *fx = &list[i];
        if (fx->type == FX_NONE) {
            continue;
        }

        EffectGPU g = vr_effect_sample(fx, t_sec);

        switch (fx->type) {
            case FX_BLUR: {
                int radius = vr_blur_radius(&g);
                if (radius == 0) {
                    continue;
                }
                k_fx_blur<<<grid, block, 0, slot->stream>>>(dst, src, res->width,
                                                            res->height, radius, 1);
                CUDA_CHECK_KERNEL();
                uchar4 *tmp = src; src = dst; dst = tmp;

                k_fx_blur<<<grid, block, 0, slot->stream>>>(dst, src, res->width,
                                                            res->height, radius, 0);
                CUDA_CHECK_KERNEL();
                break;
            }
            case FX_PIXELATE:
                k_fx_pixelate<<<grid, block, 0, slot->stream>>>(
                    dst, src, res->width, res->height, (int)lrintf(g.p[FXP_SIZE]));
                CUDA_CHECK_KERNEL();
                break;

            case FX_RGB_SPLIT:
                k_fx_rgb_split<<<grid, block, 0, slot->stream>>>(
                    dst, src, res->width, res->height, g.p[FXP_AMOUNT], g.p[FXP_ANGLE]);
                CUDA_CHECK_KERNEL();
                break;

            case FX_GLITCH:
                k_fx_glitch<<<grid, block, 0, slot->stream>>>(
                    dst, src, res->width, res->height, g.p[FXP_AMOUNT], seed);
                CUDA_CHECK_KERNEL();
                break;

            default:
                k_fx_point<<<grid, block, 0, slot->stream>>>(dst, src, res->width,
                                                             res->height, g, seed);
                CUDA_CHECK_KERNEL();
                break;
        }

        uchar4 *tmp = src; src = dst; dst = tmp;
    }

    return src; /* after the swaps the result lives here */
}

/* Renders one scene into the given buffer. */
static void render_scene_into(const EditorContext *ctx, RenderResources *res, int slot_idx,
                              const Scene *scene, const WidgetRuntime *rt, uchar4 *target,
                              int local_ms)
{
    FrameSlot   *slot   = &res->slot[slot_idx];
    cudaStream_t stream = slot->stream;

    /* 1. Background — a scene may have its own. */
    Color  bgc = scene->has_bg ? scene->bg_color : ctx->config.bg_color;
    uchar4 bg  = make_uchar4(bgc.r, bgc.g, bgc.b, bgc.a);

    const dim3 block(16, 16);
    dim3       grid((res->width + block.x - 1) / block.x,
                    (res->height + block.y - 1) / block.y);

    k_clear_background<<<grid, block, 0, stream>>>(target, res->width, res->height, bg);
    CUDA_CHECK_KERNEL();

    /* The depth buffer belongs to the scene, not to a mesh: clearing it here is
     * what lets several meshes occlude one another correctly. */
    float view[12];
    vr_camera_view(scene, (float)local_ms * 0.001f,
                   scene->camera.present && scene->camera.focal > 0.0f
                       ? scene->camera.focal : (float)res->width * 8.0f,
                   view);

    if (slot->d_depth != NULL) {
        int npx = res->width * res->height;
        k_depth_clear<<<(npx + 255) / 256, 256, 0, stream>>>(slot->d_depth, npx);
        CUDA_CHECK_KERNEL();
    }

    /*
     * 2. Widgets — back to front (painter's algorithm).
     *
     * With depth in play the authored order is not the drawing order any more,
     * so the indices are sorted farthest-first. A scene with no depth skips the
     * sort entirely and iterates as it always did.
     */
    int *order = NULL;
    if (scene->widget_count > 0) {
        order = ARENA_NEW(&((EditorContext *)ctx)->frame_arena, int, scene->widget_count);
        if (order != NULL && !vr_depth_order(scene, rt, order)) {
            order = NULL;
        }
    }

    for (size_t k = 0; k < scene->widget_count; k++) {
        size_t i = order ? (size_t)order[k] : k;

        const WidgetBase    *b = ctx->widgets[scene->first_widget + i];
        const WidgetRuntime *r = &rt[i];

        if (!r->visible) {
            continue;
        }

        /* 2a'. A mesh is not a texture: project it, upload, rasterize. */
        if (b->kind == WIDGET_MESH) {
            const MeshWidget *mw = (const MeshWidget *)b;
            if (slot->h_tris != NULL && mw->tri_base + mw->tri_count <= slot->tri_cap) {
                /* This mesh's own slice of the staging buffer — see tri_base. */
                ScreenTri *h_slice = slot->h_tris + mw->tri_base;
                ScreenTri *d_slice = slot->d_tris + mw->tri_base;

                MeshParams mp;
                int nt = vr_mesh_project(mw, r, res->width, res->height, view,
                                         scene->has_light ? scene->light : NULL,
                                         h_slice, &mp);
                if (nt > 0) {
                    gpuErrchk(cudaMemcpyAsync(d_slice, h_slice,
                                              (size_t)nt * sizeof(ScreenTri),
                                              cudaMemcpyHostToDevice, stream));
                    const dim3 mblock(16, 16);
                    dim3       mgrid((mp.bb_w + mblock.x - 1) / mblock.x,
                                     (mp.bb_h + mblock.y - 1) / mblock.y);
                    const uchar4 *mtex = (const uchar4 *)mw->tex.d_pixels;
                    k_mesh<<<mgrid, mblock, 0, stream>>>(target, slot->d_depth,
                                                         d_slice, mtex, mp);
                    CUDA_CHECK_KERNEL();
                }
            }
            continue;
        }

        /* 2a. The panel (only a code block has one) — typing does not clip it. */
        if (b->kind == WIDGET_CODE) {
            const CodeWidget *cw = (const CodeWidget *)b;
            composite_layer(res, stream, target, &cw->plate, b, r, NULL);
        }

        /* 2a'. The highlight band — above the panel, below the glyphs. */
        CompositeParams hp;
        if (vr_composite_setup(res->width, res->height, b->tex.width, b->tex.height,
                               b, r, &hp)) {
            HighlightParams hl;
            if (vr_highlight_setup(&hp, b, r, &hl)) {
                const dim3 hblock(16, 16);
                dim3       hgrid((hl.geom.bb_w + hblock.x - 1) / hblock.x,
                                 (hl.geom.bb_h + hblock.y - 1) / hblock.y);
                const uchar4 *plate = (b->kind == WIDGET_CODE)
                    ? (const uchar4 *)((const CodeWidget *)b)->plate.d_pixels : NULL;
                k_highlight<<<hgrid, hblock, 0, stream>>>(target, plate, hl);
                CUDA_CHECK_KERNEL();
            }
        }

        /* 2b. Typewriter cutoffs → VRAM (only when the text is partly visible). */
        const float  *d_cut = NULL;
        GlyphMetrics *g     = (GlyphMetrics *)&b->glyphs;

        if (g->d_cutoff != NULL && g->h_cutoff != NULL && r->reveal < 0.9999f) {
            float *h_slice = g->h_cutoff + (size_t)slot_idx * g->line_count;
            float *d_slice = (float *)g->d_cutoff + (size_t)slot_idx * g->line_count;

            vr_compute_reveal_cutoffs(g, r->reveal, h_slice);

            gpuErrchk(cudaMemcpyAsync(d_slice, h_slice,
                                      (size_t)g->line_count * sizeof(float),
                                      cudaMemcpyHostToDevice, stream));
            d_cut = d_slice;
        }

        /*
         * 2c. The layer itself.
         *
         * A clip's texture holds every frame stacked, so what is composited is
         * a slice of it. Building a local Texture that points into the stack
         * keeps the compositor unaware that video exists at all.
         */
        Texture layer = b->tex;
        size_t  voff  = 0;
        if (vr_video_slice(b, local_ms, &voff)) {
            layer.height = (int)((const VideoWidget *)b)->frame_h;
            if (layer.d_pixels != NULL) {
                layer.d_pixels = (uint8_t *)layer.d_pixels + voff;
            }
        }

        composite_layer(res, stream, target, &layer, b, r, d_cut);
    }

}


/*
 * Applies a scene's own effects *in place*.
 *
 * Because of the ping-pong the result may end up in the scratch buffer; in that
 * case we copy it back so the caller can keep relying on the same pointer. That
 * is a single D2D copy, and only when the scene has effects at all.
 */
static void scene_effects(const EditorContext *ctx, RenderResources *res, int slot_idx,
                          const Scene *scene, uchar4 *buffer, float t_local, long long frame)
{
    (void)ctx;
    if (scene->effect_count == 0) {
        return;
    }

    uchar4 *out = apply_effect_list(scene->effects, scene->effect_count, res, slot_idx,
                                    buffer, t_local, frame);
    if (out != buffer) {
        gpuErrchk(cudaMemcpyAsync(buffer, out, res->frame_bytes,
                                  cudaMemcpyDeviceToDevice, res->slot[slot_idx].stream));
    }
}

/* ------------------------------------------------------------------------- */
/* One frame: scene selection, transition, effects, NV12                      */
/* ------------------------------------------------------------------------- */

static void render_one_frame(EditorContext *ctx, RenderResources *res, int slot_idx,
                             long long frame, int time_ms)
{
    FrameSlot   *slot   = &res->slot[slot_idx];
    cudaStream_t stream = slot->stream;
    float        t_sec  = (float)time_ms * 0.001f;

    /* Which scene — or which pair, mid-transition — is on screen. */
    size_t       si;
    const Scene *A, *B;
    float        p;
    vr_select_scenes(ctx, time_ms, &si, &A, &B, &p);

    /* --- Rendering the scenes ------------------------------------------- */
    uchar4 *base = slot->d_frame;

    if (B != NULL && slot->d_scene[0] != NULL) {
        arena_reset(&ctx->frame_arena);

        WidgetRuntime *rtA = ARENA_NEW(&ctx->frame_arena, WidgetRuntime,
                                       A->widget_count ? A->widget_count : 1);
        WidgetRuntime *rtB = ARENA_NEW(&ctx->frame_arena, WidgetRuntime,
                                       B->widget_count ? B->widget_count : 1);
        if (rtA == NULL || rtB == NULL) {
            return;
        }

        vr_evaluate_scene(ctx, A, rtA, time_ms - A->start_ms);
        vr_evaluate_scene(ctx, B, rtB, time_ms - B->start_ms);

        render_scene_into(ctx, res, slot_idx, A, rtA, slot->d_scene[0],
                          time_ms - A->start_ms);
        scene_effects(ctx, res, slot_idx, A, slot->d_scene[0],
                      (float)(time_ms - A->start_ms) * 0.001f, frame);

        render_scene_into(ctx, res, slot_idx, B, rtB, slot->d_scene[1],
                          time_ms - B->start_ms);
        scene_effects(ctx, res, slot_idx, B, slot->d_scene[1],
                      (float)(time_ms - B->start_ms) * 0.001f, frame);

        TransParams tp;
        tp.w = res->width;
        tp.h = res->height;
        Color bgc = ctx->config.bg_color;
        tp.bg = make_uchar4(bgc.r, bgc.g, bgc.b, 255);

        const Transition *tr = &ctx->transitions[si];
        vr_transition_preset(tr->type, p, res->width, res->height, &tp.from, &tp.to);
        vr_transition_apply_inline(tr, p, res->width, res->height, &tp.from, &tp.to);

        const dim3 block(16, 16);
        dim3       grid((res->width + block.x - 1) / block.x,
                        (res->height + block.y - 1) / block.y);

        k_transition<<<grid, block, 0, stream>>>(base, slot->d_scene[0],
                                                 slot->d_scene[1], tp);
        CUDA_CHECK_KERNEL();
    } else {
        arena_reset(&ctx->frame_arena);
        WidgetRuntime *rt = ARENA_NEW(&ctx->frame_arena, WidgetRuntime,
                                      A->widget_count ? A->widget_count : 1);
        if (rt == NULL) {
            return;
        }
        vr_evaluate_scene(ctx, A, rt, time_ms - A->start_ms);
        render_scene_into(ctx, res, slot_idx, A, rt, base, time_ms - A->start_ms);
        scene_effects(ctx, res, slot_idx, A, base,
                      (float)(time_ms - A->start_ms) * 0.001f, frame);
    }

    /* 3. Film-wide effects on the finished frame. */
    uchar4 *final_frame = apply_effect_list(ctx->effects, ctx->effect_count, res, slot_idx,
                                            base, t_sec, frame);

    /* 4. RGBA → NV12 inside VRAM: 2.67x less data to transfer. */
    uint8_t *y_plane  = slot->d_nv12;
    uint8_t *uv_plane = slot->d_nv12 + (size_t)res->width * (size_t)res->height;

    const dim3 nv_block(16, 16);
    dim3       nv_grid((((res->width + 1) / 2) + nv_block.x - 1) / nv_block.x,
                       (((res->height + 1) / 2) + nv_block.y - 1) / nv_block.y);

    k_rgba_to_nv12<<<nv_grid, nv_block, 0, stream>>>(final_frame, y_plane, uv_plane,
                                                     res->width, res->height);
    CUDA_CHECK_KERNEL();

    /* 5. Back to the host + an event so the CPU knows when it is done. */
    gpuErrchk(cudaMemcpyAsync(slot->h_frame, slot->d_nv12, res->nv12_bytes,
                              cudaMemcpyDeviceToHost, stream));
    gpuErrchk(cudaEventRecord(slot->done, stream));
}

bool render_video(EditorContext *ctx, const char *output_file)
{
    if (ctx == NULL) {
        return false;
    }
    if (output_file == NULL || output_file[0] == '\0') {
        output_file = "output.mp4";
    }

    if (!renderer_init(ctx)) {
        fprintf(stderr, "error: renderer initialisation failed.\n");
        return false;
    }
    RenderResources *res = (RenderResources *)ctx->gpu;

    /*
     * If the project has sound, the video is first written to a temporary
     * *silent* file and a second pass then attaches the soundtrack. The video
     * stream is copied through — never re-encoded, so no quality is lost.
     */
    char       *silent_path  = NULL;
    const char *video_target = output_file;

    if (ctx->audio_count > 0) {
        silent_path = audio_make_silent_path(output_file);
        if (silent_path == NULL) {
            fprintf(stderr, "error: could not build the temporary file name.\n");
            return false;
        }
        video_target = silent_path;
    }

    /* --- The frame range to render -------------------------------------- */
    long long first, last, total_frames;
    vr_frame_range(ctx, &first, &last, &total_frames);
    long long count = last - first + 1;

    /*
     * Ignoring SIGPIPE: if ffmpeg dies unexpectedly, fwrite would kill our
     * process with a signal and no cleanup (VRAM, pipe) would run at all.
     * Once ignored, the same situation comes back simply as an fwrite error.
     */
    void (*old_sigpipe)(int) = signal(SIGPIPE, SIG_IGN);

    FILE *pipe = vr_open_ffmpeg_pipe(ctx, video_target, "h264_nvenc", true);
    if (pipe == NULL) {
        signal(SIGPIPE, old_sigpipe);
        free(silent_path);
        return false;
    }

    if (count == total_frames) {
        fprintf(stderr, "render: %lld frames @ %d fps → %s\n",
                count, ctx->config.fps, output_file);
    } else {
        fprintf(stderr, "render: frames %lld..%lld (%lld of them, %.2f–%.2f s) @ %d fps → %s\n",
                first, last, count,
                (double)first / ctx->config.fps, (double)(last + 1) / ctx->config.fps,
                ctx->config.fps, output_file);
    }

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    bool ok = true;

    /*
     * The pipeline: on every iteration we first write the *previous* frame
     * (whose GPU work has already finished), then launch the current one. That
     * way NVENC's encoding and our compositing overlap.
     *
     * Safety: before overwriting slot s's buffers we make sure frame i-2 (in
     * the same slot) has finished — which is guaranteed, because we waited on
     * its event during the previous iteration.
     */
    for (long long frame = first; frame <= last && ok; frame++) {
        int slot_idx = (int)(frame % VR_PIPELINE_DEPTH);

        int time_ms = (int)((frame * 1000) / ctx->config.fps);

        /* --- 1. Write the previous frame while the GPU works on this one -- */
        if (frame > first) {
            int        prev = (int)((frame - 1) % VR_PIPELINE_DEPTH);
            FrameSlot *ps   = &res->slot[prev];

            gpuErrchk(cudaEventSynchronize(ps->done));

            size_t written = fwrite(ps->h_frame, 1, res->nv12_bytes, pipe);
            if (written != res->nv12_bytes) {
                fprintf(stderr, "\nerror: writing to the ffmpeg pipe failed "
                                "(%zu / %zu bytes) — the encoder probably closed.\n",
                        written, res->nv12_bytes);
                ok = false;
                break;
            }
        }

        /* --- 2. Launch the current frame (asynchronous) ------------------- */
        render_one_frame(ctx, res, slot_idx, frame, time_ms);

        long long done = frame - first + 1;
        if ((done % 30) == 1 || frame == last) {
            fprintf(stderr, "\r  %lld/%lld frames (%.1f%%)", done, count,
                    100.0 * (double)done / (double)count);
            fflush(stderr);
        }
    }

    /* --- 5. Flush the final frame down the pipe --------------------------- */
    if (ok && count > 0) {
        int        last_slot = (int)(last % VR_PIPELINE_DEPTH);
        FrameSlot *ls        = &res->slot[last_slot];

        gpuErrchk(cudaEventSynchronize(ls->done));

        size_t written = fwrite(ls->h_frame, 1, res->nv12_bytes, pipe);
        if (written != res->nv12_bytes) {
            fprintf(stderr, "\nerror: writing the final frame failed.\n");
            ok = false;
        }
    }

    /* On an early exit the GPU may still be working on our buffers — wait for
     * it before we free them. */
    gpuErrchk(cudaDeviceSynchronize());

    fprintf(stderr, "\n");

    int status = pclose(pipe);
    signal(SIGPIPE, old_sigpipe);

    if (status != 0) {
        fprintf(stderr, "warning: ffmpeg exited with code %d.\n", status);
        ok = false;
    }

    double elapsed = vr_seconds_since(&start);

    /* --- 6. The second audio pass (if needed) ----------------------------- */
    if (silent_path != NULL) {
        bool muxed = false;
        if (ok) {
            muxed = audio_mux(ctx, silent_path, output_file);
            ok    = muxed;
        }

        if (muxed) {
            remove(silent_path);
        } else {
            /*
             * The mix failed — we do *not* delete the silent file. A render
             * can take minutes and throwing it away over the audio would be
             * wrong; the user can attach the sound by hand or try again.
             */
            fprintf(stderr, "note: the silent video was kept — %s\n", silent_path);
        }
        free(silent_path);
    }

    if (ok) {
        fprintf(stderr, "done: %s — %lld frames in %.2f s (%.1f fps)\n",
                output_file, count, elapsed,
                (elapsed > 0.0) ? (double)count / elapsed : 0.0);
    }

    return ok;
}
