/*
 * media_loader.c — CPU rasterization with Cairo + image loading with stb_image.
 *
 * Why the CPU?
 *   Text rasterization (hinting, kerning, ligatures, antialiasing) is
 *   sequential, branch-heavy work — a poor fit for a GPU's SIMT model. In
 *   exchange it happens once: in a 3,600-frame video the title is drawn once
 *   and merely *composited* 3,600 times on the GPU.
 *
 * ALPHA convention: every texture produced here is PREMULTIPLIED RGBA8.
 * Cairo's CAIRO_FORMAT_ARGB32 is premultiplied already, so all that happens
 * here is a byte reshuffle (ARGB uint32 → R,G,B,A bytes) — no un-premultiply,
 * which would damage quality along glyph edges.
 */

#include "media_loader.h"
#include "mesh.h"

#include "audio.h"   /* vr_shell_quote() */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cairo/cairo.h>

#include "highlighter.h"
#include "layout.h"
#include "stb_image.h"

/* A safety ceiling — a corrupt JSON must not ask for a gigabyte surface. */
#define MAX_TEXTURE_DIM 16384

/* Token limit per line; the excess collapses into TOK_TEXT (see highlighter.c). */
#define MAX_TOKENS_PER_LINE 256

/* ------------------------------------------------------------------------- */
/* Texture lifetime                                                           */
/* ------------------------------------------------------------------------- */

void texture_free(Texture *t)
{
    if (t == NULL) {
        return;
    }
    free(t->pixels);
    t->pixels = NULL;
    t->width  = 0;
    t->height = 0;
    t->stride = 0;
    /* d_pixels is deliberately untouched — VRAM is renderer_shutdown()'s job. */
}

void glyph_metrics_free(GlyphMetrics *m)
{
    if (m == NULL) {
        return;
    }
    free(m->line_start);
    free(m->char_x);
    free(m->h_cutoff);
    m->line_start  = NULL;
    m->char_x      = NULL;
    m->h_cutoff    = NULL;
    m->line_count  = 0;
    m->total_chars = 0;
    /* d_cutoff is freed by renderer_shutdown(). */
}

void media_shutdown(void)
{
    /* A Cairo "debug" function, but entirely safe: it only clears the static
     * caches the library created itself. */
    cairo_debug_reset_static_data();
}

/* Allocates an empty (transparent) packed RGBA buffer. */
static bool texture_alloc(Texture *t, int w, int h)
{
    if (w <= 0 || h <= 0 || w > MAX_TEXTURE_DIM || h > MAX_TEXTURE_DIM) {
        fprintf(stderr, "error: illegal texture size %dx%d.\n", w, h);
        return false;
    }

    size_t stride = (size_t)w * 4u;
    size_t bytes  = stride * (size_t)h;

    t->pixels = (uint8_t *)calloc(1, bytes);
    if (t->pixels == NULL) {
        fprintf(stderr, "error: could not allocate %zu bytes for a texture.\n", bytes);
        return false;
    }

    t->width         = w;
    t->height        = h;
    t->stride        = stride;
    t->premultiplied = true;
    return true;
}

/* ------------------------------------------------------------------------- */
/* UTF-8 helpers                                                              */
/* ------------------------------------------------------------------------- */

/* true if the byte starts a code point (and is not a 10xxxxxx continuation). */
static bool utf8_is_lead(unsigned char b)
{
    return (b & 0xC0) != 0x80;
}

static size_t utf8_count(const char *s, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (utf8_is_lead((unsigned char)s[i])) {
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* Splitting the font specification                                           */
/* ------------------------------------------------------------------------- */

static int ascii_casecmp(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) {
            return ca - cb;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/*
 * "FiraCode-Bold" → family="FiraCode", weight=BOLD.
 *
 * Cairo's "toy" API takes family and style separately, whereas JSON usually
 * carries a single PostScript-like name. The final family choice is made by
 * fontconfig — an approximate match is normal.
 */
static void split_font_spec(const char *spec, char *family, size_t family_size,
                            cairo_font_weight_t *weight, cairo_font_slant_t *slant)
{
    *weight = CAIRO_FONT_WEIGHT_NORMAL;
    *slant  = CAIRO_FONT_SLANT_NORMAL;

    if (spec == NULL || *spec == '\0') {
        snprintf(family, family_size, "Sans");
        return;
    }

    const char *dash = strrchr(spec, '-');
    size_t      keep = strlen(spec);

    if (dash != NULL && dash[1] != '\0') {
        const char *suffix  = dash + 1;
        bool        matched = true;

        if (ascii_casecmp(suffix, "Bold") == 0) {
            *weight = CAIRO_FONT_WEIGHT_BOLD;
        } else if (ascii_casecmp(suffix, "Italic") == 0 || ascii_casecmp(suffix, "Oblique") == 0) {
            *slant = CAIRO_FONT_SLANT_ITALIC;
        } else if (ascii_casecmp(suffix, "BoldItalic") == 0) {
            *weight = CAIRO_FONT_WEIGHT_BOLD;
            *slant  = CAIRO_FONT_SLANT_ITALIC;
        } else if (ascii_casecmp(suffix, "Regular") == 0 || ascii_casecmp(suffix, "Normal") == 0) {
            /* the defaults are already right */
        } else {
            matched = false; /* the hyphen is part of the family, e.g. "Noto-Sans" */
        }

        if (matched) {
            keep = (size_t)(dash - spec);
        }
    }

    if (keep >= family_size) {
        keep = family_size - 1;
    }
    memcpy(family, spec, keep);
    family[keep] = '\0';

    if (family[0] == '\0') {
        snprintf(family, family_size, "Sans");
    }
}

/* ------------------------------------------------------------------------- */
/* Splitting into lines                                                       */
/* ------------------------------------------------------------------------- */

static size_t count_lines(const char *text)
{
    size_t n = 1;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            n++;
        }
    }
    return n;
}

static void free_lines(char **lines, size_t count)
{
    if (lines == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(lines[i]);
    }
    free(lines);
}

/*
 * Splits `text` into a malloc'd array of lines (stripping '\r').
 * Tabs expand to four spaces — Cairo's toy API does not align them.
 */
static char **split_lines(const char *text, size_t *out_count)
{
    size_t count = count_lines(text);
    char **lines = (char **)calloc(count, sizeof(char *));
    if (lines == NULL) {
        return NULL;
    }

    size_t      idx   = 0;
    const char *start = text;
    for (const char *p = text;; p++) {
        if (*p == '\n' || *p == '\0') {
            size_t len = (size_t)(p - start);
            if (len > 0 && start[len - 1] == '\r') {
                len--; /* CRLF */
            }

            /* Count tabs first so we know how much room to allocate. */
            size_t tabs = 0;
            for (size_t k = 0; k < len; k++) {
                if (start[k] == '\t') {
                    tabs++;
                }
            }

            lines[idx] = (char *)malloc(len + tabs * 3 + 1);
            if (lines[idx] == NULL) {
                free_lines(lines, idx);
                return NULL;
            }

            size_t w = 0;
            for (size_t k = 0; k < len; k++) {
                if (start[k] == '\t') {
                    memcpy(lines[idx] + w, "    ", 4);
                    w += 4;
                } else {
                    lines[idx][w++] = start[k];
                }
            }
            lines[idx][w] = '\0';
            idx++;

            if (*p == '\0') {
                break;
            }
            start = p + 1;
        }
    }

    *out_count = idx;
    return lines;
}

/* ------------------------------------------------------------------------- */
/* Cairo surface → packed premultiplied RGBA8                                 */
/* ------------------------------------------------------------------------- */

/*
 * Cairo's ARGB32 stores a pixel as a native-endian uint32 0xAARRGGBB.
 * Indexing bytes directly would be endian-dependent, so we read the uint32 and
 * shift the fields out — correct on every architecture.
 */
static bool surface_to_rgba(cairo_surface_t *surface, Texture *out)
{
    cairo_surface_flush(surface);

    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "error (cairo surface): %s\n",
                cairo_status_to_string(cairo_surface_status(surface)));
        return false;
    }

    int            w          = cairo_image_surface_get_width(surface);
    int            h          = cairo_image_surface_get_height(surface);
    int            src_stride = cairo_image_surface_get_stride(surface);
    const uint8_t *src        = cairo_image_surface_get_data(surface);

    if (src == NULL || !texture_alloc(out, w, h)) {
        return false;
    }

    for (int y = 0; y < h; y++) {
        const uint8_t *src_row = src + (size_t)y * (size_t)src_stride;
        uint8_t       *dst_row = out->pixels + (size_t)y * out->stride;

        for (int x = 0; x < w; x++) {
            uint32_t p;
            memcpy(&p, src_row + (size_t)x * 4u, sizeof p);

            dst_row[x * 4 + 0] = (uint8_t)((p >> 16) & 0xFFu); /* R */
            dst_row[x * 4 + 1] = (uint8_t)((p >>  8) & 0xFFu); /* G */
            dst_row[x * 4 + 2] = (uint8_t)((p      ) & 0xFFu); /* B */
            dst_row[x * 4 + 3] = (uint8_t)((p >> 24) & 0xFFu); /* A */
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Building the glyph metrics                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Measures where each of a token's characters ends.
 *
 * Prefixes are measured (rather than summing individual characters) so that
 * kerning is respected — "AV" is narrower than "A" + "V" measured apart.
 * This is O(n²) shaping, but it happens once and lines are short.
 */
static void measure_char_advances(cairo_t *cr, char *scratch, const char *s, size_t len,
                                  double base_x, float *out_x, size_t *out_count)
{
    memcpy(scratch, s, len);
    scratch[len] = '\0';

    size_t written = 0;
    for (size_t b = 1; b <= len; b++) {
        /* Stop only on code-point boundaries. */
        if (b < len && !utf8_is_lead((unsigned char)scratch[b])) {
            continue;
        }

        char save   = scratch[b];
        scratch[b]  = '\0';

        cairo_text_extents_t te;
        cairo_text_extents(cr, scratch, &te);
        scratch[b] = save;

        out_x[written++] = (float)(base_x + te.x_advance);
    }
    *out_count = written;
}

/* ------------------------------------------------------------------------- */
/* The central rasterizer                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Word wrapping.
 *
 * A greedy algorithm: append words one by one until the limit is exceeded.
 * A word wider than the limit on its own is left whole — breaking mid-word
 * damages code and URLs more often than it helps.
 */
static char **wrap_lines(char **lines, size_t *count, const char *family, int font_size,
                         cairo_font_weight_t weight, cairo_font_slant_t slant, float max_w)
{
    cairo_surface_t *probe    = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t         *cr       = cairo_create(probe);
    cairo_select_font_face(cr, family, slant, weight);
    cairo_set_font_size(cr, (double)font_size);

    char **out     = NULL;
    size_t out_n   = 0, out_cap = 0;
    bool   ok      = true;

    for (size_t i = 0; i < *count && ok; i++) {
        const char *src = lines[i];
        size_t      len = strlen(src);

        char *buf = (char *)malloc(len + 1);
        if (buf == NULL) { ok = false; break; }
        buf[0] = '\0';
        size_t buf_len = 0;

        const char *word = src;
        while (ok) {
            /* boundaries of the next word */
            while (*word == ' ') word++;
            if (*word == '\0') break;

            const char *word_end = word;
            while (*word_end != '\0' && *word_end != ' ') word_end++;
            size_t wlen = (size_t)(word_end - word);

            /* candidate: current + space + word */
            size_t cand_len = (buf_len ? buf_len + 1 : 0) + wlen;
            char  *cand = (char *)malloc(cand_len + 1);
            if (cand == NULL) { ok = false; break; }
            if (buf_len) {
                memcpy(cand, buf, buf_len);
                cand[buf_len] = ' ';
                memcpy(cand + buf_len + 1, word, wlen);
            } else {
                memcpy(cand, word, wlen);
            }
            cand[cand_len] = '\0';

            cairo_text_extents_t te;
            cairo_text_extents(cr, cand, &te);

            if (te.x_advance > (double)max_w && buf_len > 0) {
                /* does not fit → close this line and start a new one with the word */
                if (out_n == out_cap) {
                    out_cap = out_cap ? out_cap * 2 : 8;
                    char **g = (char **)realloc(out, out_cap * sizeof(char *));
                    if (g == NULL) { free(cand); ok = false; break; }
                    out = g;
                }
                out[out_n++] = buf;

                buf = (char *)malloc(wlen + 1);
                if (buf == NULL) { free(cand); ok = false; break; }
                memcpy(buf, word, wlen);
                buf[wlen] = '\0';
                buf_len = wlen;
                free(cand);
            } else {
                free(buf);
                buf     = cand;
                buf_len = cand_len;
            }
            word = word_end;
        }

        if (!ok) { free(buf); break; }

        if (out_n == out_cap) {
            out_cap = out_cap ? out_cap * 2 : 8;
            char **g = (char **)realloc(out, out_cap * sizeof(char *));
            if (g == NULL) { free(buf); ok = false; break; }
            out = g;
        }
        out[out_n++] = buf;   /* empty lines are kept too (paragraph spacing) */
    }

    cairo_destroy(cr);
    cairo_surface_destroy(probe);

    if (!ok) {
        free_lines(out, out_n);
        return NULL;
    }

    free_lines(lines, *count);
    *count = out_n;
    return out;
}

/*
 * The styled pieces of one line.
 * `tokens == NULL` → the whole line is a single colour.
 */
typedef struct {
    const char *text;
    Token      *tokens;
    size_t      token_count;
} StyledLine;

/*
 * Draws the lines onto a texture and, optionally, collects glyph metrics.
 *
 * This one function serves both plain text and highlighted code — the only
 * difference is whether a line carries tokens.
 */
static bool raster_styled_lines(StyledLine *lines, size_t line_count, const char *font_family,
                                int font_size, Color base_color, float line_spacing,
                                int padding, float align, Texture *out, GlyphMetrics *metrics)
{
    char                family[128];
    cairo_font_weight_t weight;
    cairo_font_slant_t  slant;
    split_font_spec(font_family, family, sizeof family, &weight, &slant);

    /* --- stage 1: measure on a 1x1 "ruler" surface ------------------------ */
    cairo_surface_t *probe    = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t         *probe_cr = cairo_create(probe);

    cairo_select_font_face(probe_cr, family, slant, weight);
    cairo_set_font_size(probe_cr, (double)font_size);

    cairo_font_extents_t fe;
    cairo_font_extents(probe_cr, &fe);

    double max_width  = 0.0;
    double min_x_bear = 0.0;
    size_t max_bytes  = 1;

    for (size_t i = 0; i < line_count; i++) {
        cairo_text_extents_t te;
        cairo_text_extents(probe_cr, lines[i].text, &te);

        double logical = (te.x_advance > te.width + te.x_bearing) ? te.x_advance
                                                                  : te.width + te.x_bearing;
        if (logical > max_width) {
            max_width = logical;
        }
        if (te.x_bearing < min_x_bear) {
            min_x_bear = te.x_bearing;
        }

        size_t blen = strlen(lines[i].text);
        if (blen > max_bytes) {
            max_bytes = blen;
        }
    }

    cairo_destroy(probe_cr);
    cairo_surface_destroy(probe);

    double line_height = fe.height * (double)line_spacing;
    if (line_height < 1.0) {
        line_height = (double)font_size;
    }

    int tex_w = (int)ceil(max_width - min_x_bear) + 2 * padding;
    int tex_h = (int)ceil(fe.height + line_height * (double)(line_count - 1)) + 2 * padding;

    if (tex_w < 1) tex_w = 1;
    if (tex_h < 1) tex_h = 1;

    if (tex_w > MAX_TEXTURE_DIM || tex_h > MAX_TEXTURE_DIM) {
        fprintf(stderr, "error: text too large (%dx%d).\n", tex_w, tex_h);
        return false;
    }

    /* --- stage 2: allocate the metric buffers ---------------------------- */
    char  *scratch    = NULL;
    float *char_x     = NULL;
    int   *line_start = NULL;

    if (metrics != NULL) {
        size_t total_slots = 0;
        for (size_t i = 0; i < line_count; i++) {
            total_slots += utf8_count(lines[i].text, strlen(lines[i].text)) + 1;
        }

        scratch    = (char *)malloc(max_bytes + 1);
        char_x     = (float *)malloc(total_slots * sizeof(float));
        line_start = (int *)malloc((line_count + 1) * sizeof(int));

        if (scratch == NULL || char_x == NULL || line_start == NULL) {
            free(scratch);
            free(char_x);
            free(line_start);
            fprintf(stderr, "error: could not allocate the glyph metrics.\n");
            return false;
        }
    }

    /* --- stage 3: draw ----------------------------------------------------- */
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tex_w, tex_h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "error: could not create a cairo surface (%dx%d).\n", tex_w, tex_h);
        cairo_surface_destroy(surface);
        free(scratch); free(char_x); free(line_start);
        return false;
    }

    cairo_t *cr = cairo_create(surface);

    /* Transparent background: the SOURCE operator *writes* zeros rather than blending. */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_select_font_face(cr, family, slant, weight);
    cairo_set_font_size(cr, (double)font_size);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_GRAY);

    double origin_x   = (double)padding - min_x_bear;
    size_t slot       = 0;
    int    total_chars = 0;

    for (size_t i = 0; i < line_count; i++) {
        /* Cairo draws text on the BASELINE, not the top edge — hence +ascent. */
        double y = (double)padding + fe.ascent + line_height * (double)i;
        double x = origin_x;

        /* Alignment: shift the line relative to the widest one. */
        if (align > 0.0f) {
            cairo_text_extents_t le;
            cairo_text_extents(cr, lines[i].text, &le);
            x += (max_width - le.x_advance) * (double)align;
        }

        if (metrics != NULL) {
            line_start[i]   = (int)slot;
            char_x[slot++]  = (float)x; /* the line's left edge */
        }

        const char *text = lines[i].text;
        size_t      blen = strlen(text);

        if (lines[i].tokens == NULL || lines[i].token_count == 0) {
            /* single-colour line. */
            cairo_set_source_rgba(cr, base_color.r / 255.0, base_color.g / 255.0,
                                      base_color.b / 255.0, base_color.a / 255.0);
            cairo_move_to(cr, x, y);
            cairo_show_text(cr, text);

            if (metrics != NULL && blen > 0) {
                size_t n = 0;
                measure_char_advances(cr, scratch, text, blen, x, char_x + slot, &n);
                slot += n;
                total_chars += (int)n;
            }
        } else {
            /* Highlighted tokens — each in its own colour, accumulating x. */
            for (size_t t = 0; t < lines[i].token_count; t++) {
                const Token *tok = &lines[i].tokens[t];
                if (tok->len == 0 || tok->start + tok->len > blen) {
                    continue;
                }

                /* The token's text must be NUL-terminated temporarily. */
                char   piece_buf[512];
                char  *piece = piece_buf;
                bool   heap  = false;
                if (tok->len + 1 > sizeof piece_buf) {
                    piece = (char *)malloc(tok->len + 1);
                    if (piece == NULL) {
                        continue;
                    }
                    heap = true;
                }
                memcpy(piece, text + tok->start, tok->len);
                piece[tok->len] = '\0';

                Color c = highlighter_class_color(tok->cls);
                cairo_set_source_rgba(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0,
                                      (c.a / 255.0) * (base_color.a / 255.0));
                cairo_move_to(cr, x, y);
                cairo_show_text(cr, piece);

                cairo_text_extents_t te;
                cairo_text_extents(cr, piece, &te);

                if (metrics != NULL) {
                    size_t n = 0;
                    measure_char_advances(cr, scratch, piece, tok->len, x, char_x + slot, &n);
                    slot += n;
                    total_chars += (int)n;
                }

                x += te.x_advance;
                if (heap) {
                    free(piece);
                }
            }
        }
    }

    if (metrics != NULL) {
        line_start[line_count] = (int)slot;
    }

    cairo_status_t status = cairo_status(cr);
    cairo_destroy(cr);
    free(scratch);

    if (status != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "error (cairo): %s\n", cairo_status_to_string(status));
        cairo_surface_destroy(surface);
        free(char_x); free(line_start);
        return false;
    }

    /* --- stage 4: repack into packed RGBA --------------------------------- */
    bool ok = surface_to_rgba(surface, out);
    cairo_surface_destroy(surface);

    if (!ok) {
        free(char_x); free(line_start);
        return false;
    }

    if (metrics != NULL) {
        metrics->line_count  = (int)line_count;
        metrics->line_start  = line_start;
        metrics->char_x      = char_x;
        metrics->total_chars = total_chars;
        metrics->pad_y       = (float)padding;
        metrics->line_height = (float)line_height;
        metrics->h_cutoff    = (float *)malloc(line_count * VR_PIPELINE_DEPTH * sizeof(float));
        if (metrics->h_cutoff == NULL) {
            glyph_metrics_free(metrics);
            texture_free(out);
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Public: plain text                                                         */
/* ------------------------------------------------------------------------- */

bool media_render_text_rgba(const char *utf8_text, const char *font_family, int font_size,
                            Color color, float line_spacing, int padding,
                            float max_width, float align,
                            Texture *out, GlyphMetrics *metrics)
{
    if (out == NULL) {
        return false;
    }
    if (utf8_text == NULL) {
        utf8_text = "";
    }
    if (font_size < 1)        font_size = 1;
    if (line_spacing < 0.5f)  line_spacing = 1.0f;
    if (padding < 0)          padding = 0;

    memset(out, 0, sizeof *out);
    if (metrics != NULL) {
        memset(metrics, 0, sizeof *metrics);
    }

    size_t line_count = 0;
    char **lines      = split_lines(utf8_text, &line_count);
    if (lines == NULL) {
        return false;
    }

    if (max_width > 1.0f) {
        char                family[128];
        cairo_font_weight_t weight;
        cairo_font_slant_t  slant;
        split_font_spec(font_family, family, sizeof family, &weight, &slant);

        char **wrapped = wrap_lines(lines, &line_count, family, font_size,
                                    weight, slant, max_width);
        if (wrapped == NULL) {
            return false; /* wrap_lines already freed the original */
        }
        lines = wrapped;
    }

    StyledLine *styled = (StyledLine *)calloc(line_count, sizeof(StyledLine));
    if (styled == NULL) {
        free_lines(lines, line_count);
        return false;
    }
    for (size_t i = 0; i < line_count; i++) {
        styled[i].text = lines[i];
    }

    bool ok = raster_styled_lines(styled, line_count, font_family, font_size, color,
                                  line_spacing, padding, align, out, metrics);

    free(styled);
    free_lines(lines, line_count);
    return ok;
}

bool media_render_text_widget(TextWidget *w)
{
    if (w == NULL) {
        return false;
    }

    /* Padding scales with the font size so large glyphs are not clipped. */
    int padding = w->size / 4 + 4;

    return media_render_text_rgba(w->content, w->font, w->size, w->color, w->line_spacing,
                                  padding, w->max_width, w->align,
                                  &w->base.tex, &w->base.glyphs);
}

/* ------------------------------------------------------------------------- */
/* Public: code block (highlighted glyphs + a separate panel)                 */
/* ------------------------------------------------------------------------- */

/* Path for a rounded rectangle. */
static void rounded_rect_path(cairo_t *cr, double x, double y, double w, double h, double r)
{
    const double kPi = 3.14159265358979323846;

    if (r > w / 2.0) r = w / 2.0;
    if (r > h / 2.0) r = h / 2.0;

    if (r <= 0.0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }

    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -kPi / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0,         kPi / 2);
    cairo_arc(cr, x + r,     y + h - r, r, kPi / 2,   kPi);
    cairo_arc(cr, x + r,     y + r,     r, kPi,       3 * kPi / 2);
    cairo_close_path(cr);
}

static bool raster_plate(int w, int h, Color color, int radius, Texture *out)
{
    memset(out, 0, sizeof *out);

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return false;
    }

    cairo_t *cr = cairo_create(surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_set_source_rgba(cr, color.r / 255.0, color.g / 255.0, color.b / 255.0,
                          color.a / 255.0);
    rounded_rect_path(cr, 0, 0, w, h, radius);
    cairo_fill(cr);

    cairo_destroy(cr);
    bool ok = surface_to_rgba(surface, out);
    cairo_surface_destroy(surface);
    return ok;
}

/*
 * Rasterizing a geometric shape.
 *
 * Deliberately via Cairo rather than a dedicated kernel: the shape gets
 * antialiasing and rounded corners for free, and to the compositor it is just
 * another texture — fade, move, scale, rotate and effects all work unchanged.
 * The cost is one texture's worth of VRAM, acceptable even for a full-screen
 * scrim.
 */
bool media_render_shape_widget(ShapeWidget *w)
{
    if (w == NULL) {
        return false;
    }
    memset(&w->base.tex, 0, sizeof w->base.tex);

    int iw = (int)ceilf(w->w);
    int ih = (int)ceilf(w->h);
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;

    if (iw > MAX_TEXTURE_DIM || ih > MAX_TEXTURE_DIM) {
        fprintf(stderr, "error: shape too large (%dx%d).\n", iw, ih);
        return false;
    }

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return false;
    }

    cairo_t *cr = cairo_create(surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /*
     * The fill: a flat colour, or a gradient Cairo evaluates per pixel.
     *
     * Doing this at rasterization rather than in the compositor means a
     * gradient costs nothing per frame — it is baked into the texture, like
     * everything else about a shape's appearance.
     */
    cairo_pattern_t *fill_pat = NULL;
    if (w->grad_kind != 0) {
        double a = w->grad_angle * 3.14159265358979323846 / 180.0;

        if (w->grad_kind == 2) {
            double r = (iw > ih ? iw : ih) * 0.5;
            fill_pat = cairo_pattern_create_radial(iw * 0.5, ih * 0.5, 0.0,
                                                   iw * 0.5, ih * 0.5, r);
        } else {
            /* The axis runs through the centre at `angle`, sized so the whole
             * box is covered whichever way it points. */
            double hx = cos(a) * iw * 0.5, hy = sin(a) * ih * 0.5;
            fill_pat = cairo_pattern_create_linear(iw * 0.5 - hx, ih * 0.5 - hy,
                                                   iw * 0.5 + hx, ih * 0.5 + hy);
        }
        cairo_pattern_add_color_stop_rgba(fill_pat, 0.0,
            w->grad_from.r / 255.0, w->grad_from.g / 255.0,
            w->grad_from.b / 255.0, w->grad_from.a / 255.0);
        cairo_pattern_add_color_stop_rgba(fill_pat, 1.0,
            w->grad_to.r / 255.0, w->grad_to.g / 255.0,
            w->grad_to.b / 255.0, w->grad_to.a / 255.0);
        cairo_set_source(cr, fill_pat);
    } else {
        cairo_set_source_rgba(cr, w->color.r / 255.0, w->color.g / 255.0,
                                  w->color.b / 255.0, w->color.a / 255.0);
    }

    /*
     * The outline is inset by half its width so it stays inside the texture.
     * Cairo centres a stroke on its path, so without this the outer half would
     * be clipped away and the ring would look thin on one side.
     */
    double sw   = (w->stroke_width > 0.0f && w->stroke_color.a > 0) ? w->stroke_width : 0.0;
    double half = sw * 0.5;

    if (w->base.kind == WIDGET_CIRCLE) {
        /* An ellipse is a unit circle plus a scale, so w != h also works. */
        cairo_save(cr);
        cairo_translate(cr, iw / 2.0, ih / 2.0);
        cairo_scale(cr, (iw - sw) / 2.0, (ih - sw) / 2.0);
        cairo_arc(cr, 0.0, 0.0, 1.0, 0.0, 2.0 * 3.14159265358979323846);
        cairo_restore(cr);
    } else {
        rounded_rect_path(cr, half, half, iw - sw, ih - sw, w->corner_radius);
    }

    if (w->filled) {
        /* _preserve, so the same path can then be stroked. */
        if (sw > 0.0) {
            cairo_fill_preserve(cr);
        } else {
            cairo_fill(cr);
        }
    }

    if (sw > 0.0) {
        cairo_set_source_rgba(cr, w->stroke_color.r / 255.0, w->stroke_color.g / 255.0,
                                  w->stroke_color.b / 255.0, w->stroke_color.a / 255.0);
        cairo_set_line_width(cr, sw);
        cairo_stroke(cr);
    } else if (!w->filled) {
        cairo_new_path(cr);   /* nothing to draw — do not leave a dangling path */
    }

    if (fill_pat != NULL) {
        cairo_pattern_destroy(fill_pat);
    }

    cairo_destroy(cr);
    bool ok = surface_to_rgba(surface, &w->base.tex);
    cairo_surface_destroy(surface);
    return ok;
}

bool media_render_line_widget(LineWidget *w)
{
    if (w == NULL) {
        return false;
    }

    memset(&w->base.tex, 0, sizeof w->base.tex);

    float dx  = w->x2 - w->x1;
    float dy  = w->y2 - w->y1;
    float len = sqrtf(dx * dx + dy * dy);

    /* One extra pixel each way so antialiasing is not clipped. */
    int iw = (int)ceilf(len + w->width) + 2;
    int ih = (int)ceilf(w->width) + 2;
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return false;
    }

    cairo_t *cr = cairo_create(surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_set_source_rgba(cr, w->color.r / 255.0, w->color.g / 255.0,
                              w->color.b / 255.0, w->color.a / 255.0);
    cairo_set_line_width(cr, w->width);
    cairo_set_line_cap(cr, (w->cap == 1) ? CAIRO_LINE_CAP_ROUND
                         : (w->cap == 2) ? CAIRO_LINE_CAP_SQUARE
                                         : CAIRO_LINE_CAP_BUTT);

    /*
     * Always drawn along +x. The segment's real direction is base_rotation,
     * applied by the compositor — which is exactly what lets `trim` stay a
     * simple x threshold in texture space.
     */
    double y0 = ih * 0.5;
    double x0 = (iw - len) * 0.5;
    cairo_move_to(cr, x0, y0);
    cairo_line_to(cr, x0 + len, y0);
    cairo_stroke(cr);

    cairo_destroy(cr);
    bool ok = surface_to_rgba(surface, &w->base.tex);
    cairo_surface_destroy(surface);

    if (!ok) {
        return false;
    }

    /*
     * `trim` reuses the typewriter machinery verbatim: one "line" of text whose
     * character boundaries are the two ends of the segment. The compositor then
     * clips at `reveal × length` with no code of its own — which is why a line
     * that draws itself needed no new kernel.
     */
    /*
     * The cutoff is quantised to "characters", so the segment is divided into
     * sub-steps — one per pixel of length, capped. A single step would make
     * trim binary: the budget would be 0 or 1 and the line would snap from
     * invisible to complete with nothing in between.
     */
    int steps = (int)ceilf(len);
    if (steps < 2)   steps = 2;
    if (steps > 1024) steps = 1024;   /* a pixel of granularity is already invisible */

    GlyphMetrics *g = &w->base.glyphs;
    g->line_count  = 1;
    g->line_start  = (int *)calloc(2, sizeof(int));
    g->char_x      = (float *)calloc((size_t)steps + 1, sizeof(float));
    g->h_cutoff    = (float *)calloc((size_t)VR_PIPELINE_DEPTH, sizeof(float));
    if (g->line_start == NULL || g->char_x == NULL || g->h_cutoff == NULL) {
        return false;
    }
    g->line_start[0] = 0;
    g->line_start[1] = steps + 1;
    for (int k = 0; k <= steps; k++) {
        g->char_x[k] = (float)(x0 + len * (double)k / (double)steps);
    }
    g->total_chars   = steps;
    g->pad_y         = 0.0f;
    g->line_height   = (float)ih;

    return true;
}

/*
 * The path's bounding box.
 *
 * A cubic never leaves the convex hull of its four points, so taking the extent
 * of every control point is a correct — if slightly generous — bound. Being
 * generous costs a few transparent pixels; being wrong would clip the curve.
 */
static void path_bounds(const PathWidget *w, float *minx, float *miny,
                        float *maxx, float *maxy)
{
    *minx = *miny =  1e30f;
    *maxx = *maxy = -1e30f;

    for (size_t i = 0; i < w->seg_count; i++) {
        const PathSeg *s = &w->segs[i];
        int n = (s->op == 2) ? 3 : (s->op == 3) ? 0 : 1;

        for (int k = 0; k < n; k++) {
            float x = s->c[k * 2], y = s->c[k * 2 + 1];
            if (x < *minx) *minx = x;
            if (y < *miny) *miny = y;
            if (x > *maxx) *maxx = x;
            if (y > *maxy) *maxy = y;
        }
    }

    if (*minx > *maxx) {   /* no drawable segment at all */
        *minx = *miny = 0.0f;
        *maxx = *maxy = 1.0f;
    }
}

bool media_render_path_widget(PathWidget *w)
{
    if (w == NULL) {
        return false;
    }

    memset(&w->base.tex, 0, sizeof w->base.tex);

    if (w->seg_count == 0) {
        /* An unreadable path parsed to nothing; leave a 1x1 transparent
         * texture so the rest of the pipeline has something valid to skip. */
        w->base.base_w = w->base.base_h = 1.0f;
        return true;
    }

    float minx, miny, maxx, maxy;
    path_bounds(w, &minx, &miny, &maxx, &maxy);

    /* Room for the stroke, its joins, and a pixel of antialiasing. Mitres can
     * reach well past half a width, hence the whole width rather than half. */
    float pad = w->width + 2.0f;

    int iw = (int)ceilf(maxx - minx + 2.0f * pad);
    int ih = (int)ceilf(maxy - miny + 2.0f * pad);
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return false;
    }

    cairo_t *cr = cairo_create(surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* Draw in texture space: the bounding box's top-left becomes the origin. */
    cairo_translate(cr, pad - minx, pad - miny);

    for (size_t i = 0; i < w->seg_count; i++) {
        const PathSeg *s = &w->segs[i];
        switch (s->op) {
            case 0: cairo_move_to(cr, s->c[0], s->c[1]); break;
            case 1: cairo_line_to(cr, s->c[0], s->c[1]); break;
            case 2: cairo_curve_to(cr, s->c[0], s->c[1], s->c[2], s->c[3],
                                       s->c[4], s->c[5]); break;
            case 3: cairo_close_path(cr); break;
            default: break;
        }
    }
    if (w->closed) {
        cairo_close_path(cr);
    }

    if (w->filled) {
        cairo_set_source_rgba(cr, w->fill_color.r / 255.0, w->fill_color.g / 255.0,
                                  w->fill_color.b / 255.0, w->fill_color.a / 255.0);
        if (w->width > 0.0f) {
            cairo_fill_preserve(cr);
        } else {
            cairo_fill(cr);
        }
    }

    if (w->width > 0.0f) {
        cairo_set_source_rgba(cr, w->color.r / 255.0, w->color.g / 255.0,
                                  w->color.b / 255.0, w->color.a / 255.0);
        cairo_set_line_width(cr, w->width);
        cairo_set_line_cap(cr, (w->cap == 1) ? CAIRO_LINE_CAP_ROUND
                             : (w->cap == 2) ? CAIRO_LINE_CAP_SQUARE
                                             : CAIRO_LINE_CAP_BUTT);
        cairo_set_line_join(cr, (w->join == 1) ? CAIRO_LINE_JOIN_ROUND
                              : (w->join == 2) ? CAIRO_LINE_JOIN_BEVEL
                                               : CAIRO_LINE_JOIN_MITER);
        cairo_stroke(cr);
    } else {
        cairo_new_path(cr);
    }

    cairo_destroy(cr);
    bool ok = surface_to_rgba(surface, &w->base.tex);
    cairo_surface_destroy(surface);

    /*
     * The path's coordinates are absolute canvas positions, so the widget sits
     * where its bounding box says — an explicit x/y would fight the geometry
     * rather than complement it.
     */
    w->base.x = minx - pad;
    w->base.y = miny - pad;
    w->base.auto_center_x = false;
    w->base.has_track_x   = false;
    w->base.has_track_y   = false;
    free(w->base.x_expr); w->base.x_expr = NULL;
    free(w->base.y_expr); w->base.y_expr = NULL;

    return ok;
}

bool media_render_code_widget(CodeWidget *w)
{
    if (w == NULL) {
        return false;
    }

    memset(&w->base.tex, 0, sizeof w->base.tex);
    memset(&w->base.glyphs, 0, sizeof w->base.glyphs);
    memset(&w->plate, 0, sizeof w->plate);

    size_t line_count = 0;
    char **lines      = split_lines(w->code ? w->code : "", &line_count);
    if (lines == NULL) {
        return false;
    }

    StyledLine *styled = (StyledLine *)calloc(line_count, sizeof(StyledLine));
    Token      *tokens = NULL;

    if (styled == NULL) {
        free_lines(lines, line_count);
        return false;
    }

    /* One flat token buffer covering every line. */
    Language lang = w->highlight ? highlighter_language_from_name(w->language) : LANG_NONE;

    if (lang != LANG_NONE) {
        tokens = (Token *)calloc(line_count * MAX_TOKENS_PER_LINE, sizeof(Token));
        if (tokens == NULL) {
            free(styled);
            free_lines(lines, line_count);
            return false;
        }

        /* The lexer carries state between lines (block comments and triple
         * quotes span several of them). */
        HighlightState state;
        memset(&state, 0, sizeof state);

        for (size_t i = 0; i < line_count; i++) {
            Token *slot = tokens + i * MAX_TOKENS_PER_LINE;
            size_t n = highlighter_tokenize_line(lines[i], lang, &state, slot,
                                                 MAX_TOKENS_PER_LINE);
            styled[i].tokens      = slot;
            styled[i].token_count = n;
        }
    }

    for (size_t i = 0; i < line_count; i++) {
        styled[i].text = lines[i];
    }

    bool ok = raster_styled_lines(styled, line_count, w->font, w->size, w->fg,
                                  w->line_spacing, w->padding, 0.0f,
                                  &w->base.tex, &w->base.glyphs);

    free(tokens);
    free(styled);
    free_lines(lines, line_count);

    if (!ok) {
        return false;
    }

    /* The panel is exactly the glyph texture's size — both are drawn at the
     * same point, so no extra alignment is needed. */
    if (w->bg.a > 0) {
        if (!raster_plate(w->base.tex.width, w->base.tex.height, w->bg, w->corner_radius,
                          &w->plate)) {
            fprintf(stderr, "warning: could not draw the panel — continuing without it.\n");
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Images (stb_image)                                                         */
/* ------------------------------------------------------------------------- */

bool media_load_image_rgba(const char *path, Texture *out)
{
    if (path == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);

    int w = 0, h = 0, channels = 0;
    /* 4 = always give us RGBA, whatever the source's channel count. */
    stbi_uc *data = stbi_load(path, &w, &h, &channels, 4);
    if (data == NULL) {
        fprintf(stderr, "error: could not load image '%s' (%s).\n", path,
                stbi_failure_reason());
        return false;
    }

    if (!texture_alloc(out, w, h)) {
        stbi_image_free(data);
        return false;
    }

    /*
     * stb_image gives straight (non-premultiplied) alpha while our convention
     * is premultiplied → multiply here. That way the kernel needs only one
     * blend formula, whatever a texture's origin.
     */
    size_t pixel_count = (size_t)w * (size_t)h;
    for (size_t i = 0; i < pixel_count; i++) {
        unsigned a = data[i * 4 + 3];
        out->pixels[i * 4 + 0] = (uint8_t)((data[i * 4 + 0] * a + 127) / 255);
        out->pixels[i * 4 + 1] = (uint8_t)((data[i * 4 + 1] * a + 127) / 255);
        out->pixels[i * 4 + 2] = (uint8_t)((data[i * 4 + 2] * a + 127) / 255);
        out->pixels[i * 4 + 3] = (uint8_t)a;
    }

    stbi_image_free(data);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Filling the cache                                                          */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* Video                                                                      */
/* ------------------------------------------------------------------------- */

/* Asks ffprobe for the stream's size and frame rate. */
static bool probe_video(const char *path, int *w, int *h, float *fps)
{
    char quoted[2048];
    if (!vr_shell_quote(path, quoted, sizeof quoted)) {
        return false;
    }

    char cmd[2400];
    int n = snprintf(cmd, sizeof cmd,
                     "ffprobe -v error -select_streams v:0 "
                     "-show_entries stream=width,height,r_frame_rate "
                     "-of default=nw=1:nk=1 %s 2>/dev/null", quoted);
    if (n < 0 || (size_t)n >= sizeof cmd) {
        return false;
    }

    FILE *fp = popen(cmd, "r");
    if (fp == NULL) {
        return false;
    }

    char line[128];
    int  got = 0;
    *w = *h = 0;
    *fps = 30.0f;

    while (fgets(line, sizeof line, fp) != NULL) {
        if (got == 0)      *w = atoi(line);
        else if (got == 1) *h = atoi(line);
        else if (got == 2) {
            /* r_frame_rate is a rational, "30000/1001". */
            int num = 0, den = 1;
            if (sscanf(line, "%d/%d", &num, &den) == 2 && den != 0) {
                *fps = (float)num / (float)den;
            }
        }
        got++;
    }
    pclose(fp);

    return (*w > 0 && *h > 0);
}

bool media_load_video_widget(VideoWidget *w, int budget_mb)
{
    memset(&w->base.tex, 0, sizeof w->base.tex);

    if (w->path == NULL) {
        return false;
    }

    int   sw = 0, sh = 0;
    float fps = 30.0f;
    if (!probe_video(w->path, &sw, &sh, &fps)) {
        fprintf(stderr, "error: could not read video '%s' (is it a video?).\n", w->path);
        return false;
    }

    /*
     * Decode at the size it will be shown at, not the source's.
     *
     * This is the whole memory story: a 1080p frame is 8.3 MB, a 640x360 one is
     * 0.9 MB. A clip that appears as a small inset should not cost nine times
     * what it needs to.
     */
    int dw = (w->request_w > 0) ? w->request_w : sw;
    int dh = (w->request_h > 0) ? w->request_h : sh;
    if (w->request_w > 0 && w->request_h <= 0) {
        dh = (int)lrintf((float)w->request_w * (float)sh / (float)sw);
    } else if (w->request_h > 0 && w->request_w <= 0) {
        dw = (int)lrintf((float)w->request_h * (float)sw / (float)sh);
    }
    dw &= ~1; dh &= ~1;               /* even dimensions keep swscale happy */
    if (dw < 2) dw = 2;
    if (dh < 2) dh = 2;

    size_t frame_bytes = (size_t)dw * dh * 4;
    size_t budget      = (size_t)budget_mb * 1024u * 1024u;
    size_t max_frames  = (frame_bytes > 0) ? (budget / frame_bytes) : 0;
    if (max_frames < 1) {
        max_frames = 1;
    }

    char quoted[2048];
    if (!vr_shell_quote(w->path, quoted, sizeof quoted)) {
        return false;
    }

    /*
     * `-ss` before `-i` so ffmpeg seeks rather than decoding and discarding,
     * and `setpts` for speed so the frames arrive already retimed — the
     * alternative is picking frames ourselves and inheriting the rounding.
     */
    char cmd[4096];
    int  n = snprintf(cmd, sizeof cmd,
                      "ffmpeg -v error -ss %.3f -i %s "
                      "-vf \"scale=%d:%d,setpts=PTS/%.4f\" "
                      "-f rawvideo -pix_fmt rgba -",
                      (double)w->start, quoted, dw, dh, (double)w->speed);
    if (n < 0 || (size_t)n >= sizeof cmd) {
        return false;
    }

    FILE *pipe = popen(cmd, "r");
    if (pipe == NULL) {
        fprintf(stderr, "error: could not start ffmpeg to decode '%s'.\n", w->path);
        return false;
    }

    uint8_t *buf   = NULL;
    size_t   count = 0;
    size_t   cap   = 0;
    bool     ok    = true;

    while (count < max_frames) {
        if (count == cap) {
            size_t new_cap = (cap == 0) ? 16 : cap * 2;
            if (new_cap > max_frames) {
                new_cap = max_frames;
            }
            uint8_t *grown = (uint8_t *)realloc(buf, new_cap * frame_bytes);
            if (grown == NULL) {
                ok = false;
                break;
            }
            buf = grown;
            cap = new_cap;
        }

        size_t got = fread(buf + count * frame_bytes, 1, frame_bytes, pipe);
        if (got < frame_bytes) {
            break;      /* end of stream, or a partial trailing frame */
        }
        count++;
    }

    int status = pclose(pipe);
    if (status != 0 && count == 0) {
        fprintf(stderr, "error: decoding '%s' failed (ffmpeg exit %d).\n", w->path, status);
        free(buf);
        return false;
    }
    if (!ok || count == 0) {
        fprintf(stderr, "error: no frames decoded from '%s'.\n", w->path);
        free(buf);
        return false;
    }
    if (count == max_frames) {
        fprintf(stderr, "warning: '%s' truncated at %zu frames (%d MB budget).\n",
                w->path, count, budget_mb);
    }

    /*
     * RGBA from ffmpeg is *straight* alpha, while every texture in this
     * pipeline is premultiplied. Video is opaque in practice, but converting
     * unconditionally keeps the invariant true rather than true-by-luck.
     */
    for (size_t i = 0; i < count * (size_t)dw * dh; i++) {
        uint8_t *px = buf + i * 4;
        if (px[3] != 255) {
            px[0] = (uint8_t)((px[0] * px[3] + 127) / 255);
            px[1] = (uint8_t)((px[1] * px[3] + 127) / 255);
            px[2] = (uint8_t)((px[2] * px[3] + 127) / 255);
        }
    }

    w->frame_w     = dw;
    w->frame_h     = dh;
    w->frame_count = (int)count;
    w->src_fps     = fps * w->speed;

    /* One texture, frames stacked top to bottom. The compositor is handed a
     * pointer into it rather than a separate texture per frame. */
    w->base.tex.pixels        = buf;
    w->base.tex.width         = dw;
    w->base.tex.height        = dh * (int)count;
    w->base.tex.stride        = (size_t)dw * 4;
    w->base.tex.premultiplied = true;

    fprintf(stderr, "video: %s — %d frames %dx%d @ %.2f fps (%.1f MiB)\n",
            w->path, w->frame_count, dw, dh, (double)w->src_fps,
            (double)(count * frame_bytes) / (1024.0 * 1024.0));
    return true;
}

/* ------------------------------------------------------------------------- */
/* Drop shadow and glow                                                       */
/* ------------------------------------------------------------------------- */

/*
 * A separable box blur, run three times.
 *
 * Three box passes converge on a Gaussian closely enough that the difference is
 * invisible at any blur radius a shadow uses, and each pass is O(1) per pixel
 * thanks to the running sum — so a 40-pixel blur costs the same per pixel as a
 * 4-pixel one. A true Gaussian would be O(radius).
 */
static void blur_alpha(float *buf, float *tmp, int w, int h, int radius)
{
    if (radius < 1) {
        return;
    }

    for (int pass = 0; pass < 3; pass++) {
        /* --- horizontal --- */
        for (int y = 0; y < h; y++) {
            const float *in  = buf + (size_t)y * w;
            float       *out = tmp + (size_t)y * w;

            float sum = 0.0f;
            int   n   = 0;
            for (int x = 0; x <= radius && x < w; x++) {
                sum += in[x];
                n++;
            }
            for (int x = 0; x < w; x++) {
                out[x] = sum / (float)n;
                int add = x + radius + 1;
                int sub = x - radius;
                if (add < w) { sum += in[add]; n++; }
                if (sub >= 0) { sum -= in[sub]; n--; }
            }
        }
        /* --- vertical --- */
        for (int x = 0; x < w; x++) {
            float sum = 0.0f;
            int   n   = 0;
            for (int y = 0; y <= radius && y < h; y++) {
                sum += tmp[(size_t)y * w + x];
                n++;
            }
            for (int y = 0; y < h; y++) {
                buf[(size_t)y * w + x] = sum / (float)n;
                int add = y + radius + 1;
                int sub = y - radius;
                if (add < h) { sum += tmp[(size_t)add * w + x]; n++; }
                if (sub >= 0) { sum -= tmp[(size_t)sub * w + x]; n--; }
            }
        }
    }
}

/*
 * Grows a texture by `pad` on every side and draws a blurred, tinted copy of
 * its own alpha underneath, offset by (dx, dy).
 *
 * The padding is symmetric even though the offset is not, so that the content
 * stays centred in its texture — anchoring and rotation both work from the
 * texture's centre, and an asymmetric pad would shift a centred object.
 *
 * `draw_shadow` false pads without drawing anything, which is how a code
 * block's glyph layer is kept the same size as its panel.
 */
static bool texture_pad_shadow(Texture *t, const WidgetBase *b, int pad, bool draw_shadow)
{
    if (t->pixels == NULL || pad <= 0) {
        return true;
    }

    int nw = t->width + 2 * pad;
    int nh = t->height + 2 * pad;

    uint8_t *dst = (uint8_t *)calloc((size_t)nw * nh, 4);
    if (dst == NULL) {
        return false;
    }

    if (draw_shadow) {
        float *a   = (float *)calloc((size_t)nw * nh, sizeof(float));
        float *tmp = (float *)calloc((size_t)nw * nh, sizeof(float));
        if (a == NULL || tmp == NULL) {
            free(a); free(tmp); free(dst);
            return false;
        }

        /* The source alpha, placed where the shadow should fall. */
        int ox = pad + (int)lrintf(b->shadow_dx);
        int oy = pad + (int)lrintf(b->shadow_dy);

        for (int y = 0; y < t->height; y++) {
            int ty = y + oy;
            if (ty < 0 || ty >= nh) {
                continue;
            }
            for (int x = 0; x < t->width; x++) {
                int tx = x + ox;
                if (tx < 0 || tx >= nw) {
                    continue;
                }
                a[(size_t)ty * nw + tx] =
                    (float)t->pixels[((size_t)y * t->width + x) * 4 + 3] / 255.0f;
            }
        }

        blur_alpha(a, tmp, nw, nh, (int)lrintf(b->shadow_blur / 3.0f));
        free(tmp);

        const float cr = b->shadow_color.r / 255.0f;
        const float cg = b->shadow_color.g / 255.0f;
        const float cb = b->shadow_color.b / 255.0f;
        const float ca = b->shadow_color.a / 255.0f;

        for (size_t i = 0; i < (size_t)nw * nh; i++) {
            float sa = a[i] * ca;
            if (sa <= 0.0f) {
                continue;
            }
            if (sa > 1.0f) sa = 1.0f;
            /* Premultiplied, like every other texture in the pipeline. */
            dst[i * 4 + 0] = (uint8_t)(cr * sa * 255.0f + 0.5f);
            dst[i * 4 + 1] = (uint8_t)(cg * sa * 255.0f + 0.5f);
            dst[i * 4 + 2] = (uint8_t)(cb * sa * 255.0f + 0.5f);
            dst[i * 4 + 3] = (uint8_t)(sa * 255.0f + 0.5f);
        }
        free(a);
    }

    /* The original content, source-over on top of the shadow. */
    for (int y = 0; y < t->height; y++) {
        for (int x = 0; x < t->width; x++) {
            const uint8_t *sp = t->pixels + ((size_t)y * t->width + x) * 4;
            uint8_t       *dp = dst + ((size_t)(y + pad) * nw + (x + pad)) * 4;

            float sa = sp[3] / 255.0f;
            float ia = 1.0f - sa;
            for (int c = 0; c < 4; c++) {
                float v = sp[c] / 255.0f + (dp[c] / 255.0f) * ia;
                if (v > 1.0f) v = 1.0f;
                dp[c] = (uint8_t)(v * 255.0f + 0.5f);
            }
        }
    }

    free(t->pixels);
    t->pixels = dst;
    t->width  = nw;
    t->height = nh;
    t->stride = (size_t)nw * 4;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Bindings — positions expressed against other objects                       */
/* ------------------------------------------------------------------------- */

/*
 * One edge of an object's box, in canvas pixels.
 *
 * Reads the *resolved* geometry, so this only makes sense once layout has run
 * — which is exactly why bindings are a second pass rather than something the
 * parser could do.
 */
static bool object_edge(const EditorContext *ctx, const char *id, const char *edge,
                        float *out)
{
    for (size_t i = 0; i < ctx->widget_count; i++) {
        const WidgetBase *b = ctx->widgets[i];
        if (b->id == NULL || strcmp(b->id, id) != 0) {
            continue;
        }

        /*
         * Refuse while the referenced object's own binding is still pending.
         *
         * Without this a cycle "resolves" against whatever default position the
         * other object happens to hold, producing a plausible-looking wrong
         * answer instead of the warning the author needs. Size is exempt: `w`
         * and `h` never depend on a binding.
         */
        bool needs_x = (strcmp(edge, "left") == 0 || strcmp(edge, "right") == 0 ||
                        strcmp(edge, "cx") == 0);
        bool needs_y = (strcmp(edge, "top") == 0 || strcmp(edge, "bottom") == 0 ||
                        strcmp(edge, "cy") == 0);
        if ((needs_x && b->x_bind != NULL) || (needs_y && b->y_bind != NULL)) {
            return false;
        }

        float left = b->x - b->anchor_off_x;
        float top  = b->y - b->anchor_off_y;

        if      (strcmp(edge, "left")   == 0) *out = left;
        else if (strcmp(edge, "right")  == 0) *out = left + b->base_w;
        else if (strcmp(edge, "top")    == 0) *out = top;
        else if (strcmp(edge, "bottom") == 0) *out = top + b->base_h;
        else if (strcmp(edge, "cx")     == 0) *out = left + b->base_w * 0.5f;
        else if (strcmp(edge, "cy")     == 0) *out = top  + b->base_h * 0.5f;
        else if (strcmp(edge, "w")      == 0) *out = b->base_w;
        else if (strcmp(edge, "h")      == 0) *out = b->base_h;
        else return false;
        return true;
    }
    return false;
}

/*
 * Evaluates `=other.bottom + 24`.
 *
 * Same tiny grammar as an event's `time`: a term, then any number of signed
 * terms. A term is either a number or `object.edge`. Keeping the two grammars
 * identical is deliberate — one shape to learn, not two.
 */
static bool eval_binding(const EditorContext *ctx, const char *expr, float *out)
{
    const char *p = expr;
    if (*p == '=') {
        p++;
    }

    float total = 0.0f;
    float sign  = 1.0f;
    bool  first = true;

    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        if (!first) {
            if (*p == '+')      { sign =  1.0f; p++; }
            else if (*p == '-') { sign = -1.0f; p++; }
            else return false;
            while (*p == ' ' || *p == '\t') p++;
        }

        if (isdigit((unsigned char)*p) || *p == '.') {
            char *end = NULL;
            double v = strtod(p, &end);
            if (end == p) return false;
            total += sign * (float)v;
            p = end;
        } else if (isalpha((unsigned char)*p) || *p == '_') {
            const char *start = p;
            while (isalnum((unsigned char)*p) || *p == '_' || *p == '-') p++;

            char id[128];
            size_t n = (size_t)(p - start);
            if (n >= sizeof id) return false;
            memcpy(id, start, n);
            id[n] = '\0';

            if (*p != '.') return false;
            p++;

            const char *estart = p;
            while (isalpha((unsigned char)*p)) p++;

            char edge[24];
            size_t m = (size_t)(p - estart);
            if (m == 0 || m >= sizeof edge) return false;
            memcpy(edge, estart, m);
            edge[m] = '\0';

            float v = 0.0f;
            if (!object_edge(ctx, id, edge, &v)) {
                return false;
            }
            total += sign * v;
        } else {
            return false;
        }

        sign  = 1.0f;
        first = false;
    }

    *out = total;
    return true;
}

/*
 * Resolves every binding, repeatedly.
 *
 * An object may be bound to one that is itself bound, so a single pass is not
 * enough; and a cycle must not loop forever. Bounded by the object count, which
 * is the longest a legitimate chain can be.
 */
static void resolve_bindings(EditorContext *ctx)
{
    bool any = false;
    for (size_t i = 0; i < ctx->widget_count && !any; i++) {
        any = (ctx->widgets[i]->x_bind != NULL || ctx->widgets[i]->y_bind != NULL);
    }
    if (!any) {
        return;
    }

    for (size_t pass = 0; pass <= ctx->widget_count; pass++) {
        bool progress = false;

        for (size_t i = 0; i < ctx->widget_count; i++) {
            WidgetBase *b = ctx->widgets[i];
            float v;

            /*
             * The value becomes the object's *anchor point*, not its left edge
             * — exactly what a layout expression does. So `"anchor": "bottom"`
             * with `"x": "=bar.cx"` centres the label on the bar, which is what
             * anyone writing that means.
             */
            if (b->x_bind != NULL && eval_binding(ctx, b->x_bind, &v)) {
                b->x = v;
                free(b->x_bind);
                b->x_bind = NULL;
                b->auto_center_x = false;
                progress = true;
            }
            if (b->y_bind != NULL && eval_binding(ctx, b->y_bind, &v)) {
                b->y = v;
                free(b->y_bind);
                b->y_bind = NULL;
                progress = true;
            }
        }
        if (!progress) {
            break;
        }
    }

    for (size_t i = 0; i < ctx->widget_count; i++) {
        WidgetBase *b = ctx->widgets[i];
        if (b->x_bind != NULL) {
            fprintf(stderr, "warning: '%s' — cannot resolve x binding \"%s\".\n",
                    b->id ? b->id : "(null)", b->x_bind);
            free(b->x_bind);
            b->x_bind = NULL;
        }
        if (b->y_bind != NULL) {
            fprintf(stderr, "warning: '%s' — cannot resolve y binding \"%s\".\n",
                    b->id ? b->id : "(null)", b->y_bind);
            free(b->y_bind);
            b->y_bind = NULL;
        }
    }
}

bool media_prepare_textures(EditorContext *ctx)
{
    if (ctx == NULL) {
        return false;
    }

    for (size_t i = 0; i < ctx->widget_count; i++) {
        WidgetBase *b  = ctx->widgets[i];
        bool        ok = false;

        /* base is every widget's first field → these casts are safe. */
        switch (b->kind) {
            case WIDGET_TEXT:
                ok = media_render_text_widget((TextWidget *)b);
                break;
            case WIDGET_CODE:
                ok = media_render_code_widget((CodeWidget *)b);
                break;
            case WIDGET_IMAGE: {
                ImageWidget *iw = (ImageWidget *)b;
                ok = media_load_image_rgba(iw->path, &b->tex);
                break;
            }
            case WIDGET_RECT:
            case WIDGET_CIRCLE:
                ok = media_render_shape_widget((ShapeWidget *)b);
                break;
            case WIDGET_LINE:
                ok = media_render_line_widget((LineWidget *)b);
                break;
            case WIDGET_PATH:
                ok = media_render_path_widget((PathWidget *)b);
                break;
            case WIDGET_MESH: {
                /* A mesh's geometry came from the parser; only an optional
                 * surface texture is loaded here, where stb_image already is. */
                MeshWidget *mw = (MeshWidget *)b;
                ok = true;
                if (mw->tex_path != NULL) {
                    if (!media_load_image_rgba(mw->tex_path, &mw->tex)) {
                        fprintf(stderr, "warning: mesh '%s' — cannot load texture '%s'.\n",
                                b->id ? b->id : "(null)", mw->tex_path);
                    }
                }
                if (mw->ao_path != NULL) {
                    if (!media_load_image_rgba(mw->ao_path, &mw->ao)) {
                        fprintf(stderr, "warning: mesh '%s' — cannot load occlusion map "
                                        "'%s'.\n", b->id ? b->id : "(null)", mw->ao_path);
                    } else if (mw->ao.pixels != NULL) {
                        /*
                         * Occlusion is mostly open sky: a plausible map averages
                         * bright, with dark only in the creases. A very dark one
                         * means the red channel was never occlusion at all —
                         * an unused channel left at zero — and multiplying by it
                         * would black the object out. Better to drop it and say
                         * so than to render something uniformly wrong.
                         */
        /* RGBA8, so the red byte of texel k is at 4k. */
                        const uint8_t *px = mw->ao.pixels;
                        size_t n = (size_t)mw->ao.width * mw->ao.height;
                        unsigned long long sum = 0;
                        for (size_t k = 0; k < n; k++) {
                            sum += px[k * 4];
                        }
                        double mean = (n > 0) ? (double)sum / (double)n : 0.0;
                        if (mean < 25.0) {
                            fprintf(stderr, "warning: mesh '%s' — '%s' averages %.0f/255 "
                                            "in red; that is not occlusion, ignoring it.\n",
                                    b->id ? b->id : "(null)", mw->ao_path, mean);
                            texture_free(&mw->ao);
                        }
                    }
                }
                if (mw->nrm_path != NULL) {
                    if (!media_load_image_rgba(mw->nrm_path, &mw->nrm)) {
                        fprintf(stderr, "warning: mesh '%s' — cannot load normal map "
                                        "'%s'.\n", b->id ? b->id : "(null)", mw->nrm_path);
                    } else if (mw->nrm.pixels != NULL && mw->tans == NULL) {
                        fprintf(stderr, "warning: mesh '%s' — a normal map without "
                                        "tangents cannot be applied; ignoring it.\n",
                                b->id ? b->id : "(null)");
                        texture_free(&mw->nrm);
                    }
                }
                if (mw->emis_path != NULL) {
                    if (!media_load_image_rgba(mw->emis_path, &mw->emis)) {
                        fprintf(stderr, "warning: mesh '%s' — cannot load emissive map "
                                        "'%s'.\n", b->id ? b->id : "(null)", mw->emis_path);
                    }
                }
                break;
            }
            case WIDGET_VIDEO:
                /* 512 MB per clip: generous for an inset, still far short of
                 * decoding a feature film into RAM by accident. */
                ok = media_load_video_widget((VideoWidget *)b, 512);
                break;
            default:
                fprintf(stderr, "warning: '%s' — unknown type, skipped.\n",
                        b->id ? b->id : "(null)");
                continue;
        }

        if (!ok) {
            fprintf(stderr, "error: rasterizing '%s' failed.\n",
                    b->id ? b->id : "(null)");
            return false;
        }

        /*
         * --- shadow / glow -------------------------------------------------
         *
         * Done here rather than inside each rasterizer: one place, and it acts
         * on plain RGBA, so it works identically for text, shapes, lines,
         * paths and images.
         */
        /*
         * A clip's texture is every frame stacked, and the slice the compositor
         * receives assumes they are packed with no margin. Padding the strip
         * would put the margin around the *whole* stack, so every frame after
         * the first would be read from the wrong offset — which showed up as a
         * sliver of the neighbouring frame beside the picture.
         *
         * Padding each frame instead would mean re-blurring 120 images and
         * carrying the margin in memory, for an effect a video rarely needs.
         * Refusing, loudly, is the better trade.
         */
        if (b->shadow_on && b->kind == WIDGET_VIDEO) {
            fprintf(stderr, "warning: '%s' — shadow/glow is not supported on video; ignored.\n",
                    b->id ? b->id : "(null)");
            b->shadow_on = false;
        }

        if (b->shadow_on) {
            int pad = (int)lrintf(b->shadow_blur) +
                      (int)lrintf(fabsf(b->shadow_dx) > fabsf(b->shadow_dy)
                                      ? fabsf(b->shadow_dx) : fabsf(b->shadow_dy)) + 2;

            /* A code block's shadow belongs to its panel, not to the glyphs —
             * but both layers must stay the same size or they would no longer
             * line up, so the glyph layer is padded without a shadow. */
            bool code = (b->kind == WIDGET_CODE);
            CodeWidget *cw = code ? (CodeWidget *)b : NULL;

            if (code && cw->plate.pixels != NULL) {
                if (!texture_pad_shadow(&cw->plate, b, pad, true) ||
                    !texture_pad_shadow(&b->tex, b, pad, false)) {
                    return false;
                }
            } else if (!texture_pad_shadow(&b->tex, b, pad, true)) {
                return false;
            }

            b->tex_pad = pad;
        }

        /* --- base size (scale = 1) ----------------------------------------- */
        b->base_w = (float)b->tex.width;
        b->base_h = (float)b->tex.height;

        /* A mesh's extent is its own `size`, not a texture's. Giving it a box
         * keeps layout, anchoring and the validator working unchanged. */
        if (b->kind == WIDGET_MESH) {
            const MeshWidget *mw = (const MeshWidget *)b;
            b->base_w = mw->size[0];
            b->base_h = mw->size[1];
        }

        /* A clip's texture holds every frame stacked, so its height is the
         * whole strip; the object is one frame tall. */
        if (b->kind == WIDGET_VIDEO) {
            const VideoWidget *vw = (const VideoWidget *)b;
            b->base_h = (float)vw->frame_h;
        }

        if (b->kind == WIDGET_IMAGE) {
            const ImageWidget *iw = (const ImageWidget *)b;

            /* Requested size; if only one axis is given, keep the aspect. */
            if (iw->request_w > 0 && iw->request_h > 0) {
                b->base_w = (float)iw->request_w;
                b->base_h = (float)iw->request_h;
            } else if (iw->request_w > 0) {
                b->base_w = (float)iw->request_w;
                b->base_h = (float)iw->request_w * (float)b->tex.height / (float)b->tex.width;
            } else if (iw->request_h > 0) {
                b->base_h = (float)iw->request_h;
                b->base_w = (float)iw->request_h * (float)b->tex.width / (float)b->tex.height;
            }
        }

        /*
         * --- resolving the layout --------------------------------------
         *
         * Only here is the object's size known, so this is where
         * "center" / "bottom-160" / anchors become pixels.
         */
        if (b->auto_center_x) {
            b->x = ((float)ctx->config.width - b->base_w) * 0.5f;
        }

        if (b->x_expr != NULL) {
            float pos, def_anchor;
            if (layout_eval(b->x_expr, (float)ctx->config.width, LAYOUT_AXIS_X,
                            &pos, &def_anchor)) {
                b->x = pos;
                if (!b->has_anchor_x) {
                    b->anchor_x = def_anchor;
                }
            } else {
                fprintf(stderr, "warning: '%s' — unparsable x '%s'.\n",
                        b->id ? b->id : "(null)", b->x_expr);
            }
        }

        if (b->y_expr != NULL) {
            float pos, def_anchor;
            if (layout_eval(b->y_expr, (float)ctx->config.height, LAYOUT_AXIS_Y,
                            &pos, &def_anchor)) {
                b->y = pos;
                if (!b->has_anchor_y) {
                    b->anchor_y = def_anchor;
                }
            } else {
                fprintf(stderr, "warning: '%s' — unparsable y '%s'.\n",
                        b->id ? b->id : "(null)", b->y_expr);
            }
        }

        /*
         * `repeat_dx` and `tex_pad` are NOT applied here.
         *
         * `b->x` is only consulted when the object has no x track — and a plain
         * `"x": 240` counts as a track whose constant is 240, so an adjustment
         * made here would be skipped for most objects. They are applied in
         * vr_evaluate_scene instead, alongside `anchor_off_*`, which has always
         * had to solve exactly this problem.
         */

        /* auto-center already yields a final coordinate — anchoring skips it. */
        b->anchor_off_x = b->auto_center_x ? 0.0f : b->anchor_x * b->base_w;
        b->anchor_off_y = b->anchor_y * b->base_h;
    }

    /* Bindings last: they read other objects' final geometry. */
    resolve_bindings(ctx);

    return true;
}
