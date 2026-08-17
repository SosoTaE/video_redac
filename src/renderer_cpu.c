/*
 * renderer_cpu.c — the CPU rendering backend.
 *
 * Implements exactly the same three functions as renderer.cu (renderer_init,
 * render_video, renderer_shutdown), so main.c cannot tell which backend it was
 * linked against. Only one of the two is ever compiled in; see the CPU=1 target
 * in the Makefile.
 *
 * What is *not* here, deliberately: any decision about what a frame looks like.
 * The timeline, the transition matrices, the compositing geometry and the
 * effect parameters all come from render_common.c, and the per-pixel arithmetic
 * from pixel_ops.h — the same code the CUDA kernels run. This file only owns
 * allocation and the loops.
 *
 * A frame's journey:
 *
 *   [CPU]  timeline evaluation (in the arena) →  per-widget runtime state
 *     ↓
 *   [CPU]  clear + composite each layer       →  RGBA framebuffer
 *     ↓
 *   [CPU]  effect stack (ping-pong)
 *     ↓
 *   [CPU]  RGBA → NV12                        →  8.29 MB → 3.11 MB (1080x1920)
 *     ↓
 *   [PIPE] fwrite → ffmpeg -c:v libx264       →  .mp4
 *
 * Parallelism: OpenMP over destination rows. Rows are a good grain here because
 * every pixel is independent and a row is long enough to amortise the barrier;
 * the compositor additionally only walks the layer's bounding box, so a small
 * title costs a small loop rather than a full-screen one.
 *
 * Without OpenMP the file still compiles and runs — just single-threaded. That
 * matters for portability more than the speed does: the pragmas are ignored by
 * any compiler that does not know them.
 *
 * Performance expectations: on this workload the encoder, not the compositing,
 * is the limit — exactly as on the GPU, except that libx264 is a great deal
 * slower than NVENC. Do not expect the GPU backend's throughput; do expect a
 * correct, identical-looking result.
 */

#include "renderer.h"

#include "arena.h"
#include "audio.h"
#include "effects.h"
#include "pixel_ops.h"
#include "render_common.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _OPENMP
#include <omp.h>
#endif

/* ------------------------------------------------------------------------- */
/* Host resources (hidden behind the opaque EditorContext::gpu)               */
/* ------------------------------------------------------------------------- */

/*
 * There are no "slots" here.
 *
 * The GPU backend keeps VR_PIPELINE_DEPTH frames in flight so that the CPU can
 * write frame i-1 into the pipe while the GPU renders frame i. On the CPU there
 * is nothing to overlap with — the same cores would do both — so one buffer set
 * is not just enough, it is the honest amount.
 */
typedef struct {
    uchar4  *frame;      /* RGBA compositing target                            */
    uchar4  *fx;         /* ping-pong buffer for the effects; NULL if unused   */
    uchar4  *scene[2];   /* the two scenes during a transition; NULL if unused */
    uint8_t *nv12;       /* the frame as NV12 — what goes into the pipe        */

    int      width;
    int      height;
    size_t   frame_bytes;
    size_t   nv12_bytes;
} CpuResources;

/* ------------------------------------------------------------------------- */
/* Setup and teardown                                                         */
/* ------------------------------------------------------------------------- */

bool renderer_init(EditorContext *ctx)
{
    if (ctx == NULL) {
        return false;
    }
    if (ctx->gpu != NULL) {
        return true; /* idempotent */
    }

    CpuResources *res = (CpuResources *)calloc(1, sizeof *res);
    if (res == NULL) {
        return false;
    }

    res->width       = ctx->config.width;
    res->height      = ctx->config.height;
    res->frame_bytes = (size_t)res->width * (size_t)res->height * sizeof(uchar4);
    /* NV12: a full Y plane + half-resolution UV → w*h*3/2. */
    res->nv12_bytes  = (size_t)res->width * (size_t)res->height * 3 / 2;

    res->frame = (uchar4 *)malloc(res->frame_bytes);
    res->nv12  = (uint8_t *)malloc(res->nv12_bytes);
    if (res->frame == NULL || res->nv12 == NULL) {
        free(res->frame);
        free(res->nv12);
        free(res);
        return false;
    }

    /* The ping-pong buffer only if effects exist at all. */
    bool any_effects = (ctx->effect_count > 0);
    for (size_t i = 0; i < ctx->scene_count && !any_effects; i++) {
        any_effects = (ctx->scenes[i].effect_count > 0);
    }
    if (any_effects) {
        res->fx = (uchar4 *)malloc(res->frame_bytes);
        if (res->fx == NULL) {
            goto fail;
        }
    }

    /* Scene buffers only when a transition is even possible. */
    if (ctx->scene_count > 1) {
        res->scene[0] = (uchar4 *)malloc(res->frame_bytes);
        res->scene[1] = (uchar4 *)malloc(res->frame_bytes);
        if (res->scene[0] == NULL || res->scene[1] == NULL) {
            goto fail;
        }
    }

    ctx->gpu = res;

    size_t total = res->frame_bytes + res->nv12_bytes
                 + (res->fx ? res->frame_bytes : 0)
                 + (res->scene[0] ? 2 * res->frame_bytes : 0);

    int threads = 1;
#ifdef _OPENMP
    threads = omp_get_max_threads();
#endif
    fprintf(stderr, "CPU backend: %d thread(s), %.1f MiB frame buffers\n",
            threads, (double)total / (1024.0 * 1024.0));

    return true;

fail:
    free(res->frame);
    free(res->fx);
    free(res->scene[0]);
    free(res->scene[1]);
    free(res->nv12);
    free(res);
    return false;
}

void renderer_shutdown(EditorContext *ctx)
{
    if (ctx == NULL) {
        return;
    }

    CpuResources *res = (CpuResources *)ctx->gpu;
    if (res != NULL) {
        free(res->frame);
        free(res->fx);
        free(res->scene[0]);
        free(res->scene[1]);
        free(res->nv12);
        free(res);
    }
    ctx->gpu = NULL; /* → calling this again is safe */
}

/* ------------------------------------------------------------------------- */
/* The pixel loops                                                            */
/* ------------------------------------------------------------------------- */

static void clear_background(uchar4 *fb, int w, int h, uchar4 bg)
{
    #pragma omp parallel for schedule(static)
    for (int y = 0; y < h; y++) {
        uchar4 *row = fb + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            row[x] = bg;
        }
    }
}

/*
 * Composites one layer: derives the transform, then walks the bounding box.
 *
 * `tex` may be the glyph texture or a code block's panel — the geometry is
 * identical for both; only `cutoff` differs (a panel is never clipped).
 */
static void composite_layer(const CpuResources *res, uchar4 *fb, const Texture *tex,
                            const WidgetBase *b, const WidgetRuntime *rt,
                            const float *cutoff)
{
    if (tex->pixels == NULL) {
        return;
    }

    CompositeParams p;
    if (!vr_composite_setup(res->width, res->height, tex->width, tex->height, b, rt, &p)) {
        return; /* degenerate size, or entirely off-screen */
    }

    const uchar4 *src = (const uchar4 *)tex->pixels;

    /*
     * Only the outer loop is parallel. Two rows never touch the same
     * destination pixel, so the read-modify-write blend below stays safe
     * without any synchronisation.
     */
    #pragma omp parallel for schedule(static)
    for (int j = 0; j < p.bb_h; j++) {
        for (int i = 0; i < p.bb_w; i++) {
            vr_px_composite(fb, src, cutoff, &p, i, j);
        }
    }
}

static void frame_to_nv12(const uchar4 *rgba, uint8_t *nv12, int w, int h)
{
    uint8_t *y_plane  = nv12;
    uint8_t *uv_plane = nv12 + (size_t)w * (size_t)h;

    int blocks_y = (h + 1) / 2;
    int blocks_x = (w + 1) / 2;

    #pragma omp parallel for schedule(static)
    for (int by = 0; by < blocks_y; by++) {
        for (int bx = 0; bx < blocks_x; bx++) {
            vr_px_nv12(rgba, y_plane, uv_plane, w, h, bx, by);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* The effect stack                                                           */
/* ------------------------------------------------------------------------- */

/* One full-frame pass of a neighbour-reading effect. */
#define FX_PASS(call)                                        \
    do {                                                     \
        _Pragma("omp parallel for schedule(static)")         \
        for (int y = 0; y < h; y++) {                        \
            for (int x = 0; x < w; x++) {                    \
                call;                                        \
            }                                                \
        }                                                    \
    } while (0)

/*
 * Runs the whole stack and returns the buffer the final frame ended up in.
 *
 * Ping-pong: every effect reads from `src` and writes into `dst`, then the two
 * swap. Working in place is not allowed, because effects that read neighbours
 * (blur, glitch) would then read already-modified pixels and the result would
 * depend on the traversal order — which, with threads, would not even be
 * deterministic.
 */
static uchar4 *apply_effect_list(const Effect *list, size_t count, CpuResources *res,
                                 uchar4 *source, float t_sec, long long frame)
{
    uchar4 *src = source;

    if (count == 0 || res->fx == NULL) {
        return src;
    }

    uchar4 *dst = res->fx;
    int     w   = res->width;
    int     h   = res->height;

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
                FX_PASS(vr_px_fx_blur(dst, src, w, h, radius, 1, x, y));
                { uchar4 *tmp = src; src = dst; dst = tmp; }

                FX_PASS(vr_px_fx_blur(dst, src, w, h, radius, 0, x, y));
                break;
            }
            case FX_PIXELATE:
                FX_PASS(vr_px_fx_pixelate(dst, src, w, h, (int)lrintf(g.p[FXP_SIZE]), x, y));
                break;

            case FX_RGB_SPLIT:
                FX_PASS(vr_px_fx_rgb_split(dst, src, w, h,
                                           g.p[FXP_AMOUNT], g.p[FXP_ANGLE], x, y));
                break;

            case FX_GLITCH:
                FX_PASS(vr_px_fx_glitch(dst, src, w, h, g.p[FXP_AMOUNT], seed, x, y));
                break;

            default:
                FX_PASS(vr_px_fx_point(dst, src, w, h, &g, seed, x, y));
                break;
        }

        { uchar4 *tmp = src; src = dst; dst = tmp; }
    }

    return src; /* after the swaps the result lives here */
}

/*
 * Applies a scene's own effects *in place*.
 *
 * Because of the ping-pong the result may end up in the scratch buffer; in that
 * case we copy it back so the caller can keep relying on the same pointer.
 */
static void scene_effects(CpuResources *res, const Scene *scene, uchar4 *buffer,
                          float t_local, long long frame)
{
    if (scene->effect_count == 0) {
        return;
    }

    uchar4 *out = apply_effect_list(scene->effects, scene->effect_count, res,
                                    buffer, t_local, frame);
    if (out != buffer) {
        memcpy(buffer, out, res->frame_bytes);
    }
}

/* ------------------------------------------------------------------------- */
/* One scene, one frame                                                       */
/* ------------------------------------------------------------------------- */

static void render_scene_into(const EditorContext *ctx, CpuResources *res,
                              const Scene *scene, const WidgetRuntime *rt, uchar4 *target)
{
    /* 1. Background — a scene may have its own. */
    Color  bgc = scene->has_bg ? scene->bg_color : ctx->config.bg_color;
    uchar4 bg  = make_uchar4(bgc.r, bgc.g, bgc.b, bgc.a);

    clear_background(target, res->width, res->height, bg);

    /* 2. Widgets — back to front (painter's algorithm). */
    for (size_t i = 0; i < scene->widget_count; i++) {
        const WidgetBase    *b = ctx->widgets[scene->first_widget + i];
        const WidgetRuntime *r = &rt[i];

        if (!r->visible) {
            continue;
        }

        /* 2a. The panel (only a code block has one) — typing does not clip it. */
        if (b->kind == WIDGET_CODE) {
            const CodeWidget *cw = (const CodeWidget *)b;
            composite_layer(res, target, &cw->plate, b, r, NULL);
        }

        /* 2a'. The highlight band — above the panel, below the glyphs. */
        CompositeParams hp;
        if (vr_composite_setup(res->width, res->height, b->tex.width, b->tex.height,
                               b, r, &hp)) {
            HighlightParams hl;
            if (vr_highlight_setup(&hp, b, r, &hl)) {
                const uchar4 *plate = (b->kind == WIDGET_CODE)
                    ? (const uchar4 *)((const CodeWidget *)b)->plate.pixels : NULL;

                /* bx/by, not i/j — `i` is the enclosing widget index. */
                #pragma omp parallel for schedule(static)
                for (int by = 0; by < hl.geom.bb_h; by++) {
                    for (int bx = 0; bx < hl.geom.bb_w; bx++) {
                        vr_px_highlight(target, plate, &hl, bx, by);
                    }
                }
            }
        }

        /* 2b. Typewriter cutoffs (only when the text is partly visible).
         *
         * Unlike the GPU path there is no upload and no need to keep several
         * sets alive: the compositing below finishes before anything else can
         * touch the buffer, so slot 0 is reused every time. */
        const float  *cut = NULL;
        GlyphMetrics *g   = (GlyphMetrics *)&b->glyphs;

        if (g->h_cutoff != NULL && g->line_count > 0 && r->reveal < 0.9999f) {
            vr_compute_reveal_cutoffs(g, r->reveal, g->h_cutoff);
            cut = g->h_cutoff;
        }

        /* 2c. The layer itself. */
        composite_layer(res, target, &b->tex, b, r, cut);
    }
}

static void render_one_frame(EditorContext *ctx, CpuResources *res,
                             long long frame, int time_ms)
{
    float t_sec = (float)time_ms * 0.001f;

    /* Which scene — or which pair, mid-transition — is on screen. */
    size_t       si;
    const Scene *A, *B;
    float        p;
    vr_select_scenes(ctx, time_ms, &si, &A, &B, &p);

    uchar4 *base = res->frame;

    if (B != NULL && res->scene[0] != NULL) {
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

        render_scene_into(ctx, res, A, rtA, res->scene[0]);
        scene_effects(res, A, res->scene[0],
                      (float)(time_ms - A->start_ms) * 0.001f, frame);

        render_scene_into(ctx, res, B, rtB, res->scene[1]);
        scene_effects(res, B, res->scene[1],
                      (float)(time_ms - B->start_ms) * 0.001f, frame);

        TransParams tp;
        tp.w = res->width;
        tp.h = res->height;
        Color bgc = ctx->config.bg_color;
        tp.bg = make_uchar4(bgc.r, bgc.g, bgc.b, 255);

        const Transition *tr = &ctx->transitions[si];
        vr_transition_preset(tr->type, p, res->width, res->height, &tp.from, &tp.to);
        vr_transition_apply_inline(tr, p, res->width, res->height, &tp.from, &tp.to);

        const uchar4 *from = res->scene[0];
        const uchar4 *to   = res->scene[1];

        #pragma omp parallel for schedule(static)
        for (int y = 0; y < res->height; y++) {
            for (int x = 0; x < res->width; x++) {
                vr_px_transition(base, from, to, &tp, x, y);
            }
        }
    } else {
        arena_reset(&ctx->frame_arena);
        WidgetRuntime *rt = ARENA_NEW(&ctx->frame_arena, WidgetRuntime,
                                      A->widget_count ? A->widget_count : 1);
        if (rt == NULL) {
            return;
        }
        vr_evaluate_scene(ctx, A, rt, time_ms - A->start_ms);
        render_scene_into(ctx, res, A, rt, base);
        scene_effects(res, A, base, (float)(time_ms - A->start_ms) * 0.001f, frame);
    }

    /* 3. Film-wide effects on the finished frame. */
    uchar4 *final_frame = apply_effect_list(ctx->effects, ctx->effect_count, res,
                                            base, t_sec, frame);

    /* 4. RGBA → NV12: 2.67x less data to push into the pipe. */
    frame_to_nv12(final_frame, res->nv12, res->width, res->height);
}

/* ------------------------------------------------------------------------- */
/* The render loop                                                            */
/* ------------------------------------------------------------------------- */

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
    CpuResources *res = (CpuResources *)ctx->gpu;

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
     * process with a signal and no cleanup (buffers, pipe) would run at all.
     * Once ignored, the same situation comes back simply as an fwrite error.
     */
    void (*old_sigpipe)(int) = signal(SIGPIPE, SIG_IGN);

    FILE *pipe = vr_open_ffmpeg_pipe(ctx, video_target, "libx264", false);
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
     * A plain synchronous loop: render, then write.
     *
     * The GPU backend overlaps the two because its work happens on a different
     * engine. Here rendering and encoding compete for the same cores, so
     * pipelining would only add a buffer copy and a thread — it would not make
     * anything finish sooner.
     */
    for (long long frame = first; frame <= last && ok; frame++) {
        int time_ms = (int)((frame * 1000) / ctx->config.fps);

        render_one_frame(ctx, res, frame, time_ms);

        size_t written = fwrite(res->nv12, 1, res->nv12_bytes, pipe);
        if (written != res->nv12_bytes) {
            fprintf(stderr, "\nerror: writing to the ffmpeg pipe failed "
                            "(%zu / %zu bytes) — the encoder probably closed.\n",
                    written, res->nv12_bytes);
            ok = false;
            break;
        }

        long long done = frame - first + 1;
        if ((done % 30) == 1 || frame == last) {
            fprintf(stderr, "\r  %lld/%lld frames (%.1f%%)", done, count,
                    100.0 * (double)done / (double)count);
            fflush(stderr);
        }
    }

    fprintf(stderr, "\n");

    int status = pclose(pipe);
    signal(SIGPIPE, old_sigpipe);

    if (status != 0) {
        fprintf(stderr, "warning: ffmpeg exited with code %d.\n", status);
        ok = false;
    }

    double elapsed = vr_seconds_since(&start);

    /* --- The second audio pass (if needed) -------------------------------- */
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
