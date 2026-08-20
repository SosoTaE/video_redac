/*
 * parser.c — video.json → EditorContext.
 *
 * Principles:
 *   1. The parser only *reads* — it draws nothing and never touches the GPU.
 *   2. Every allocation is recorded in the context, so a single call to
 *      editor_context_free() cleans up everything.
 *   3. Any JSON field may be absent — everything has a default.
 *      A missing field is not an error; only genuine failures return NULL.
 */

#include "parser.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>

#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "anim.h"
#include "arena.h"
#include "audio.h"
#include "effects.h"
#include "layout.h"
#include "media_loader.h"
#include "lut.h"
#include "mesh.h"
#include "renderer.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Scratch memory for one frame. 4 MiB comfortably holds several thousand
 * WidgetRuntime entries; change it here if that ever stops being true. */
#define FRAME_ARENA_BYTES (4u * 1024u * 1024u)

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

/* strdup is POSIX and we build with -std=c11 — so we roll our own. */
static char *dup_string(const char *src)
{
    if (src == NULL) {
        return NULL;
    }
    size_t len = strlen(src) + 1;
    char  *dst = (char *)malloc(len);
    if (dst != NULL) {
        memcpy(dst, src, len);
    }
    return dst;
}

char *read_file_to_string(const char *filename, size_t *out_len)
{
    if (out_len != NULL) {
        *out_len = 0;
    }

    FILE *file = fopen(filename, "rb");
    if (file == NULL) {
        fprintf(stderr, "error: could not open '%s'.\n", filename);
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "error: '%s' is not seekable.\n", filename);
        fclose(file);
        return NULL;
    }

    long length = ftell(file);
    if (length < 0) {
        fprintf(stderr, "error: could not determine the size of '%s'.\n", filename);
        fclose(file);
        return NULL;
    }
    rewind(file);

    char *buffer = (char *)malloc((size_t)length + 1);
    if (buffer == NULL) {
        fprintf(stderr, "error: allocation failed (%ld bytes).\n", length + 1);
        fclose(file);
        return NULL;
    }

    size_t read_size = fread(buffer, 1, (size_t)length, file);
    buffer[read_size] = '\0'; /* terminate at what fread *actually* read, not at length */
    fclose(file);

    if (out_len != NULL) {
        *out_len = read_size;
    }
    return buffer;
}

/*
 * "#RRGGBB" or "#RRGGBBAA" (also without the leading '#') → Color.
 * false → malformed; the caller keeps its default.
 */
bool parse_hex_color(const char *hex, Color *out)
{
    if (hex == NULL || out == NULL) {
        return false;
    }
    if (*hex == '#') {
        hex++;
    }

    size_t len = strlen(hex);
    if (len != 6 && len != 8) {
        return false;
    }

    unsigned int v[4] = { 0, 0, 0, 255 };
    for (size_t i = 0; i < len; i += 2) {
        char        pair[3] = { hex[i], hex[i + 1], '\0' };
        char       *end     = NULL;
        unsigned long byte  = strtoul(pair, &end, 16);
        if (end != pair + 2) {
            return false; /* a non-hex character */
        }
        v[i / 2] = (unsigned int)byte;
    }

    out->r = (uint8_t)v[0];
    out->g = (uint8_t)v[1];
    out->b = (uint8_t)v[2];
    out->a = (uint8_t)v[3];
    return true;
}

/* Convenience readers over cJSON, with defaults. */
static int json_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(it) ? (int)it->valuedouble : fallback;
}

static float json_float(const cJSON *obj, const char *key, float fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(it) ? (float)it->valuedouble : fallback;
}

static const char *json_str(const cJSON *obj, const char *key, const char *fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(it) && it->valuestring != NULL) ? it->valuestring : fallback;
}

static bool json_has(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key) != NULL;
}

static Color json_color(const cJSON *obj, const char *key, Color fallback)
{
    Color c = fallback;
    parse_hex_color(json_str(obj, key, NULL), &c); /* on failure c is untouched */
    return c;
}

/*
 * Grows a dynamic array and returns one empty (zeroed) slot.
 *
 * `items` is void** so a single implementation serves every array.
 * NULL → realloc failed; the old array is left intact (nothing is lost).
 */
static void *array_push(void **items, size_t *count, size_t *cap, size_t elem_size)
{
    if (*count == *cap) {
        size_t new_cap = (*cap == 0) ? 8 : (*cap * 2);
        if (new_cap > SIZE_MAX / elem_size) {
            return NULL; /* size overflow */
        }
        void *grown = realloc(*items, new_cap * elem_size);
        if (grown == NULL) {
            return NULL;
        }
        *items = grown;
        *cap   = new_cap;
    }

    void *slot = (uint8_t *)(*items) + (*count) * elem_size;
    memset(slot, 0, elem_size);
    (*count)++;
    return slot;
}


/* ------------------------------------------------------------------------- */
/* Variables: ${name} → value                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Substitution happens on the *parsed tree*, never on the raw text.
 *
 * Substituting into raw JSON is tempting but dangerous: a quote or backslash
 * inside a value would break the whole document. On the tree, a value always
 * stays inside one string node and can never affect the syntax.
 */
static char *expand_vars(const char *src, const cJSON *vars)
{
    if (src == NULL || vars == NULL || strstr(src, "${") == NULL) {
        return NULL; /* nothing to substitute */
    }

    size_t cap = strlen(src) + 64, len = 0;
    char  *out = (char *)malloc(cap);
    if (out == NULL) {
        return NULL;
    }

    for (const char *p = src; *p != '\0'; ) {
        if (p[0] == '$' && p[1] == '{') {
            const char *close = strchr(p + 2, '}');
            if (close != NULL) {
                size_t name_len = (size_t)(close - (p + 2));
                char   name[128];

                if (name_len < sizeof name) {
                    memcpy(name, p + 2, name_len);
                    name[name_len] = '\0';

                    const cJSON *v = cJSON_GetObjectItemCaseSensitive(vars, name);
                    const char  *rep = NULL;
                    char         numbuf[64];

                    if (cJSON_IsString(v)) {
                        rep = v->valuestring;
                    } else if (cJSON_IsNumber(v)) {
                        snprintf(numbuf, sizeof numbuf, "%g", v->valuedouble);
                        rep = numbuf;
                    }

                    if (rep == NULL) {
                        fprintf(stderr, "warning: variable '${%s}' is not defined.\n", name);
                        rep = "";
                    }

                    size_t rlen = strlen(rep);
                    while (len + rlen + 1 > cap) {
                        cap *= 2;
                        char *grown = (char *)realloc(out, cap);
                        if (grown == NULL) {
                            free(out);
                            return NULL;
                        }
                        out = grown;
                    }
                    memcpy(out + len, rep, rlen);
                    len += rlen;
                    p = close + 1;
                    continue;
                }
            }
        }

        if (len + 2 > cap) {
            cap *= 2;
            char *grown = (char *)realloc(out, cap);
            if (grown == NULL) {
                free(out);
                return NULL;
            }
            out = grown;
        }
        out[len++] = *p++;
    }

    out[len] = '\0';
    return out;
}

/* Walks the tree recursively, expanding variables in every string value. */
static void substitute_vars(cJSON *node, const cJSON *vars)
{
    for (cJSON *it = node; it != NULL; it = it->next) {
        if (cJSON_IsString(it) && it->valuestring != NULL) {
            char *expanded = expand_vars(it->valuestring, vars);
            if (expanded != NULL) {
                cJSON_SetValuestring(it, expanded);
                free(expanded);
            }
        } else if (cJSON_IsObject(it) || cJSON_IsArray(it)) {
            substitute_vars(it->child, vars);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Styles: describing repeated properties in one place                        */
/* ------------------------------------------------------------------------- */

/*
 * Merges a named style's fields into an object.
 *
 * The object's *own* fields always win — a style only fills what is missing.
 * `"style"` may be one name or an array of names (applied in order, the first
 * being the most general).
 */
static void apply_style_chain(cJSON *obj, const cJSON *styles, const char *name, int depth)
{
    if (depth > 8) {
        fprintf(stderr, "warning: style cycle at '%s' — stopping.\n", name);
        return;
    }

    const cJSON *st = cJSON_GetObjectItemCaseSensitive(styles, name);
    if (!cJSON_IsObject(st)) {
        fprintf(stderr, "warning: style '%s' not found.\n", name);
        return;
    }

    for (const cJSON *field = st->child; field != NULL; field = field->next) {
        if (field->string == NULL || strcmp(field->string, "style") == 0) {
            continue; /* "style" is metadata, not a property */
        }
        if (cJSON_GetObjectItemCaseSensitive(obj, field->string) != NULL) {
            continue; /* the object has its own — leave it alone */
        }
        cJSON *copy = cJSON_Duplicate(field, true);
        if (copy != NULL) {
            cJSON_AddItemToObject(obj, field->string, copy);
        }
    }

    /*
     * A style may itself have a parent: {"hero": {"style": "base", ...}}.
     * The parent is applied *afterwards*, so the more specific style wins.
     */
    const cJSON *parent = cJSON_GetObjectItemCaseSensitive(st, "style");
    if (cJSON_IsString(parent)) {
        apply_style_chain(obj, styles, parent->valuestring, depth + 1);
    } else if (cJSON_IsArray(parent)) {
        const cJSON *n = NULL;
        cJSON_ArrayForEach(n, parent) {
            if (cJSON_IsString(n)) {
                apply_style_chain(obj, styles, n->valuestring, depth + 1);
            }
        }
    }
}

static void apply_styles(cJSON *obj, const cJSON *styles)
{
    if (styles == NULL) {
        return;
    }
    const cJSON *ref = cJSON_GetObjectItemCaseSensitive(obj, "style");

    if (cJSON_IsString(ref)) {
        apply_style_chain(obj, styles, ref->valuestring, 0);
    } else if (cJSON_IsArray(ref)) {
        const cJSON *n = NULL;
        cJSON_ArrayForEach(n, ref) {
            if (cJSON_IsString(n)) {
                apply_style_chain(obj, styles, n->valuestring, 0);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Parsing objects                                                            */
/* ------------------------------------------------------------------------- */


/* ------------------------------------------------------------------------- */
/* Parsing keyframe tracks                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Keys must be in ascending time order — track_sample relies on it. The JSON
 * author may get that wrong, so we sort here (insertion sort: there are only a
 * handful of keys and they are almost always sorted already).
 *
 * Called twice for a track that mixes absolute and relative times: once during
 * parsing, and again after resolution, because "50%" and 2.0 cannot be ordered
 * against each other until the percentage has become seconds.
 */
static void sort_keys(Keyframe *keys, int count)
{
    for (int i = 1; i < count; i++) {
        Keyframe cur = keys[i];
        int      j   = i - 1;
        while (j >= 0 && keys[j].t > cur.t) {
            keys[j + 1] = keys[j];
            j--;
        }
        keys[j + 1] = cur;
    }
}

/*
 * A keyframe's time, in seconds or as a fraction of the scene.
 *
 *     {"t": 1.2}     → 1.2 seconds
 *     {"t": "50%"}   → halfway through the owning scene
 *
 * The percentage form exists because absolute times make a scene's duration
 * impossible to change: retiming a six-second clip to eight meant editing every
 * keyframe by hand. `*out_relative` tells the caller the value still needs
 * resolving — see resolve_relative_times().
 */
static float read_key_time(const cJSON *k, bool *out_relative)
{
    *out_relative = false;

    const cJSON *it = cJSON_GetObjectItemCaseSensitive(k, "t");
    if (cJSON_IsNumber(it)) {
        return (float)it->valuedouble;
    }
    if (cJSON_IsString(it) && it->valuestring != NULL) {
        char  *end = NULL;
        double v   = strtod(it->valuestring, &end);

        if (end != it->valuestring) {
            while (*end == ' ') {
                end++;
            }
            if (*end == '%') {
                *out_relative = true;
                return (float)(v / 100.0);
            }
            /* A plain numeric string ("1.2") — accept it as seconds. */
            if (*end == '\0') {
                return (float)v;
            }
        }
        fprintf(stderr, "warning: keyframe time '%s' not understood — using 0.\n",
                it->valuestring);
    }
    return 0.0f;
}

/* ------------------------------------------------------------------------- */
/* Custom easing curves                                                       */
/* ------------------------------------------------------------------------- */

/*
 * A project-level `"eases"` block:
 *
 *   "eases": {
 *     "snappy":  [0.4, 0.0, 0.2, 1.0],                  a CSS cubic-bezier
 *     "springy": {"type": "spring", "bounces": 3, "damping": 0.45}
 *   }
 *
 * Names defined here may be used anywhere `ease` is accepted.
 */
#define VR_MAX_CUSTOM_EASES 32
#define VR_EASE_SAMPLES     24   /* sub-keys per eased segment, see below */

typedef struct {
    char  name[48];
    int   kind;      /* 1 = cubic-bezier, 2 = spring */
    float p[4];      /* bezier: x1,y1,x2,y2 | spring: bounces, damping */
} CustomEase;

typedef struct {
    CustomEase items[VR_MAX_CUSTOM_EASES];
    int        count;
} EaseTable;

static float vr_clampf01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* y at x for a CSS-style cubic-bezier through (0,0), (x1,y1), (x2,y2), (1,1). */
static float bezier_solve(float x1, float y1, float x2, float y2, float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;

    /* The curve is only monotonic in x if the control points behave, so a
     * bisection is used rather than Newton: slower, but it cannot diverge on
     * the odd parameters a hand-written JSON file will eventually contain. */
    float lo = 0.0f, hi = 1.0f, t = x;
    for (int i = 0; i < 32; i++) {
        float mt = 1.0f - t;
        float bx = 3.0f * mt * mt * t * x1 + 3.0f * mt * t * t * x2 + t * t * t;
        if (bx < x) {
            lo = t;
        } else {
            hi = t;
        }
        t = 0.5f * (lo + hi);
    }
    float mt = 1.0f - t;
    return 3.0f * mt * mt * t * y1 + 3.0f * mt * t * t * y2 + t * t * t;
}

/*
 * A damped oscillation settling on 1.
 *
 * `bounces` sets the frequency, `damping` 0..1 how fast it settles — 0 is
 * loose and bouncy, 1 nearly critically damped. The decay is exponential in
 * `1 + 8·damping` rather than `1/damping`, because the latter made the default
 * overshoot by nearly half its travel: a "spring" that shot an object clean off
 * the canvas before coming back.
 */
static float spring_solve(float bounces, float damping, float p)
{
    if (p <= 0.0f) return 0.0f;
    if (p >= 1.0f) return 1.0f;

    float freq = (bounces > 0.0f ? bounces : 3.0f) * 3.14159265f;
    float d    = vr_clampf01(damping <= 0.0f ? 0.45f : damping);

    return 1.0f - expf(-p * (1.0f + 8.0f * d)) * cosf(freq * p);
}

static float custom_ease_apply(const CustomEase *ce, float p)
{
    if (ce->kind == 2) {
        return spring_solve(ce->p[0], ce->p[1], p);
    }
    return bezier_solve(ce->p[0], ce->p[1], ce->p[2], ce->p[3], p);
}

static const CustomEase *ease_lookup(const EaseTable *tab, const char *name)
{
    if (tab == NULL || name == NULL) {
        return NULL;
    }
    for (int i = 0; i < tab->count; i++) {
        if (strcmp(tab->items[i].name, name) == 0) {
            return &tab->items[i];
        }
    }
    return NULL;
}

static void parse_ease_table(EaseTable *tab, const cJSON *node)
{
    tab->count = 0;
    if (!cJSON_IsObject(node)) {
        return;
    }

    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, node) {
        if (tab->count >= VR_MAX_CUSTOM_EASES) {
            fprintf(stderr, "warning: more than %d custom eases — the rest ignored.\n",
                    VR_MAX_CUSTOM_EASES);
            break;
        }
        CustomEase *ce = &tab->items[tab->count];
        memset(ce, 0, sizeof *ce);
        snprintf(ce->name, sizeof ce->name, "%s", e->string ? e->string : "");

        if (cJSON_IsArray(e) && cJSON_GetArraySize(e) >= 4) {
            ce->kind = 1;
            for (int i = 0; i < 4; i++) {
                ce->p[i] = (float)cJSON_GetArrayItem(e, i)->valuedouble;
            }
        } else if (cJSON_IsObject(e)) {
            const char *type = json_str(e, "type", "bezier");
            if (strcmp(type, "spring") == 0) {
                ce->kind = 2;
                ce->p[0] = json_float(e, "bounces", 3.0f);
                ce->p[1] = json_float(e, "damping", 0.45f);
            } else {
                ce->kind = 1;
                ce->p[0] = json_float(e, "x1", 0.25f);
                ce->p[1] = json_float(e, "y1", 0.1f);
                ce->p[2] = json_float(e, "x2", 0.25f);
                ce->p[3] = json_float(e, "y2", 1.0f);
            }
        } else {
            fprintf(stderr, "warning: ease '%s' is neither a 4-number array nor an object.\n",
                    ce->name);
            continue;
        }
        tab->count++;
    }
}

/* The largest value a track reaches, or `fallback` if it has no keys. */
static float track_peak(const Track *tr, float fallback)
{
    if (tr->keys == NULL || tr->count <= 0) {
        return fallback;
    }
    float m = tr->keys[0].v;
    for (int i = 1; i < tr->count; i++) {
        if (tr->keys[i].v > m) {
            m = tr->keys[i].v;
        }
    }
    return m;
}

/*
 * A field may be either a number or an array of keyframes:
 *
 *     "opacity": 0.5
 *     "opacity": [ {"t": 0, "v": 0}, {"t": 1.2, "v": 1, "ease": "backout"} ]
 *     "opacity": [ {"t": "0%", "v": 0}, {"t": "100%", "v": 1} ]
 *
 * Returns true only when a real animation was built.
 */
/*
 * Replaces a segment whose easing is a custom curve with VR_EASE_SAMPLES
 * linearly-interpolated sub-keys.
 *
 * Doing it here means the renderer never learns that custom curves exist: no
 * per-key parameters to carry, no lookup table to reach from a sampler that has
 * no context, and no global. The same trick a line's `trim` uses — resolve the
 * awkward thing at parse time and let the existing machinery do the rest.
 *
 * Twenty-four samples is well past what a 60 fps timeline can show: a one-second
 * segment gets a sub-key every 40 ms, and the error between two of them is far
 * below a pixel for any curve that is not deliberately pathological.
 */
static int expand_custom_ease(Keyframe *out, const Keyframe *a, const Keyframe *b,
                              const CustomEase *ce)
{
    int n = 0;
    for (int i = 1; i <= VR_EASE_SAMPLES; i++) {
        float u = (float)i / (float)VR_EASE_SAMPLES;
        float e = custom_ease_apply(ce, u);

        out[n].t          = a->t + (b->t - a->t) * u;
        out[n].v          = a->v + (b->v - a->v) * e;
        out[n].ease       = EASE_LINEAR;
        out[n].t_relative = false;
        n++;
    }
    return n;
}

static bool parse_track_ex(const cJSON *item, Track *tr, float fallback,
                           const EaseTable *eases);

static bool parse_track(const cJSON *item, Track *tr, float fallback)
{
    return parse_track_ex(item, tr, fallback, NULL);
}

static bool parse_track_ex(const cJSON *item, Track *tr, float fallback,
                           const EaseTable *eases)
{
    track_set_constant(tr, fallback);

    if (item == NULL) {
        return false;
    }
    if (cJSON_IsNumber(item)) {
        track_set_constant(tr, (float)item->valuedouble);
        return false; /* a constant — not an animation */
    }
    if (!cJSON_IsArray(item)) {
        return false;
    }

    int n = cJSON_GetArraySize(item);
    if (n <= 0) {
        return false;
    }

    Keyframe *keys = (Keyframe *)calloc((size_t)n, sizeof(Keyframe));
    if (keys == NULL) {
        return false;
    }

    /* Which keys named a custom curve — parallel to `keys`, dropped below. */
    const CustomEase **custom = (const CustomEase **)calloc((size_t)n, sizeof(void *));
    if (custom == NULL) {
        free(keys);
        return false;
    }

    int          idx = 0;
    const cJSON *k   = NULL;
    cJSON_ArrayForEach(k, item) {
        if (!cJSON_IsObject(k)) {
            continue;
        }
        keys[idx].t    = read_key_time(k, &keys[idx].t_relative);
        keys[idx].v    = json_float(k, "v", 0.0f);

        const char *ename = json_str(k, "ease", NULL);
        const CustomEase *ce = ease_lookup(eases, ename);
        keys[idx].ease = (ce != NULL) ? EASE_LINEAR : easing_from_name(ename);
        custom[idx]    = ce;
        idx++;
    }

    if (idx == 0) {
        free(keys);
        free(custom);
        return false;
    }

    sort_keys(keys, idx);

    /*
     * Expand any custom-eased segment. Done after sorting, since a segment is
     * only defined once its neighbours are in order.
     */
    bool any_custom = false;
    for (int i = 0; i < idx; i++) {
        if (custom[i] != NULL) {
            any_custom = true;
            break;
        }
    }

    if (any_custom) {
        int       cap = idx * (VR_EASE_SAMPLES + 1) + 1;
        Keyframe *ex  = (Keyframe *)calloc((size_t)cap, sizeof(Keyframe));
        if (ex == NULL) {
            free(keys);
            free(custom);
            return false;
        }

        int m = 0;
        ex[m++] = keys[0];
        for (int i = 1; i < idx; i++) {
            /* `custom` is indexed by the key's ORIGINAL slot; after sorting the
             * association would be wrong, so it is looked up by identity. */
            const CustomEase *ce = NULL;
            for (int j = 0; j < idx; j++) {
                if (custom[j] != NULL && keys[i].t == keys[j].t && keys[i].v == keys[j].v) {
                    ce = custom[j];
                    break;
                }
            }
            if (ce != NULL && keys[i].t > keys[i - 1].t) {
                m += expand_custom_ease(&ex[m], &keys[i - 1], &keys[i], ce);
            } else {
                ex[m++] = keys[i];
            }
        }

        free(keys);
        keys = ex;
        idx  = m;
    }

    free(custom);

    tr->keys  = keys;
    tr->count = idx;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Parsing effects                                                            */
/* ------------------------------------------------------------------------- */

static const struct {
    const char *name;
    EffectParam slot;
} kEffectParams[] = {
    { "amount",      FXP_AMOUNT      }, { "radius",     FXP_RADIUS     },
    { "softness",    FXP_SOFTNESS    }, { "angle",      FXP_ANGLE      },
    { "count",       FXP_COUNT       }, { "size",       FXP_SIZE       },
    { "level",       FXP_LEVEL       }, { "levels",     FXP_LEVELS     },
    { "balance",     FXP_BALANCE     }, { "exposure",   FXP_EXPOSURE   },
    { "brightness",  FXP_BRIGHTNESS  }, { "contrast",   FXP_CONTRAST   },
    { "gamma",       FXP_GAMMA       }, { "saturation", FXP_SATURATION },
    { "vibrance",    FXP_VIBRANCE    }, { "hue",        FXP_HUE        },
    { "temperature", FXP_TEMPERATURE }, { "tint",       FXP_TINT       },
};

/* Neutral defaults — whatever the JSON does not override leaves the frame alone. */
/* Defined below, next to the other path helpers; the effect parser needs it
 * here to resolve a LUT's path against the JSON's directory. */
static char *resolve_relative_path(const char *base_file, const char *path);

static void effect_set_defaults(Effect *fx)
{
    for (int i = 0; i < FXP_MAX; i++) {
        track_set_constant(&fx->param[i], 0.0f);
    }

    track_set_constant(&fx->param[FXP_AMOUNT],     1.0f);
    track_set_constant(&fx->param[FXP_CONTRAST],   1.0f);
    track_set_constant(&fx->param[FXP_GAMMA],      1.0f);
    track_set_constant(&fx->param[FXP_SATURATION], 1.0f);
    track_set_constant(&fx->param[FXP_LEVEL],      0.5f);
    track_set_constant(&fx->param[FXP_LEVELS],     6.0f);

    /* Lift is an offset (neutral 0); gamma and gain are multipliers (neutral
     * 1). Getting these wrong makes an unmentioned channel crush to black. */
    for (int k = 0; k < 3; k++) {
        track_set_constant(&fx->param[FXP_GAMMA_R + k], 1.0f);
        track_set_constant(&fx->param[FXP_GAIN_R  + k], 1.0f);
    }

    /* A qualifier that selects everything, so the slots are safe to read even
     * when the JSON never mentioned one. */
    track_set_constant(&fx->param[FXP_Q_HUE0],  0.0f);
    track_set_constant(&fx->param[FXP_Q_HUE1],  360.0f);
    track_set_constant(&fx->param[FXP_Q_SAT0],  0.0f);
    track_set_constant(&fx->param[FXP_Q_SAT1],  1.0f);
    track_set_constant(&fx->param[FXP_Q_LUMA0], 0.0f);
    track_set_constant(&fx->param[FXP_Q_LUMA1], 1.0f);
    track_set_constant(&fx->param[FXP_Q_SOFT],  0.06f);

    switch (fx->type) {
        case FX_VIGNETTE:
            track_set_constant(&fx->param[FXP_AMOUNT],   0.35f);
            track_set_constant(&fx->param[FXP_RADIUS],   0.60f);
            track_set_constant(&fx->param[FXP_SOFTNESS], 0.55f);
            fx->color_a = (Color){ 0, 0, 0, 255 };
            break;
        case FX_BLUR:
            track_set_constant(&fx->param[FXP_RADIUS], 8.0f);
            break;
        case FX_PIXELATE:
            track_set_constant(&fx->param[FXP_SIZE], 8.0f);
            break;
        case FX_BLOOM:
            /* A threshold near the top of the range: bloom is the light that
             * spills off things that are *bright*, and a low threshold blooms
             * the whole picture into haze. */
            track_set_constant(&fx->param[FXP_LEVEL],  0.75f);
            track_set_constant(&fx->param[FXP_RADIUS], 18.0f);
            track_set_constant(&fx->param[FXP_AMOUNT], 0.7f);
            break;
        case FX_LGG:
            track_set_constant(&fx->param[FXP_AMOUNT], 1.0f);
            break;
        case FX_SCANLINES:
            track_set_constant(&fx->param[FXP_AMOUNT], 0.35f);
            track_set_constant(&fx->param[FXP_COUNT],  240.0f);
            break;
        case FX_GRAIN:
            track_set_constant(&fx->param[FXP_AMOUNT], 0.04f);
            break;
        case FX_RGB_SPLIT:
            track_set_constant(&fx->param[FXP_AMOUNT], 3.0f);
            break;
        case FX_GLITCH:
            track_set_constant(&fx->param[FXP_AMOUNT], 0.05f);
            break;
        case FX_SPLIT_TONE:
            fx->color_a = (Color){  40,  70, 120, 255 }; /* cool shadows    */
            fx->color_b = (Color){ 255, 190, 130, 255 }; /* warm highlights */
            track_set_constant(&fx->param[FXP_AMOUNT], 0.25f);
            break;
        case FX_GRADIENT_MAP:
            fx->color_a = (Color){   0,   0,   0, 255 };
            fx->color_b = (Color){ 255, 255, 255, 255 };
            break;
        case FX_VIBRANCE:
            track_set_constant(&fx->param[FXP_AMOUNT], 0.3f);
            break;
        default:
            break;
    }
}

static bool parse_effects_into(struct Effect **list, size_t *count, size_t *cap,
                               const cJSON *arr, const char *base_file)
{
    if (!cJSON_IsArray(arr)) {
        return true; /* effects are optional */
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        const char *name = json_str(item, "type", NULL);
        EffectType  type = effect_from_name(name);

        if (type == FX_NONE) {
            fprintf(stderr, "warning: unknown effect '%s' — skipped.\n",
                    name ? name : "(null)");
            continue;
        }

        Effect *fx = (Effect *)array_push((void **)list, count, cap, sizeof(Effect));
        if (fx == NULL) {
            return false;
        }

        fx->type = type;
        effect_set_defaults(fx);

        for (size_t i = 0; i < sizeof kEffectParams / sizeof kEffectParams[0]; i++) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(item, kEffectParams[i].name);
            if (v != NULL) {
                Track *slot = &fx->param[kEffectParams[i].slot];
                parse_track(v, slot, slot->constant);
            }
        }

        /* Colour synonyms: vignette uses "color", toning "shadows"/"highlights". */
        fx->color_a = json_color(item, "color",     fx->color_a);
        fx->color_a = json_color(item, "shadows",   fx->color_a);
        fx->color_a = json_color(item, "shadow",    fx->color_a);
        fx->color_b = json_color(item, "highlights", fx->color_b);
        fx->color_b = json_color(item, "highlight",  fx->color_b);

        /*
         * A LUT's table, read here rather than at media-load time.
         *
         * Effects are not widgets and never pass through the media loader, and
         * a look that cannot be found should stop the parse the way a missing
         * font or audio file does — a film delivered without its grade is worse
         * than one that refused to render.
         */
        /* --- lift / gamma / gain ---------------------------------------- */
        {
            static const struct { const char *key; int base; } kTriples[] = {
                { "lift", FXP_LIFT_R }, { "gamma", FXP_GAMMA_R }, { "gain", FXP_GAIN_R },
            };
            static const char *const kCh[3] = { "r", "g", "b" };

            for (size_t q = 0; q < sizeof kTriples / sizeof *kTriples; q++) {
                const cJSON *o = cJSON_GetObjectItemCaseSensitive(item, kTriples[q].key);
                if (o == NULL) {
                    continue;
                }
                /*
                 * A bare number is the master: it moves all three channels
                 * together, which is how a colourist reaches for the wheel
                 * before deciding the shot has a cast at all.
                 *
                 * `gamma` is deliberately shared with color_grade's own scalar
                 * gamma — same key, same meaning — so the triple form only
                 * applies to the three-way corrector.
                 */
                if (!cJSON_IsObject(o)) {
                    for (int c = 0; c < 3; c++) {
                        parse_track(o, &fx->param[kTriples[q].base + c],
                                    fx->param[kTriples[q].base + c].constant);
                    }
                    continue;
                }
                for (int c = 0; c < 3; c++) {
                    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, kCh[c]);
                    if (v != NULL) {
                        parse_track(v, &fx->param[kTriples[q].base + c],
                                    fx->param[kTriples[q].base + c].constant);
                    }
                }
            }
        }

        /* --- HSL qualifier ---------------------------------------------- */
        const cJSON *ql = cJSON_GetObjectItemCaseSensitive(item, "qualifier");
        if (ql == NULL) {
            ql = cJSON_GetObjectItemCaseSensitive(item, "key");
        }
        if (cJSON_IsObject(ql)) {
            fx->qual_on = true;
            /*
             * Each band is [lo, hi] as a two-element array, which is how the
             * range reads in one glance — "hue 80 to 160" rather than two keys
             * that have to be matched up by eye.
             */
            static const struct { const char *key; int lo, hi; } kBands[] = {
                { "hue",  FXP_Q_HUE0,  FXP_Q_HUE1  },
                { "sat",  FXP_Q_SAT0,  FXP_Q_SAT1  },
                { "luma", FXP_Q_LUMA0, FXP_Q_LUMA1 },
            };
            for (size_t q = 0; q < sizeof kBands / sizeof *kBands; q++) {
                const cJSON *b = cJSON_GetObjectItemCaseSensitive(ql, kBands[q].key);
                if (cJSON_IsArray(b) && cJSON_GetArraySize(b) >= 2) {
                    parse_track(cJSON_GetArrayItem(b, 0), &fx->param[kBands[q].lo],
                                fx->param[kBands[q].lo].constant);
                    parse_track(cJSON_GetArrayItem(b, 1), &fx->param[kBands[q].hi],
                                fx->param[kBands[q].hi].constant);
                }
            }
            const cJSON *sf = cJSON_GetObjectItemCaseSensitive(ql, "softness");
            if (sf != NULL) {
                parse_track(sf, &fx->param[FXP_Q_SOFT], fx->param[FXP_Q_SOFT].constant);
            }
            fx->qual_invert = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ql, "invert"));
        }

        /* --- power window --------------------------------------------- */
        const cJSON *wn = cJSON_GetObjectItemCaseSensitive(item, "window");
        if (wn == NULL) {
            wn = cJSON_GetObjectItemCaseSensitive(item, "mask");
        }
        track_set_constant(&fx->win_cx, 0.5f);
        track_set_constant(&fx->win_cy, 0.5f);
        track_set_constant(&fx->win_rx, 0.35f);
        track_set_constant(&fx->win_ry, 0.35f);
        track_set_constant(&fx->win_feather, 0.08f);
        if (cJSON_IsObject(wn)) {
            const char *shape = json_str(wn, "shape", "ellipse");
            fx->win_shape = (strcmp(shape, "rect") == 0 ||
                             strcmp(shape, "rectangle") == 0) ? 2 : 1;

            static const char *const kWin[] = { "cx", "cy", "rx", "ry", "feather" };
            Track *ws[] = { &fx->win_cx, &fx->win_cy, &fx->win_rx,
                            &fx->win_ry, &fx->win_feather };
            for (size_t q = 0; q < sizeof ws / sizeof *ws; q++) {
                const cJSON *v = cJSON_GetObjectItemCaseSensitive(wn, kWin[q]);
                if (v != NULL) {
                    parse_track(v, ws[q], ws[q]->constant);
                }
            }
            /* "r" sets both radii — a circular window is the common case and
             * writing the same number twice invites them drifting apart. */
            const cJSON *r = cJSON_GetObjectItemCaseSensitive(wn, "r");
            if (r != NULL) {
                parse_track(r, &fx->win_rx, fx->win_rx.constant);
                parse_track(r, &fx->win_ry, fx->win_ry.constant);
            }
            fx->win_invert = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(wn, "invert"));
        }

        if (type == FX_LUT) {
            const char *lp = json_str(item, "path", json_str(item, "file", NULL));
            if (lp == NULL) {
                fprintf(stderr, "error: a 'lut' effect needs a \"path\" to a "
                                ".cube file.\n");
                return false;
            }
            fx->lut_path = resolve_relative_path(base_file, lp);
            if (fx->lut_path == NULL ||
                !lut_load_cube(fx->lut_path, &fx->lut, &fx->lut_size)) {
                return false;
            }
        }
    }
    return true;
}

/* Reads the fields common to every WidgetBase. */
static void parse_widget_base(WidgetBase *base, const cJSON *obj, WidgetKind kind, int z,
                              const EaseTable *eases)
{
    base->kind    = kind;
    base->id      = dup_string(json_str(obj, "id", "unnamed"));
    base->z_order = z;

    /* Injected by repeat_expand(); absent on hand-written objects. */
    base->repeat_dx = json_float(obj, "_repeat_dx", 0.0f);
    base->repeat_dy = json_float(obj, "_repeat_dy", 0.0f);

    /* Added to base_rotation, so it composes with a line's own angle. */
    base->base_rotation = json_float(obj, "_repeat_rot", 0.0f) * (float)(M_PI / 180.0);

    base->group_name  = dup_string(json_str(obj, "group", NULL));
    base->group_index = -1;   /* resolved once the widget index exists */

    /*
     * A shadow and a glow are the same thing with different defaults: a glow is
     * centred and bright, a shadow offset and dark. Whichever key is present
     * fills the same fields.
     */
    const cJSON *sh = cJSON_GetObjectItemCaseSensitive(obj, "shadow");
    const cJSON *gl = cJSON_GetObjectItemCaseSensitive(obj, "glow");

    if (cJSON_IsObject(sh) || cJSON_IsObject(gl)) {
        const cJSON *src = cJSON_IsObject(sh) ? sh : gl;
        bool is_glow = !cJSON_IsObject(sh);

        base->shadow_on    = true;
        base->shadow_dx    = json_float(src, "dx", 0.0f);
        base->shadow_dy    = json_float(src, "dy", is_glow ? 0.0f : 8.0f);
        base->shadow_blur  = json_float(src, "blur", is_glow ? 24.0f : 16.0f);
        base->shadow_color = json_color(src, "color",
                                        is_glow ? (Color){ 255, 255, 255, 140 }
                                                : (Color){ 0, 0, 0, 150 });

        if (base->shadow_blur < 0.0f)   base->shadow_blur = 0.0f;
        if (base->shadow_blur > 128.0f) base->shadow_blur = 128.0f;
    }

    const char *bl = json_str(obj, "blend", NULL);
    if (bl != NULL) {
        if (strcmp(bl, "add") == 0 || strcmp(bl, "additive") == 0) {
            base->blend = 1;
        } else if (strcmp(bl, "screen") == 0) {
            base->blend = 2;
        } else if (strcmp(bl, "normal") != 0) {
            fprintf(stderr, "warning: unknown blend '%s' — using normal.\n", bl);
        }
    }

    /* An explicit z overrides array order, which is otherwise the draw order. */
    base->seq = (size_t)z;

    if (json_has(obj, "z")) {
        base->z_order = json_int(obj, "z", z);
    }

    /*
     * Clip mask. Fractions of the object's own box, so "clip":{"shape":"rect",
     * "w":0.5} means "the left half" at any size the object animates to.
     */
    const cJSON *mk = cJSON_GetObjectItemCaseSensitive(obj, "clip");
    if (mk == NULL) {
        mk = cJSON_GetObjectItemCaseSensitive(obj, "mask");
    }
    if (cJSON_IsObject(mk)) {
        const char *shape = json_str(mk, "shape", "rect");
        if (strcmp(shape, "circle") == 0) {
            base->mask_shape = 1;
            base->mask[0] = json_float(mk, "cx", 0.5f);
            base->mask[1] = json_float(mk, "cy", 0.5f);
            base->mask[2] = json_float(mk, "r",  0.5f);
        } else {
            base->mask_shape = 2;
            base->mask[0] = json_float(mk, "x", 0.0f);
            base->mask[1] = json_float(mk, "y", 0.0f);
            base->mask[2] = json_float(mk, "w", 1.0f);
            base->mask[3] = json_float(mk, "h", 1.0f);
        }
        base->mask_invert = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(mk, "invert"));
    }

    /* --- chroma key ------------------------------------------------------ */
    const cJSON *ky = cJSON_GetObjectItemCaseSensitive(obj, "key");
    if (ky == NULL) {
        ky = cJSON_GetObjectItemCaseSensitive(obj, "chroma_key");
    }
    if (cJSON_IsObject(ky)) {
        /* Default green rather than none: "key": {} on a green-screen clip is
         * what almost everyone means, and the studio green #00B140 is the one
         * colour a backdrop is most likely to be. */
        base->key_on        = true;
        base->key_color     = json_color(ky, "color", (Color){ 0x00, 0xB1, 0x40, 255 });
        base->key_tolerance = json_float(ky, "tolerance", 0.14f);
        base->key_softness  = json_float(ky, "softness",  0.08f);
        base->key_spill     = json_float(ky, "spill",     0.7f);

        if (base->key_tolerance < 0.0f) base->key_tolerance = 0.0f;
        if (base->key_softness  < 0.0f) base->key_softness  = 0.0f;
    } else if (cJSON_IsString(ky)) {
        /* "key": "#00B140" — the colour alone, everything else defaulted. */
        base->key_on        = true;
        base->key_color     = (Color){ 0x00, 0xB1, 0x40, 255 };
        parse_hex_color(ky->valuestring, &base->key_color);
        base->key_tolerance = 0.14f;
        base->key_softness  = 0.08f;
        base->key_spill     = 0.7f;
    }

    /* --- anchoring ("anchor": "center", "bottomright", …) ---------------- */
    const char *anchor_name = json_str(obj, "anchor", NULL);
    if (anchor_name != NULL) {
        float ax, ay;
        if (layout_anchor_from_name(anchor_name, &ax, &ay)) {
            base->anchor_x = ax;
            base->anchor_y = ay;
            base->has_anchor_x = base->has_anchor_y = true;
        } else {
            fprintf(stderr, "warning: unknown anchor '%s'.\n", anchor_name);
        }
    }

    /* --- relative expressions ("center", "bottom-160") ------------------- */
    const cJSON *x_item = cJSON_GetObjectItemCaseSensitive(obj, "x");
    const cJSON *y_item = cJSON_GetObjectItemCaseSensitive(obj, "y");

    if (cJSON_IsString(x_item)) {
        base->x_expr = dup_string(x_item->valuestring);
    }
    if (cJSON_IsString(y_item)) {
        base->y_expr = dup_string(y_item->valuestring);
    }

    /* Accept both spellings: "x" and "x_pos" (two JSON dialects). */
    if (json_has(obj, "x")) {
        base->x = json_float(obj, "x", 0.0f);
    } else if (json_has(obj, "x_pos")) {
        base->x = json_float(obj, "x_pos", 0.0f);
    } else {
        /* No X given → centre horizontally once the texture width is known
         * (see media_prepare_textures). */
        base->auto_center_x = true;
    }

    base->y = json_has(obj, "y") ? json_float(obj, "y", 0.0f)
                                 : json_float(obj, "y_pos", 0.0f);

    base->tex.premultiplied = true;

    /*
     * Property tracks. If a field is an array it becomes an animation and
     * replaces the static base value; a number is simply a constant.
     */
    parse_track_ex(cJSON_GetObjectItemCaseSensitive(obj, "x"),        &base->tr_x, base->x, eases);
    parse_track_ex(cJSON_GetObjectItemCaseSensitive(obj, "y"),        &base->tr_y, base->y, eases);
    parse_track_ex(cJSON_GetObjectItemCaseSensitive(obj, "opacity"),  &base->tr_opacity, 1.0f, eases);
    parse_track_ex(cJSON_GetObjectItemCaseSensitive(obj, "scale"),    &base->tr_scale, 1.0f, eases);
    parse_track_ex(cJSON_GetObjectItemCaseSensitive(obj, "rotation"), &base->tr_rotation, 0.0f, eases);

    /*
     * Size and tint tracks.
     *
     * "w"/"h" are read here only as *animations*: a plain number is the
     * object's own width/height and is handled by each type's parser (a
     * rectangle's `w` sets its texture size). Only an array means "animate the
     * destination size", so a static `"w": 200` keeps its existing meaning.
     */
    base->has_track_w = parse_track_ex(cJSON_GetObjectItemCaseSensitive(obj, "w"), &base->tr_w, 0.0f, eases);
    base->has_track_h = parse_track_ex(cJSON_GetObjectItemCaseSensitive(obj, "h"), &base->tr_h, 0.0f, eases);

    /*
     * 2.5D. `z` is depth (positive away from the viewer); rotate_x / rotate_y
     * turn the layer out of the screen plane. All three default to zero, which
     * leaves compositing on its original affine path.
     */
    base->has_track_z  = parse_track_ex(cJSON_GetObjectItemCaseSensitive(obj, "z_depth"),
                                        &base->tr_z, 0.0f, eases) ||
                         json_has(obj, "z_depth");
    base->has_track_rx = json_has(obj, "rotate_x");
    base->has_track_ry = json_has(obj, "rotate_y");
    /* z_depth is parsed once, just above. Parsing it a second time into the
     * same Track overwrote the first keyframe array without freeing it, which
     * leaked every depth track in the project — invisible until a scene used
     * enough of them, and then only under LeakSanitizer. */
    parse_track_ex(cJSON_GetObjectItemCaseSensitive(obj, "rotate_x"), &base->tr_rx, 0.0f, eases);
    parse_track_ex(cJSON_GetObjectItemCaseSensitive(obj, "rotate_y"), &base->tr_ry, 0.0f, eases);

    base->shading = json_float(obj, "shading", 0.0f);
    if (base->shading < 0.0f) base->shading = 0.0f;
    if (base->shading > 1.0f) base->shading = 1.0f;

    const char *bf = json_str(obj, "backface", NULL);
    if (bf != NULL) {
        if (strcmp(bf, "hide") == 0)      base->backface = 1;
        else if (strcmp(bf, "dim") == 0)  base->backface = 2;
        else if (strcmp(bf, "show") != 0) {
            fprintf(stderr, "warning: unknown backface '%s' — using show.\n", bf);
        }
    }

    base->has_track_trim = json_has(obj, "trim");
    parse_track_ex(cJSON_GetObjectItemCaseSensitive(obj, "trim"), &base->tr_trim, 1.0f, eases);

    base->tint_color     = json_color(obj, "tint", (Color){ 255, 255, 255, 255 });
    base->has_track_tint = json_has(obj, "tint_amount");
    parse_track_ex(cJSON_GetObjectItemCaseSensitive(obj, "tint_amount"), &base->tr_tint, 0.0f, eases);

    /*
     * "has_track_*" means "the JSON specified this property", not "it is
     * animated".
     *
     * The distinction matters: `"opacity": 0.0` is a constant but is still
     * *specified*. Relying on parse_track()'s return value (true only for
     * arrays) would silently discard it and draw the object at the default 1.0 —
     * so an attempt to hide something would instead make it fully visible.
     */
    /*
     * A string x/y is an expression, not a track — its value is computed later,
     * once the size is known, and written straight into base->x/y. Setting
     * has_track_x here would make the renderer read the track instead (whose
     * constant is 0 at parse time), ignoring the computed position.
     */
    /*
     * A leading '=' marks a binding rather than a layout expression. It is kept
     * as text until every object has a size — the same reason x_expr is.
     */
    if (base->x_expr != NULL && base->x_expr[0] == '=') {
        base->x_bind = base->x_expr;
        base->x_expr = NULL;
    }
    if (base->y_expr != NULL && base->y_expr[0] == '=') {
        base->y_bind = base->y_expr;
        base->y_expr = NULL;
    }

    base->has_track_x        = json_has(obj, "x") && base->x_expr == NULL
                                                  && base->x_bind == NULL;
    base->has_track_y        = json_has(obj, "y") && base->y_expr == NULL
                                                  && base->y_bind == NULL;
    base->has_track_opacity  = json_has(obj, "opacity");
    base->has_track_scale    = json_has(obj, "scale");
    base->has_track_rotation = json_has(obj, "rotation");

    /* An animated X defines the position itself — no auto-centring needed. */
    if (base->has_track_x) {
        base->auto_center_x = false;
    }
}

/*
 * Reads a length: a number is pixels, a string is an expression ("80%").
 * That lets max_width be expressed relative to the canvas, so it adapts when
 * the resolution changes.
 */
static float json_length(const cJSON *obj, const char *key, float canvas, float fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) {
        return (float)it->valuedouble;
    }
    if (cJSON_IsString(it)) {
        float pos, anchor;
        if (layout_eval(it->valuestring, canvas, LAYOUT_AXIS_X, &pos, &anchor)) {
            return pos;
        }
        fprintf(stderr, "warning: unparsable '%s' = '%s'.\n", key, it->valuestring);
    }
    return fallback;
}

static float json_align(const cJSON *obj, const char *key, float fallback)
{
    const char *a = json_str(obj, key, NULL);
    if (a == NULL) {
        return fallback;
    }
    if (strcmp(a, "center") == 0 || strcmp(a, "middle") == 0) return 0.5f;
    if (strcmp(a, "right")  == 0)                             return 1.0f;
    if (strcmp(a, "left")   == 0)                             return 0.0f;
    fprintf(stderr, "warning: unknown align '%s'.\n", a);
    return fallback;
}

static bool parse_text_object(EditorContext *ctx, const cJSON *obj, int z, const EaseTable *eases)
{
    TextWidget *w = (TextWidget *)array_push((void **)&ctx->texts, &ctx->text_count,
                                             &ctx->text_cap, sizeof(TextWidget));
    if (w == NULL) {
        return false;
    }

    parse_widget_base(&w->base, obj, WIDGET_TEXT, z, eases);

    w->content      = dup_string(json_str(obj, "content", ""));
    w->font         = dup_string(json_str(obj, "font", "Sans"));
    w->size         = json_int(obj, "size", 48);
    w->color        = json_color(obj, "color", (Color){ 255, 255, 255, 255 });
    w->line_spacing = json_float(obj, "line_spacing", 1.25f);
    w->max_width    = json_length(obj, "max_width", (float)ctx->config.width, 0.0f);
    w->align        = json_align(obj, "align", 0.0f);

    if (w->size < 1) {
        w->size = 1; /* a zero or negative size is meaningless to Cairo */
    }
    if (w->max_width < 0.0f) {
        w->max_width = 0.0f;
    }
    return w->content != NULL && w->font != NULL;
}

/*
 * A relative path resolves against the JSON's own directory.
 *
 * That way `"path": "logo.png"` depends on where the project file lives, not
 * on where the user happened to run the program from.
 */
static char *resolve_relative_path(const char *base_file, const char *path)
{
    if (path == NULL) {
        return NULL;
    }
    if (path[0] == '/' || base_file == NULL) {
        return dup_string(path); /* absolute — leave it alone */
    }

    const char *slash = strrchr(base_file, '/');
    if (slash == NULL) {
        return dup_string(path); /* the JSON is in the current directory */
    }

    size_t dir_len = (size_t)(slash - base_file) + 1; /* including the '/' */
    size_t path_len = strlen(path);

    char *full = (char *)malloc(dir_len + path_len + 1);
    if (full == NULL) {
        return NULL;
    }
    memcpy(full, base_file, dir_len);
    memcpy(full + dir_len, path, path_len + 1);
    return full;
}

static bool parse_shape_object(EditorContext *ctx, const cJSON *obj, int z, WidgetKind kind, const EaseTable *eases)
{
    ShapeWidget *w = (ShapeWidget *)array_push((void **)&ctx->shapes, &ctx->shape_count,
                                               &ctx->shape_cap, sizeof(ShapeWidget));
    if (w == NULL) {
        return false;
    }

    parse_widget_base(&w->base, obj, kind, z, eases);

    w->color = json_color(obj, "color", (Color){ 255, 255, 255, 255 });

    if (kind == WIDGET_CIRCLE) {
        /* A circle is written with a radius but stored as a bounding box. */
        float r = json_float(obj, "radius", 50.0f);
        w->w = json_float(obj, "w", r * 2.0f);
        w->h = json_float(obj, "h", r * 2.0f);

        /* cx/cy = centre (videogen's convention) → our top-left corner. */
        if (json_has(obj, "cx")) {
            w->base.x = json_float(obj, "cx", 0.0f) - w->w * 0.5f;
            w->base.auto_center_x = false;
        }
        if (json_has(obj, "cy")) {
            w->base.y = json_float(obj, "cy", 0.0f) - w->h * 0.5f;
        }
    } else {
        w->w = json_float(obj, "w", json_float(obj, "width",  100.0f));
        w->h = json_float(obj, "h", json_float(obj, "height", 100.0f));
    }

    /*
     * An animated w/h leaves the texture without a size, since the JSON field
     * is an array rather than a number. Rasterize at the animation's *peak*:
     * the texture is then downscaled at smaller values, never upscaled, so the
     * shape stays sharp at its largest.
     *
     * (A rounded rectangle's corners do squash slightly at other sizes — the
     * price of not re-rasterizing every frame.)
     */
    if (w->base.has_track_w) {
        w->w = track_peak(&w->base.tr_w, w->w);
    }
    if (w->base.has_track_h) {
        w->h = track_peak(&w->base.tr_h, w->h);
    }

    /* A gradient fill replaces the flat colour when present. */
    const cJSON *gr = cJSON_GetObjectItemCaseSensitive(obj, "gradient");
    if (cJSON_IsObject(gr)) {
        const char *gk = json_str(gr, "kind", json_str(gr, "type", "linear"));
        w->grad_kind  = (strcmp(gk, "radial") == 0) ? 2 : 1;
        w->grad_from  = json_color(gr, "from", w->color);
        w->grad_to    = json_color(gr, "to", (Color){ 0, 0, 0, 255 });
        w->grad_angle = json_float(gr, "angle", 90.0f);
    }

    w->corner_radius = json_int(obj, "corner_radius", 0);

    /*
     * An outline. `stroke` alone implies a 2 px line, because a colour with no
     * width would silently draw nothing — the same trap the transparent-fill
     * workaround fell into.
     */
    w->stroke_color = json_color(obj, "stroke", (Color){ 255, 255, 255, 0 });
    w->stroke_width = json_float(obj, "stroke_width",
                                 (w->stroke_color.a > 0) ? 2.0f : 0.0f);

    /* "fill": "none" → outline only. */
    const char *fill = json_str(obj, "fill", NULL);
    w->filled = !(fill != NULL && strcmp(fill, "none") == 0);

    if (w->w < 1.0f) w->w = 1.0f;
    if (w->h < 1.0f) w->h = 1.0f;
    if (w->corner_radius < 0) w->corner_radius = 0;
    return true;
}

/*
 * A straight segment.
 *
 * The texture is drawn horizontally, `width` longer than the segment so round
 * and square caps have room, and the real angle becomes base_rotation. The
 * object's centre is placed at the segment's midpoint, which is the point the
 * compositor rotates about — so the drawn line lands exactly on (x1,y1)-(x2,y2).
 */
static bool parse_line_object(EditorContext *ctx, const cJSON *obj, int z, const EaseTable *eases)
{
    LineWidget *w = (LineWidget *)array_push((void **)&ctx->lines, &ctx->line_count,
                                             &ctx->line_cap, sizeof(LineWidget));
    if (w == NULL) {
        return false;
    }

    parse_widget_base(&w->base, obj, WIDGET_LINE, z, eases);

    w->x1 = json_float(obj, "x1", 0.0f);
    w->y1 = json_float(obj, "y1", 0.0f);
    w->x2 = json_float(obj, "x2", 100.0f);
    w->y2 = json_float(obj, "y2", 0.0f);

    w->width = json_float(obj, "width", json_float(obj, "stroke_width", 2.0f));
    if (w->width < 0.1f) {
        w->width = 0.1f;
    }
    w->color = json_color(obj, "color", json_color(obj, "stroke",
                                                   (Color){ 255, 255, 255, 255 }));

    const char *cap = json_str(obj, "cap", "butt");
    w->cap = (strcmp(cap, "round") == 0) ? 1 : (strcmp(cap, "square") == 0) ? 2 : 0;

    float dx  = w->x2 - w->x1;
    float dy  = w->y2 - w->y1;
    float len = sqrtf(dx * dx + dy * dy);

    w->base.base_rotation += atan2f(dy, dx);   /* += : a repeat may have set one */

    /*
     * A line's position is its endpoints, not an x/y — so the usual position
     * handling is overridden here. The midpoint is what the rotation pivots
     * about, and the texture is centred on it.
     */
    float tex_w = len + w->width;    /* caps need half a width at each end */
    float tex_h = w->width;

    w->base.x = (w->x1 + w->x2) * 0.5f - tex_w * 0.5f;
    w->base.y = (w->y1 + w->y2) * 0.5f - tex_h * 0.5f;
    w->base.auto_center_x = false;
    w->base.has_track_x   = false;
    w->base.has_track_y   = false;
    free(w->base.x_expr); w->base.x_expr = NULL;
    free(w->base.y_expr); w->base.y_expr = NULL;

    return true;
}

/* ------------------------------------------------------------------------- */
/* Path parsing (SVG `d`, or a list of points)                                */
/* ------------------------------------------------------------------------- */

static bool path_push(PathWidget *w, uint8_t op, const float *c, int n)
{
    PathSeg *seg = (PathSeg *)array_push((void **)&w->segs, &w->seg_count,
                                         &w->seg_cap, sizeof(PathSeg));
    if (seg == NULL) {
        return false;
    }
    memset(seg, 0, sizeof *seg);
    seg->op = op;
    for (int i = 0; i < n; i++) {
        seg->c[i] = c[i];
    }
    return true;
}

/* Skips whitespace and the commas SVG allows between any two numbers. */
static void d_skip(const char **p)
{
    while (**p == ' ' || **p == ',' || **p == '\t' || **p == '\n' || **p == '\r') {
        (*p)++;
    }
}

static bool d_number(const char **p, float *out)
{
    d_skip(p);
    char *end = NULL;
    double v = strtod(*p, &end);
    if (end == *p) {
        return false;
    }
    *p   = end;
    *out = (float)v;
    return true;
}

/*
 * An SVG path string → PathSegs.
 *
 * Supports M L H V C S Q T Z in both cases (upper = absolute, lower =
 * relative), which covers everything a drawing tool emits for a plotted curve.
 * Arcs (A) are deliberately absent: they need a parameter conversion of their
 * own and nothing in this project has asked for one.
 *
 * Quadratics become cubics on the way in, so the rest of the pipeline sees one
 * curve type.
 */
static bool parse_path_d(PathWidget *w, const char *d)
{
    float cx = 0.0f, cy = 0.0f;   /* current point */
    float sx = 0.0f, sy = 0.0f;   /* start of the current subpath */
    float px = 0.0f, py = 0.0f;   /* previous cubic control, for S */
    float qx = 0.0f, qy = 0.0f;   /* previous quadratic control, for T */
    char  prev = 0;

    const char *p = d;
    while (*p != '\0') {
        d_skip(&p);
        if (*p == '\0') {
            break;
        }

        char cmd;
        if (isalpha((unsigned char)*p)) {
            cmd = *p++;
        } else if (prev != 0) {
            /* A repeated coordinate set continues the previous command; after
             * a moveto it means lineto, as the SVG grammar specifies. */
            cmd = (prev == 'M') ? 'L' : (prev == 'm') ? 'l' : prev;
        } else {
            return false;
        }

        bool  rel = (cmd >= 'a' && cmd <= 'z');
        char  up  = (char)toupper((unsigned char)cmd);
        float a[6];

        switch (up) {
            case 'M':
                if (!d_number(&p, &a[0]) || !d_number(&p, &a[1])) return false;
                if (rel) { a[0] += cx; a[1] += cy; }
                cx = sx = a[0]; cy = sy = a[1];
                px = cx; py = cy; qx = cx; qy = cy;
                if (!path_push(w, 0, a, 2)) return false;
                break;

            case 'L':
                if (!d_number(&p, &a[0]) || !d_number(&p, &a[1])) return false;
                if (rel) { a[0] += cx; a[1] += cy; }
                cx = a[0]; cy = a[1];
                px = cx; py = cy; qx = cx; qy = cy;
                if (!path_push(w, 1, a, 2)) return false;
                break;

            case 'H':
                if (!d_number(&p, &a[0])) return false;
                if (rel) a[0] += cx;
                a[1] = cy; cx = a[0];
                px = cx; py = cy; qx = cx; qy = cy;
                if (!path_push(w, 1, a, 2)) return false;
                break;

            case 'V':
                if (!d_number(&p, &a[1])) return false;
                if (rel) a[1] += cy;
                a[0] = cx; cy = a[1];
                px = cx; py = cy; qx = cx; qy = cy;
                if (!path_push(w, 1, a, 2)) return false;
                break;

            case 'C':
                for (int i = 0; i < 6; i++) {
                    if (!d_number(&p, &a[i])) return false;
                }
                if (rel) {
                    a[0] += cx; a[1] += cy; a[2] += cx; a[3] += cy; a[4] += cx; a[5] += cy;
                }
                px = a[2]; py = a[3];
                cx = a[4]; cy = a[5]; qx = cx; qy = cy;
                if (!path_push(w, 2, a, 6)) return false;
                break;

            case 'S': {
                /* The first control point mirrors the previous one. */
                float m0 = 2.0f * cx - px, m1 = 2.0f * cy - py;
                if (!d_number(&p, &a[2]) || !d_number(&p, &a[3]) ||
                    !d_number(&p, &a[4]) || !d_number(&p, &a[5])) return false;
                if (rel) { a[2] += cx; a[3] += cy; a[4] += cx; a[5] += cy; }
                a[0] = m0; a[1] = m1;
                px = a[2]; py = a[3];
                cx = a[4]; cy = a[5]; qx = cx; qy = cy;
                if (!path_push(w, 2, a, 6)) return false;
                break;
            }

            case 'Q':
            case 'T': {
                float ctrlx, ctrly, ex, ey;
                if (up == 'Q') {
                    float t[4];
                    for (int i = 0; i < 4; i++) {
                        if (!d_number(&p, &t[i])) return false;
                    }
                    if (rel) { t[0] += cx; t[1] += cy; t[2] += cx; t[3] += cy; }
                    ctrlx = t[0]; ctrly = t[1]; ex = t[2]; ey = t[3];
                } else {
                    ctrlx = 2.0f * cx - qx;
                    ctrly = 2.0f * cy - qy;
                    if (!d_number(&p, &ex) || !d_number(&p, &ey)) return false;
                    if (rel) { ex += cx; ey += cy; }
                }

                /* Quadratic → cubic: the controls sit two thirds of the way
                 * from each endpoint toward the quadratic's control point. */
                a[0] = cx + 2.0f / 3.0f * (ctrlx - cx);
                a[1] = cy + 2.0f / 3.0f * (ctrly - cy);
                a[2] = ex + 2.0f / 3.0f * (ctrlx - ex);
                a[3] = ey + 2.0f / 3.0f * (ctrly - ey);
                a[4] = ex; a[5] = ey;

                qx = ctrlx; qy = ctrly;
                px = a[2];  py = a[3];
                cx = ex;    cy = ey;
                if (!path_push(w, 2, a, 6)) return false;
                break;
            }

            case 'Z':
                cx = sx; cy = sy;
                px = cx; py = cy; qx = cx; qy = cy;
                if (!path_push(w, 3, a, 0)) return false;
                break;

            default:
                fprintf(stderr, "warning: path command '%c' is not supported — path truncated.\n", cmd);
                return w->seg_count > 0;
        }
        prev = cmd;
    }
    return w->seg_count > 0;
}

/* A plain "points": [[x,y], …] polyline — the common case, without a d-string. */
static bool parse_path_points(PathWidget *w, const cJSON *pts)
{
    const cJSON *pt = NULL;
    bool first = true;

    cJSON_ArrayForEach(pt, pts) {
        float a[2];
        if (cJSON_IsArray(pt) && cJSON_GetArraySize(pt) >= 2) {
            a[0] = (float)cJSON_GetArrayItem(pt, 0)->valuedouble;
            a[1] = (float)cJSON_GetArrayItem(pt, 1)->valuedouble;
        } else if (cJSON_IsObject(pt)) {
            a[0] = json_float(pt, "x", 0.0f);
            a[1] = json_float(pt, "y", 0.0f);
        } else {
            continue;
        }
        if (!path_push(w, first ? 0 : 1, a, 2)) {
            return false;
        }
        first = false;
    }
    return w->seg_count > 0;
}

static bool parse_path_object(EditorContext *ctx, const cJSON *obj, int z, const EaseTable *eases)
{
    PathWidget *w = (PathWidget *)array_push((void **)&ctx->paths, &ctx->path_count,
                                             &ctx->path_cap, sizeof(PathWidget));
    if (w == NULL) {
        return false;
    }

    parse_widget_base(&w->base, obj, WIDGET_PATH, z, eases);

    w->width      = json_float(obj, "width", json_float(obj, "stroke_width", 2.0f));
    w->color      = json_color(obj, "stroke", json_color(obj, "color",
                                                         (Color){ 255, 255, 255, 255 }));
    w->fill_color = json_color(obj, "fill", (Color){ 0, 0, 0, 0 });

    const char *fill = json_str(obj, "fill", NULL);
    w->filled = (fill != NULL && strcmp(fill, "none") != 0);
    w->closed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(obj, "closed"));

    const char *cap = json_str(obj, "cap", "butt");
    w->cap = (strcmp(cap, "round") == 0) ? 1 : (strcmp(cap, "square") == 0) ? 2 : 0;
    const char *join = json_str(obj, "join", "miter");
    w->join = (strcmp(join, "round") == 0) ? 1 : (strcmp(join, "bevel") == 0) ? 2 : 0;

    const char  *d   = json_str(obj, "d", NULL);
    const cJSON *pts = cJSON_GetObjectItemCaseSensitive(obj, "points");

    bool ok;
    if (d != NULL) {
        ok = parse_path_d(w, d);
        if (!ok) {
            fprintf(stderr, "warning: path '%s' — could not read its \"d\".\n",
                    w->base.id ? w->base.id : "(unnamed)");
        }
    } else if (cJSON_IsArray(pts)) {
        ok = parse_path_points(w, pts);
    } else {
        fprintf(stderr, "warning: path '%s' has neither \"d\" nor \"points\".\n",
                w->base.id ? w->base.id : "(unnamed)");
        ok = false;
    }

    /* An unreadable path stays as an empty widget rather than failing the whole
     * parse — one bad curve should not cost the author the entire render. */
    (void)ok;
    return true;
}

/*
 * A video clip. Decoding happens later, in media_prepare_textures — the parser
 * only records what was asked for.
 */
static bool parse_video_object(EditorContext *ctx, const cJSON *obj, int z,
                               const char *json_path, const EaseTable *eases)
{
    VideoWidget *w = (VideoWidget *)array_push((void **)&ctx->videos, &ctx->video_count,
                                               &ctx->video_cap, sizeof(VideoWidget));
    if (w == NULL) {
        return false;
    }

    parse_widget_base(&w->base, obj, WIDGET_VIDEO, z, eases);

    const char *src = json_str(obj, "path", json_str(obj, "src", NULL));
    w->path  = (src != NULL) ? resolve_relative_path(json_path, src) : NULL;
    w->start = json_float(obj, "start", 0.0f);
    w->speed = json_float(obj, "speed", 1.0f);
    w->loop  = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(obj, "loop"));

    if (w->speed < 0.01f) {
        w->speed = 0.01f;
    }
    if (w->start < 0.0f) {
        w->start = 0.0f;
    }

    w->request_w = json_int(obj, "width",  json_int(obj, "w", 0));
    w->request_h = json_int(obj, "height", json_int(obj, "h", 0));

    if (w->path == NULL) {
        fprintf(stderr, "warning: video '%s' has no path.\n",
                w->base.id ? w->base.id : "(unnamed)");
    }
    return true;
}

/*
 * A mesh: either an OBJ on disk, or a named primitive.
 *
 * Geometry is loaded here rather than in media_prepare_textures because a mesh
 * has no texture to rasterize — the vertices *are* the asset.
 */
static bool parse_mesh_object(EditorContext *ctx, const cJSON *obj, int z,
                              const char *base_file, const EaseTable *eases)
{
    MeshWidget *w = (MeshWidget *)array_push((void **)&ctx->meshes, &ctx->mesh_count,
                                             &ctx->mesh_cap, sizeof(MeshWidget));
    if (w == NULL) {
        return false;
    }

    parse_widget_base(&w->base, obj, WIDGET_MESH, z, eases);

    const char *src = json_str(obj, "path", json_str(obj, "src", NULL));
    w->path  = (src != NULL) ? resolve_relative_path(base_file, src) : NULL;
    w->shape = dup_string(json_str(obj, "shape", (src == NULL) ? "box" : NULL));

    /* "size": 300 is uniform; "size": [w, h, d] scales each axis on its own. */
    const cJSON *sz = cJSON_GetObjectItemCaseSensitive(obj, "size");
    if (cJSON_IsArray(sz) && cJSON_GetArraySize(sz) >= 3) {
        for (int k = 0; k < 3; k++) {
            const cJSON *v = cJSON_GetArrayItem(sz, k);
            w->size[k] = cJSON_IsNumber(v) ? (float)v->valuedouble : 300.0f;
        }
    } else {
        float u = json_float(obj, "size", 300.0f);
        w->size[0] = w->size[1] = w->size[2] = u;
    }
    w->color   = json_color(obj, "color", (Color){ 0x7E, 0xE7, 0x87, 255 });
    w->ambient = json_float(obj, "ambient", 0.25f);

    /* Closed shapes want culling; an open one (a plane) would lose half of
     * itself, so the default follows the primitive rather than being fixed. */
    /* Smooth shading suits curved primitives and looks wrong on a cube, whose
     * corner-averaged normals would round it. So the default follows the shape
     * and an explicit "smooth" overrides. */
    bool curved = (w->shape != NULL &&
                   (strcmp(w->shape, "sphere") == 0 || strcmp(w->shape, "torus") == 0 ||
                    strcmp(w->shape, "cylinder") == 0));
    const cJSON *sm = cJSON_GetObjectItemCaseSensitive(obj, "smooth");
    w->smooth = cJSON_IsBool(sm) ? cJSON_IsTrue(sm) : (curved || w->path != NULL);

    /* "wire": true takes a sensible default width; a number sets it directly. */
    const cJSON *wr = cJSON_GetObjectItemCaseSensitive(obj, "wire");
    if (cJSON_IsNumber(wr)) {
        w->wire = (float)wr->valuedouble;
    } else if (cJSON_IsTrue(wr)) {
        w->wire = 1.6f;
    }
    if (w->wire < 0.0f) {
        w->wire = 0.0f;
    }

    const cJSON *aa = cJSON_GetObjectItemCaseSensitive(obj, "antialias");
    w->antialias = cJSON_IsBool(aa) ? cJSON_IsTrue(aa) : true;

    const char *flt = json_str(obj, "filter", "bilinear");
    w->filter = (strcmp(flt, "nearest") != 0);

    const char *tp = json_str(obj, "texture", NULL);
    w->tex_path = (tp != NULL) ? resolve_relative_path(base_file, tp) : NULL;

    /* An explicit map wins over whatever the model's material names, so a
     * scene can override what the exporter chose. */
    const char *aop = json_str(obj, "ao", json_str(obj, "occlusion", NULL));
    w->ao_path = (aop != NULL) ? resolve_relative_path(base_file, aop) : NULL;
    w->ao_strength = vr_clampf01(json_float(obj, "ao_strength", 1.0f));

    const char *nmp = json_str(obj, "normal_map", json_str(obj, "normal", NULL));
    w->nrm_path = (nmp != NULL) ? resolve_relative_path(base_file, nmp) : NULL;
    /*
     * Negative means "unset", so the glTF material's own normalTexture.scale
     * can fill it in. A plain default of 1 would silently overrule what the
     * exporter said, which is the one number in the material worth respecting.
     */
    w->normal_scale = json_float(obj, "normal_scale", -1.0f);

    /*
     * "emissive" takes either a colour or a path, and which one is decided by
     * whether the string parses as a colour.
     *
     * One key rather than two because the two spellings mean the same thing to
     * whoever is writing the scene — "this surface glows" — and the difference
     * between a flat glow and a mapped one is a detail of how it is specified.
     * "emissive_map" stays available for the case where a filename genuinely
     * does look like a hex colour.
     */
    Color emis_col = { 255, 255, 255, 255 };
    bool  emis_is_colour = false;
    const char *ems = json_str(obj, "emissive", NULL);
    if (ems != NULL) {
        emis_is_colour = parse_hex_color(ems, &emis_col);
    }

    const char *emp = json_str(obj, "emissive_map", emis_is_colour ? NULL : ems);
    w->emis_path = (emp != NULL) ? resolve_relative_path(base_file, emp) : NULL;
    w->emissive  = json_color(obj, "emissive_color", emis_col);

    /*
     * Negative means unset, which lets a glTF emissiveFactor fill it in. A
     * colour or a map named in the JSON is an explicit request, so it defaults
     * to full strength instead.
     */
    w->emissive_strength = json_float(obj, "emissive_strength",
                                      (emis_is_colour || emp != NULL) ? 1.0f : -1.0f);

    w->specular  = json_float(obj, "specular", 0.0f);
    w->shininess = json_float(obj, "shininess", 32.0f);
    if (w->specular < 0.0f)  w->specular = 0.0f;
    if (w->shininess < 1.0f) w->shininess = 1.0f;

    bool open_shape = (w->shape != NULL &&
                       (strcmp(w->shape, "plane") == 0 ||
                        strcmp(w->shape, "ring") == 0));
    const cJSON *cu = cJSON_GetObjectItemCaseSensitive(obj, "cull");
    w->cull = cJSON_IsBool(cu) ? cJSON_IsTrue(cu) : !open_shape;

    for (int k = 0; k < 3; k++) {
        if (w->size[k] < 1.0f) {
            w->size[k] = 1.0f;
        }
    }

    if (!mesh_load(w)) {
        /* An unreadable mesh leaves an empty widget rather than failing the
         * parse — one bad asset should not cost the whole render. */
        return true;
    }
    return true;
}

static bool parse_image_object(EditorContext *ctx, const cJSON *obj, int z,
                               const char *base_file, const EaseTable *eases)
{
    ImageWidget *w = (ImageWidget *)array_push((void **)&ctx->images, &ctx->image_count,
                                               &ctx->image_cap, sizeof(ImageWidget));
    if (w == NULL) {
        return false;
    }

    parse_widget_base(&w->base, obj, WIDGET_IMAGE, z, eases);

    const char *src = json_str(obj, "path", NULL);
    if (src == NULL) {
        src = json_str(obj, "src", NULL);
    }
    if (src == NULL) {
        fprintf(stderr, "error: image '%s' has no 'path'.\n",
                w->base.id ? w->base.id : "(null)");
        return false;
    }

    w->path      = resolve_relative_path(base_file, src);
    w->request_w = json_int(obj, "width", 0);
    w->request_h = json_int(obj, "height", 0);

    if (w->request_w < 0) w->request_w = 0;
    if (w->request_h < 0) w->request_h = 0;

    return w->path != NULL;
}

static bool parse_code_object(EditorContext *ctx, const cJSON *obj, int z, const EaseTable *eases)
{
    CodeWidget *w = (CodeWidget *)array_push((void **)&ctx->codes, &ctx->code_count,
                                             &ctx->code_cap, sizeof(CodeWidget));
    if (w == NULL) {
        return false;
    }

    parse_widget_base(&w->base, obj, WIDGET_CODE, z, eases);

    /* "code" or "content" — both are accepted. */
    const char *src = json_str(obj, "code", NULL);
    if (src == NULL) {
        src = json_str(obj, "content", "");
    }

    w->code         = dup_string(src);
    w->language     = dup_string(json_str(obj, "language", "text"));
    w->font         = dup_string(json_str(obj, "font", "monospace"));
    w->size         = json_int(obj, "size", 32);
    w->fg           = json_color(obj, "color",    (Color){ 230, 230, 230, 255 });
    w->bg           = json_color(obj, "bg_color", (Color){  24,  24,  37, 220 });
    w->padding      = json_int(obj, "padding", 24);
    w->line_spacing = json_float(obj, "line_spacing", 1.35f);
    w->corner_radius = json_int(obj, "corner_radius", 12);

    /* Highlighting is on unless the JSON explicitly turns it off. */
    const cJSON *hl = cJSON_GetObjectItemCaseSensitive(obj, "highlight");
    w->highlight = cJSON_IsBool(hl) ? cJSON_IsTrue(hl) : true;

    if (w->size < 1) {
        w->size = 1;
    }
    if (w->padding < 0) {
        w->padding = 0;
    }
    if (w->corner_radius < 0) {
        w->corner_radius = 0;
    }
    return w->code != NULL && w->language != NULL && w->font != NULL;
}

/* ------------------------------------------------------------------------- */
/* Parsing audio tracks                                                       */
/* ------------------------------------------------------------------------- */

static bool parse_audio(EditorContext *ctx, const cJSON *arr, const char *base_file)
{
    if (!cJSON_IsArray(arr)) {
        return true; /* audio is optional */
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        const char *path = json_str(item, "path", NULL);
        if (path == NULL) {
            path = json_str(item, "file", NULL);
        }

        if (path == NULL) {
            /* TTS is deliberately absent — say so plainly. */
            if (json_has(item, "text")) {
                fprintf(stderr, "warning: audio track has 'text', but speech synthesis "
                                "is not part of this project — supply a file via 'path'.\n");
            } else {
                fprintf(stderr, "warning: audio track has no 'path' — skipped.\n");
            }
            continue;
        }

        AudioTrack *a = (AudioTrack *)array_push((void **)&ctx->audio, &ctx->audio_count,
                                                 &ctx->audio_cap, sizeof(AudioTrack));
        if (a == NULL) {
            return false;
        }

        a->path     = resolve_relative_path(base_file, path);
        a->start    = json_float(item, "start", 0.0f);
        a->in       = json_float(item, "in", 0.0f);
        a->duration = json_float(item, "duration", 0.0f);
        a->volume   = json_float(item, "volume", 1.0f);
        a->fade_in  = json_float(item, "fade_in", 0.0f);
        a->fade_out = json_float(item, "fade_out", 0.0f);

        const cJSON *lp = cJSON_GetObjectItemCaseSensitive(item, "loop");
        a->loop = cJSON_IsBool(lp) ? cJSON_IsTrue(lp) : false;

        const char *aid = json_str(item, "id", NULL);
        a->id = (aid != NULL) ? dup_string(aid) : NULL;

        a->pan = clampf(json_float(item, "pan", 0.0f), -1.0f, 1.0f);

        const cJSON *eq = cJSON_GetObjectItemCaseSensitive(item, "eq");
        if (cJSON_IsObject(eq)) {
            a->eq_low  = json_float(eq, "low",  0.0f);
            a->eq_mid  = json_float(eq, "mid",  0.0f);
            a->eq_high = json_float(eq, "high", 0.0f);
            /* All-zero is a bypass, so an "eq": {} does not add three filters
             * that each do nothing. */
            a->has_eq = (a->eq_low != 0.0f || a->eq_mid != 0.0f || a->eq_high != 0.0f);
        }

        const cJSON *cp = cJSON_GetObjectItemCaseSensitive(item, "compress");
        if (cp == NULL) {
            cp = cJSON_GetObjectItemCaseSensitive(item, "compressor");
        }
        if (cJSON_IsObject(cp) || cJSON_IsTrue(cp)) {
            a->has_comp          = true;
            a->comp_threshold_db = json_float(cp, "threshold", -18.0f);
            a->comp_ratio        = json_float(cp, "ratio", 4.0f);
            a->comp_attack_ms    = json_float(cp, "attack", 20.0f);
            a->comp_release_ms   = json_float(cp, "release", 250.0f);
            a->comp_makeup       = json_float(cp, "makeup", 1.0f);

            /* ffmpeg's acompressor refuses values outside these, and a refusal
             * arrives as an opaque ffmpeg error long after the render. */
            a->comp_ratio      = clampf(a->comp_ratio, 1.0f, 20.0f);
            a->comp_attack_ms  = clampf(a->comp_attack_ms, 0.01f, 2000.0f);
            a->comp_release_ms = clampf(a->comp_release_ms, 0.01f, 9000.0f);
            a->comp_makeup     = clampf(a->comp_makeup, 1.0f, 64.0f);
            a->comp_threshold_db = clampf(a->comp_threshold_db, -60.0f, 0.0f);
        }

        const cJSON *dk = cJSON_GetObjectItemCaseSensitive(item, "duck");
        if (cJSON_IsObject(dk)) {
            const char *by = json_str(dk, "by", NULL);
            a->duck_by         = (by != NULL) ? dup_string(by) : NULL;
            a->duck_amount     = clampf(json_float(dk, "amount", 0.7f), 0.0f, 1.0f);
            a->duck_release_ms = clampf(json_float(dk, "release", 300.0f),
                                           0.01f, 9000.0f);
            if (a->duck_by == NULL) {
                fprintf(stderr, "warning: audio 'duck' needs \"by\": the id of the "
                                "track to duck under — ignoring it.\n");
            }
        }

        if (a->start    < 0.0f) a->start    = 0.0f;
        if (a->in       < 0.0f) a->in       = 0.0f;
        if (a->duration < 0.0f) a->duration = 0.0f;
        if (a->volume   < 0.0f) a->volume   = 0.0f;
        if (a->fade_in  < 0.0f) a->fade_in  = 0.0f;
        if (a->fade_out < 0.0f) a->fade_out = 0.0f;

        if (a->path == NULL) {
            return false;
        }

        /*
         * File existence is checked at *parse* time, not at mix time.
         *
         * The reason is practical: audio is the last stage, so a typo in a path
         * would otherwise surface only after thousands of frames had already
         * been rendered. Better to stop in the first second.
         */
        FILE *probe = fopen(a->path, "rb");
        if (probe == NULL) {
            fprintf(stderr, "error: could not open audio file '%s'.\n", a->path);
            return false;
        }
        fclose(probe);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Parsing the timeline                                                       */
/* ------------------------------------------------------------------------- */

static ActionType action_from_string(const char *s)
{
    if (s == NULL)                       return ACTION_UNKNOWN;
    if (strcmp(s, "fade_in")   == 0)     return ACTION_FADE_IN;
    if (strcmp(s, "fade_out")  == 0)     return ACTION_FADE_OUT;
    if (strcmp(s, "move")      == 0 ||
        strcmp(s, "move_x")    == 0 ||
        strcmp(s, "move_y")    == 0)     return ACTION_MOVE;
    if (strcmp(s, "typewrite") == 0)     return ACTION_TYPEWRITE;
    if (strcmp(s, "scale")     == 0 ||
        strcmp(s, "zoom")      == 0)     return ACTION_SCALE;
    if (strcmp(s, "rotate")    == 0)     return ACTION_ROTATE;
    if (strcmp(s, "highlight") == 0)     return ACTION_HIGHLIGHT;
    if (strcmp(s, "orbit") == 0)        return ACTION_ORBIT;
    if (strcmp(s, "emit") == 0)         return ACTION_EMIT;
    if (strcmp(s, "animate") == 0 ||
        strcmp(s, "tween") == 0)        return ACTION_ANIMATE;
    return ACTION_UNKNOWN;
}

/*
 * A coordinate that may be a number or a layout expression ("center", "60%-20").
 *
 * Unlike a widget's position this never depends on the object's own size — it
 * is a point on the canvas — so it can be resolved here rather than deferred
 * until after rasterization.
 */
static float json_canvas_coord(const cJSON *obj, const char *key,
                               float canvas, LayoutAxis axis, float fallback)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) {
        return (float)it->valuedouble;
    }
    if (cJSON_IsString(it) && it->valuestring != NULL) {
        float pos = 0.0f, anchor = 0.0f;
        if (layout_eval(it->valuestring, canvas, axis, &pos, &anchor)) {
            return pos;
        }
        fprintf(stderr, "warning: '%s': \'%s\' not understood — using %.0f.\n",
                key, it->valuestring, (double)fallback);
    }
    return fallback;
}

static bool parse_timeline_event(const EditorContext *ctx, Scene *scene, const cJSON *obj,
                                 const EaseTable *eases)
{
    TimelineEvent *e = (TimelineEvent *)array_push((void **)&scene->events, &scene->event_count,
                                                   &scene->event_cap, sizeof(TimelineEvent));
    if (e == NULL) {
        return false;
    }

    const char *action_name = json_str(obj, "action", NULL);

    e->label     = dup_string(json_str(obj, "id", json_str(obj, "label", NULL)));
    e->time_expr = NULL;

    /*
     * "time" may be a number of seconds or an expression against another
     * event. The expression cannot be evaluated yet — it may name an event
     * further down the array — so it is kept until the scene is complete.
     */
    const cJSON *tv = cJSON_GetObjectItemCaseSensitive(obj, "time");
    if (cJSON_IsString(tv) && tv->valuestring != NULL) {
        e->time_expr = dup_string(tv->valuestring);
    }

    e->time_ms      = json_int(obj, "time_ms",
                               cJSON_IsNumber(tv) ? (int)(tv->valuedouble * 1000.0) : 0);
    e->duration_ms  = json_int(obj, "duration_ms", 0);
    e->action       = action_from_string(action_name);
    e->target_id    = dup_string(json_str(obj, "target", ""));
    e->target_index = -1; /* resolved in resolve_timeline_targets() */
    e->target_group = -1;

    /* SCALE's neutral value is 1.0, not 0 — which would make the object vanish. */
    e->value = json_float(obj, "value", (e->action == ACTION_SCALE) ? 1.0f : 0.0f);

    /* smoothstep is the default — exactly the behaviour that used to be hard-wired. */
    e->ease = json_has(obj, "ease") ? easing_from_name(json_str(obj, "ease", NULL))
                                    : EASE_SMOOTH;

    /* MOVE axes: "move_x"/"move_y" use the single `value`, while plain "move"
     * uses value_x/value_y. */
    if (action_name != NULL && strcmp(action_name, "move_x") == 0) {
        e->value_x = e->value;
    } else if (action_name != NULL && strcmp(action_name, "move_y") == 0) {
        e->value_y = e->value;
    } else {
        e->value_x = json_float(obj, "value_x", 0.0f);
        e->value_y = json_float(obj, "value_y", 0.0f);
    }

    /*
     * HIGHLIGHT: which lines to mark, and in what colour.
     *
     * Lines are 1-based in the JSON, because that is what an editor's gutter
     * shows and what anyone reading the code block will count. A single "line"
     * is shorthand for a one-line range.
     */
    if (e->action == ACTION_HIGHLIGHT) {
        int from = json_int(obj, "line", json_int(obj, "from", 1));
        int to   = json_int(obj, "to", json_has(obj, "line") ? from : from);

        e->hl_from = (from > 0) ? from - 1 : 0;
        e->hl_to   = (to   > 0) ? to   - 1 : e->hl_from;
        if (e->hl_to < e->hl_from) {
            int swap = e->hl_from; e->hl_from = e->hl_to; e->hl_to = swap;
        }

        Color fallback = { 0xFF, 0xD1, 0x66, 0x40 }; /* a soft amber, ~25% opaque */
        e->hl_color = json_color(obj, "color", fallback);
    }

    /*
     * ORBIT: centre, radii, and the arc travelled.
     *
     * `sweep` rather than an end angle, because "go round one and a half times"
     * is 540 while an end angle of 180 would be ambiguous about how many turns
     * to take on the way.
     */
    if (e->action == ACTION_ORBIT) {
        float cw = (float)ctx->config.width;
        float ch = (float)ctx->config.height;

        e->orbit_cx = json_canvas_coord(obj, "cx", cw, LAYOUT_AXIS_X, cw * 0.5f);
        e->orbit_cy = json_canvas_coord(obj, "cy", ch, LAYOUT_AXIS_Y, ch * 0.5f);

        e->orbit_r0 = json_float(obj, "radius", 100.0f);
        /* A second radius turns the circle into a spiral; absent → a circle. */
        e->orbit_r1 = json_float(obj, "radius_to", e->orbit_r0);

        e->orbit_a0    = json_float(obj, "from_angle", 0.0f);
        e->orbit_sweep = json_has(obj, "sweep")
                             ? json_float(obj, "sweep", 360.0f)
                             : json_float(obj, "to_angle", e->orbit_a0 + 360.0f) - e->orbit_a0;

        e->orbit_orient = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(obj, "orient"));
    }

    /*
     * ANIMATE: which property, and the track driving it.
     *
     * Two spellings, because both are natural in different places:
     *   "keys":  [ {"t":0,"v":0}, {"t":1.2,"v":300} ]   full control
     *   "from"/"to"                                      the common two-key case
     */
    if (e->action == ACTION_ANIMATE) {
        static const struct { const char *name; AnimProp prop; } kProps[] = {
            { "x", PROP_X }, { "y", PROP_Y },
            { "opacity", PROP_OPACITY }, { "alpha", PROP_OPACITY },
            { "scale", PROP_SCALE },
            { "rotation", PROP_ROTATION }, { "rotate", PROP_ROTATION },
            { "w", PROP_W }, { "width", PROP_W },
            { "h", PROP_H }, { "height", PROP_H },
            { "tint", PROP_TINT }, { "tint_amount", PROP_TINT },
            { "trim", PROP_TRIM },
            { "z", PROP_Z }, { "z_depth", PROP_Z },
            { "rotate_x", PROP_RX }, { "rx", PROP_RX },
            { "rotate_y", PROP_RY }, { "ry", PROP_RY },
        };

        const char *pname = json_str(obj, "property", json_str(obj, "prop", NULL));
        e->anim_prop = PROP_NONE;
        for (size_t k = 0; pname != NULL && k < sizeof kProps / sizeof kProps[0]; k++) {
            if (strcmp(pname, kProps[k].name) == 0) {
                e->anim_prop = kProps[k].prop;
                break;
            }
        }
        if (e->anim_prop == PROP_NONE) {
            fprintf(stderr, "warning: animate (t=%d ms) — unknown property '%s'.\n",
                    e->time_ms, pname ? pname : "(missing)");
        }

        const cJSON *keys = cJSON_GetObjectItemCaseSensitive(obj, "keys");
        if (cJSON_IsArray(keys)) {
            e->has_keys = parse_track_ex(keys, &e->anim_track, 0.0f, eases);

            /* Without an explicit duration, the last key ends the event. */
            if (e->duration_ms <= 0 && e->anim_track.count > 0) {
                e->duration_ms =
                    (int)(e->anim_track.keys[e->anim_track.count - 1].t * 1000.0f);
            }
        } else if (json_has(obj, "from") || json_has(obj, "to")) {
            /* The two-key shorthand, built into the same track so evaluation
             * has exactly one path. */
            Keyframe *k2 = (Keyframe *)calloc(2, sizeof(Keyframe));
            if (k2 == NULL) {
                return false;
            }
            float dur = (e->duration_ms > 0) ? (float)e->duration_ms * 0.001f : 1.0f;
            k2[0].t = 0.0f;   k2[0].v = json_float(obj, "from", 0.0f); k2[0].ease = e->ease;
            k2[1].t = dur;    k2[1].v = json_float(obj, "to", 0.0f);   k2[1].ease = e->ease;

            e->anim_track.keys     = k2;
            e->anim_track.count    = 2;
            e->anim_track.constant = k2[1].v;
            e->has_keys            = true;
            if (e->duration_ms <= 0) {
                e->duration_ms = 1000;
            }
        } else {
            fprintf(stderr, "warning: animate (t=%d ms) has neither \"keys\" nor \"from\"/\"to\".\n",
                    e->time_ms);
        }
    }

    if (e->action == ACTION_EMIT) {
        e->emit_vx      = json_float(obj, "vx", 0.0f);
        e->emit_vy      = json_float(obj, "vy", 0.0f);
        e->emit_gravity = json_float(obj, "gravity", 0.0f);
        e->emit_fade    = json_float(obj, "fade", 0.35f);
        e->emit_spin    = json_float(obj, "spin", 0.0f);
    }

    if (e->action == ACTION_UNKNOWN) {
        fprintf(stderr, "warning: unknown action '%s' (t=%d ms) — skipped.\n",
                action_name ? action_name : "(null)", e->time_ms);
    }
    if (e->duration_ms < 0) {
        e->duration_ms = 0;
    }
    return e->target_id != NULL;
}


/* ------------------------------------------------------------------------- */
/* Transitions                                                                */
/* ------------------------------------------------------------------------- */

static TransitionType transition_from_name(const char *name)
{
    if (name == NULL) return TRANS_CUT;

    static const struct { const char *n; TransitionType t; } kT[] = {
        { "cut",         TRANS_CUT         }, { "crossfade",  TRANS_CROSSFADE  },
        { "dissolve",    TRANS_CROSSFADE   }, { "fade",       TRANS_FADE       },
        { "slide_left",  TRANS_SLIDE_LEFT  }, { "slide_right",TRANS_SLIDE_RIGHT},
        { "slide_up",    TRANS_SLIDE_UP    }, { "slide_down", TRANS_SLIDE_DOWN },
        { "push_left",   TRANS_PUSH_LEFT   }, { "push_right", TRANS_PUSH_RIGHT },
        { "push_up",     TRANS_PUSH_UP     }, { "push_down",  TRANS_PUSH_DOWN  },
        { "zoom_in",     TRANS_ZOOM_IN     }, { "zoom_out",   TRANS_ZOOM_OUT   },
        { "spin",        TRANS_SPIN        }, { "wipe_left",  TRANS_WIPE_LEFT  },
        { "wipe_right",  TRANS_WIPE_RIGHT  }, { "iris",       TRANS_IRIS       },
    };

    for (size_t i = 0; i < sizeof kT / sizeof kT[0]; i++) {
        const char *a = name, *b = kT[i].n;
        while (*a && *b) {
            while (*a == '_' || *a == '-' || *a == ' ') a++;
            while (*b == '_' || *b == '-' || *b == ' ') b++;
            if (*a == '\0' || *b == '\0') break;
            int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
            if (ca != *b) break;
            a++; b++;
        }
        while (*a == '_' || *a == '-' || *a == ' ') a++;
        while (*b == '_' || *b == '-' || *b == ' ') b++;
        if (*a == '\0' && *b == '\0') {
            return kT[i].t;
        }
    }

    fprintf(stderr, "warning: unknown transition '%s' — falling back to a cut.\n", name);
    return TRANS_CUT;
}

/* Channels of a `from`/`to` block. Here a Track's "t" is progress in [0,1]. */
static bool parse_transition_side(const cJSON *side, Track *op, Track *x, Track *y,
                                  Track *sc, Track *rot,
                                  float d_op, float d_sc)
{
    track_set_constant(op,  d_op);
    track_set_constant(x,   0.0f);
    track_set_constant(y,   0.0f);
    track_set_constant(sc,  d_sc);
    track_set_constant(rot, 0.0f);

    if (!cJSON_IsObject(side)) {
        return false;
    }

    parse_track(cJSON_GetObjectItemCaseSensitive(side, "opacity"), op,  d_op);
    parse_track(cJSON_GetObjectItemCaseSensitive(side, "x"),       x,   0.0f);
    parse_track(cJSON_GetObjectItemCaseSensitive(side, "y"),       y,   0.0f);
    parse_track(cJSON_GetObjectItemCaseSensitive(side, "scale"),   sc,  d_sc);
    parse_track(cJSON_GetObjectItemCaseSensitive(side, "rotate"),  rot, 0.0f);
    return true;
}

/*
 * Reads a mask: { "shape": "circle", "cx": .5, "cy": .5, "r": [...] }
 *                  { "shape": "rect",   "x": 0, "y": 0, "w": [...], "h": 1 }
 * Parameters are canvas fractions and may themselves be tracks.
 */
static int parse_mask(const cJSON *node, Track slots[4])
{
    for (int i = 0; i < 4; i++) {
        track_set_constant(&slots[i], 0.0f);
    }
    if (!cJSON_IsObject(node)) {
        return 0;
    }

    const char *shape = json_str(node, "shape", "rect");

    if (strcmp(shape, "circle") == 0) {
        parse_track(cJSON_GetObjectItemCaseSensitive(node, "cx"), &slots[0], 0.5f);
        parse_track(cJSON_GetObjectItemCaseSensitive(node, "cy"), &slots[1], 0.5f);
        parse_track(cJSON_GetObjectItemCaseSensitive(node, "r"),  &slots[2], 0.5f);
        return 1;
    }

    parse_track(cJSON_GetObjectItemCaseSensitive(node, "x"), &slots[0], 0.0f);
    parse_track(cJSON_GetObjectItemCaseSensitive(node, "y"), &slots[1], 0.0f);
    parse_track(cJSON_GetObjectItemCaseSensitive(node, "w"), &slots[2], 1.0f);
    parse_track(cJSON_GetObjectItemCaseSensitive(node, "h"), &slots[3], 1.0f);
    return 2;
}

/*
 * One transition object → one Transition.
 *
 * `item` may be NULL, which yields the historical default: a hard cut of zero
 * duration. That is what makes the padding below able to treat "no default was
 * given" as just another call.
 */
static void parse_transition_one(Transition *tr, const cJSON *item)
{
    const char *use = json_str(item, "use", NULL);
    if (use == NULL) {
        use = json_str(item, "type", NULL);
    }
    tr->type = transition_from_name(use);

    /* "duration" is in seconds (videogen's convention), "duration_ms" in ms. */
    if (json_has(item, "duration_ms")) {
        tr->duration_ms = json_int(item, "duration_ms", 600);
    } else {
        tr->duration_ms = (int)(json_float(item, "duration", 0.6f) * 1000.0f);
    }
    if (tr->duration_ms < 0) {
        tr->duration_ms = 0;
    }

    tr->has_from = parse_transition_side(cJSON_GetObjectItemCaseSensitive(item, "from"),
                                         &tr->from_opacity, &tr->from_x, &tr->from_y,
                                         &tr->from_scale, &tr->from_rotate, 1.0f, 1.0f);
    tr->has_to   = parse_transition_side(cJSON_GetObjectItemCaseSensitive(item, "to"),
                                         &tr->to_opacity, &tr->to_x, &tr->to_y,
                                         &tr->to_scale, &tr->to_rotate, 1.0f, 1.0f);

    tr->from_mask_shape = parse_mask(cJSON_GetObjectItemCaseSensitive(item, "fromMask"),
                                     tr->from_mask);
    tr->to_mask_shape   = parse_mask(cJSON_GetObjectItemCaseSensitive(item, "toMask"),
                                     tr->to_mask);
}

/*
 * The transition list, with a project-wide default.
 *
 * Three ways to say the same thing, in increasing order of specificity:
 *
 *   "transition":  {...}          the default for every gap between scenes
 *   "transitions": [ ..., null ]  a null entry falls back to that default
 *   "transitions": [ {...} ]      this gap, explicitly
 *
 * The motivation is concrete: listing_scenes.json carried fifteen transition
 * objects, all of them byte-identical. One `"transition"` key replaces the lot.
 *
 * Note that the default is re-parsed for each slot rather than struct-copied.
 * A Transition owns malloc'd Keyframe arrays, so copying it by value would give
 * several entries the same `keys` pointer and free it several times over.
 */
static bool parse_transitions(EditorContext *ctx, const cJSON *arr, const cJSON *def)
{
    const cJSON *item = NULL;

    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(item, arr) {
            Transition *tr = (Transition *)array_push((void **)&ctx->transitions,
                                                      &ctx->transition_count,
                                                      &ctx->transition_cap, sizeof(Transition));
            if (tr == NULL) {
                return false;
            }
            /* A non-object entry (null, mostly) means "whatever the default is". */
            parse_transition_one(tr, cJSON_IsObject(item) ? item : def);
        }
    }

    /*
     * Pad the remaining gaps with the default. Without a default this loop does
     * nothing and the gaps stay hard cuts, exactly as before.
     */
    if (def != NULL && ctx->scene_count > 1) {
        while (ctx->transition_count < ctx->scene_count - 1) {
            Transition *tr = (Transition *)array_push((void **)&ctx->transitions,
                                                      &ctx->transition_count,
                                                      &ctx->transition_cap, sizeof(Transition));
            if (tr == NULL) {
                return false;
            }
            parse_transition_one(tr, def);
        }
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Building the index and resolving references                                */
/* ------------------------------------------------------------------------- */

static int compare_by_z(const void *a, const void *b)
{
    const WidgetBase *wa = *(WidgetBase *const *)a;
    const WidgetBase *wb = *(WidgetBase *const *)b;
    return (wa->z_order > wb->z_order) - (wa->z_order < wb->z_order);
}

/*
 * Builds the uniform `widgets` index.
 *
 * Critical: this must happen *after* the `texts`/`codes` arrays stop growing —
 * a realloc moves the elements and every pointer in the index would dangle.
 */
static bool ctx_build_widget_index(EditorContext *ctx)
{
    size_t total = ctx->text_count + ctx->code_count + ctx->image_count + ctx->shape_count +
                       ctx->line_count + ctx->path_count +
                       ctx->video_count + ctx->mesh_count;
    if (total == 0) {
        ctx->widgets      = NULL;
        ctx->widget_count = 0;
        return true;
    }

    ctx->widgets = (WidgetBase **)calloc(total, sizeof(WidgetBase *));
    if (ctx->widgets == NULL) {
        return false;
    }

    size_t n = 0;
    for (size_t i = 0; i < ctx->text_count; i++) {
        ctx->widgets[n++] = &ctx->texts[i].base;
    }
    for (size_t i = 0; i < ctx->code_count; i++) {
        ctx->widgets[n++] = &ctx->codes[i].base;
    }
    for (size_t i = 0; i < ctx->image_count; i++) {
        ctx->widgets[n++] = &ctx->images[i].base;
    }
    for (size_t i = 0; i < ctx->shape_count; i++) {
        ctx->widgets[n++] = &ctx->shapes[i].base;
    }
    for (size_t i = 0; i < ctx->line_count; i++) {
        ctx->widgets[n++] = &ctx->lines[i].base;
    }
    for (size_t i = 0; i < ctx->path_count; i++) {
        ctx->widgets[n++] = &ctx->paths[i].base;
    }
    for (size_t i = 0; i < ctx->video_count; i++) {
        ctx->widgets[n++] = &ctx->videos[i].base;
    }
    for (size_t i = 0; i < ctx->mesh_count; i++) {
        ctx->widgets[n++] = &ctx->meshes[i].base;
    }
    ctx->widget_count = n;

    /*
     * JSON order = drawing order (painter's algorithm).
     *
     * This sort is load-bearing beyond ordering: the array above is grouped by
     * *type* (every text, then every image, ...), and sorting by z is what puts
     * it back into parse order. A scene then finds its objects as the slice
     * [first_widget, +widget_count), which is a count of objects parsed before
     * it — so the slice is only correct while z increases across the whole
     * file. Do not sort the slices individually: before this call they are not
     * slices of anything.
     */
    qsort(ctx->widgets, ctx->widget_count, sizeof(WidgetBase *), compare_by_z);

    /*
     * ...and because that invariant is a convention rather than a structure, it
     * is checked. Writing `"z"` by hand is the way to break it: repeat a small
     * z in a later scene and that object sorts into an earlier scene's slice,
     * so the earlier scene draws a stranger's background over its own contents
     * while its own captions vanish. Every index stays in range, nothing
     * crashes, and the picture is quietly wrong — which is exactly the kind of
     * failure worth spending a loop to name.
     */
    for (size_t si = 0; si < ctx->scene_count; si++) {
        const Scene *sc = &ctx->scenes[si];
        size_t end = sc->first_widget + sc->widget_count;

        if (end > ctx->widget_count) {
            continue;
        }
        for (size_t i = sc->first_widget; i < end; i++) {
            const WidgetBase *b = ctx->widgets[i];
            if (b->seq >= sc->first_widget && b->seq < end) {
                continue;
            }
            fprintf(stderr,
                    "warning: scene '%s' — object '%s' belongs to another scene. "
                    "An explicit \"z\" must increase across the whole file, not "
                    "restart per scene; objects will be drawn in the wrong scene.\n",
                    sc->id ? sc->id : "(unnamed)", b->id ? b->id : "(unnamed)");
            break;      /* one line per scene is enough to find the cause */
        }
    }

    return true;
}

/*
 * Targets are resolved *within* a scene, and the index is scene-local too.
 * That is what lets the same id exist independently in different scenes.
 */
/* A group id → its scene-local index, or -1. */
static int find_group(const Scene *sc, const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (size_t g = 0; g < sc->group_count; g++) {
        if (sc->groups[g].id != NULL && strcmp(sc->groups[g].id, name) == 0) {
            return (int)g;
        }
    }
    return -1;
}

static void resolve_timeline_targets(EditorContext *ctx)
{
    for (size_t si = 0; si < ctx->scene_count; si++) {
        Scene *sc = &ctx->scenes[si];

        /* Widgets → their group. Done here rather than while parsing, because
         * this is the first point at which the widget index exists. */
        for (size_t w = 0; w < sc->widget_count; w++) {
            WidgetBase *b = ctx->widgets[sc->first_widget + w];
            if (b->group_name == NULL) {
                continue;
            }
            b->group_index = find_group(sc, b->group_name);
            if (b->group_index < 0) {
                fprintf(stderr, "warning: scene '%s' — object '%s' names group '%s', "
                                "which does not exist.\n",
                        sc->id ? sc->id : "(unnamed)",
                        b->id ? b->id : "(null)", b->group_name);
            }
        }

        for (size_t i = 0; i < sc->event_count; i++) {
            TimelineEvent *e = &sc->events[i];

            for (size_t w = 0; w < sc->widget_count; w++) {
                const char *id = ctx->widgets[sc->first_widget + w]->id;
                if (id != NULL && e->target_id != NULL && strcmp(id, e->target_id) == 0) {
                    e->target_index = (int)w;
                    break;
                }
            }

            /* Not an object — perhaps a group. A name cannot be both: objects
             * are searched first, so an accidental clash is at least
             * deterministic rather than order-dependent. */
            if (e->target_index < 0) {
                e->target_group = find_group(sc, e->target_id);
            }

            if (e->target_index < 0 && e->target_group < 0 && e->action != ACTION_UNKNOWN) {
                fprintf(stderr, "warning: scene '%s' — target '%s' not found.\n",
                        sc->id ? sc->id : "(unnamed)",
                        e->target_id ? e->target_id : "(null)");
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Relative event times                                                       */
/* ------------------------------------------------------------------------- */

/*
 * Resolves one `time` expression against the events and labels of its scene.
 *
 * The grammar is deliberately tiny:
 *
 *     <base> ( ('+'|'-') <number> )*
 *     <base> ::= <label> | <event>.start | <event>.end | <number>
 *
 * Numbers are milliseconds. Anything richer would want a real expression
 * parser, and the point here is only to say "just after that other thing".
 *
 * Returns false when a referenced event is not resolved *yet* — the caller
 * simply tries again on the next pass.
 */
static bool resolve_time_expr(const Scene *sc, const char *expr, int *out_ms,
                              const bool *resolved, bool *unknown_name)
{
    const char *p = expr;
    long        total = 0;
    int         sign = 1;
    bool        first = true;

    *unknown_name = false;

    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        if (!first) {
            if (*p == '+')      { sign =  1; p++; }
            else if (*p == '-') { sign = -1; p++; }
            else {
                *unknown_name = true;
                return false;   /* junk between terms */
            }
            while (*p == ' ' || *p == '\t') p++;
        }

        if (isdigit((unsigned char)*p) || *p == '.') {
            char *end = NULL;
            double v = strtod(p, &end);
            if (end == p) {
                *unknown_name = true;
                return false;
            }
            total += (long)(sign * v);
            p = end;
        } else if (isalpha((unsigned char)*p) || *p == '_') {
            const char *start = p;
            while (isalnum((unsigned char)*p) || *p == '_' || *p == '-') p++;

            char name[128];
            size_t n = (size_t)(p - start);
            if (n >= sizeof name) {
                *unknown_name = true;
                return false;
            }
            memcpy(name, start, n);
            name[n] = '\0';

            bool want_end = false;
            if (*p == '.') {
                p++;
                if (strncmp(p, "end", 3) == 0)        { want_end = true;  p += 3; }
                else if (strncmp(p, "start", 5) == 0) { want_end = false; p += 5; }
                else {
                    *unknown_name = true;
                    return false;
                }
            }

            /* A scene label first — a fixed instant, always available. */
            bool found = false;
            for (size_t l = 0; l < sc->label_count && !found; l++) {
                if (strcmp(sc->label_names[l], name) == 0) {
                    total += sign * sc->label_times[l];
                    found = true;
                }
            }

            if (!found) {
                for (size_t i = 0; i < sc->event_count && !found; i++) {
                    const TimelineEvent *ev = &sc->events[i];
                    if (ev->label == NULL || strcmp(ev->label, name) != 0) {
                        continue;
                    }
                    if (!resolved[i]) {
                        return false;   /* not yet — try again next pass */
                    }
                    total += sign * (ev->time_ms + (want_end ? ev->duration_ms : 0));
                    found = true;
                }
            }

            if (!found) {
                *unknown_name = true;
                return false;
            }
        } else {
            *unknown_name = true;
            return false;
        }

        sign  = 1;
        first = false;
    }

    *out_ms = (int)(total < 0 ? 0 : total);
    return true;
}

/*
 * Turns every `"time": "intro.end + 200"` into milliseconds.
 *
 * Iterative because an event may hang off another that is itself relative.
 * Each pass resolves whatever it can; when a pass achieves nothing, whatever
 * is left is either a cycle or a typo, and both get the same warning — the
 * event stays at its default time rather than the parse failing.
 */
static void resolve_event_times(Scene *sc)
{
    {
        if (sc->event_count == 0) {
            return;
        }

        bool *resolved = (bool *)calloc(sc->event_count, sizeof(bool));
        if (resolved == NULL) {
            return;
        }
        for (size_t i = 0; i < sc->event_count; i++) {
            resolved[i] = (sc->events[i].time_expr == NULL);
        }

        for (size_t pass = 0; pass < sc->event_count + 1; pass++) {
            bool progress = false;

            for (size_t i = 0; i < sc->event_count; i++) {
                if (resolved[i]) {
                    continue;
                }
                int  ms = 0;
                bool bad = false;
                if (resolve_time_expr(sc, sc->events[i].time_expr, &ms, resolved, &bad)) {
                    sc->events[i].time_ms = ms;
                    resolved[i] = true;
                    progress = true;
                } else if (bad) {
                    fprintf(stderr, "warning: scene '%s' — cannot read time \"%s\".\n",
                            sc->id ? sc->id : "(unnamed)", sc->events[i].time_expr);
                    resolved[i] = true;   /* stop retrying a broken expression */
                    progress = true;
                }
            }
            if (!progress) {
                break;
            }
        }

        for (size_t i = 0; i < sc->event_count; i++) {
            if (!resolved[i]) {
                fprintf(stderr, "warning: scene '%s' — time \"%s\" refers to an event "
                                "that never resolves (a cycle?).\n",
                        sc->id ? sc->id : "(unnamed)", sc->events[i].time_expr);
            }
            free(sc->events[i].time_expr);
            sc->events[i].time_expr = NULL;
        }
        free(resolved);
    }
}

/* ------------------------------------------------------------------------- */
/* Resolving relative keyframe times                                          */
/* ------------------------------------------------------------------------- */

/* One track: fractions → seconds, then re-sort if anything moved. */
static void resolve_track(Track *tr, float duration_sec)
{
    if (tr->keys == NULL || tr->count <= 0) {
        return;
    }

    bool touched = false;
    for (int i = 0; i < tr->count; i++) {
        if (tr->keys[i].t_relative) {
            tr->keys[i].t         *= duration_sec;
            tr->keys[i].t_relative = false;
            touched                = true;
        }
    }

    /* Only now can a "50%" key be ordered against a literal 2.0. */
    if (touched) {
        sort_keys(tr->keys, tr->count);
    }
}

static void resolve_effect_times(struct Effect *list, size_t count, float duration_sec)
{
    for (size_t i = 0; i < count; i++) {
        for (int p = 0; p < FXP_MAX; p++) {
            resolve_track(&list[i].param[p], duration_sec);
        }
    }
}

/*
 * Turns every "50%" keyframe time into seconds.
 *
 * Must run after compute_scene_times(), because that is the first moment every
 * scene's duration is final — in flat mode the single scene inherits the
 * project's duration, which is only applied at the very end of parsing.
 *
 * What a percentage is relative to depends on what owns the track:
 *
 *   widget property tracks    the widget's scene       (time is scene-local)
 *   scene effect tracks       that scene
 *   project effect tracks     the whole film           (applied to finished frames)
 *   transition from/to        nothing — already progress in [0,1]
 *
 * That last case is why transitions are handled by clearing the flag rather
 * than scaling: there "50%" already *means* 0.5, and multiplying by a duration
 * would push the key far outside the range the sampler uses.
 */
static void resolve_relative_times(EditorContext *ctx)
{
    for (size_t si = 0; si < ctx->scene_count; si++) {
        Scene *sc  = &ctx->scenes[si];
        float  sec = (float)sc->duration_ms * 0.001f;

        for (size_t w = 0; w < sc->widget_count; w++) {
            WidgetBase *b = ctx->widgets[sc->first_widget + w];
            resolve_track(&b->tr_x,        sec);
            resolve_track(&b->tr_y,        sec);
            resolve_track(&b->tr_opacity,  sec);
            resolve_track(&b->tr_scale,    sec);
            resolve_track(&b->tr_rotation, sec);
        }

        resolve_effect_times(sc->effects, sc->effect_count, sec);
    }

    resolve_effect_times(ctx->effects, ctx->effect_count,
                         (float)ctx->config.duration_ms * 0.001f);

    for (size_t i = 0; i < ctx->transition_count; i++) {
        Transition *tr = &ctx->transitions[i];
        Track      *all[] = {
            &tr->from_opacity, &tr->from_x, &tr->from_y, &tr->from_scale, &tr->from_rotate,
            &tr->to_opacity,   &tr->to_x,   &tr->to_y,   &tr->to_scale,   &tr->to_rotate,
            &tr->from_mask[0], &tr->from_mask[1], &tr->from_mask[2], &tr->from_mask[3],
            &tr->to_mask[0],   &tr->to_mask[1],   &tr->to_mask[2],   &tr->to_mask[3],
        };
        /* A duration of 1.0 leaves the fraction untouched — see above. */
        for (size_t k = 0; k < sizeof all / sizeof all[0]; k++) {
            resolve_track(all[k], 1.0f);
        }
    }
}

/* Scene start times, accounting for transition overlap. */
static void compute_scene_times(EditorContext *ctx)
{
    int t = 0;
    for (size_t i = 0; i < ctx->scene_count; i++) {
        ctx->scenes[i].start_ms = t;

        int overlap = 0;
        if (i < ctx->transition_count) {
            overlap = ctx->transitions[i].duration_ms;

            /* A transition can never be longer than either neighbouring scene. */
            int limit = ctx->scenes[i].duration_ms;
            if (i + 1 < ctx->scene_count && ctx->scenes[i + 1].duration_ms < limit) {
                limit = ctx->scenes[i + 1].duration_ms;
            }
            if (overlap > limit) {
                fprintf(stderr, "warning: transition #%zu (%d ms) is longer than its neighbour — clamped to %d ms.\n",
                        i, overlap, limit);
                overlap = limit;
                ctx->transitions[i].duration_ms = overlap;
            }
        }

        t += ctx->scenes[i].duration_ms - overlap;
    }

    /* The film's total length = the end of the last scene. */
    if (ctx->scene_count > 0) {
        const Scene *last = &ctx->scenes[ctx->scene_count - 1];
        ctx->config.duration_ms = last->start_ms + last->duration_ms;
    }
}

/* Video duration: from the JSON, or from the last timeline event plus a tail. */


/* ------------------------------------------------------------------------- */
/* `repeat` — one object description, many objects                            */
/* ------------------------------------------------------------------------- */

/*
 * A repeated template, remembered so the timeline can be expanded to match.
 *
 * When the author writes an event targeting "dot", they mean all sixteen dots,
 * not a widget that no longer exists under that name. So the ids the expansion
 * produced have to be recoverable when the timeline is read a moment later.
 */
typedef struct {
    char *template_id;   /* the id as written, e.g. "dot"     */
    int   count;
    int   stagger_ms;    /* per-copy delay applied to events  */
} RepeatGroup;

typedef struct {
    RepeatGroup *items;
    size_t       count, cap;
} RepeatTable;

static void repeat_table_free(RepeatTable *t)
{
    for (size_t i = 0; i < t->count; i++) {
        free(t->items[i].template_id);
    }
    free(t->items);
    t->items = NULL;
    t->count = t->cap = 0;
}

static const RepeatGroup *repeat_lookup(const RepeatTable *t, const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->items[i].template_id, id) == 0) {
            return &t->items[i];
        }
    }
    return NULL;
}

/*
 * Places copy `i` of `count` according to the `layout` given.
 *
 * The result is a *displacement*, not a position: it is added on top of
 * whatever the template's own x/y resolve to, so `"x": "center"` and a radial
 * repeat compose instead of fighting.
 */
static void repeat_offset(const cJSON *rep, int i, int count,
                          float *dx, float *dy, float *rot_deg)
{
    const char *layout = json_str(rep, "layout", "radial");

    *dx = 0.0f;
    *dy = 0.0f;

    /*
     * Per-copy rotation.
     *
     * Displacement alone cannot make a fan of spokes: twelve lines with the
     * same endpoints land on top of one another however they are moved. Either
     * a fixed step per copy, or `orient` to face along the radial direction.
     */
    float step = json_float(rep, "rotate_step", 0.0f);
    *rot_deg = step * (float)i;

    if (strcmp(layout, "radial") == 0) {
        float radius = json_float(rep, "radius", 200.0f);
        float a0     = json_float(rep, "angle_start", 0.0f);
        /* A full turn by default, so `count` copies land evenly around it. */
        float sweep  = json_float(rep, "sweep", 360.0f);
        float ang    = (a0 + sweep * (float)i / (float)count) * (float)(M_PI / 180.0);

        *dx = radius * cosf(ang);
        *dy = radius * sinf(ang);

    } else if (strcmp(layout, "grid") == 0) {
        int   cols = json_int(rep, "cols", 4);
        float sx   = json_float(rep, "spacing_x", 100.0f);
        float sy   = json_float(rep, "spacing_y", 100.0f);
        if (cols < 1) {
            cols = 1;
        }
        int rows = (count + cols - 1) / cols;

        /* Centred on the template's position — a grid that grows to one side
         * would make "x": "center" mean something different for every count. */
        *dx = ((float)(i % cols) - (float)(cols - 1) * 0.5f) * sx;
        *dy = ((float)(i / cols) - (float)(rows - 1) * 0.5f) * sy;

    } else if (strcmp(layout, "line") == 0) {
        float sx = json_float(rep, "spacing_x", 100.0f);
        float sy = json_float(rep, "spacing_y", 0.0f);
        *dx = ((float)i - (float)(count - 1) * 0.5f) * sx;
        *dy = ((float)i - (float)(count - 1) * 0.5f) * sy;

    } else if (strcmp(layout, "stack") != 0) {
        fprintf(stderr, "warning: unknown repeat layout '%s' — using 'stack'.\n", layout);
    }

    /* `orient` turns each copy to match its own angle around the circle, which
     * is what makes spokes out of a stack of identical lines. */
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rep, "orient"))) {
        float a0    = json_float(rep, "angle_start", 0.0f);
        float sweep = json_float(rep, "sweep", 360.0f);
        *rot_deg += a0 + sweep * (float)i / (float)count;
    }
    /* "stack": every copy sits at the template's position. Useful when the
     * spread comes from the timeline (a staggered fan-out) rather than here. */
}

/*
 * Expands one template into `count` sibling objects, appended to `out`.
 *
 * This runs entirely at parse time: the renderer never learns that a repeat
 * existed, and every copy is an ordinary object. That keeps a frame a pure
 * function of time — the property --range and the two backends depend on.
 */
static bool repeat_expand(const cJSON *tmpl, const cJSON *rep, cJSON *out, RepeatTable *table)
{
    int count = json_int(rep, "count", 0);
    if (count < 1) {
        fprintf(stderr, "warning: repeat.count = %d — object skipped.\n", count);
        return true;
    }
    if (count > 4096) {
        fprintf(stderr, "warning: repeat.count = %d capped at 4096.\n", count);
        count = 4096;
    }

    const char *base_id = json_str(tmpl, "id", NULL);
    const cJSON *cycle  = cJSON_GetObjectItemCaseSensitive(rep, "color_cycle");
    int          ncol   = cJSON_IsArray(cycle) ? cJSON_GetArraySize(cycle) : 0;
    int          stagger = json_int(rep, "stagger_ms", 0);

    for (int i = 0; i < count; i++) {
        cJSON *copy = cJSON_Duplicate(tmpl, 1);
        if (copy == NULL) {
            return false;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(copy, "repeat");

        /* Unique ids so the validator's duplicate check stays meaningful and
         * a single copy can still be addressed directly if wanted. */
        if (base_id != NULL) {
            char id[256];
            snprintf(id, sizeof id, "%s#%d", base_id, i);
            cJSON_DeleteItemFromObjectCaseSensitive(copy, "id");
            cJSON_AddStringToObject(copy, "id", id);
        }

        float dx, dy, rot;
        repeat_offset(rep, i, count, &dx, &dy, &rot);
        cJSON_AddNumberToObject(copy, "_repeat_dx", dx);
        cJSON_AddNumberToObject(copy, "_repeat_dy", dy);
        cJSON_AddNumberToObject(copy, "_repeat_rot", rot);

        if (ncol > 0) {
            const cJSON *c = cJSON_GetArrayItem(cycle, i % ncol);
            if (cJSON_IsString(c) && c->valuestring != NULL) {
                /*
                 * Different types name their colour differently: shapes use
                 * "color", code blocks "fg", lines and paths "stroke". Setting
                 * whichever the template already carries avoids adding a key
                 * the object would ignore — which is what silently made a
                 * `color_cycle` do nothing on a path.
                 */
                const char *key = json_has(copy, "fg")     ? "fg"
                                : json_has(copy, "stroke") ? "stroke"
                                                           : "color";
                cJSON_DeleteItemFromObjectCaseSensitive(copy, key);
                cJSON_AddStringToObject(copy, key, c->valuestring);
            }
        }

        cJSON_AddItemToArray(out, copy);
    }

    if (base_id != NULL) {
        RepeatGroup *g = (RepeatGroup *)array_push((void **)&table->items, &table->count,
                                                   &table->cap, sizeof(RepeatGroup));
        if (g == NULL) {
            return false;
        }
        g->template_id = dup_string(base_id);
        g->count       = count;
        g->stagger_ms  = stagger;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* `counter` — a number that changes over time                                */
/* ------------------------------------------------------------------------- */

/*
 * Expands a counter into one text object per displayed value, each visible for
 * its own slice of the timeline.
 *
 * The obvious implementation — re-rasterizing the text every frame — would
 * break the invariant the whole renderer is built on: a texture is drawn once
 * and composited many times. So the values are enumerated instead.
 *
 * That is only affordable because a counter does not need to change every
 * frame. At the default twelve updates a second a five-second count is sixty
 * small textures, where per-frame would be three hundred and a rasterizer call
 * inside the render loop.
 */
static bool counter_expand(const cJSON *tmpl, const cJSON *cnt, cJSON *out)
{
    double from     = json_float(cnt, "from", 0.0f);
    double to       = json_float(cnt, "to", 100.0f);
    int    decimals = json_int(cnt, "decimals", 0);
    int    rate     = json_int(cnt, "rate", 12);          /* updates per second */
    int    start_ms = json_int(cnt, "start_ms", 0);
    int    dur_ms   = json_has(cnt, "duration_ms")
                          ? json_int(cnt, "duration_ms", 1000)
                          : (int)(json_float(cnt, "duration", 1.0f) * 1000.0f);

    const char *prefix = json_str(cnt, "prefix", "");
    const char *suffix = json_str(cnt, "suffix", "");
    const char *ease   = json_str(cnt, "ease", "cubicout");
    bool        group  = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cnt, "thousands"));

    if (decimals < 0) decimals = 0;
    if (decimals > 6) decimals = 6;
    if (rate < 1)     rate = 1;
    if (rate > 60)    rate = 60;
    if (dur_ms < 1)   dur_ms = 1;

    int steps = (int)((double)dur_ms / 1000.0 * rate);
    if (steps < 1)    steps = 1;
    if (steps > 2048) steps = 2048;   /* a runaway rate must not eat all of RAM */

    const char *base_id = json_str(tmpl, "id", "counter");
    EaseType    et      = easing_from_name(ease);

    char prev[256] = { 0 };
    for (int i = 0; i <= steps; i++) {
        float  p = (steps > 0) ? (float)i / (float)steps : 1.0f;
        double v = from + (to - from) * (double)easing_apply(et, p);

        char num[192];
        snprintf(num, sizeof num, "%.*f", decimals, v);

        if (group) {
            /* Thousands separators, inserted from the right of the integer
             * part so the decimals are left alone. */
            char *dot  = strchr(num, '.');
            int   ilen = (int)(dot ? (size_t)(dot - num) : strlen(num));
            int   neg  = (num[0] == '-') ? 1 : 0;
            char  tmp[192];
            int   w = 0;
            for (int k = 0; k < ilen && w < (int)sizeof tmp - 1; k++) {
                if (k > neg && ((ilen - k) % 3) == 0) {
                    tmp[w++] = ' ';
                }
                tmp[w++] = num[k];
            }
            for (const char *q = num + ilen; *q != '\0' && w < (int)sizeof tmp - 1; q++) {
                tmp[w++] = *q;
            }
            tmp[w] = '\0';
            snprintf(num, sizeof num, "%s", tmp);
        }

        char text[256];
        snprintf(text, sizeof text, "%s%s%s", prefix, num, suffix);

        /*
         * Steps that render the same string are merged: at a low `decimals`
         * most of them do, and one object per identical frame is pure waste.
         */
        if (i > 0 && strcmp(text, prev) == 0) {
            continue;
        }
        snprintf(prev, sizeof prev, "%s", text);

        int t0 = start_ms + (int)((double)dur_ms * i / (double)(steps + 1));
        int t1 = start_ms + (int)((double)dur_ms * (i + 1) / (double)(steps + 1));
        if (i == steps) {
            t1 = start_ms + dur_ms + 3600000;   /* the last value holds */
        }

        cJSON *copy = cJSON_Duplicate(tmpl, 1);
        if (copy == NULL) {
            return false;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(copy, "counter");
        cJSON_DeleteItemFromObjectCaseSensitive(copy, "content");
        cJSON_DeleteItemFromObjectCaseSensitive(copy, "id");
        cJSON_DeleteItemFromObjectCaseSensitive(copy, "opacity");
        cJSON_DeleteItemFromObjectCaseSensitive(copy, "type");

        char id[256];
        snprintf(id, sizeof id, "%s$%d", base_id, i);
        cJSON_AddStringToObject(copy, "id", id);
        cJSON_AddStringToObject(copy, "type", "text");
        cJSON_AddStringToObject(copy, "content", text);

        /*
         * A hard on/off window rather than a crossfade. Two numbers dissolving
         * through each other reads as a blur, not as a count — and the point of
         * enumerating values is that exactly one is on screen.
         */
        cJSON *op = cJSON_CreateArray();
        if (op == NULL) {
            cJSON_Delete(copy);
            return false;
        }
        const double kt[] = { 0.0, (t0 - 1) / 1000.0, t0 / 1000.0,
                              (t1 - 1) / 1000.0, t1 / 1000.0 };
        const double kv[] = { 0.0, 0.0, 1.0, 1.0, 0.0 };
        for (size_t j = 0; j < sizeof kt / sizeof kt[0]; j++) {
            if (kt[j] < 0.0) {
                continue;
            }
            cJSON *kf = cJSON_CreateObject();
            cJSON_AddNumberToObject(kf, "t", kt[j]);
            cJSON_AddNumberToObject(kf, "v", kv[j]);
            cJSON_AddStringToObject(kf, "ease", "linear");
            cJSON_AddItemToArray(op, kf);
        }
        cJSON_AddItemToObject(copy, "opacity", op);

        cJSON_AddItemToArray(out, copy);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* `emitter` — one description, a burst of particles                          */
/* ------------------------------------------------------------------------- */

/*
 * A deterministic pseudo-random stream.
 *
 * rand() is deliberately avoided: it would make a render depend on libc and on
 * how many times anything else happened to call it, so the same project could
 * produce different frames on two machines. This hash is fixed, seeded from the
 * JSON, and therefore reproducible everywhere — the same property --range and
 * the two backends already rely on.
 */
static float emit_rand(unsigned int *state)
{
    unsigned int x = (*state += 0x9E3779B9u);
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return (float)(x & 0x00FFFFFFU) / (float)0x01000000U;
}

/* A number, or a [min,max] pair sampled uniformly. */
static float emit_range(const cJSON *obj, const char *key, float lo, float hi,
                        unsigned int *state)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (cJSON_IsNumber(it)) {
        return (float)it->valuedouble;
    }
    if (cJSON_IsArray(it) && cJSON_GetArraySize(it) >= 2) {
        float a = (float)cJSON_GetArrayItem(it, 0)->valuedouble;
        float b = (float)cJSON_GetArrayItem(it, 1)->valuedouble;
        return a + (b - a) * emit_rand(state);
    }
    return lo + (hi - lo) * emit_rand(state);
}

/*
 * Expands an emitter into `count` particles: one object each, plus one `emit`
 * event each carrying that particle's velocity and lifetime.
 *
 * Why events rather than keyframes: a ballistic path is a closed form, so the
 * renderer needs five numbers per particle instead of a keyframe array. A
 * seven-hundred-particle burst therefore costs about 28 KB of events, and any
 * frame in the middle of it can still be rendered on its own.
 */
static bool emitter_expand(const cJSON *tmpl, const cJSON *em, cJSON *out,
                           cJSON *events, int canvas_w, int canvas_h)
{
    int count = json_int(em, "count", 0);
    if (count < 1) {
        fprintf(stderr, "warning: emitter.count = %d — object skipped.\n", count);
        return true;
    }
    if (count > 4096) {
        fprintf(stderr, "warning: emitter.count = %d capped at 4096.\n", count);
        count = 4096;
    }

    unsigned int state = (unsigned int)json_int(em, "seed", 1) * 2654435761u + 12345u;

    const char *base_id = json_str(tmpl, "id", NULL);
    const cJSON *cycle  = cJSON_GetObjectItemCaseSensitive(em, "color_cycle");
    int          ncol   = cJSON_IsArray(cycle) ? cJSON_GetArraySize(cycle) : 0;

    float ox = json_canvas_coord(em, "cx", (float)canvas_w, LAYOUT_AXIS_X, canvas_w * 0.5f);
    float oy = json_canvas_coord(em, "cy", (float)canvas_h, LAYOUT_AXIS_Y, canvas_h * 0.5f);

    int   emit_ms  = json_int(em, "emit_ms", 0);       /* how long spawning lasts */
    int   start_ms = json_int(em, "start_ms", 0);
    float gravity  = json_float(em, "gravity", 0.0f);
    float fade     = json_float(em, "fade", 0.35f);

    for (int i = 0; i < count; i++) {
        cJSON *copy = cJSON_Duplicate(tmpl, 1);
        if (copy == NULL) {
            return false;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(copy, "emitter");

        char id[256];
        snprintf(id, sizeof id, "%s@%d", base_id ? base_id : "p", i);
        cJSON_DeleteItemFromObjectCaseSensitive(copy, "id");
        cJSON_AddStringToObject(copy, "id", id);

        /*
         * --- spawn ---
         *
         * `spread` is a disc; `spread_x`/`spread_y` override each axis so an
         * emitter can be a line rather than a circle. A strip along the bottom
         * of the frame is a common enough case that a radial-only spread would
         * scatter half the particles off-canvas to achieve it.
         */
        float sa = emit_rand(&state) * 6.28318530718f;
        float sr = sqrtf(emit_rand(&state));       /* even coverage of the area */

        float spread = emit_range(em, "spread", 0.0f, 0.0f, &state);
        float sx_r   = json_has(em, "spread_x")
                           ? emit_range(em, "spread_x", 0.0f, 0.0f, &state) : spread;
        float sy_r   = json_has(em, "spread_y")
                           ? emit_range(em, "spread_y", 0.0f, 0.0f, &state) : spread;

        cJSON_DeleteItemFromObjectCaseSensitive(copy, "x");
        cJSON_DeleteItemFromObjectCaseSensitive(copy, "y");
        cJSON_AddNumberToObject(copy, "x", ox + sx_r * sr * cosf(sa));
        cJSON_AddNumberToObject(copy, "y", oy + sy_r * sr * sinf(sa));

        /* --- velocity --- */
        float ang   = emit_range(em, "angle", 0.0f, 360.0f, &state) * (float)(M_PI / 180.0);
        float speed = emit_range(em, "speed", 100.0f, 300.0f, &state);
        float life  = emit_range(em, "life", 1.0f, 2.0f, &state);
        if (life < 0.05f) {
            life = 0.05f;
        }

        /* --- per-particle size and colour --- */
        const cJSON *sj = cJSON_GetObjectItemCaseSensitive(em, "size_jitter");
        if (sj != NULL) {
            float k = emit_range(em, "size_jitter", 1.0f, 1.0f, &state);
            cJSON_DeleteItemFromObjectCaseSensitive(copy, "scale");
            cJSON_AddNumberToObject(copy, "scale", k);
        }
        if (ncol > 0) {
            int c = (int)(emit_rand(&state) * (float)ncol);
            if (c >= ncol) c = ncol - 1;
            const cJSON *cc = cJSON_GetArrayItem(cycle, c);
            if (cJSON_IsString(cc) && cc->valuestring != NULL) {
                const char *key = json_has(copy, "fg")     ? "fg"
                                : json_has(copy, "stroke") ? "stroke"
                                                           : "color";
                cJSON_DeleteItemFromObjectCaseSensitive(copy, key);
                cJSON_AddStringToObject(copy, key, cc->valuestring);
            }
        }

        cJSON_AddItemToArray(out, copy);

        /* --- the particle's own event --- */
        cJSON *ev = cJSON_CreateObject();
        if (ev == NULL) {
            return false;
        }
        cJSON_AddStringToObject(ev, "action", "emit");
        cJSON_AddStringToObject(ev, "target", id);
        cJSON_AddNumberToObject(ev, "time_ms",
                                start_ms + (int)(emit_rand(&state) * (float)emit_ms));
        cJSON_AddNumberToObject(ev, "duration_ms", (int)(life * 1000.0f));
        cJSON_AddNumberToObject(ev, "vx", cosf(ang) * speed);
        cJSON_AddNumberToObject(ev, "vy", sinf(ang) * speed);
        cJSON_AddNumberToObject(ev, "gravity", gravity);
        cJSON_AddNumberToObject(ev, "fade", fade);
        cJSON_AddNumberToObject(ev, "spin", emit_range(em, "spin", 0.0f, 0.0f, &state));
        cJSON_AddItemToArray(events, ev);
    }
    return true;
}

/*
 * Walks an "objects" array and returns a new array with every `repeat`
 * expanded. Objects without a `repeat` are copied through untouched.
 *
 * The caller owns the result and must cJSON_Delete it.
 */
static cJSON *repeat_expand_all(const cJSON *objects, RepeatTable *table,
                                cJSON *emitted_events, int canvas_w, int canvas_h)
{
    cJSON *out = cJSON_CreateArray();
    if (out == NULL) {
        return NULL;
    }

    const cJSON *obj = NULL;
    cJSON_ArrayForEach(obj, objects) {
        const cJSON *rep = cJSON_GetObjectItemCaseSensitive(obj, "repeat");
        const cJSON *em  = cJSON_GetObjectItemCaseSensitive(obj, "emitter");
        const cJSON *cnt = cJSON_GetObjectItemCaseSensitive(obj, "counter");

        if (cJSON_IsObject(cnt)) {
            if (!counter_expand(obj, cnt, out)) {
                cJSON_Delete(out);
                return NULL;
            }
        } else if (cJSON_IsObject(em)) {
            if (!emitter_expand(obj, em, out, emitted_events, canvas_w, canvas_h)) {
                cJSON_Delete(out);
                return NULL;
            }
        } else if (cJSON_IsObject(rep)) {
            if (!repeat_expand(obj, rep, out, table)) {
                cJSON_Delete(out);
                return NULL;
            }
        } else {
            cJSON *copy = cJSON_Duplicate(obj, 1);
            if (copy == NULL) {
                cJSON_Delete(out);
                return NULL;
            }
            cJSON_AddItemToArray(out, copy);
        }
    }
    return out;
}

/* ------------------------------------------------------------------------- */
/* Parsing a scene                                                            */
/* ------------------------------------------------------------------------- */

/*
 * Reads one scene: its objects, timeline and duration.
 *
 * Flat mode uses the same function — there `node` is the root itself and there
 * is a single scene. One code path means both modes behave identically.
 */
static bool parse_scene(EditorContext *ctx, const cJSON *node, const cJSON *styles,
                        const char *filepath, int *z, bool is_root,
                        const EaseTable *eases)
{
    Scene *sc = (Scene *)array_push((void **)&ctx->scenes, &ctx->scene_count,
                                    &ctx->scene_cap, sizeof(Scene));
    if (sc == NULL) {
        return false;
    }

    sc->id           = dup_string(json_str(node, "id", NULL));
    sc->first_widget = ctx->text_count + ctx->code_count + ctx->image_count + ctx->shape_count +
                       ctx->line_count + ctx->path_count +
                       ctx->video_count + ctx->mesh_count;

    if (json_has(node, "bg_color")) {
        sc->bg_color = json_color(node, "bg_color", ctx->config.bg_color);
        sc->has_bg   = true;
    }

    /* --- objects --- */
    const cJSON *objects = cJSON_GetObjectItemCaseSensitive(node, "objects");
    if (!cJSON_IsArray(objects)) {
        objects = cJSON_GetObjectItemCaseSensitive(node, "layers"); /* videogen synonym */
    }

    /* --- labels --- */
    const cJSON *labels = cJSON_GetObjectItemCaseSensitive(node, "labels");
    if (cJSON_IsObject(labels)) {
        const cJSON *lb = NULL;
        cJSON_ArrayForEach(lb, labels) {
            char **nm = (char **)array_push((void **)&sc->label_names, &sc->label_count,
                                            &sc->label_cap, sizeof(char *));
            if (nm == NULL) {
                return false;
            }
            /* The times array is grown in lockstep, so one index serves both. */
            int *tm = (int *)realloc(sc->label_times, sc->label_cap * sizeof(int));
            if (tm == NULL) {
                return false;
            }
            sc->label_times = tm;

            *nm = dup_string(lb->string ? lb->string : "");
            sc->label_times[sc->label_count - 1] =
                cJSON_IsNumber(lb) ? (int)lb->valuedouble : 0;
        }
    }

    /* --- camera --- */
    const cJSON *cam = cJSON_GetObjectItemCaseSensitive(node, "camera");
    if (cJSON_IsObject(cam)) {
        sc->camera.present = true;
        parse_track_ex(cJSON_GetObjectItemCaseSensitive(cam, "zoom"),
                       &sc->camera.zoom, 1.0f, eases);
        parse_track_ex(cJSON_GetObjectItemCaseSensitive(cam, "x"),
                       &sc->camera.x, 0.0f, eases);
        parse_track_ex(cJSON_GetObjectItemCaseSensitive(cam, "y"),
                       &sc->camera.y, 0.0f, eases);
        parse_track_ex(cJSON_GetObjectItemCaseSensitive(cam, "rotation"),
                       &sc->camera.rotation, 0.0f, eases);
        parse_track_ex(cJSON_GetObjectItemCaseSensitive(cam, "shake"),
                       &sc->camera.shake, 0.0f, eases);

        /* Focal length in pixels; 0 (the default) means no projection. */
        sc->camera.focal = json_float(cam, "perspective", json_float(cam, "focal", 0.0f));

        /*
         * A camera that moves through the space. Any of these keys switches the
         * view from the fixed default to a look-at built from them, so a scene
         * that never mentions them behaves exactly as before.
         */
        static const char *kEye[] = { "px", "py", "pz", "tx", "ty", "tz", "roll" };
        Track *dst[] = { &sc->camera.eye.px, &sc->camera.eye.py, &sc->camera.eye.pz,
                         &sc->camera.eye.tx, &sc->camera.eye.ty, &sc->camera.eye.tz,
                         &sc->camera.eye.roll };
        const float defs[] = { 0, 0, -sc->camera.focal, 0, 0, 0, 0 };

        for (size_t ci = 0; ci < sizeof kEye / sizeof kEye[0]; ci++) {
            if (json_has(cam, kEye[ci])) {
                sc->camera.eye.moving = true;
            }
            parse_track_ex(cJSON_GetObjectItemCaseSensitive(cam, kEye[ci]),
                           dst[ci], defs[ci], eases);
        }
    }

    /* --- groups --- */
    const cJSON *groups = cJSON_GetObjectItemCaseSensitive(node, "groups");
    if (cJSON_IsArray(groups)) {
        const cJSON *g = NULL;
        cJSON_ArrayForEach(g, groups) {
            if (sc->group_count >= VR_MAX_GROUPS) {
                fprintf(stderr, "warning: more than %d groups in one scene — the rest ignored.\n",
                        VR_MAX_GROUPS);
                break;
            }
            GroupDef *gd = (GroupDef *)array_push((void **)&sc->groups, &sc->group_count,
                                                  &sc->group_cap, sizeof(GroupDef));
            if (gd == NULL) {
                return false;
            }
            gd->id = dup_string(json_str(g, "id", NULL));

            /* Like an orbit centre: a point on the canvas, so an expression can
             * be resolved now — a group has no size to defer to. */
            gd->pivot_x = json_canvas_coord(g, "pivot_x", (float)ctx->config.width,
                                            LAYOUT_AXIS_X, (float)ctx->config.width * 0.5f);
            gd->pivot_y = json_canvas_coord(g, "pivot_y", (float)ctx->config.height,
                                            LAYOUT_AXIS_Y, (float)ctx->config.height * 0.5f);
        }
    }

    /*
     * `repeat` is expanded here, before anything else looks at the array, so
     * the rest of the parser — and the whole renderer — only ever sees plain
     * objects.
     */
    RepeatTable repeats;
    memset(&repeats, 0, sizeof repeats);

    cJSON *expanded = NULL;
    cJSON *emitted  = cJSON_CreateArray();   /* events an emitter generated */
    if (emitted == NULL) {
        repeat_table_free(&repeats);
        return false;
    }

    if (cJSON_IsArray(objects)) {
        expanded = repeat_expand_all(objects, &repeats, emitted,
                                     ctx->config.width, ctx->config.height);
        if (expanded == NULL) {
            fprintf(stderr, "error: could not expand a repeat or emitter block.\n");
            cJSON_Delete(emitted);
            repeat_table_free(&repeats);
            return false;
        }
        objects = expanded;
    }

    if (cJSON_IsArray(objects)) {
        cJSON *obj = NULL;
        cJSON_ArrayForEach(obj, objects) {
            apply_styles(obj, styles);

            const char *type = json_str(obj, "type", "text");
            bool        ok;

            if (strcmp(type, "code") == 0) {
                ok = parse_code_object(ctx, obj, *z, eases);
            } else if (strcmp(type, "text") == 0) {
                ok = parse_text_object(ctx, obj, *z, eases);
            } else if (strcmp(type, "image") == 0) {
                ok = parse_image_object(ctx, obj, *z, filepath, eases);
            } else if (strcmp(type, "rect") == 0) {
                ok = parse_shape_object(ctx, obj, *z, WIDGET_RECT, eases);
            } else if (strcmp(type, "circle") == 0) {
                ok = parse_shape_object(ctx, obj, *z, WIDGET_CIRCLE, eases);
            } else if (strcmp(type, "line") == 0) {
                ok = parse_line_object(ctx, obj, *z, eases);
            } else if (strcmp(type, "path") == 0 || strcmp(type, "polyline") == 0) {
                ok = parse_path_object(ctx, obj, *z, eases);
            } else if (strcmp(type, "video") == 0) {
                ok = parse_video_object(ctx, obj, *z, filepath, eases);
            } else if (strcmp(type, "mesh") == 0 || strcmp(type, "model") == 0) {
                ok = parse_mesh_object(ctx, obj, *z, filepath, eases);
            } else {
                fprintf(stderr, "warning: unknown object type '%s' — skipped.\n", type);
                continue;
            }

            if (!ok) {
                fprintf(stderr, "error: could not allocate an object.\n");
                cJSON_Delete(expanded);
                cJSON_Delete(emitted);
                repeat_table_free(&repeats);
                return false;
            }
            (*z)++;   /* z increases globally → a scene's objects stay adjacent in the index */
        }
    }

    sc->widget_count = (ctx->text_count + ctx->code_count + ctx->image_count +
                        ctx->shape_count + ctx->line_count +
                        ctx->path_count + ctx->video_count +
                        ctx->mesh_count) - sc->first_widget;

    /*
     * --- the scene's own effects ---
     *
     * In flat mode `node` is the root itself, so the "effects" here is the
     * *global* stack, read separately by parse_video_project_ex(). Reading it
     * a second time meant every effect was applied to the frame twice.
     */
    /*
     * A point light. Naming it switches meshes from camera-mounted shading to
     * being lit from this position, which is what puts a terminator on a body.
     */
    const cJSON *lit = cJSON_GetObjectItemCaseSensitive(node, "light");
    if (cJSON_IsObject(lit) || cJSON_IsArray(lit)) {
        /* One light or several: an object is the common case and an array the
         * same thing repeated, so both spellings resolve to the same table. */
        const cJSON *first = cJSON_IsArray(lit) ? lit->child : lit;
        for (const cJSON *l = first; l != NULL && sc->light_count < VR_MAX_LIGHTS;
             l = cJSON_IsArray(lit) ? l->next : NULL) {
            if (!cJSON_IsObject(l)) {
                continue;
            }
            int         idx = sc->light_count++;
            LightTrack *anim = &sc->light_anim[idx];

            /*
             * Every channel is a track. A plain number parses into a constant
             * one, so a scene whose lights never move costs five constants and
             * behaves exactly as it did when these were bare floats.
             */
            static const char *const kLightKeys[] = {
                "x", "y", "z", "intensity", "range", "dx", "dy", "dz"
            };
            static const float kLightDefs[] = {
                0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f
            };
            Track *slots[] = { &anim->x, &anim->y, &anim->z,
                               &anim->intensity, &anim->range,
                               &anim->dx, &anim->dy, &anim->dz };

            for (size_t q = 0; q < sizeof slots / sizeof *slots; q++) {
                parse_track_ex(cJSON_GetObjectItemCaseSensitive(l, kLightKeys[q]),
                               slots[q], kLightDefs[q], eases);
            }

            /* The value at t=0, so anything reading the scene before a frame is
             * rendered — --check, --dump — sees a sensible light rather than
             * zeros. The renderer overwrites this every frame. */
            sc->lights[idx].x         = track_sample(&anim->x, 0.0f);
            sc->lights[idx].y         = track_sample(&anim->y, 0.0f);
            sc->lights[idx].z         = track_sample(&anim->z, 0.0f);
            sc->lights[idx].intensity = track_sample(&anim->intensity, 0.0f);
            sc->lights[idx].range     = track_sample(&anim->range, 0.0f);

            /*
             * The kind of source. "point" is the default and what every scene
             * written before this got, so leaving `type` out changes nothing.
             */
            const char *lt = json_str(l, "type", "point");
            if (strcmp(lt, "sun") == 0 || strcmp(lt, "directional") == 0) {
                sc->lights[idx].type = VR_LIGHT_DIR;
            } else if (strcmp(lt, "spot") == 0) {
                sc->lights[idx].type = VR_LIGHT_SPOT;
            } else {
                sc->lights[idx].type = VR_LIGHT_POINT;
            }

            /*
             * A cone given in degrees from the axis, which is how a lamp is
             * described; the renderer wants cosines and converts once here
             * rather than per pixel. `penumbra` is the soft margin *outside*
             * the stated angle, so "angle" stays the edge of full brightness.
             */
            float ang = json_float(l, "angle", 35.0f);
            float pen = json_float(l, "penumbra", 8.0f);
            ang = clampf(ang, 0.5f, 89.0f);
            pen = clampf(pen, 0.0f, 89.0f);
            sc->lights[idx].cos_inner = cosf(ang * (float)(M_PI / 180.0));
            sc->lights[idx].cos_outer = cosf(clampf(ang + pen, 0.5f, 89.9f)
                                             * (float)(M_PI / 180.0));

            /*
             * A spot may be aimed at a point instead of given a direction,
             * which is nearly always the more natural way to say it: you point
             * a lamp *at* something.
             */
            const cJSON *tgt = cJSON_GetObjectItemCaseSensitive(l, "target");
            if (cJSON_IsArray(tgt) && cJSON_GetArraySize(tgt) >= 3) {
                float t3[3] = { 0.0f, 0.0f, 0.0f };
                for (int c = 0; c < 3; c++) {
                    const cJSON *v = cJSON_GetArrayItem(tgt, c);
                    if (cJSON_IsNumber(v)) t3[c] = (float)v->valuedouble;
                }
                track_set_constant(&anim->dx, t3[0] - sc->lights[idx].x);
                track_set_constant(&anim->dy, t3[1] - sc->lights[idx].y);
                track_set_constant(&anim->dz, t3[2] - sc->lights[idx].z);
            }
            sc->lights[idx].dx = track_sample(&anim->dx, 0.0f);
            sc->lights[idx].dy = track_sample(&anim->dy, 0.0f);
            sc->lights[idx].dz = track_sample(&anim->dz, 0.0f);
        }
        if (cJSON_IsArray(lit) && cJSON_GetArraySize(lit) > VR_MAX_LIGHTS) {
            fprintf(stderr, "warning: scene '%s' — %d lights given, only %d are used.\n",
                    sc->id ? sc->id : "(unnamed)", cJSON_GetArraySize(lit), VR_MAX_LIGHTS);
        }
    }

    if (!is_root &&
        !parse_effects_into(&sc->effects, &sc->effect_count, &sc->effect_cap,
                            cJSON_GetObjectItemCaseSensitive(node, "effects"),
                            filepath)) {
        cJSON_Delete(expanded);
        cJSON_Delete(emitted);
        repeat_table_free(&repeats);
        return false;
    }

    /*
     * --- timeline ---
     *
     * An event aimed at a repeated template ("dot") is expanded to one event
     * per copy ("dot#0" … "dot#15"). Without this a `repeat` would be useless
     * for anything animated: the author would have to write sixteen events
     * naming ids the expansion invented.
     *
     * `stagger_ms` delays each successive copy, which is what turns a ring of
     * dots into a wave travelling around it.
     */
    const cJSON *timeline = cJSON_GetObjectItemCaseSensitive(node, "timeline");
    if (cJSON_IsArray(timeline)) {
        const cJSON *ev = NULL;
        cJSON_ArrayForEach(ev, timeline) {
            const RepeatGroup *g = repeat_lookup(&repeats, json_str(ev, "target", NULL));

            if (g == NULL) {
                if (!parse_timeline_event(ctx, sc, ev, eases)) {
                    fprintf(stderr, "error: could not allocate a timeline event.\n");
                    cJSON_Delete(expanded);
                    cJSON_Delete(emitted);
                    repeat_table_free(&repeats);
                    return false;
                }
                continue;
            }

            for (int i = 0; i < g->count; i++) {
                cJSON *copy = cJSON_Duplicate(ev, 1);
                if (copy == NULL) {
                    cJSON_Delete(expanded);
                    cJSON_Delete(emitted);
                    repeat_table_free(&repeats);
                    return false;
                }

                char id[256];
                snprintf(id, sizeof id, "%s#%d", g->template_id, i);
                cJSON_DeleteItemFromObjectCaseSensitive(copy, "target");
                cJSON_AddStringToObject(copy, "target", id);

                if (g->stagger_ms != 0) {
                    int t = json_int(copy, "time_ms", 0) + g->stagger_ms * i;
                    if (t < 0) {
                        t = 0;
                    }
                    cJSON_DeleteItemFromObjectCaseSensitive(copy, "time_ms");
                    cJSON_AddNumberToObject(copy, "time_ms", t);
                }

                bool ok_ev = parse_timeline_event(ctx, sc, copy, eases);
                cJSON_Delete(copy);

                if (!ok_ev) {
                    fprintf(stderr, "error: could not allocate a timeline event.\n");
                    cJSON_Delete(expanded);
                    cJSON_Delete(emitted);
                    repeat_table_free(&repeats);
                    return false;
                }
            }
        }
    }

    /*
     * Emitter-generated events, appended after the hand-written ones so an
     * author's event on the same object still has the last word.
     */
    {
        const cJSON *ev = NULL;
        cJSON_ArrayForEach(ev, emitted) {
            if (!parse_timeline_event(ctx, sc, ev, eases)) {
                fprintf(stderr, "error: could not allocate a particle event.\n");
                cJSON_Delete(expanded);
                cJSON_Delete(emitted);
                repeat_table_free(&repeats);
                return false;
            }
        }
    }

    /*
     * Relative times are resolved here rather than after every scene is read,
     * because the scene's own duration may be derived from its last event —
     * and that has to happen with real numbers, not with unresolved
     * expressions all reading as zero.
     *
     * Labels are scene-local anyway, so nothing is lost by resolving early.
     */
    resolve_event_times(sc);

    /* --- duration --- */
    if (json_has(node, "duration_ms")) {
        sc->duration_ms = json_int(node, "duration_ms", 0);
    } else if (json_has(node, "duration")) {
        sc->duration_ms = (int)(json_float(node, "duration", 0.0f) * 1000.0f);
    }

    if (sc->duration_ms <= 0) {
        int last = 0;
        for (size_t i = 0; i < sc->event_count; i++) {
            int end = sc->events[i].time_ms + sc->events[i].duration_ms;
            if (end > last) {
                last = end;
            }
        }
        sc->duration_ms = (last > 0) ? last + 1000 : 3000;
    }

    cJSON_Delete(expanded);      /* NULL-safe; only set when a repeat existed */
    cJSON_Delete(emitted);
    repeat_table_free(&repeats);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Main entry point                                                           */
/* ------------------------------------------------------------------------- */

EditorContext *parse_video_project(const char *filepath)
{
    return parse_video_project_ex(filepath, NULL, 0);
}

EditorContext *parse_video_project_ex(const char *filepath, char **defines, int define_count)
{
    if (filepath == NULL) {
        return NULL;
    }

    char *json_text = read_file_to_string(filepath, NULL);
    if (json_text == NULL) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(json_text);
    free(json_text); /* cJSON has already taken its own copies */

    if (root == NULL) {
        const char *err = cJSON_GetErrorPtr();
        fprintf(stderr, "error parsing JSON%s%.40s\n",
                err ? " near: " : ".", err ? err : "");
        return NULL;
    }

    /* --- 0. Variables: the JSON's "vars" plus --set from the CLI --------- */
    cJSON *vars = cJSON_GetObjectItemCaseSensitive(root, "vars");
    if (vars == NULL) {
        vars = cJSON_AddObjectToObject(root, "vars");
    }

    for (int i = 0; i < define_count; i++) {
        const char *eq = strchr(defines[i], '=');
        if (eq == NULL) {
            fprintf(stderr, "warning: --set '%s' contains no '='.\n", defines[i]);
            continue;
        }
        size_t klen = (size_t)(eq - defines[i]);
        char   key[128];
        if (klen >= sizeof key) {
            klen = sizeof key - 1;
        }
        memcpy(key, defines[i], klen);
        key[klen] = '\0';

        /* The command line overrides the JSON's value. */
        cJSON_DeleteItemFromObjectCaseSensitive(vars, key);
        cJSON_AddStringToObject(vars, key, eq + 1);
    }

    if (vars->child != NULL) {
        substitute_vars(root->child, vars);
    }

    const cJSON *styles = cJSON_GetObjectItemCaseSensitive(root, "styles");

    EditorContext *ctx = (EditorContext *)calloc(1, sizeof(EditorContext));
    if (ctx == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    /* --- 1. project { } --------------------------------------------------- */
    /*
     * Custom easing curves, read before anything that could reference one.
     * Passed down by pointer rather than stashed anywhere: the project's rule
     * is that state travels through the call chain, not through globals.
     */
    EaseTable eases;
    parse_ease_table(&eases, cJSON_GetObjectItemCaseSensitive(root, "eases"));

    const cJSON *project = cJSON_GetObjectItemCaseSensitive(root, "project");
    ctx->config.width       = json_int(project, "width", 1920);
    ctx->config.height      = json_int(project, "height", 1080);
    ctx->config.fps         = json_int(project, "fps", 60);
    ctx->config.bg_color    = json_color(project, "bg_color", (Color){ 0, 0, 0, 255 });
    ctx->config.duration_ms = json_int(project, "duration_ms", 0);

    /* --- motion blur ------------------------------------------------------ */
    {
        const cJSON *mb = cJSON_GetObjectItemCaseSensitive(project, "motion_blur");
        if (mb == NULL) {
            mb = cJSON_GetObjectItemCaseSensitive(root, "motion_blur");
        }
        ctx->config.mb_samples = 0;
        ctx->config.mb_shutter = 0.5f;

        if (cJSON_IsNumber(mb)) {
            /* "motion_blur": 8 — the sample count alone, at a 180° shutter. */
            ctx->config.mb_samples = (int)mb->valuedouble;
        } else if (cJSON_IsTrue(mb)) {
            ctx->config.mb_samples = 8;
        } else if (cJSON_IsObject(mb)) {
            ctx->config.mb_samples = json_int(mb, "samples", 8);
            ctx->config.mb_shutter = json_float(mb, "shutter", 0.5f);
        }

        /*
         * Capped at 32. Cost is linear in the sample count — 32 samples is 32
         * full renders of every frame — and the returns stop well before that:
         * a sample count high enough to smear a fast object smoothly is set by
         * how far it travels in pixels, and past about 16 the difference is
         * below a code value on anything moving slowly enough to look at.
         */
        if (ctx->config.mb_samples < 0)  ctx->config.mb_samples = 0;
        if (ctx->config.mb_samples > 32) {
            fprintf(stderr, "warning: motion_blur samples capped at 32 (asked %d).\n",
                    ctx->config.mb_samples);
            ctx->config.mb_samples = 32;
        }
        ctx->config.mb_shutter = vr_clampf01(ctx->config.mb_shutter);
    }

    /* --- 1b. output { } — encoding parameters ----------------------------- */
    {
        const cJSON *out_cfg = cJSON_GetObjectItemCaseSensitive(root, "output");
        ctx->output.encoder = dup_string(json_str(out_cfg, "encoder", "h264_nvenc"));
        ctx->output.preset  = dup_string(json_str(out_cfg, "preset", "p5"));
        ctx->output.cq      = json_int(out_cfg, "cq", 21);

        const char *br = json_str(out_cfg, "bitrate", NULL);
        ctx->output.bitrate = (br != NULL) ? dup_string(br) : NULL;

        ctx->output.alpha = cJSON_IsTrue(
            cJSON_GetObjectItemCaseSensitive(out_cfg, "alpha"));

        if (ctx->output.cq < 0)  ctx->output.cq = 0;
        if (ctx->output.cq > 51) ctx->output.cq = 51;

        if (ctx->output.encoder == NULL || ctx->output.preset == NULL) {
            goto fail;
        }
    }

    /* Sane limits — a bad config would ask for an absurd amount of VRAM. */
    if (ctx->config.width  < 16 || ctx->config.width  > 16384 ||
        ctx->config.height < 16 || ctx->config.height > 16384 ||
        ctx->config.fps    < 1  || ctx->config.fps    > 480) {
        fprintf(stderr, "error: invalid config %dx%d @ %d fps.\n",
                ctx->config.width, ctx->config.height, ctx->config.fps);
        goto fail;
    }

    /*
     * H.264/HEVC 4:2:0 chroma takes one sample per two pixels, so an odd
     * dimension fails at encode time. Rounding down quietly beats a cryptic
     * ffmpeg error at the very end of a render.
     */
    if ((ctx->config.width % 2) != 0 || (ctx->config.height % 2) != 0) {
        fprintf(stderr, "warning: %dx%d is odd — rounding to %dx%d (4:2:0 needs even).\n",
                ctx->config.width, ctx->config.height,
                ctx->config.width & ~1, ctx->config.height & ~1);
        ctx->config.width  &= ~1;
        ctx->config.height &= ~1;
    }

    /* --- 2. Scenes, or a flat objects[] ----------------------------------- */
    {
        const cJSON *scenes_arr = cJSON_GetObjectItemCaseSensitive(root, "scenes");
        int          z          = 0;

        if (cJSON_IsArray(scenes_arr)) {
            const cJSON *sc_json = NULL;
            cJSON_ArrayForEach(sc_json, scenes_arr) {
                if (!parse_scene(ctx, sc_json, styles, filepath, &z, false, &eases)) {
                    goto fail;
                }
            }
            if (ctx->scene_count == 0) {
                fprintf(stderr, "error: 'scenes' is empty.\n");
                goto fail;
            }
        } else {
            /*
             * Flat mode: one implicit scene holding every object. That leaves
             * the renderer a single code path and keeps older projects
             * working unchanged.
             */
            if (!parse_scene(ctx, root, styles, filepath, &z, true, &eases)) {
                goto fail;
            }
        }
    }


    /* --- 3b. effects [ ] -------------------------------------------------- */
    if (!parse_effects_into(&ctx->effects, &ctx->effect_count, &ctx->effect_cap,
                            cJSON_GetObjectItemCaseSensitive(root, "effects"),
                            filepath)) {
        fprintf(stderr, "error: the film's effects could not be prepared.\n");
        goto fail;
    }

    /* --- 3c. audio [ ] ---------------------------------------------------- */
    if (!parse_audio(ctx, cJSON_GetObjectItemCaseSensitive(root, "audio"), filepath)) {
        fprintf(stderr, "error: could not process the audio tracks.\n");
        goto fail;
    }

    /* --- 4. Index, references, duration, arena ---------------------------- */
    if (!ctx_build_widget_index(ctx)) {
        fprintf(stderr, "error: could not build the widget index.\n");
        goto fail;
    }
    resolve_timeline_targets(ctx);

    if (!parse_transitions(ctx, cJSON_GetObjectItemCaseSensitive(root, "transitions"),
                                cJSON_GetObjectItemCaseSensitive(root, "transition"))) {
        fprintf(stderr, "error: could not allocate the transitions.\n");
        goto fail;
    }

    /*
     * In flat mode the project's duration dictates the single scene's; in
     * scene mode it is the other way round — the total comes from the scenes.
     */
    if (ctx->scene_count == 1 && ctx->config.duration_ms > 0) {
        ctx->scenes[0].duration_ms = ctx->config.duration_ms;
    }
    compute_scene_times(ctx);

    /* Durations are final only now — so this is the earliest "50%" can become
     * seconds, and the latest it may still be one. */
    resolve_relative_times(ctx);

    if (!arena_init(&ctx->frame_arena, FRAME_ARENA_BYTES)) {
        fprintf(stderr, "error: could not allocate the frame arena.\n");
        goto fail;
    }

    cJSON_Delete(root);
    return ctx;

fail:
    cJSON_Delete(root);
    editor_context_free(ctx);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Cleanup                                                                    */
/* ------------------------------------------------------------------------- */

/* Resources owned by a widget's common part (property tracks included). */
static void widget_base_free(WidgetBase *b)
{
    free(b->id);
    texture_free(&b->tex);
    glyph_metrics_free(&b->glyphs);

    track_free(&b->tr_x);
    track_free(&b->tr_y);
    track_free(&b->tr_opacity);
    track_free(&b->tr_scale);
    track_free(&b->tr_rotation);
    track_free(&b->tr_w);
    track_free(&b->tr_h);
    track_free(&b->tr_tint);
    track_free(&b->tr_trim);
    track_free(&b->tr_z);
    track_free(&b->tr_rx);
    track_free(&b->tr_ry);

    free(b->x_expr);
    free(b->y_expr);
    free(b->x_bind);
    free(b->y_bind);
    free(b->group_name);
    b->x_expr = b->y_expr = NULL;
    b->x_bind = b->y_bind = NULL;
    b->group_name = NULL;
}

void editor_context_free(EditorContext *ctx)
{
    if (ctx == NULL) {
        return;
    }

    /* VRAM first — the renderer may still hold device pointers stored inside
     * the textures. Idempotent. */
    renderer_shutdown(ctx);

    for (size_t i = 0; i < ctx->text_count; i++) {
        TextWidget *w = &ctx->texts[i];
        widget_base_free(&w->base);
        free(w->content);
        free(w->font);
    }
    free(ctx->texts);

    for (size_t i = 0; i < ctx->code_count; i++) {
        CodeWidget *w = &ctx->codes[i];
        widget_base_free(&w->base);
        texture_free(&w->plate); /* the panel is a separate layer */
        free(w->code);
        free(w->language);
        free(w->font);
    }
    free(ctx->codes);

    for (size_t i = 0; i < ctx->image_count; i++) {
        ImageWidget *w = &ctx->images[i];
        widget_base_free(&w->base);
        free(w->path);
    }
    free(ctx->images);

    for (size_t i = 0; i < ctx->line_count; i++) {
        widget_base_free(&ctx->lines[i].base);
    }
    free(ctx->lines);

    for (size_t i = 0; i < ctx->path_count; i++) {
        widget_base_free(&ctx->paths[i].base);
        free(ctx->paths[i].segs);
    }
    free(ctx->paths);

    for (size_t i = 0; i < ctx->video_count; i++) {
        widget_base_free(&ctx->videos[i].base);
        free(ctx->videos[i].path);
    }
    free(ctx->videos);

    for (size_t i = 0; i < ctx->mesh_count; i++) {
        widget_base_free(&ctx->meshes[i].base);
        mesh_free(&ctx->meshes[i]);
        texture_free(&ctx->meshes[i].tex);
        texture_free(&ctx->meshes[i].ao);
        texture_free(&ctx->meshes[i].nrm);
        texture_free(&ctx->meshes[i].emis);
        free(ctx->meshes[i].path);
        free(ctx->meshes[i].shape);
        free(ctx->meshes[i].tex_path);
        free(ctx->meshes[i].ao_path);
        free(ctx->meshes[i].nrm_path);
        free(ctx->meshes[i].emis_path);
    }
    free(ctx->meshes);

    for (size_t i = 0; i < ctx->shape_count; i++) {
        widget_base_free(&ctx->shapes[i].base);
    }
    free(ctx->shapes);

    for (size_t i = 0; i < ctx->effect_count; i++) {
        effect_free(&ctx->effects[i]);
    }
    free(ctx->effects);

    for (size_t i = 0; i < ctx->audio_count; i++) {
        free(ctx->audio[i].path);
        free(ctx->audio[i].id);
        free(ctx->audio[i].duck_by);
    }
    free(ctx->audio);

    free(ctx->output.encoder);
    free(ctx->output.preset);
    free(ctx->output.bitrate);

    for (size_t i = 0; i < ctx->scene_count; i++) {
        Scene *sc = &ctx->scenes[i];
        free(sc->id);
        for (size_t e = 0; e < sc->effect_count; e++) {
            effect_free(&sc->effects[e]);
        }
        free(sc->effects);
        for (size_t e = 0; e < sc->event_count; e++) {
            free(sc->events[e].target_id);
            free(sc->events[e].label);
            free(sc->events[e].time_expr);
            track_free(&sc->events[e].anim_track);
        }
        for (size_t l = 0; l < sc->label_count; l++) {
            free(sc->label_names[l]);
        }
        free(sc->label_names);
        free(sc->label_times);
        free(sc->events);
        for (size_t g = 0; g < sc->group_count; g++) {
            free(sc->groups[g].id);
        }
        free(sc->groups);
        track_free(&sc->camera.eye.px); track_free(&sc->camera.eye.py);
        track_free(&sc->camera.eye.pz); track_free(&sc->camera.eye.tx);
        track_free(&sc->camera.eye.ty); track_free(&sc->camera.eye.tz);
        track_free(&sc->camera.eye.roll);
        for (int li = 0; li < VR_MAX_LIGHTS; li++) {
            LightTrack *lt = &sc->light_anim[li];
            track_free(&lt->x); track_free(&lt->y); track_free(&lt->z);
            track_free(&lt->intensity); track_free(&lt->range);
            track_free(&lt->dx); track_free(&lt->dy); track_free(&lt->dz);
        }
        track_free(&sc->camera.zoom);     track_free(&sc->camera.x);
        track_free(&sc->camera.y);        track_free(&sc->camera.rotation);
        track_free(&sc->camera.shake);
    }
    free(ctx->scenes);

    for (size_t i = 0; i < ctx->transition_count; i++) {
        Transition *tr = &ctx->transitions[i];
        for (int m = 0; m < 4; m++) {
            track_free(&tr->from_mask[m]);
            track_free(&tr->to_mask[m]);
        }
        track_free(&tr->from_opacity); track_free(&tr->from_x); track_free(&tr->from_y);
        track_free(&tr->from_scale);   track_free(&tr->from_rotate);
        track_free(&tr->to_opacity);   track_free(&tr->to_x);   track_free(&tr->to_y);
        track_free(&tr->to_scale);     track_free(&tr->to_rotate);
    }
    free(ctx->transitions);

    free(ctx->widgets); /* the index only — the widgets themselves are already freed */
    arena_destroy(&ctx->frame_arena);
    free(ctx);
}

/* ------------------------------------------------------------------------- */
/* Diagnostics                                                                */
/* ------------------------------------------------------------------------- */

static const char *action_name(ActionType a)
{
    switch (a) {
        case ACTION_FADE_IN:   return "fade_in";
        case ACTION_FADE_OUT:  return "fade_out";
        case ACTION_MOVE:      return "move";
        case ACTION_TYPEWRITE: return "typewrite";
        case ACTION_SCALE:     return "scale";
        case ACTION_ROTATE:    return "rotate";
        case ACTION_HIGHLIGHT: return "highlight";
        case ACTION_ORBIT:     return "orbit";
        case ACTION_EMIT:      return "emit";
        case ACTION_ANIMATE:   return "animate";
        default:               return "unknown";
    }
}

void editor_context_dump(const EditorContext *ctx)
{
    if (ctx == NULL) {
        return;
    }

    fprintf(stderr, "--- project -------------------------------------------\n");
    fprintf(stderr, "  resolution : %dx%d @ %d fps\n",
            ctx->config.width, ctx->config.height, ctx->config.fps);
    fprintf(stderr, "  duration   : %d ms (%d frames)\n", ctx->config.duration_ms,
            (int)(((long long)ctx->config.duration_ms * ctx->config.fps + 999) / 1000));
    fprintf(stderr, "  background : #%02X%02X%02X%02X\n", ctx->config.bg_color.r,
            ctx->config.bg_color.g, ctx->config.bg_color.b, ctx->config.bg_color.a);

    fprintf(stderr, "--- objects (%zu) -------------------------------------\n", ctx->widget_count);
    for (size_t i = 0; i < ctx->widget_count; i++) {
        const WidgetBase *b = ctx->widgets[i];
        fprintf(stderr, "  [%zu] %-14s %-6s pos=(%s%.0f, %.0f) tex=%dx%d  %d chars\n", i,
                b->id ? b->id : "(null)",
                (b->kind == WIDGET_TEXT)  ? "text"  : (b->kind == WIDGET_CODE)   ? "code" :
                (b->kind == WIDGET_IMAGE) ? "image" : (b->kind == WIDGET_CIRCLE) ? "circle" : "rect",
                b->auto_center_x ? "auto:" : "",
                (double)(b->x - b->anchor_off_x), (double)(b->y - b->anchor_off_y),
                b->tex.width, b->tex.height, b->glyphs.total_chars);
    }

    fprintf(stderr, "--- scenes (%zu) --------------------------------------\n", ctx->scene_count);
    for (size_t si = 0; si < ctx->scene_count; si++) {
        const Scene *sc = &ctx->scenes[si];
        fprintf(stderr, "  [%zu] %-12s %6d..%-6d ms  %zu objects, %zu events\n", si,
                sc->id ? sc->id : "(unnamed)", sc->start_ms,
                sc->start_ms + sc->duration_ms, sc->widget_count, sc->event_count);

        for (size_t i = 0; i < sc->event_count; i++) {
            const TimelineEvent *e = &sc->events[i];
            fprintf(stderr, "        t=%5d ms  %-10s → %-14s dur=%d ms\n",
                    e->time_ms, action_name(e->action),
                    e->target_id ? e->target_id : "(null)", e->duration_ms);
        }
        if (si < ctx->transition_count && ctx->transitions[si].type != TRANS_CUT) {
            fprintf(stderr, "      ↕ transition: %d ms\n", ctx->transitions[si].duration_ms);
        }
    }
    if (ctx->effect_count > 0) {
        fprintf(stderr, "--- effects (%zu) -------------------------------------\n",
                ctx->effect_count);
        for (size_t i = 0; i < ctx->effect_count; i++) {
            fprintf(stderr, "  [%zu] %s\n", i, effect_name(ctx->effects[i].type));
        }
    }
    if (ctx->audio_count > 0) {
        fprintf(stderr, "--- audio (%zu) ----------------------------------------\n",
                ctx->audio_count);
        for (size_t i = 0; i < ctx->audio_count; i++) {
            const AudioTrack *a = &ctx->audio[i];
            fprintf(stderr, "  [%zu] %s  start=%.2fs vol=%.2f%s\n", i,
                    a->path ? a->path : "(null)", (double)a->start, (double)a->volume,
                    a->loop ? " loop" : "");
        }
    }
    fprintf(stderr, "-------------------------------------------------------\n");
}

/* ------------------------------------------------------------------------- */
/* Validation                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * After anchoring, `base->x/y` is the anchor *point*, not the top-left corner.
 * Diagnostics and validation must see the real edge, otherwise an object
 * anchored to "right" is falsely reported as off-canvas.
 */
static float widget_left(const WidgetBase *b) { return b->x - b->anchor_off_x; }
static float widget_top (const WidgetBase *b) { return b->y - b->anchor_off_y; }

static const char *kind_name(WidgetKind k)
{
    switch (k) {
        case WIDGET_TEXT:   return "text";
        case WIDGET_CODE:   return "code";
        case WIDGET_IMAGE:  return "image";
        case WIDGET_CIRCLE: return "circle";
        case WIDGET_LINE:   return "line";
        case WIDGET_PATH:   return "path";
        case WIDGET_VIDEO:  return "video";
        case WIDGET_MESH:   return "mesh";
        default:            return "rect";
    }
}

/* Total timeline events across every scene — reported by both check forms. */
static size_t vr_total_events(const EditorContext *ctx)
{
    size_t n = 0;
    for (size_t i = 0; i < ctx->scene_count; i++) {
        n += ctx->scenes[i].event_count;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* Machine-readable output                                                    */
/* ------------------------------------------------------------------------- */

/* Writes `in` as a JSON string body (no surrounding quotes). */
static void json_escape(FILE *f, const char *in)
{
    for (const unsigned char *p = (const unsigned char *)in; *p != '\0'; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f);  break;
            case '\r': fputs("\\r", f);  break;
            case '\t': fputs("\\t", f);  break;
            default:
                if (*p < 0x20) {
                    fprintf(f, "\\u%04x", *p);
                } else {
                    fputc(*p, f);   /* UTF-8 passes through unchanged */
                }
        }
    }
}

/*
 * Where a check message goes.
 *
 * The validator is one body of logic with two audiences: a person reading
 * stderr, and a program parsing stdout. Keeping one implementation and two
 * sinks is what stops the JSON report from quietly drifting away from the
 * human one — a divergence nobody would notice until an agent trusted it.
 */
typedef struct {
    bool  json;
    FILE *out;        /* JSON mode: where the array is accumulated */
    int   problems;
    int   emitted;    /* entries written, so commas land correctly */
} CheckSink;

static void check_emit(CheckSink *sk, const char *severity, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    /* The callers' format strings all end in a newline, for the human form. */
    size_t n = strlen(msg);
    while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) {
        msg[--n] = '\0';
    }

    bool is_problem = (strcmp(severity, "error") == 0);
    if (is_problem) {
        sk->problems++;
    }

    if (sk->json) {
        fprintf(sk->out, "%s\n    {\"severity\": \"%s\", \"message\": \"",
                sk->emitted ? "," : "", severity);
        json_escape(sk->out, msg);
        fprintf(sk->out, "\"}");
        sk->emitted++;
    } else {
        fprintf(stderr, "  %s %s\n", is_problem ? "\xe2\x9c\x97" : "\xc2\xb7", msg);
    }
}

static int check_run(const EditorContext *ctx, CheckSink *sk);

/*
 * The engine's vocabulary, as JSON.
 *
 * These tables already existed inside the parser and the effect registry; they
 * simply had no way out. Exposing them means a caller never has to guess a name
 * or keep a copy of the list that can fall out of date — the binary is the
 * single source of truth for what it accepts.
 */
bool vr_list_table(const char *what)
{
    if (what == NULL) {
        return false;
    }

    if (strcmp(what, "effects") == 0) {
        printf("[");
        for (int t = FX_GRAYSCALE, first = 1; t < FX_TYPE_COUNT; t++) {
            const char *n = effect_name((EffectType)t);
            if (n == NULL || strcmp(n, "none") == 0) {
                continue;
            }
            printf("%s\"%s\"", first ? "" : ", ", n);
            first = 0;
        }
        printf("]\n");
        return true;
    }

    if (strcmp(what, "lights") == 0) {
        /*
         * Spelled out rather than derived, because unlike the effects there is
         * no name table in the code to read: the parser matches these strings
         * directly. Keep the two in step — a name here that the parser does not
         * accept is worse than no list at all, because it is discoverable.
         */
        static const char *kL[] = { "point", "sun", "directional", "spot" };
        printf("[");
        for (size_t i = 0; i < sizeof kL / sizeof kL[0]; i++) {
            printf("%s\"%s\"", i ? ", " : "", kL[i]);
        }
        printf("]\n");
        return true;
    }

    if (strcmp(what, "transitions") == 0) {
        static const char *kT[] = {
            "cut", "crossfade", "fade",
            "slide_left", "slide_right", "slide_up", "slide_down",
            "push_left", "push_right", "push_up", "push_down",
            "zoom_in", "zoom_out", "spin",
            "wipe_left", "wipe_right", "iris",
        };
        printf("[");
        for (size_t i = 0; i < sizeof kT / sizeof kT[0]; i++) {
            printf("%s\"%s\"", i ? ", " : "", kT[i]);
        }
        printf("]\n");
        return true;
    }

    if (strcmp(what, "easings") == 0) {
        printf("[");
        for (int e = 0, first = 1; e <= EASE_SMOOTH; e++) {
            const char *n = easing_name((EaseType)e);
            printf("%s\"%s\"", first ? "" : ", ", n);
            first = 0;
        }
        printf("]\n");
        return true;
    }

    if (strcmp(what, "actions") == 0) {
        static const char *kA[] = {
            "fade_in", "fade_out", "move", "move_x", "move_y", "typewrite",
            "scale", "rotate", "highlight", "orbit", "animate", "emit",
        };
        printf("[");
        for (size_t i = 0; i < sizeof kA / sizeof kA[0]; i++) {
            printf("%s\"%s\"", i ? ", " : "", kA[i]);
        }
        printf("]\n");
        return true;
    }

    if (strcmp(what, "properties") == 0) {
        static const char *kP[] = {
            "x", "y", "opacity", "scale", "rotation", "w", "h", "tint", "trim",
            "z", "rotate_x", "rotate_y",
        };
        printf("[");
        for (size_t i = 0; i < sizeof kP / sizeof kP[0]; i++) {
            printf("%s\"%s\"", i ? ", " : "", kP[i]);
        }
        printf("]\n");
        return true;
    }

    if (strcmp(what, "fonts") == 0) {
        /*
         * Installed font families, via fontconfig.
         *
         * This is the one class of mistake a rendered preview cannot catch.
         * Cairo's "toy" API substitutes silently, so a scene naming a font that
         * is not installed still renders — in some other face, looking entirely
         * plausible. Nothing about the picture says the typography is wrong, so
         * the only defence is knowing the list beforehand.
         */
        FcConfig  *cfg = FcInitLoadConfigAndFonts();
        if (cfg == NULL) {
            printf("[]\n");
            return true;
        }

        FcPattern   *pat  = FcPatternCreate();
        FcObjectSet *os   = FcObjectSetBuild(FC_FAMILY, (char *)NULL);
        FcFontSet   *set  = FcFontList(cfg, pat, os);

        printf("[");
        int printed = 0;
        for (int i = 0; set != NULL && i < set->nfont; i++) {
            FcChar8 *fam = NULL;
            if (FcPatternGetString(set->fonts[i], FC_FAMILY, 0, &fam) != FcResultMatch ||
                fam == NULL) {
                continue;
            }

            /* fontconfig lists a family once per style, so duplicates are
             * common; a linear scan is fine for a few hundred names. */
            bool seen = false;
            for (int j = 0; j < i && !seen; j++) {
                FcChar8 *prev = NULL;
                if (FcPatternGetString(set->fonts[j], FC_FAMILY, 0, &prev) == FcResultMatch &&
                    prev != NULL && strcmp((const char *)prev, (const char *)fam) == 0) {
                    seen = true;
                }
            }
            if (seen) {
                continue;
            }

            printf("%s\"", printed ? ", " : "");
            json_escape(stdout, (const char *)fam);
            printf("\"");
            printed++;
        }
        printf("]\n");

        if (set != NULL) FcFontSetDestroy(set);
        FcObjectSetDestroy(os);
        FcPatternDestroy(pat);
        FcConfigDestroy(cfg);
        return true;
    }

    if (strcmp(what, "shapes") == 0) {
        printf("[");
        for (size_t i = 0; mesh_shape_name(i) != NULL; i++) {
            printf("%s\"%s\"", i ? ", " : "", mesh_shape_name(i));
        }
        printf("]\n");
        return true;
    }

    if (strcmp(what, "widgets") == 0) {
        printf("[\"text\", \"code\", \"image\", \"video\", \"mesh\", "
               "\"rect\", \"circle\", \"line\", \"path\"]\n");
        return true;
    }

    return false;
}

/*
 * The parsed project as JSON: what the engine actually decided, after
 * variables, styles, layout expressions and repeat/emitter expansion.
 *
 * This is the difference between an author's intent and the engine's reading of
 * it, which is exactly what is worth inspecting when a scene does not look
 * right.
 */
void editor_context_dump_json(const EditorContext *ctx)
{
    if (ctx == NULL) {
        printf("null\n");
        return;
    }

    printf("{\n  \"width\": %d, \"height\": %d, \"fps\": %d, \"duration_ms\": %d,\n",
           ctx->config.width, ctx->config.height, ctx->config.fps,
           ctx->config.duration_ms);

    printf("  \"scenes\": [");
    for (size_t i = 0; i < ctx->scene_count; i++) {
        const Scene *sc = &ctx->scenes[i];
        printf("%s\n    {\"id\": \"", i ? "," : "");
        json_escape(stdout, sc->id ? sc->id : "");
        printf("\", \"start_ms\": %d, \"duration_ms\": %d, \"objects\": %zu,"
               " \"events\": %zu, \"groups\": %zu}",
               sc->start_ms, sc->duration_ms, sc->widget_count,
               sc->event_count, sc->group_count);
    }
    printf("%s],\n", ctx->scene_count ? "\n  " : "");

    static const char *kKind[] = { "text", "code", "image", "rect", "circle",
                                   "line", "path", "video", "mesh" };

    printf("  \"objects\": [");
    for (size_t i = 0; i < ctx->widget_count; i++) {
        const WidgetBase *b = ctx->widgets[i];
        printf("%s\n    {\"id\": \"", i ? "," : "");
        json_escape(stdout, b->id ? b->id : "");
        printf("\", \"type\": \"%s\", \"x\": %.1f, \"y\": %.1f,"
               " \"w\": %.1f, \"h\": %.1f, \"z\": %d",
               (b->kind <= WIDGET_MESH) ? kKind[b->kind] : "?",
               (double)widget_left(b), (double)widget_top(b),
               (double)b->base_w, (double)b->base_h, b->z_order);
        if (b->glyphs.line_count > 0) {
            printf(", \"lines\": %d, \"chars\": %d",
                   b->glyphs.line_count, b->glyphs.total_chars);
        }
        printf("}");
    }
    printf("%s],\n", ctx->widget_count ? "\n  " : "");

    printf("  \"audio\": %zu,\n  \"effects\": %zu,\n  \"transitions\": %zu\n}\n",
           ctx->audio_count, ctx->effect_count, ctx->transition_count);
}

int editor_context_check(const EditorContext *ctx)
{
    if (ctx == NULL) {
        return 1;
    }
    CheckSink sk = { false, NULL, 0, 0 };

    fprintf(stderr, "--- check ---------------------------------------------\n");
    check_run(ctx, &sk);

    if (sk.problems == 0) {
        fprintf(stderr, "  \xe2\x9c\x93 no problems found (%zu scenes, %zu objects, %zu events)\n",
                ctx->scene_count, ctx->widget_count, vr_total_events(ctx));
    } else {
        fprintf(stderr, "  %d problem(s)\n", sk.problems);
    }
    fprintf(stderr, "-------------------------------------------------------\n");
    return sk.problems;
}

int editor_context_check_json(const EditorContext *ctx)
{
    if (ctx == NULL) {
        printf("{\"ok\": false, \"problems\": [{\"severity\": \"error\","
               " \"message\": \"project failed to load\"}]}\n");
        return 1;
    }

    /*
     * The verdict has to precede the array in the output, but is only known
     * after every check has run — so the array is accumulated in memory first.
     * The alternative, printing a placeholder and seeking back over it, breaks
     * the moment stdout is a pipe, which for an MCP server it always is.
     */
    char   *buf  = NULL;
    size_t  len  = 0;
    FILE   *mem  = open_memstream(&buf, &len);
    if (mem == NULL) {
        printf("{\"ok\": false, \"problems\": [{\"severity\": \"error\","
               " \"message\": \"out of memory\"}]}\n");
        return 1;
    }

    CheckSink sk = { true, mem, 0, 0 };
    check_run(ctx, &sk);
    fclose(mem);

    printf("{\n  \"ok\": %s,\n", (sk.problems == 0) ? "true" : "false");
    printf("  \"problems\": [%s%s],\n", buf ? buf : "", sk.emitted ? "\n  " : "");
    printf("  \"problem_count\": %d,\n", sk.problems);
    printf("  \"scenes\": %zu,\n  \"objects\": %zu,\n  \"events\": %zu,\n",
           ctx->scene_count, ctx->widget_count, vr_total_events(ctx));
    printf("  \"width\": %d,\n  \"height\": %d,\n  \"fps\": %d,\n",
           ctx->config.width, ctx->config.height, ctx->config.fps);
    printf("  \"duration_ms\": %d,\n", ctx->config.duration_ms);
    printf("  \"frames\": %lld\n}\n",
           ((long long)ctx->config.duration_ms * ctx->config.fps + 999) / 1000);

    free(buf);
    return sk.problems;
}

static int check_run(const EditorContext *ctx, CheckSink *sk)
{
    int problems = 0;
    #define VR_PROBLEM(...) check_emit(sk, "error", __VA_ARGS__)
    #define VR_NOTE(...)    check_emit(sk, "note",  __VA_ARGS__)


    /* --- project --------------------------------------------------------- */
    if (ctx->config.duration_ms <= 0) {
        VR_PROBLEM("duration is zero.\n");
    }
    if (ctx->widget_count == 0) {
        VR_PROBLEM("the project has no objects at all.\n");
    }

    /* --- objects ---------------------------------------------------------- */
    for (size_t i = 0; i < ctx->widget_count; i++) {
        const WidgetBase *b  = ctx->widgets[i];
        const char       *id = b->id ? b->id : "(null)";

        /*
         * A mesh is checked on its geometry, not on a rectangle.
         *
         * base_w/base_h describe a flat layer's box; a mesh has no such box —
         * what it covers on screen falls out of `size`, its rotation and the
         * projection, and is different every frame. Measuring it as a rectangle
         * produced a page of "extends past the canvas" notes about spheres
         * sitting squarely in the middle of the frame, which is worse than no
         * check at all: noise teaches you to stop reading the output.
         *
         * What can actually go wrong is that the model did not load, and that
         * was previously only a line on stderr at parse time.
         */
        if (b->kind == WIDGET_MESH) {
            const MeshWidget *mw = (const MeshWidget *)b;
            if (mw->tri_count == 0) {
                VR_PROBLEM("'%s' (mesh) — no geometry; the model failed to load.\n", id);
            }
            continue;      /* `size` is already clamped to >= 1 when parsed */
        }

        if (b->base_w < 1.0f || b->base_h < 1.0f) {
            VR_PROBLEM("'%s' — zero size (%.0fx%.0f).\n", id,
                       (double)b->base_w, (double)b->base_h);
            continue;
        }

        float x0 = widget_left(b), y0 = widget_top(b);
        float x1 = x0 + b->base_w,  y1 = y0 + b->base_h;

        /* Entirely off-canvas — always an error. */
        if (x1 <= 0.0f || y1 <= 0.0f ||
            x0 >= (float)ctx->config.width || y0 >= (float)ctx->config.height) {
            VR_PROBLEM("'%s' (%s) — entirely off-canvas: x=%.0f..%.0f y=%.0f..%.0f\n",
                       id, kind_name(b->kind), (double)x0, (double)x1, (double)y0, (double)y1);
            continue;
        }

        /*
         * Partial overflow is normal for an image (a cover shot is meant to be
         * cropped) but is almost always a bug for text.
         */
        bool clipped = (x0 < 0.0f) || (y0 < 0.0f) ||
                       (x1 > (float)ctx->config.width) || (y1 > (float)ctx->config.height);

        if (clipped) {
            if (b->kind == WIDGET_TEXT || b->kind == WIDGET_CODE) {
                VR_PROBLEM("'%s' (%s) — clipped at the edge: x=%.0f..%.0f y=%.0f..%.0f (canvas %dx%d)\n",
                           id, kind_name(b->kind), (double)x0, (double)x1,
                           (double)y0, (double)y1, ctx->config.width, ctx->config.height);
            } else {
                VR_NOTE("'%s' (%s) extends past the canvas — probably intentional (cover).\n",
                        id, kind_name(b->kind));
            }
        }

        if (b->tex.pixels == NULL) {
            /* A mesh legitimately has no texture — its geometry is the asset. */
            if (b->kind != WIDGET_MESH) {
                VR_PROBLEM("'%s' — the texture was not rasterized.\n", id);
            }
        }
    }

    /* --- timeline (per scene) --------------------------------------------- */
    for (size_t si = 0; si < ctx->scene_count; si++) {
        const Scene *sc = &ctx->scenes[si];
        const char  *sn = sc->id ? sc->id : "(unnamed)";

        if (sc->duration_ms <= 0) {
            VR_PROBLEM("scene '%s' — zero duration.\n", sn);
        }

        /*
         * A duplicate id is a problem only *within one scene* — targets resolve
         * scene-locally, so the same name in different scenes is perfectly
         * normal (and desirable in templates).
         */
        for (size_t a = 0; a < sc->widget_count; a++) {
            const char *ia = ctx->widgets[sc->first_widget + a]->id;
            if (ia == NULL) {
                continue;
            }
            for (size_t b2 = a + 1; b2 < sc->widget_count; b2++) {
                const char *ib = ctx->widgets[sc->first_widget + b2]->id;
                if (ib != NULL && strcmp(ia, ib) == 0) {
                    VR_PROBLEM("scene '%s' — duplicate id '%s'; the timeline reaches only the first.\n",
                               sn, ia);
                    break;
                }
            }
        }

        for (size_t i = 0; i < sc->event_count; i++) {
            const TimelineEvent *e = &sc->events[i];

            if (e->action == ACTION_UNKNOWN) {
                VR_PROBLEM("'%s' #%zu — unknown action (t=%d ms).\n", sn, i, e->time_ms);
                continue;
            }
            /* A group is a legitimate target, and has no widget index. */
            if (e->target_index < 0 && e->target_group < 0) {
                VR_PROBLEM("'%s' #%zu — target '%s' not found.\n", sn, i,
                           e->target_id ? e->target_id : "(null)");
            }
            if (e->time_ms > sc->duration_ms) {
                VR_PROBLEM("'%s' #%zu — t=%d ms is past the scene's end (%d ms).\n",
                           sn, i, e->time_ms, sc->duration_ms);
            }
        }
    }

    /* --- audio ------------------------------------------------------------- */
    for (size_t i = 0; i < ctx->audio_count; i++) {
        const AudioTrack *a = &ctx->audio[i];
        if (a->start * 1000.0f >= (float)ctx->config.duration_ms) {
            VR_PROBLEM("audio '%s' starts past the end of the video (%.2f s).\n",
                       a->path ? a->path : "(null)", (double)a->start);
        }
    }

    #undef VR_PROBLEM
    #undef VR_NOTE
    (void)problems;          /* the sink counts them now */
    return sk->problems;
}
