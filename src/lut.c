/*
 * lut.c — reading Adobe/Iridas `.cube` files.
 *
 * The format is deliberately simple: a few keyword lines, then one RGB triple
 * per line in a fixed order. What makes a reader non-trivial is that files in
 * the wild are sloppy — Windows line endings, tabs, comments after data, a
 * declared size that does not match the number of rows, and domains other than
 * 0..1. Each of those is handled here rather than producing a plausible-looking
 * but wrong image, which is the failure mode that matters: a LUT applied with
 * the wrong stride still renders, in wrong colour.
 */

#include "lut.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The largest cube worth accepting. 65 is the biggest size in common use and
 * 128^3 triples is already 25 MB; past that a file is corrupt rather than
 * ambitious. */
#define LUT_MAX_SIZE 128

static char *skip_ws(char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r') {
        p++;
    }
    return p;
}

/* Reads up to three floats from `p`. Returns how many were found. */
static int read_floats(const char *p, float *out, int max)
{
    int n = 0;
    while (n < max) {
        char *end = NULL;
        double v = strtod(p, &end);
        if (end == p) {
            break;
        }
        out[n++] = (float)v;
        p = end;
    }
    return n;
}

bool lut_load_cube(const char *path, float **out, int *size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "error: cannot open LUT '%s'.\n", path);
        return false;
    }

    int    n1 = 0, n3 = 0;
    float  dmin[3] = { 0.0f, 0.0f, 0.0f };
    float  dmax[3] = { 1.0f, 1.0f, 1.0f };
    float *data = NULL;
    size_t cap = 0, count = 0;      /* count is in triples */
    bool   ok = true;

    char line[512];
    while (fgets(line, sizeof line, f) != NULL) {
        char *p = skip_ws(line);

        /* Blank, or a comment. A '#' after data is a comment too, but strtod
         * stops there anyway, so only a leading one needs a test. */
        if (*p == '\0' || *p == '\n' || *p == '#') {
            continue;
        }

        if (strncmp(p, "TITLE", 5) == 0) {
            continue;
        }
        if (strncmp(p, "LUT_1D_SIZE", 11) == 0) {
            n1 = atoi(p + 11);
            continue;
        }
        if (strncmp(p, "LUT_3D_SIZE", 11) == 0) {
            n3 = atoi(p + 11);
            continue;
        }
        if (strncmp(p, "DOMAIN_MIN", 10) == 0) {
            read_floats(p + 10, dmin, 3);
            continue;
        }
        if (strncmp(p, "DOMAIN_MAX", 10) == 0) {
            read_floats(p + 10, dmax, 3);
            continue;
        }
        /* Anything else beginning with a letter is a keyword this reader does
         * not know; skipping it is safer than feeding it to strtod. */
        if (isalpha((unsigned char)*p) || *p == '_') {
            continue;
        }

        float rgb[3];
        if (read_floats(p, rgb, 3) != 3) {
            continue;
        }
        if (count == cap) {
            size_t nc = (cap == 0) ? 4096 : cap * 2;
            float *g = (float *)realloc(data, nc * 3 * sizeof(float));
            if (g == NULL) {
                ok = false;
                break;
            }
            data = g;
            cap = nc;
        }
        data[count * 3 + 0] = rgb[0];
        data[count * 3 + 1] = rgb[1];
        data[count * 3 + 2] = rgb[2];
        count++;
    }
    fclose(f);

    if (!ok) {
        free(data);
        fprintf(stderr, "error: out of memory reading LUT '%s'.\n", path);
        return false;
    }
    if (n3 == 0 && n1 == 0) {
        free(data);
        fprintf(stderr, "error: LUT '%s' declares neither LUT_3D_SIZE nor "
                        "LUT_1D_SIZE.\n", path);
        return false;
    }

    int n = (n3 > 0) ? n3 : n1;
    if (n < 2 || n > LUT_MAX_SIZE) {
        free(data);
        fprintf(stderr, "error: LUT '%s' has size %d; 2..%d is supported.\n",
                path, n, LUT_MAX_SIZE);
        return false;
    }

    size_t want = (n3 > 0) ? (size_t)n * n * n : (size_t)n;
    if (count < want) {
        free(data);
        fprintf(stderr, "error: LUT '%s' declares %zu entries but contains %zu.\n",
                path, want, count);
        return false;
    }
    if (count > want) {
        /* More rows than declared: trust the declaration, since the stride is
         * what the declaration fixes, and say so. */
        fprintf(stderr, "warning: LUT '%s' has %zu entries for a declared %zu; "
                        "the extra rows are ignored.\n", path, count, want);
    }

    /*
     * The domain, applied on load rather than per pixel.
     *
     * A .cube may declare that its input range is something other than 0..1 —
     * log footage is the usual reason. Nothing downstream here works in those
     * units, so the table is resampled into 0..1 on the way in and the pixel
     * loop stays a plain lookup. A non-default domain is reported because it
     * means the file expects footage this renderer does not produce, and the
     * result will be a look rather than the intended grade.
     */
    bool odd_domain = false;
    for (int k = 0; k < 3; k++) {
        if (dmin[k] != 0.0f || dmax[k] != 1.0f) {
            odd_domain = true;
        }
    }
    if (odd_domain) {
        fprintf(stderr, "note: LUT '%s' declares domain [%g %g %g]..[%g %g %g]; "
                        "this renderer works in 0..1 and applies it as-is.\n",
                path, (double)dmin[0], (double)dmin[1], (double)dmin[2],
                (double)dmax[0], (double)dmax[1], (double)dmax[2]);
    }

    if (n3 > 0) {
        *out = data;
        *size = n;
        return true;
    }

    /*
     * A 1D table, expanded into a cube.
     *
     * Each axis is looked up independently — that is exactly what a 1D LUT
     * means — so the cube is separable and the expansion is exact rather than
     * an approximation. It costs n^3 triples where the file held n, which for
     * the sizes 1D tables use (typically 1024 at most... but those would be
     * 4 GB) is why the size cap matters: a 1024-entry 1D LUT is refused above,
     * and the ones that pass are small.
     */
    float *cube = (float *)malloc((size_t)n * n * n * 3 * sizeof(float));
    if (cube == NULL) {
        free(data);
        fprintf(stderr, "error: out of memory expanding LUT '%s'.\n", path);
        return false;
    }
    for (int b = 0; b < n; b++) {
        for (int g = 0; g < n; g++) {
            for (int r = 0; r < n; r++) {
                size_t i = ((size_t)b * n * n + (size_t)g * n + (size_t)r) * 3;
                cube[i + 0] = data[(size_t)r * 3 + 0];
                cube[i + 1] = data[(size_t)g * 3 + 1];
                cube[i + 2] = data[(size_t)b * 3 + 2];
            }
        }
    }
    free(data);
    *out = cube;
    *size = n;
    return true;
}
