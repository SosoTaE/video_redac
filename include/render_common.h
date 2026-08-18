#ifndef VIDEO_REDAC_RENDER_COMMON_H
#define VIDEO_REDAC_RENDER_COMMON_H

/*
 * render_common.h — the backend-independent half of the renderer.
 *
 * Everything here is pure host computation: sampling the timeline, packing
 * effect parameters, deriving transition matrices, choosing which scenes are
 * on screen, and starting ffmpeg. None of it knows whether the pixels will
 * ultimately be pushed by CUDA or by a loop over rows.
 *
 * Together with pixel_ops.h this is what makes two backends affordable. The
 * split is:
 *
 *   pixel_ops.h      what to compute for one pixel   (shared, compiled twice)
 *   render_common.c  what to compute for one frame   (shared, compiled once)
 *   renderer.cu      how to get the GPU to do it     (backend)
 *   renderer_cpu.c   how to get the CPU to do it     (backend)
 *
 * A backend should contain allocation, scheduling and the pixel loop — nothing
 * that decides what a frame *looks* like. If you find yourself computing a
 * position, an opacity or a matrix inside a backend, it belongs here instead.
 */

#include "effects.h"
#include "pixel_ops.h"
#include "types.h"

#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

float  vr_clampf(float v, float lo, float hi);
double vr_seconds_since(const struct timespec *start);

/* ------------------------------------------------------------------------- */
/* Timeline                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Fills `rt[]` for every widget in `scene` at scene-local time `local_ms`.
 *
 * Pure: the result depends only on the timeline and the timestamp, never on
 * the previous frame — which is what lets any frame be rendered on its own
 * (seek, preview, --range, or a parallel backend).
 *
 * `rt` must have room for scene->widget_count entries.
 */
void vr_evaluate_scene(const EditorContext *ctx, const Scene *scene,
                       WidgetRuntime *rt, int local_ms);

/*
 * Typewriter cutoffs: `reveal` (0..1) → one x threshold per line.
 * `out` must have room for g->line_count floats.
 */
void vr_compute_reveal_cutoffs(const GlyphMetrics *g, float reveal, float *out);

/* ------------------------------------------------------------------------- */
/* Scene selection                                                            */
/* ------------------------------------------------------------------------- */

/*
 * Which scene — or which pair, mid-transition — is visible at `time_ms`.
 *
 * `*out_b` is NULL when a single scene covers the moment; otherwise `*out_p` is
 * the transition's progress in 0..1 and `*out_index` identifies the transition
 * between the two.
 */
void vr_select_scenes(const EditorContext *ctx, int time_ms,
                      size_t *out_index, const Scene **out_a, const Scene **out_b,
                      float *out_p);

/* ------------------------------------------------------------------------- */
/* Compositing geometry                                                       */
/* ------------------------------------------------------------------------- */

/*
 * Derives the compositing parameters for one layer: the inverse matrix, the
 * destination centre and the screen-clipped bounding box.
 *
 * Returns false when there is nothing to draw (degenerate size, or entirely
 * off-screen) — the caller should then skip the layer entirely.
 */
bool vr_composite_setup(int fb_w, int fb_h, int tex_w, int tex_h,
                        const WidgetBase *b, const WidgetRuntime *rt,
                        CompositeParams *out);

/*
 * Fills in a highlight band for a widget, reusing the compositing transform so
 * the band lines up with the text exactly.
 *
 * Returns false when there is nothing to draw — no band, no line metrics, or a
 * line range entirely outside the text. Callers can therefore treat it as
 * "should I run the band pass at all?".
 */
bool vr_highlight_setup(const CompositeParams *geom, const WidgetBase *b,
                        const WidgetRuntime *rt, HighlightParams *out);

/*
 * Which frame of a clip is showing, and the byte offset of its slice.
 *
 * Returns false for anything that is not a video, so a caller can use it as
 * "does this widget need a per-frame slice?".
 */
bool vr_video_slice(const WidgetBase *b, int local_ms, size_t *out_offset);

/*
 * Fills `order` with the scene's widget indices, farthest first.
 *
 * Returns false when the scene uses no depth at all, in which case the caller
 * should iterate normally — that keeps the painter's order (and therefore every
 * existing project's output) exactly as it was.
 *
 * `order` must have room for scene->widget_count entries.
 */
bool vr_depth_order(const Scene *scene, const WidgetRuntime *rt, int *order);

/*
 * Transforms, projects, shades and culls a mesh into `out`, and fills `mp` with
 * the screen bounding box.
 *
 * `cap` is how many entries `out` holds. It must be at least twice the mesh's
 * triangle count: clipping against the near plane turns a triangle that
 * straddles the eye into two.
 *
 * Returns the number of triangles actually written — culled and off-screen
 * faces never reach the rasterizer — or 0 when there is nothing to draw.
 *
 * `lights` are world-space point lights; a count of 0 gives the camera-mounted
 * default, where whatever faces the viewer is lit.
 */
int vr_mesh_project(const MeshWidget *m, const WidgetRuntime *rt,
                    int fb_w, int fb_h, const float view[12],
                    const Light *lights, int light_count,
                    ScreenTri *out, int cap, MeshParams *mp);

/*
 * The camera's view transform at time `t`, as a 3x4 matrix (rotation then
 * translation) that takes a world point into view space.
 *
 * With no camera tracks this is the fixed viewpoint — eye at (0,0,-focal)
 * looking down +z — which is a pure translation, so scenes that never move the
 * camera go through exactly the arithmetic they always did.
 */
void vr_camera_view(const Scene *scene, float t_sec, float focal, float view[12]);

/* ------------------------------------------------------------------------- */
/* Effects                                                                    */
/* ------------------------------------------------------------------------- */

/* Samples an effect's Tracks at time `t` and packs them into a plain POD. */
EffectGPU vr_effect_sample(const Effect *fx, float t);

/*
 * The per-frame seed for grain and glitch. Tied to the frame index, so the
 * animation moves while any single frame stays reproducible.
 */
unsigned int vr_effect_seed(long long frame);

/* The blur radius actually used for an effect, or 0 to skip the pass.
 * Capped so neither backend can be handed an unbounded tap count. */
int vr_blur_radius(const EffectGPU *g);

/* ------------------------------------------------------------------------- */
/* Transitions                                                                */
/* ------------------------------------------------------------------------- */

/* Builds one side's inverse matrix from translation / scale / rotation. */
void vr_trans_side_set(TransSide *s, float opacity, float tx, float ty,
                       float scale, float rot_deg, int w, int h);

/* The preset's parameters at progress `p`. */
void vr_transition_preset(TransitionType type, float p, int w, int h,
                          TransSide *from, TransSide *to);

/* The JSON's `from`/`to` tracks and masks override the preset. */
void vr_transition_apply_inline(const Transition *tr, float p, int w, int h,
                                TransSide *from, TransSide *to);

/* ------------------------------------------------------------------------- */
/* Output                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * The software equivalent of a hardware encoder ("h264_nvenc" → "libx264"),
 * or NULL if the name is not a hardware encoder in the first place.
 */
const char *vr_software_encoder(const char *encoder);

/*
 * Starts ffmpeg as a subprocess and returns its stdin.
 *
 * `default_encoder` is what the backend would like to use when neither the
 * environment nor the project names one — "h264_nvenc" for CUDA, "libx264" for
 * the CPU. The precedence is: $VIDEO_REDAC_ENCODER > the JSON "output" block >
 * this default.
 *
 * `allow_hardware` false makes a project's NVENC request fall back to the
 * software equivalent, with a warning. The CPU backend passes false because the
 * machine it exists for may well have no NVIDIA GPU — and a project file that
 * says "h264_nvenc" should still render there rather than dying inside ffmpeg.
 * $VIDEO_REDAC_ENCODER is never overridden: naming an encoder explicitly is
 * taken as knowing what you are asking for.
 */
FILE *vr_open_ffmpeg_pipe(const EditorContext *ctx, const char *output_file,
                          const char *default_encoder, bool allow_hardware);

/*
 * The frame range to render: [*out_first, *out_last], plus the timeline's full
 * length in *out_total. Honours ctx->range_start_sec / range_end_sec.
 */
void vr_frame_range(const EditorContext *ctx, long long *out_first,
                    long long *out_last, long long *out_total);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_REDAC_RENDER_COMMON_H */
