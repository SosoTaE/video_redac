#ifndef VIDEO_REDAC_RENDERER_H
#define VIDEO_REDAC_RENDERER_H

/*
 * renderer.h — the host-visible interface to the CUDA rendering pipeline.
 *
 * This header deliberately contains no CUDA types (`uchar4`, `cudaStream_t`) so
 * that plain C11 modules (main.c, parser.c) can include it without nvcc. Every
 * device resource is hidden behind the opaque `EditorContext::gpu`.
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Allocates VRAM: frame buffers, a device copy of every texture, and pinned
 * host staging. Textures must already be rasterized (media_prepare_textures).
 * Idempotent.
 */
bool renderer_init(EditorContext *ctx);

/*
 * Renders the whole timeline and streams frames into the FFmpeg (nvenc) pipe.
 * Calls renderer_init() itself if needed.
 *
 * `output_file` — the output container, e.g. "out.mp4"; NULL → "output.mp4".
 * Returns false on any failure (the reason goes to stderr).
 *
 * Note: the original specification described this as `void`; the bool return
 * exists so main() can produce a correct exit code for CI and scripts.
 */
bool render_video(EditorContext *ctx, const char *output_file);

/* Frees every device resource. Safe on NULL and when called repeatedly. */
void renderer_shutdown(EditorContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_REDAC_RENDERER_H */
