#ifndef VIDEO_REDAC_MEDIA_LOADER_H
#define VIDEO_REDAC_MEDIA_LOADER_H

/*
 * media_loader.h — CPU-side rasterization, the "hot" half of the hybrid design.
 *
 * Cairo turns text into a buffer of RGBA pixels once; that buffer is uploaded
 * to VRAM and from then on the GPU only *composites* it — moving, fading and
 * rotating. The same string is never rasterized on the CPU twice.
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The core function: a string → an array of RGBA bytes.
 *
 * `out` receives a freshly allocated host buffer (premultiplied RGBA8); the
 * caller must release it with texture_free().
 * `metrics` — if non-NULL, filled with per-glyph metrics for TYPEWRITE
 * (release with glyph_metrics_free()).
 * Any '\n' starts a new line.
 */
bool media_render_text_rgba(const char *utf8_text,
                            const char *font_family,
                            int         font_size,
                            Color       color,
                            float       line_spacing,
                            int         padding,
                            float       max_width,  /* 0 = no wrapping      */
                            float       align,      /* 0 | 0.5 | 1          */
                            Texture      *out,
                            GlyphMetrics *metrics);

/* Widget-specific wrappers. */
bool media_render_text_widget(TextWidget *w);

/*
 * A code block produces *two* textures:
 *   w->base.tex — highlighted glyphs on a transparent background,
 *   w->plate    — the rounded panel behind them.
 * Two layers, because the typewriter must clip only the glyphs, never the panel.
 */
bool media_render_code_widget(CodeWidget *w);

/* Rectangle / circle → an antialiased RGBA texture. */
bool media_render_shape_widget(ShapeWidget *w);

/* Rasterizes a straight segment horizontally; the angle is applied by the
 * compositor via base_rotation. */
bool media_render_line_widget(LineWidget *w);

/* Rasterizes a polyline/bezier path into its own bounding box. */
bool media_render_path_widget(PathWidget *w);

/* PNG/JPG → premultiplied RGBA8 (stb_image). */
bool media_load_image_rgba(const char *path, Texture *out);

/*
 * Rasterizes every widget once, computes base sizes and resolves layout
 * (auto-centring, expressions, anchors). This is where the cache is filled.
 */
bool media_prepare_textures(EditorContext *ctx);

/* Frees the host pixels. The device copy is renderer_shutdown()'s business.
 * Safe on NULL and when called repeatedly. */
void texture_free(Texture *t);

/* Frees the host arrays of the glyph metrics. */
void glyph_metrics_free(GlyphMetrics *m);

/*
 * Releases Cairo/fontconfig's *global* font caches.
 *
 * These caches live inside the libraries and normally survive until the process
 * exits — they are not real leaks. LeakSanitizer counts them anyway, burying
 * our *genuine* leaks in noise. One call on shutdown keeps the LSAN report
 * clean.
 */
void media_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_REDAC_MEDIA_LOADER_H */
