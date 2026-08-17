/*
 * parser.c — video.json → EditorContext.
 *
 * პრინციპები:
 *   1. პარსერი მხოლოდ *კითხულობს* — ის არაფერს არ ხატავს და GPU-ს არ ეხება.
 *   2. ყოველი გამოყოფილი რესურსი აღირიცხება კონტექსტში, რომ
 *      editor_context_free() ერთი გამოძახებით ასუფთავებდეს ყველაფერს.
 *   3. JSON-ის ნებისმიერი ველი შეიძლება აკლდეს — ყველგან გვაქვს default.
 *      აკლია ველი ≠ შეცდომა; მხოლოდ ავარიული სიტუაციები აბრუნებს NULL-ს.
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

/* ერთი კადრის დროებითი მეხსიერება. 4 MiB უხვად ჰყოფნის რამდენიმე ათას
 * WidgetRuntime-ს; საჭიროებისას ერთ ადგილას იცვლება. */
#define FRAME_ARENA_BYTES (4u * 1024u * 1024u)

/* ------------------------------------------------------------------------- */
/* პატარა დამხმარეები                                                         */
/* ------------------------------------------------------------------------- */

/* strdup POSIX-ისაა, ჩვენ კი -std=c11 გვაქვს — ვწერთ საკუთარს. */
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
        fprintf(stderr, "შეცდომა: ფაილი '%s' ვერ გაიხსნა.\n", filename);
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "შეცდომა: '%s' არ არის seekable.\n", filename);
        fclose(file);
        return NULL;
    }

    long length = ftell(file);
    if (length < 0) {
        fprintf(stderr, "შეცდომა: '%s'-ის ზომა ვერ დადგინდა.\n", filename);
        fclose(file);
        return NULL;
    }
    rewind(file);

    char *buffer = (char *)malloc((size_t)length + 1);
    if (buffer == NULL) {
        fprintf(stderr, "შეცდომა: მეხსიერება ვერ გამოიყო (%ld ბაიტი).\n", length + 1);
        fclose(file);
        return NULL;
    }

    size_t read_size = fread(buffer, 1, (size_t)length, file);
    buffer[read_size] = '\0'; /* fread-ის *ფაქტობრივ* შედეგზე ვხურავთ, არა length-ზე */
    fclose(file);

    if (out_len != NULL) {
        *out_len = read_size;
    }
    return buffer;
}

/*
 * "#RRGGBB" ან "#RRGGBBAA" (ასევე "RRGGBB" ლატით-გარეშე) → Color.
 * false → ფორმატი არასწორია; გამომძახებელი default-ს ტოვებს.
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
            return false; /* არა-hex სიმბოლო */
        }
        v[i / 2] = (unsigned int)byte;
    }

    out->r = (uint8_t)v[0];
    out->g = (uint8_t)v[1];
    out->b = (uint8_t)v[2];
    out->a = (uint8_t)v[3];
    return true;
}

/* cJSON-ის მოსახერხებელი წამკითხავები default-ებით. */
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
    parse_hex_color(json_str(obj, key, NULL), &c); /* ჩავარდნისას c უცვლელია */
    return c;
}

/*
 * დინამიური მასივის გაზრდა და ერთი ცარიელი (განულებული) სლოტის დაბრუნება.
 *
 * `items` void**-ია, რომ ერთი იმპლემენტაცია სამივე მასივს ემსახუროს.
 * NULL → realloc ჩავარდა; მაშინ ძველი მასივი ხელუხლებელი რჩება (არ იკარგება).
 */
static void *array_push(void **items, size_t *count, size_t *cap, size_t elem_size)
{
    if (*count == *cap) {
        size_t new_cap = (*cap == 0) ? 8 : (*cap * 2);
        if (new_cap > SIZE_MAX / elem_size) {
            return NULL; /* ზომის overflow */
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
/* ცვლადები: ${name} → მნიშვნელობა                                            */
/* ------------------------------------------------------------------------- */

/*
 * ჩანაცვლება ხდება *დაპარსულ* ხეზე და არა ნედლ ტექსტზე.
 *
 * ნედლ JSON-ში ჩანაცვლება მაცდური, მაგრამ საშიშია: მნიშვნელობაში მოხვედრილი
 * ბრჭყალი ან უკუსახაზავი მთელ დოკუმენტს გატეხდა. ხეზე მუშაობისას მნიშვნელობა
 * ყოველთვის რჩება ერთ სტრიქონულ კვანძში და სინტაქსს ვერ შეეხება.
 */
static char *expand_vars(const char *src, const cJSON *vars)
{
    if (src == NULL || vars == NULL || strstr(src, "${") == NULL) {
        return NULL; /* არაფერი შესაცვლელია */
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
                        fprintf(stderr, "გაფრთხილება: ცვლადი '${%s}' განსაზღვრული არაა.\n", name);
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

/* რეკურსიულად გადის ხეზე და ყველა სტრიქონულ მნიშვნელობაში ავრცობს ცვლადებს. */
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
/* სტილები: გამეორებადი თვისებების ერთ ადგილას აღწერა                         */
/* ------------------------------------------------------------------------- */

/*
 * ობიექტს ურთავს დასახელებული სტილის ველებს.
 *
 * ობიექტის *საკუთარი* ველები ყოველთვის იმარჯვებს — სტილი მხოლოდ იმას ავსებს,
 * რაც ობიექტს არ აქვს. `"style"` შეიძლება იყოს ერთი სახელი ან სახელების მასივი
 * (მაშინ თანმიმდევრობით ედება, პირველი ყველაზე ზოგადი).
 */
static void apply_style_chain(cJSON *obj, const cJSON *styles, const char *name, int depth)
{
    if (depth > 8) {
        fprintf(stderr, "გაფრთხილება: სტილების ციკლი '%s'-თან — შევჩერდი.\n", name);
        return;
    }

    const cJSON *st = cJSON_GetObjectItemCaseSensitive(styles, name);
    if (!cJSON_IsObject(st)) {
        fprintf(stderr, "გაფრთხილება: სტილი '%s' ვერ მოიძებნა.\n", name);
        return;
    }

    for (const cJSON *field = st->child; field != NULL; field = field->next) {
        if (field->string == NULL || strcmp(field->string, "style") == 0) {
            continue; /* "style" მეტამონაცემია და არა თვისება */
        }
        if (cJSON_GetObjectItemCaseSensitive(obj, field->string) != NULL) {
            continue; /* ობიექტს თავისი აქვს — ხელს არ ვახლებთ */
        }
        cJSON *copy = cJSON_Duplicate(field, true);
        if (copy != NULL) {
            cJSON_AddItemToObject(obj, field->string, copy);
        }
    }

    /*
     * სტილს თავად შეიძლება ჰქონდეს მშობელი: {"hero": {"style": "base", ...}}.
     * მშობელი *შემდეგ* ედება, ანუ უფრო კონკრეტული სტილი ყოველთვის იმარჯვებს.
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
/* ობიექტების პარსინგი                                                        */
/* ------------------------------------------------------------------------- */


/* ------------------------------------------------------------------------- */
/* Keyframe track-ების პარსინგი                                               */
/* ------------------------------------------------------------------------- */

/*
 * ველი შეიძლება იყოს ან რიცხვი, ან საკვანძო კადრების მასივი:
 *
 *     "opacity": 0.5
 *     "opacity": [ {"t": 0, "v": 0}, {"t": 1.2, "v": 1, "ease": "backout"} ]
 *
 * აბრუნებს true-ს მხოლოდ მაშინ, თუ რეალური ანიმაცია აიგო.
 */
static bool parse_track(const cJSON *item, Track *tr, float fallback)
{
    track_set_constant(tr, fallback);

    if (item == NULL) {
        return false;
    }
    if (cJSON_IsNumber(item)) {
        track_set_constant(tr, (float)item->valuedouble);
        return false; /* კონსტანტა — ანიმაცია არაა */
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
     * გასაღებები დროის ზრდადობით უნდა იდგეს — track_sample სწორედ ამას ეყრდნობა.
     * JSON-ის ავტორს ეს შეიძლება შეშალოს, ამიტომ თავად ვალაგებთ (ჩასმით
     * დახარისხება: გასაღებები ერთეულებია და თითქმის ყოველთვის უკვე დალაგებულია).
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
/* ეფექტების პარსინგი                                                         */
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

/* ნეიტრალური საწყისები — რასაც JSON არ გადააწერს, ის უცვლელს ტოვებს კადრს. */
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
            fx->color_a = (Color){  40,  70, 120, 255 }; /* ცივი ჩრდილები */
            fx->color_b = (Color){ 255, 190, 130, 255 }; /* თბილი შუქები   */
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
        return true; /* ეფექტები არჩევითია */
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        const char *name = json_str(item, "type", NULL);
        EffectType  type = effect_from_name(name);

        if (type == FX_NONE) {
            fprintf(stderr, "გაფრთხილება: უცნობი ეფექტი '%s' — გამოტოვებულია.\n",
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

        /* ფერების სინონიმები: vignette-ს "color", ტონირებას "shadows"/"highlights". */
        fx->color_a = json_color(item, "color",     fx->color_a);
        fx->color_a = json_color(item, "shadows",   fx->color_a);
        fx->color_a = json_color(item, "shadow",    fx->color_a);
        fx->color_b = json_color(item, "highlights", fx->color_b);
        fx->color_b = json_color(item, "highlight",  fx->color_b);
    }
    return true;
}

/* საერთო ველების წაკითხვა WidgetBase-ში. */
static void parse_widget_base(WidgetBase *base, const cJSON *obj, WidgetKind kind, int z)
{
    base->kind    = kind;
    base->id      = dup_string(json_str(obj, "id", "unnamed"));
    base->z_order = z;

    /* --- მიმაგრება ("anchor": "center", "bottomright", …) ---------------- */
    const char *anchor_name = json_str(obj, "anchor", NULL);
    if (anchor_name != NULL) {
        float ax, ay;
        if (layout_anchor_from_name(anchor_name, &ax, &ay)) {
            base->anchor_x = ax;
            base->anchor_y = ay;
            base->has_anchor_x = base->has_anchor_y = true;
        } else {
            fprintf(stderr, "გაფრთხილება: უცნობი anchor '%s'.\n", anchor_name);
        }
    }

    /* --- ფარდობითი გამოსახულებები ("center", "bottom-160") --------------- */
    const cJSON *x_item = cJSON_GetObjectItemCaseSensitive(obj, "x");
    const cJSON *y_item = cJSON_GetObjectItemCaseSensitive(obj, "y");

    if (cJSON_IsString(x_item)) {
        base->x_expr = dup_string(x_item->valuestring);
    }
    if (cJSON_IsString(y_item)) {
        base->y_expr = dup_string(y_item->valuestring);
    }

    /* ვიღებთ ორივე ჩაწერას: "x" და "x_pos" (JSON-ის ორივე დიალექტი). */
    if (json_has(obj, "x")) {
        base->x = json_float(obj, "x", 0.0f);
    } else if (json_has(obj, "x_pos")) {
        base->x = json_float(obj, "x_pos", 0.0f);
    } else {
        /* X არ მითითებულა → ჰორიზონტალურად ვაცენტრებთ, როცა ტექსტურის
         * სიგანე გახდება ცნობილი (იხ. media_prepare_textures). */
        base->auto_center_x = true;
    }

    base->y = json_has(obj, "y") ? json_float(obj, "y", 0.0f)
                                 : json_float(obj, "y_pos", 0.0f);

    base->tex.premultiplied = true;

    /*
     * თვისებების ტრეკები. თუ ველი მასივია, ის ანიმაციად იქცევა და საბაზისო
     * სტატიკურ მნიშვნელობას ჩაანაცვლებს; რიცხვი კი უბრალოდ კონსტანტაა.
     */
    parse_track(cJSON_GetObjectItemCaseSensitive(obj, "x"),        &base->tr_x, base->x);
    parse_track(cJSON_GetObjectItemCaseSensitive(obj, "y"),        &base->tr_y, base->y);
    parse_track(cJSON_GetObjectItemCaseSensitive(obj, "opacity"),  &base->tr_opacity, 1.0f);
    parse_track(cJSON_GetObjectItemCaseSensitive(obj, "scale"),    &base->tr_scale, 1.0f);
    parse_track(cJSON_GetObjectItemCaseSensitive(obj, "rotation"), &base->tr_rotation, 0.0f);

    /*
     * "has_track_*" ნიშნავს "JSON-მა ეს თვისება მიუთითა" და არა "ის ანიმირებულია".
     *
     * განსხვავება არსებითია: `"opacity": 0.0` მუდმივია, მაგრამ მაინც *მითითებულია*.
     * თუ აქ parse_track()-ის შედეგს დავეყრდნობოდით (ის მხოლოდ მასივზე აბრუნებს true-ს),
     * მუდმივი მნიშვნელობა ჩუმად დაიკარგებოდა და ობიექტი ნაგულისხმევ 1.0-ზე დაიხატებოდა —
     * ანუ დამალვის მცდელობა პირიქით, სრულ ხილვადობას იძლეოდა.
     */
    /*
     * სტრიქონული x/y გამოსახულებაა და არა ტრეკი — მისი მნიშვნელობა მოგვიანებით,
     * ზომის ცოდნის შემდეგ ითვლება და პირდაპირ base->x/y-ში ჩაიწერება. თუ აქ
     * has_track_x-ს დავაყენებდით, რენდერერი ტრეკს წაიკითხავდა (რომლის მუდმივაც
     * პარსინგის მომენტში 0-ია) და გამოთვლილ პოზიციას უგულებელყოფდა.
     */
    base->has_track_x        = json_has(obj, "x") && base->x_expr == NULL;
    base->has_track_y        = json_has(obj, "y") && base->y_expr == NULL;
    base->has_track_opacity  = json_has(obj, "opacity");
    base->has_track_scale    = json_has(obj, "scale");
    base->has_track_rotation = json_has(obj, "rotation");

    /* ანიმირებული X თავად განსაზღვრავს პოზიციას — ავტო-ცენტრირება აღარ სჭირდება. */
    if (base->has_track_x) {
        base->auto_center_x = false;
    }
}

/*
 * სიგრძის წაკითხვა: რიცხვი პიქსელებია, სტრიქონი კი გამოსახულება ("80%").
 * ასე max_width კადრის ზომაზე მიბმულად იწერება და გარჩევადობის შეცვლისას
 * თავისით ერგება.
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
        fprintf(stderr, "გაფრთხილება: გაუგებარი '%s' = '%s'.\n", key, it->valuestring);
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
    fprintf(stderr, "გაფრთხილება: უცნობი align '%s'.\n", a);
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
        w->size = 1; /* Cairo-სთვის 0/უარყოფითი ზომა უაზროა */
    }
    if (w->max_width < 0.0f) {
        w->max_width = 0.0f;
    }
    return w->content != NULL && w->font != NULL;
}

/*
 * ფარდობითი გზა JSON-ის საქაღალდის მიმართ იხსნება.
 *
 * ასე `"path": "logo.png"` მუშაობს იმის მიხედვით, სად დევს პროექტის ფაილი და
 * არა იმის მიხედვით, საიდან გაუშვა მომხმარებელმა პროგრამა.
 */
static char *resolve_relative_path(const char *base_file, const char *path)
{
    if (path == NULL) {
        return NULL;
    }
    if (path[0] == '/' || base_file == NULL) {
        return dup_string(path); /* აბსოლუტური — ხელს არ ვახლებთ */
    }

    const char *slash = strrchr(base_file, '/');
    if (slash == NULL) {
        return dup_string(path); /* JSON მიმდინარე საქაღალდეშია */
    }

    size_t dir_len = (size_t)(slash - base_file) + 1; /* '/'-ის ჩათვლით */
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
        /* წრეს რადიუსით ვწერთ, მაგრამ შიგნით ის შემომსაზღვრელი კვადრატია. */
        float r = json_float(obj, "radius", 50.0f);
        w->w = json_float(obj, "w", r * 2.0f);
        w->h = json_float(obj, "h", r * 2.0f);

        /* cx/cy = ცენტრი (videogen-ის კონვენცია) → ჩვენი ზედა-მარცხენა კუთხე. */
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
        fprintf(stderr, "შეცდომა: სურათს '%s' არ აქვს 'path'.\n",
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

    /* "code" ან "content" — ორივე მიიღება. */
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

    /* გაფერადება ჩართულია, თუ ენა ცნობილია და JSON-ში პირდაპირ არ გამორთეს. */
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
/* ხმოვანი ბილიკების პარსინგი                                                 */
/* ------------------------------------------------------------------------- */

static bool parse_audio(EditorContext *ctx, const cJSON *arr, const char *base_file)
{
    if (!cJSON_IsArray(arr)) {
        return true; /* აუდიო არჩევითია */
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        const char *path = json_str(item, "path", NULL);
        if (path == NULL) {
            path = json_str(item, "file", NULL);
        }

        if (path == NULL) {
            /* TTS განზრახ არ გვაქვს — ვამბობთ ღიად, რომ მიზეზი გასაგები იყოს. */
            if (json_has(item, "text")) {
                fprintf(stderr, "გაფრთხილება: ხმოვან ბილიკს 'text' აქვს, მაგრამ სინთეზი (TTS) "
                                "ამ პროექტის ნაწილი არ არის — მიუთითე მზა ფაილი 'path'-ით.\n");
            } else {
                fprintf(stderr, "გაფრთხილება: ხმოვან ბილიკს 'path' აკლია — გამოტოვებულია.\n");
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
         * ფაილის არსებობას *პარსინგისას* ვამოწმებთ და არა მიქსისას.
         *
         * მიზეზი პრაქტიკულია: ხმა ბოლო ეტაპია, ანუ გზაში დაშვებული შეცდომა
         * მხოლოდ მაშინ გამოჩნდებოდა, როცა ათასობით კადრი უკვე დარენდერებულია.
         * ჯობს პროგრამა პირველივე წამში გაჩერდეს.
         */
        FILE *probe = fopen(a->path, "rb");
        if (probe == NULL) {
            fprintf(stderr, "შეცდომა: ხმოვანი ფაილი '%s' ვერ გაიხსნა.\n", a->path);
            return false;
        }
        fclose(probe);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* ტაიმლაინის პარსინგი                                                        */
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
    e->target_index = -1; /* გაიხსნება resolve_timeline_targets()-ში */

    /* SCALE-ის ნეიტრალური მნიშვნელობა 1.0-ია (და არა 0, რაც ობიექტს გააქრობდა). */
    e->value = json_float(obj, "value", (e->action == ACTION_SCALE) ? 1.0f : 0.0f);

    /* ნაგულისხმევი smoothstep-ია — ზუსტად ის ქცევა, რაც აქამდე ჩაშენებული იყო. */
    e->ease = json_has(obj, "ease") ? easing_from_name(json_str(obj, "ease", NULL))
                                    : EASE_SMOOTH;

    /* MOVE-ის ღერძები: "move_x"/"move_y" ერთ `value`-ს იყენებს, ზოგადი "move" კი
     * ცალკე value_x/value_y-ს. */
    if (action_name != NULL && strcmp(action_name, "move_x") == 0) {
        e->value_x = e->value;
    } else if (action_name != NULL && strcmp(action_name, "move_y") == 0) {
        e->value_y = e->value;
    } else {
        e->value_x = json_float(obj, "value_x", 0.0f);
        e->value_y = json_float(obj, "value_y", 0.0f);
    }

    if (e->action == ACTION_UNKNOWN) {
        fprintf(stderr, "გაფრთხილება: უცნობი action '%s' (t=%d ms) — გამოტოვებულია.\n",
                action_name ? action_name : "(null)", e->time_ms);
    }
    if (e->duration_ms < 0) {
        e->duration_ms = 0;
    }
    return e->target_id != NULL;
}


/* ------------------------------------------------------------------------- */
/* გადასვლები                                                                 */
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

    fprintf(stderr, "გაფრთხილება: უცნობი გადასვლა '%s' — მაგივრად ჭრა.\n", name);
    return TRANS_CUT;
}

/* `from`/`to` ბლოკის არხები. აქ Track-ის "t" პროგრესია [0,1], და არა წამები. */
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
 * მასკის წაკითხვა: { "shape": "circle", "cx": .5, "cy": .5, "r": [...] }
 *                  { "shape": "rect",   "x": 0, "y": 0, "w": [...], "h": 1 }
 * პარამეტრები კადრის წილადებშია და ტრეკებიც შეიძლება იყოს.
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

        /* "duration" წამებშია (videogen-ის კონვენცია), "duration_ms" — მილიწამებში. */
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
/* ინდექსის აგება და ბმულების გახსნა                                          */
/* ------------------------------------------------------------------------- */

static int compare_by_z(const void *a, const void *b)
{
    const WidgetBase *wa = *(WidgetBase *const *)a;
    const WidgetBase *wb = *(WidgetBase *const *)b;
    return (wa->z_order > wb->z_order) - (wa->z_order < wb->z_order);
}

/*
 * აგებს ერთგვაროვან `widgets` ინდექსს.
 *
 * კრიტიკული: ეს უნდა მოხდეს *მას შემდეგ*, რაც `texts`/`codes` მასივები აღარ
 * გაიზრდება — realloc გადაანაცვლებს ელემენტებს და ინდექსის ყველა მაჩვენებელი
 * dangling გახდება.
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

    /* JSON-ის თანმიმდევრობა = ხატვის თანმიმდევრობა (painter's algorithm). */
    qsort(ctx->widgets, ctx->widget_count, sizeof(WidgetBase *), compare_by_z);
    return true;
}

/*
 * სამიზნეები სცენის *შიგნით* იხსნება და ინდექსიც სცენა-ლოკალურია.
 * ასე ერთი და იგივე id სხვადასხვა სცენაში დამოუკიდებლად შეიძლება იარსებოს.
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
                fprintf(stderr, "გაფრთხილება: სცენა '%s' — target '%s' არ მოიძებნა.\n",
                        sc->id ? sc->id : "(უსახელო)",
                        e->target_id ? e->target_id : "(null)");
            }
        }
    }
}

/* სცენების დაწყების მომენტები, გადასვლების გადაფარვის გათვალისწინებით. */
static void compute_scene_times(EditorContext *ctx)
{
    int t = 0;
    for (size_t i = 0; i < ctx->scene_count; i++) {
        ctx->scenes[i].start_ms = t;

        int overlap = 0;
        if (i < ctx->transition_count) {
            overlap = ctx->transitions[i].duration_ms;

            /* გადასვლა ვერც ერთ მეზობელ სცენაზე გრძელი ვერ იქნება. */
            int limit = ctx->scenes[i].duration_ms;
            if (i + 1 < ctx->scene_count && ctx->scenes[i + 1].duration_ms < limit) {
                limit = ctx->scenes[i + 1].duration_ms;
            }
            if (overlap > limit) {
                fprintf(stderr, "გაფრთხილება: გადასვლა #%zu (%d ms) მეზობელ სცენაზე გრძელია — %d ms-მდე შევკვეცე.\n",
                        i, overlap, limit);
                overlap = limit;
                ctx->transitions[i].duration_ms = overlap;
            }
        }

        t += ctx->scenes[i].duration_ms - overlap;
    }

    /* ფილმის სრული სიგრძე = ბოლო სცენის დასასრული. */
    if (ctx->scene_count > 0) {
        const Scene *last = &ctx->scenes[ctx->scene_count - 1];
        ctx->config.duration_ms = last->start_ms + last->duration_ms;
    }
}

/* ვიდეოს ხანგრძლივობა: ან JSON-იდან, ან ტაიმლაინის ბოლო მოვლენიდან + კუდი. */


/* ------------------------------------------------------------------------- */
/* სცენის პარსინგი                                                            */
/* ------------------------------------------------------------------------- */

/*
 * კითხულობს ერთ სცენას: მის ობიექტებს, ტაიმლაინსა და ხანგრძლივობას.
 *
 * იმავე ფუნქციას იყენებს ბრტყელი რეჟიმიც — მაშინ `node` თავად root-ია და
 * სცენა ერთადერთია. ერთი გზა ნიშნავს, რომ ორივე რეჟიმი ერთნაირად იქცევა.
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

    /* --- ობიექტები --- */
    const cJSON *objects = cJSON_GetObjectItemCaseSensitive(node, "objects");
    if (!cJSON_IsArray(objects)) {
        objects = cJSON_GetObjectItemCaseSensitive(node, "layers"); /* videogen-ის სინონიმი */
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
                fprintf(stderr, "გაფრთხილება: უცნობი ობიექტის ტიპი '%s' — გამოტოვებულია.\n", type);
                continue;
            }

            if (!ok) {
                fprintf(stderr, "შეცდომა: ობიექტისთვის მეხსიერება ვერ გამოიყო.\n");
                return false;
            }
            (*z)++;   /* z გლობალურად იზრდება → სცენის ობიექტები ინდექსში მომიჯნავე რჩება */
        }
    }

    sc->widget_count = (ctx->text_count + ctx->code_count + ctx->image_count +
                        ctx->shape_count) - sc->first_widget;

    /*
     * --- სცენის საკუთარი ეფექტები ---
     *
     * ბრტყელ რეჟიმში `node` თავად root-ია, ანუ აქაური "effects" *გლობალური*
     * სტეკია და მას ცალკე კითხულობს parse_video_project_ex(). მისი აქ მეორედ
     * წაკითხვა იმას ნიშნავდა, რომ ყველა ეფექტი ორჯერ დაედებოდა კადრს.
     */
    if (!is_root &&
        !parse_effects_into(&sc->effects, &sc->effect_count, &sc->effect_cap,
                            cJSON_GetObjectItemCaseSensitive(node, "effects"))) {
        return false;
    }

    /* --- ტაიმლაინი --- */
    const cJSON *timeline = cJSON_GetObjectItemCaseSensitive(node, "timeline");
    if (cJSON_IsArray(timeline)) {
        const cJSON *ev = NULL;
        cJSON_ArrayForEach(ev, timeline) {
            if (!parse_timeline_event(sc, ev)) {
                fprintf(stderr, "შეცდომა: ტაიმლაინისთვის მეხსიერება ვერ გამოიყო.\n");
                return false;
            }
        }
    }

    /* --- ხანგრძლივობა --- */
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
/* მთავარი შესასვლელი                                                         */
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
    free(json_text); /* cJSON-მა უკვე გადმოიღო თავისი ასლები */

    if (root == NULL) {
        const char *err = cJSON_GetErrorPtr();
        fprintf(stderr, "შეცდომა JSON-ის პარსინგისას%s%.40s\n",
                err ? " ახლოს: " : ".", err ? err : "");
        return NULL;
    }

    /* --- 0. ცვლადები: JSON-ის "vars" + ბრძანების ხაზის --set ------------- */
    cJSON *vars = cJSON_GetObjectItemCaseSensitive(root, "vars");
    if (vars == NULL) {
        vars = cJSON_AddObjectToObject(root, "vars");
    }

    for (int i = 0; i < define_count; i++) {
        const char *eq = strchr(defines[i], '=');
        if (eq == NULL) {
            fprintf(stderr, "გაფრთხილება: --set '%s' არ შეიცავს '='.\n", defines[i]);
            continue;
        }
        size_t klen = (size_t)(eq - defines[i]);
        char   key[128];
        if (klen >= sizeof key) {
            klen = sizeof key - 1;
        }
        memcpy(key, defines[i], klen);
        key[klen] = '\0';

        /* ბრძანების ხაზი JSON-ის მნიშვნელობას გადააწერს. */
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

    /* --- 1b. output { } — კოდირების პარამეტრები --------------------------- */
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

    /* გონივრული საზღვრები — არასწორი კონფიგი VRAM-ის უაზრო მოთხოვნას ნიშნავს. */
    if (ctx->config.width  < 16 || ctx->config.width  > 16384 ||
        ctx->config.height < 16 || ctx->config.height > 16384 ||
        ctx->config.fps    < 1  || ctx->config.fps    > 480) {
        fprintf(stderr, "შეცდომა: არასწორი კონფიგი %dx%d @ %d fps.\n",
                ctx->config.width, ctx->config.height, ctx->config.fps);
        goto fail;
    }

    /*
     * H.264/HEVC-ის 4:2:0 ქრომა ორ პიქსელს ერთ ნიმუშად კრებს, ამიტომ კენტი
     * ზომა კოდირებისას შეცდომას იძლევა. ჩუმად დამრგვალება უკეთესია, ვიდრე
     * ffmpeg-ის გაუგებარი შეცდომა რენდერის ბოლოს.
     */
    if ((ctx->config.width % 2) != 0 || (ctx->config.height % 2) != 0) {
        fprintf(stderr, "გაფრთხილება: %dx%d კენტია — ვამრგვალებთ %dx%d-მდე (4:2:0 მოითხოვს ლუწს).\n",
                ctx->config.width, ctx->config.height,
                ctx->config.width & ~1, ctx->config.height & ~1);
        ctx->config.width  &= ~1;
        ctx->config.height &= ~1;
    }

    /* --- 2. სცენები ან ბრტყელი objects[] ---------------------------------- */
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
                fprintf(stderr, "შეცდომა: 'scenes' ცარიელია.\n");
                goto fail;
            }
        } else {
            /*
             * ბრტყელი რეჟიმი: ერთი იმპლიციტური სცენა, რომელიც ყველა ობიექტს
             * იტევს. ასე რენდერერს ერთი კოდის გზა რჩება და ძველი პროექტები
             * უცვლელად მუშაობს.
             */
            if (!parse_scene(ctx, root, styles, filepath, &z, true)) {
                goto fail;
            }
        }
    }


    /* --- 3b. effects [ ] -------------------------------------------------- */
    if (!parse_effects_into(&ctx->effects, &ctx->effect_count, &ctx->effect_cap,
                            cJSON_GetObjectItemCaseSensitive(root, "effects"))) {
        fprintf(stderr, "შეცდომა: ეფექტებისთვის მეხსიერება ვერ გამოიყო.\n");
        goto fail;
    }

    /* --- 3c. audio [ ] ---------------------------------------------------- */
    if (!parse_audio(ctx, cJSON_GetObjectItemCaseSensitive(root, "audio"), filepath)) {
        fprintf(stderr, "შეცდომა: ხმოვანი ბილიკები ვერ დამუშავდა.\n");
        goto fail;
    }

    /* --- 4. ინდექსი, ბმულები, ხანგრძლივობა, არენა ------------------------- */
    if (!ctx_build_widget_index(ctx)) {
        fprintf(stderr, "შეცდომა: ვიჯეტების ინდექსი ვერ აიგო.\n");
        goto fail;
    }
    resolve_timeline_targets(ctx);

    if (!parse_transitions(ctx, cJSON_GetObjectItemCaseSensitive(root, "transitions"))) {
        fprintf(stderr, "შეცდომა: გადასვლებისთვის მეხსიერება ვერ გამოიყო.\n");
        goto fail;
    }

    /*
     * ბრტყელ რეჟიმში პროექტის ხანგრძლივობა კარნახობს ერთადერთი სცენისას;
     * სცენების რეჟიმში კი პირიქით — ჯამი განისაზღვრება სცენებით.
     */
    if (ctx->scene_count == 1 && ctx->config.duration_ms > 0) {
        ctx->scenes[0].duration_ms = ctx->config.duration_ms;
    }
    compute_scene_times(ctx);

    if (!arena_init(&ctx->frame_arena, FRAME_ARENA_BYTES)) {
        fprintf(stderr, "შეცდომა: კადრის არენა ვერ გამოიყო.\n");
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
/* გასუფთავება                                                                */
/* ------------------------------------------------------------------------- */

/* ვიჯეტის საერთო ნაწილის რესურსები (მათ შორის თვისებების ტრეკები). */
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

    /* VRAM ჯერ — renderer-მა შეიძლება ჯერ კიდევ ეჭიროს device მაჩვენებლები,
     * რომლებიც ტექსტურებში წერია. იდემპოტენტურია. */
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
        texture_free(&w->plate); /* ფირფიტა ცალკე შრეა */
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

    free(ctx->widgets); /* მხოლოდ ინდექსი — თვით ვიჯეტები უკვე გათავისუფლდა */
    arena_destroy(&ctx->frame_arena);
    free(ctx);
}

/* ------------------------------------------------------------------------- */
/* დიაგნოსტიკა                                                                */
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

    fprintf(stderr, "--- პროექტი -------------------------------------------\n");
    fprintf(stderr, "  გარჩევადობა : %dx%d @ %d fps\n",
            ctx->config.width, ctx->config.height, ctx->config.fps);
    fprintf(stderr, "  ხანგრძლივობა: %d ms (%d კადრი)\n", ctx->config.duration_ms,
            (int)(((long long)ctx->config.duration_ms * ctx->config.fps + 999) / 1000));
    fprintf(stderr, "  ფონი        : #%02X%02X%02X%02X\n", ctx->config.bg_color.r,
            ctx->config.bg_color.g, ctx->config.bg_color.b, ctx->config.bg_color.a);

    fprintf(stderr, "--- ობიექტები (%zu) -----------------------------------\n", ctx->widget_count);
    for (size_t i = 0; i < ctx->widget_count; i++) {
        const WidgetBase *b = ctx->widgets[i];
        fprintf(stderr, "  [%zu] %-14s %-6s pos=(%s%.0f, %.0f) tex=%dx%d  %d სიმბოლო\n", i,
                b->id ? b->id : "(null)",
                (b->kind == WIDGET_TEXT)  ? "text"  : (b->kind == WIDGET_CODE)   ? "code" :
                (b->kind == WIDGET_IMAGE) ? "image" : (b->kind == WIDGET_CIRCLE) ? "circle" : "rect",
                b->auto_center_x ? "auto:" : "",
                (double)(b->x - b->anchor_off_x), (double)(b->y - b->anchor_off_y),
                b->tex.width, b->tex.height, b->glyphs.total_chars);
    }

    fprintf(stderr, "--- სცენები (%zu) -------------------------------------\n", ctx->scene_count);
    for (size_t si = 0; si < ctx->scene_count; si++) {
        const Scene *sc = &ctx->scenes[si];
        fprintf(stderr, "  [%zu] %-12s %6d..%-6d ms  %zu ობიექტი, %zu მოვლენა\n", si,
                sc->id ? sc->id : "(უსახელო)", sc->start_ms,
                sc->start_ms + sc->duration_ms, sc->widget_count, sc->event_count);

        for (size_t i = 0; i < sc->event_count; i++) {
            const TimelineEvent *e = &sc->events[i];
            fprintf(stderr, "        t=%5d ms  %-10s → %-14s dur=%d ms\n",
                    e->time_ms, action_name(e->action),
                    e->target_id ? e->target_id : "(null)", e->duration_ms);
        }
        if (si < ctx->transition_count && ctx->transitions[si].type != TRANS_CUT) {
            fprintf(stderr, "      ↕ გადასვლა: %d ms\n", ctx->transitions[si].duration_ms);
        }
    }
    if (ctx->effect_count > 0) {
        fprintf(stderr, "--- ეფექტები (%zu) -------------------------------------\n",
                ctx->effect_count);
        for (size_t i = 0; i < ctx->effect_count; i++) {
            fprintf(stderr, "  [%zu] %s\n", i, effect_name(ctx->effects[i].type));
        }
    }
    if (ctx->audio_count > 0) {
        fprintf(stderr, "--- ხმა (%zu) ------------------------------------------\n",
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
/* ვალიდაცია                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * მიმაგრების შემდეგ `base->x/y` მიმაგრების *წერტილია* და არა ზედა-მარცხენა
 * კუთხე. დიაგნოსტიკამ და ვალიდაციამ ნამდვილი კიდე უნდა დაინახოს, თორემ
 * "right"-ზე მიმაგრებული ობიექტი ცრუდ აღინიშნება კადრს გარეთ გასულად.
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

    fprintf(stderr, "--- შემოწმება ------------------------------------------\n");

    /* --- პროექტი --------------------------------------------------------- */
    if (ctx->config.duration_ms <= 0) {
        VR_PROBLEM("ხანგრძლივობა ნულოვანია.\n");
    }
    if (ctx->widget_count == 0) {
        VR_PROBLEM("პროექტში არც ერთი ობიექტი არაა.\n");
    }

    /* --- ობიექტები -------------------------------------------------------- */
    for (size_t i = 0; i < ctx->widget_count; i++) {
        const WidgetBase *b  = ctx->widgets[i];
        const char       *id = b->id ? b->id : "(null)";

        if (b->base_w < 1.0f || b->base_h < 1.0f) {
            VR_PROBLEM("'%s' — ნულოვანი ზომა (%.0fx%.0f).\n", id,
                       (double)b->base_w, (double)b->base_h);
            continue;
        }

        float x0 = widget_left(b), y0 = widget_top(b);
        float x1 = x0 + b->base_w,  y1 = y0 + b->base_h;

        /* სრულიად კადრს გარეთ — ეს ყოველთვის შეცდომაა. */
        if (x1 <= 0.0f || y1 <= 0.0f ||
            x0 >= (float)ctx->config.width || y0 >= (float)ctx->config.height) {
            VR_PROBLEM("'%s' (%s) — მთლიანად კადრს გარეთაა: x=%.0f..%.0f y=%.0f..%.0f\n",
                       id, kind_name(b->kind), (double)x0, (double)x1, (double)y0, (double)y1);
            continue;
        }

        /*
         * ნაწილობრივ ამოვარდნა სურათისთვის ნორმაა (cover-კადრი განზრახ იჭრება),
         * ტექსტისთვის კი თითქმის ყოველთვის ხარვეზია.
         */
        bool clipped = (x0 < 0.0f) || (y0 < 0.0f) ||
                       (x1 > (float)ctx->config.width) || (y1 > (float)ctx->config.height);

        if (clipped) {
            if (b->kind == WIDGET_TEXT || b->kind == WIDGET_CODE) {
                VR_PROBLEM("'%s' (%s) — კიდეზე იჭრება: x=%.0f..%.0f y=%.0f..%.0f (კადრი %dx%d)\n",
                           id, kind_name(b->kind), (double)x0, (double)x1,
                           (double)y0, (double)y1, ctx->config.width, ctx->config.height);
            } else {
                VR_NOTE("'%s' (%s) კადრს სცდება — სავარაუდოდ განზრახ (cover).\n",
                        id, kind_name(b->kind));
            }
        }

        if (b->tex.pixels == NULL) {
            VR_PROBLEM("'%s' — ტექსტურა არ დარასტერიზდა.\n", id);
        }
    }

    /* --- ტაიმლაინი (სცენების მიხედვით) ------------------------------------ */
    size_t total_events = 0;
    for (size_t si = 0; si < ctx->scene_count; si++) {
        const Scene *sc = &ctx->scenes[si];
        const char  *sn = sc->id ? sc->id : "(უსახელო)";
        total_events += sc->event_count;

        if (sc->duration_ms <= 0) {
            VR_PROBLEM("სცენა '%s' — ნულოვანი ხანგრძლივობა.\n", sn);
        }

        /*
         * დუბლირებული id მხოლოდ *ერთი სცენის შიგნით* არის პრობლემა — სამიზნეები
         * სცენა-ლოკალურად იხსნება, ამიტომ სხვადასხვა სცენაში ერთი და იგივე
         * სახელი სრულიად ნორმალურია (და შაბლონებში სასურველიც).
         */
        for (size_t a = 0; a < sc->widget_count; a++) {
            const char *ia = ctx->widgets[sc->first_widget + a]->id;
            if (ia == NULL) {
                continue;
            }
            for (size_t b2 = a + 1; b2 < sc->widget_count; b2++) {
                const char *ib = ctx->widgets[sc->first_widget + b2]->id;
                if (ib != NULL && strcmp(ia, ib) == 0) {
                    VR_PROBLEM("სცენა '%s' — id '%s' მეორდება; ტაიმლაინი მხოლოდ პირველს მიწვდება.\n",
                               sn, ia);
                    break;
                }
            }
        }

        for (size_t i = 0; i < sc->event_count; i++) {
            const TimelineEvent *e = &sc->events[i];

            if (e->action == ACTION_UNKNOWN) {
                VR_PROBLEM("'%s' #%zu — უცნობი action (t=%d ms).\n", sn, i, e->time_ms);
                continue;
            }
            if (e->target_index < 0) {
                VR_PROBLEM("'%s' #%zu — target '%s' ვერ მოიძებნა.\n", sn, i,
                           e->target_id ? e->target_id : "(null)");
            }
            if (e->time_ms > sc->duration_ms) {
                VR_PROBLEM("'%s' #%zu — t=%d ms სცენის ბოლოს (%d ms) მიღმაა.\n",
                           sn, i, e->time_ms, sc->duration_ms);
            }
        }
    }

    /* --- ხმა -------------------------------------------------------------- */
    for (size_t i = 0; i < ctx->audio_count; i++) {
        const AudioTrack *a = &ctx->audio[i];
        if (a->start * 1000.0f >= (float)ctx->config.duration_ms) {
            VR_PROBLEM("ხმა '%s' ვიდეოს ბოლოს მიღმა იწყება (%.2f წმ).\n",
                       a->path ? a->path : "(null)", (double)a->start);
        }
    }

    if (problems == 0) {
        fprintf(stderr, "  ✓ პრობლემა ვერ მოიძებნა (%zu სცენა, %zu ობიექტი, %zu მოვლენა)\n",
                ctx->scene_count, ctx->widget_count, total_events);
    } else {
        fprintf(stderr, "  %d პრობლემა\n", problems);
    }
    fprintf(stderr, "-------------------------------------------------------\n");

    #undef VR_PROBLEM
    #undef VR_NOTE
    return problems;
}
