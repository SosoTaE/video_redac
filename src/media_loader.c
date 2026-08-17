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

    cairo_set_source_rgba(cr, w->color.r / 255.0, w->color.g / 255.0,
                              w->color.b / 255.0, w->color.a / 255.0);

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

        /* --- base size (scale = 1) ----------------------------------------- */
        b->base_w = (float)b->tex.width;
        b->base_h = (float)b->tex.height;

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

        /* A `repeat` displacement lands on top of the resolved position, so
         * that "the twelfth copy, 240 px out" still honours "center". */
        b->x += b->repeat_dx;
        b->y += b->repeat_dy;

        /* auto-center already yields a final coordinate — anchoring skips it. */
        b->anchor_off_x = b->auto_center_x ? 0.0f : b->anchor_x * b->base_w;
        b->anchor_off_y = b->anchor_y * b->base_h;
    }

    return true;
}
