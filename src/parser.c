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
#include "renderer.h"

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
 * A field may be either a number or an array of keyframes:
 *
 *     "opacity": 0.5
 *     "opacity": [ {"t": 0, "v": 0}, {"t": 1.2, "v": 1, "ease": "backout"} ]
 *
 * Returns true only when a real animation was built.
 */
static bool parse_track(const cJSON *item, Track *tr, float fallback)
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

    int          idx = 0;
    const cJSON *k   = NULL;
    cJSON_ArrayForEach(k, item) {
        if (!cJSON_IsObject(k)) {
            continue;
        }
        keys[idx].t    = json_float(k, "t", 0.0f);
        keys[idx].v    = json_float(k, "v", 0.0f);
        keys[idx].ease = easing_from_name(json_str(k, "ease", NULL));
        idx++;
    }

    if (idx == 0) {
        free(keys);
        return false;
    }

    /*
     * Keys must be in ascending time order — track_sample relies on it. The
     * JSON author may get that wrong, so we sort here (insertion sort: there
     * are only a handful of keys and they are almost always sorted already).
     */
    for (int i = 1; i < idx; i++) {
        Keyframe cur = keys[i];
        int      j   = i - 1;
        while (j >= 0 && keys[j].t > cur.t) {
            keys[j + 1] = keys[j];
            j--;
        }
        keys[j + 1] = cur;
    }

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
                               const cJSON *arr)
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
    }
    return true;
}

/* Reads the fields common to every WidgetBase. */
static void parse_widget_base(WidgetBase *base, const cJSON *obj, WidgetKind kind, int z)
{
    base->kind    = kind;
    base->id      = dup_string(json_str(obj, "id", "unnamed"));
    base->z_order = z;

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
    parse_track(cJSON_GetObjectItemCaseSensitive(obj, "x"),        &base->tr_x, base->x);
    parse_track(cJSON_GetObjectItemCaseSensitive(obj, "y"),        &base->tr_y, base->y);
    parse_track(cJSON_GetObjectItemCaseSensitive(obj, "opacity"),  &base->tr_opacity, 1.0f);
    parse_track(cJSON_GetObjectItemCaseSensitive(obj, "scale"),    &base->tr_scale, 1.0f);
    parse_track(cJSON_GetObjectItemCaseSensitive(obj, "rotation"), &base->tr_rotation, 0.0f);

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
    base->has_track_x        = json_has(obj, "x") && base->x_expr == NULL;
    base->has_track_y        = json_has(obj, "y") && base->y_expr == NULL;
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

static bool parse_text_object(EditorContext *ctx, const cJSON *obj, int z)
{
    TextWidget *w = (TextWidget *)array_push((void **)&ctx->texts, &ctx->text_count,
                                             &ctx->text_cap, sizeof(TextWidget));
    if (w == NULL) {
        return false;
    }

    parse_widget_base(&w->base, obj, WIDGET_TEXT, z);

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

static bool parse_shape_object(EditorContext *ctx, const cJSON *obj, int z, WidgetKind kind)
{
    ShapeWidget *w = (ShapeWidget *)array_push((void **)&ctx->shapes, &ctx->shape_count,
                                               &ctx->shape_cap, sizeof(ShapeWidget));
    if (w == NULL) {
        return false;
    }

    parse_widget_base(&w->base, obj, kind, z);

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

    w->corner_radius = json_int(obj, "corner_radius", 0);

    if (w->w < 1.0f) w->w = 1.0f;
    if (w->h < 1.0f) w->h = 1.0f;
    if (w->corner_radius < 0) w->corner_radius = 0;
    return true;
}

static bool parse_image_object(EditorContext *ctx, const cJSON *obj, int z,
                               const char *base_file)
{
    ImageWidget *w = (ImageWidget *)array_push((void **)&ctx->images, &ctx->image_count,
                                               &ctx->image_cap, sizeof(ImageWidget));
    if (w == NULL) {
        return false;
    }

    parse_widget_base(&w->base, obj, WIDGET_IMAGE, z);

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

static bool parse_code_object(EditorContext *ctx, const cJSON *obj, int z)
{
    CodeWidget *w = (CodeWidget *)array_push((void **)&ctx->codes, &ctx->code_count,
                                             &ctx->code_cap, sizeof(CodeWidget));
    if (w == NULL) {
        return false;
    }

    parse_widget_base(&w->base, obj, WIDGET_CODE, z);

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
    return ACTION_UNKNOWN;
}

static bool parse_timeline_event(Scene *scene, const cJSON *obj)
{
    TimelineEvent *e = (TimelineEvent *)array_push((void **)&scene->events, &scene->event_count,
                                                   &scene->event_cap, sizeof(TimelineEvent));
    if (e == NULL) {
        return false;
    }

    const char *action_name = json_str(obj, "action", NULL);

    e->time_ms      = json_int(obj, "time_ms", 0);
    e->duration_ms  = json_int(obj, "duration_ms", 0);
    e->action       = action_from_string(action_name);
    e->target_id    = dup_string(json_str(obj, "target", ""));
    e->target_index = -1; /* resolved in resolve_timeline_targets() */

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

static bool parse_transitions(EditorContext *ctx, const cJSON *arr)
{
    if (!cJSON_IsArray(arr)) {
        return true;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        Transition *tr = (Transition *)array_push((void **)&ctx->transitions,
                                                  &ctx->transition_count,
                                                  &ctx->transition_cap, sizeof(Transition));
        if (tr == NULL) {
            return false;
        }

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
    size_t total = ctx->text_count + ctx->code_count + ctx->image_count + ctx->shape_count;
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
    ctx->widget_count = n;

    /* JSON order = drawing order (painter's algorithm). */
    qsort(ctx->widgets, ctx->widget_count, sizeof(WidgetBase *), compare_by_z);
    return true;
}

/*
 * Targets are resolved *within* a scene, and the index is scene-local too.
 * That is what lets the same id exist independently in different scenes.
 */
static void resolve_timeline_targets(EditorContext *ctx)
{
    for (size_t si = 0; si < ctx->scene_count; si++) {
        Scene *sc = &ctx->scenes[si];

        for (size_t i = 0; i < sc->event_count; i++) {
            TimelineEvent *e = &sc->events[i];

            for (size_t w = 0; w < sc->widget_count; w++) {
                const char *id = ctx->widgets[sc->first_widget + w]->id;
                if (id != NULL && e->target_id != NULL && strcmp(id, e->target_id) == 0) {
                    e->target_index = (int)w;
                    break;
                }
            }

            if (e->target_index < 0 && e->action != ACTION_UNKNOWN) {
                fprintf(stderr, "warning: scene '%s' — target '%s' not found.\n",
                        sc->id ? sc->id : "(unnamed)",
                        e->target_id ? e->target_id : "(null)");
            }
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
/* Parsing a scene                                                            */
/* ------------------------------------------------------------------------- */

/*
 * Reads one scene: its objects, timeline and duration.
 *
 * Flat mode uses the same function — there `node` is the root itself and there
 * is a single scene. One code path means both modes behave identically.
 */
static bool parse_scene(EditorContext *ctx, const cJSON *node, const cJSON *styles,
                        const char *filepath, int *z, bool is_root)
{
    Scene *sc = (Scene *)array_push((void **)&ctx->scenes, &ctx->scene_count,
                                    &ctx->scene_cap, sizeof(Scene));
    if (sc == NULL) {
        return false;
    }

    sc->id           = dup_string(json_str(node, "id", NULL));
    sc->first_widget = ctx->text_count + ctx->code_count + ctx->image_count + ctx->shape_count;

    if (json_has(node, "bg_color")) {
        sc->bg_color = json_color(node, "bg_color", ctx->config.bg_color);
        sc->has_bg   = true;
    }

    /* --- objects --- */
    const cJSON *objects = cJSON_GetObjectItemCaseSensitive(node, "objects");
    if (!cJSON_IsArray(objects)) {
        objects = cJSON_GetObjectItemCaseSensitive(node, "layers"); /* videogen synonym */
    }

    if (cJSON_IsArray(objects)) {
        cJSON *obj = NULL;
        cJSON_ArrayForEach(obj, objects) {
            apply_styles(obj, styles);

            const char *type = json_str(obj, "type", "text");
            bool        ok;

            if (strcmp(type, "code") == 0) {
                ok = parse_code_object(ctx, obj, *z);
            } else if (strcmp(type, "text") == 0) {
                ok = parse_text_object(ctx, obj, *z);
            } else if (strcmp(type, "image") == 0) {
                ok = parse_image_object(ctx, obj, *z, filepath);
            } else if (strcmp(type, "rect") == 0) {
                ok = parse_shape_object(ctx, obj, *z, WIDGET_RECT);
            } else if (strcmp(type, "circle") == 0) {
                ok = parse_shape_object(ctx, obj, *z, WIDGET_CIRCLE);
            } else {
                fprintf(stderr, "warning: unknown object type '%s' — skipped.\n", type);
                continue;
            }

            if (!ok) {
                fprintf(stderr, "error: could not allocate an object.\n");
                return false;
            }
            (*z)++;   /* z increases globally → a scene's objects stay adjacent in the index */
        }
    }

    sc->widget_count = (ctx->text_count + ctx->code_count + ctx->image_count +
                        ctx->shape_count) - sc->first_widget;

    /*
     * --- the scene's own effects ---
     *
     * In flat mode `node` is the root itself, so the "effects" here is the
     * *global* stack, read separately by parse_video_project_ex(). Reading it
     * a second time meant every effect was applied to the frame twice.
     */
    if (!is_root &&
        !parse_effects_into(&sc->effects, &sc->effect_count, &sc->effect_cap,
                            cJSON_GetObjectItemCaseSensitive(node, "effects"))) {
        return false;
    }

    /* --- timeline --- */
    const cJSON *timeline = cJSON_GetObjectItemCaseSensitive(node, "timeline");
    if (cJSON_IsArray(timeline)) {
        const cJSON *ev = NULL;
        cJSON_ArrayForEach(ev, timeline) {
            if (!parse_timeline_event(sc, ev)) {
                fprintf(stderr, "error: could not allocate a timeline event.\n");
                return false;
            }
        }
    }

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
    const cJSON *project = cJSON_GetObjectItemCaseSensitive(root, "project");
    ctx->config.width       = json_int(project, "width", 1920);
    ctx->config.height      = json_int(project, "height", 1080);
    ctx->config.fps         = json_int(project, "fps", 60);
    ctx->config.bg_color    = json_color(project, "bg_color", (Color){ 0, 0, 0, 255 });
    ctx->config.duration_ms = json_int(project, "duration_ms", 0);

    /* --- 1b. output { } — encoding parameters ----------------------------- */
    {
        const cJSON *out_cfg = cJSON_GetObjectItemCaseSensitive(root, "output");
        ctx->output.encoder = dup_string(json_str(out_cfg, "encoder", "h264_nvenc"));
        ctx->output.preset  = dup_string(json_str(out_cfg, "preset", "p5"));
        ctx->output.cq      = json_int(out_cfg, "cq", 21);

        const char *br = json_str(out_cfg, "bitrate", NULL);
        ctx->output.bitrate = (br != NULL) ? dup_string(br) : NULL;

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
                if (!parse_scene(ctx, sc_json, styles, filepath, &z, false)) {
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
            if (!parse_scene(ctx, root, styles, filepath, &z, true)) {
                goto fail;
            }
        }
    }


    /* --- 3b. effects [ ] -------------------------------------------------- */
    if (!parse_effects_into(&ctx->effects, &ctx->effect_count, &ctx->effect_cap,
                            cJSON_GetObjectItemCaseSensitive(root, "effects"))) {
        fprintf(stderr, "error: could not allocate the effects.\n");
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

    if (!parse_transitions(ctx, cJSON_GetObjectItemCaseSensitive(root, "transitions"))) {
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

    free(b->x_expr);
    free(b->y_expr);
    b->x_expr = b->y_expr = NULL;
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
        }
        free(sc->events);
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
        default:            return "rect";
    }
}

int editor_context_check(const EditorContext *ctx)
{
    if (ctx == NULL) {
        return 1;
    }

    int problems = 0;
    #define VR_PROBLEM(...) do { fprintf(stderr, "  ✗ " __VA_ARGS__); problems++; } while (0)
    #define VR_NOTE(...)    do { fprintf(stderr, "  · " __VA_ARGS__); } while (0)

    fprintf(stderr, "--- check ---------------------------------------------\n");

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
            VR_PROBLEM("'%s' — the texture was not rasterized.\n", id);
        }
    }

    /* --- timeline (per scene) --------------------------------------------- */
    size_t total_events = 0;
    for (size_t si = 0; si < ctx->scene_count; si++) {
        const Scene *sc = &ctx->scenes[si];
        const char  *sn = sc->id ? sc->id : "(unnamed)";
        total_events += sc->event_count;

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
            if (e->target_index < 0) {
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

    if (problems == 0) {
        fprintf(stderr, "  ✓ no problems found (%zu scenes, %zu objects, %zu events)\n",
                ctx->scene_count, ctx->widget_count, total_events);
    } else {
        fprintf(stderr, "  %d problem(s)\n", problems);
    }
    fprintf(stderr, "-------------------------------------------------------\n");

    #undef VR_PROBLEM
    #undef VR_NOTE
    return problems;
}
