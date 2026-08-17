/*
 * anim.c — easing-ის მრუდები და keyframe track-ების შერჩევა.
 *
 * ყველა მრუდი განსაზღვრულია [0,1] → ℝ. back/elastic/bounce განზრახ გამოდის
 * [0,1] დიაპაზონიდან — სწორედ ეს გადაჭარბება ქმნის "ცოცხალი" მოძრაობის
 * შეგრძნებას, რომელსაც წრფივი ინტერპოლაცია ვერ იძლევა.
 */

#include "anim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* სახელების ცხრილი                                                           */
/* ------------------------------------------------------------------------- */

static const struct {
    const char *name;
    EaseType    type;
} kEaseNames[] = {
    { "linear",       EASE_LINEAR       },
    { "easein",       EASE_IN           },
    { "easeout",      EASE_OUT          },
    { "easeinout",    EASE_INOUT        },
    { "cubicin",      EASE_CUBIC_IN     },
    { "cubicout",     EASE_CUBIC_OUT    },
    { "cubicinout",   EASE_CUBIC_INOUT  },
    { "expoin",       EASE_EXPO_IN      },
    { "expoout",      EASE_EXPO_OUT     },
    { "expoinout",    EASE_EXPO_INOUT   },
    { "backin",       EASE_BACK_IN      },
    { "backout",      EASE_BACK_OUT     },
    { "backinout",    EASE_BACK_INOUT   },
    { "elasticout",   EASE_ELASTIC_OUT  },
    { "bounceout",    EASE_BOUNCE_OUT   },
    { "smooth",       EASE_SMOOTH       },
    { "smoothstep",   EASE_SMOOTH       },
};

/*
 * შედარება, რომელიც უგულებელყოფს რეგისტრს, '_'-ს, '-'-ს და ხარეებს.
 * ასე "easeInOut", "ease_in_out" და "easeinout" ერთი და იგივეა — JSON-ის
 * ავტორს არ უნდა ახსოვდეს ჩვენი მართლწერა.
 */
static bool loose_equal(const char *a, const char *b)
{
    while (*a && *b) {
        while (*a == '_' || *a == '-' || *a == ' ') a++;
        while (*b == '_' || *b == '-' || *b == ' ') b++;
        if (*a == '\0' || *b == '\0') {
            break;
        }

        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) {
            return false;
        }
        a++; b++;
    }

    while (*a == '_' || *a == '-' || *a == ' ') a++;
    while (*b == '_' || *b == '-' || *b == ' ') b++;
    return *a == '\0' && *b == '\0';
}

EaseType easing_from_name(const char *name)
{
    if (name == NULL) {
        return EASE_LINEAR;
    }
    for (size_t i = 0; i < sizeof kEaseNames / sizeof kEaseNames[0]; i++) {
        if (loose_equal(name, kEaseNames[i].name)) {
            return kEaseNames[i].type;
        }
    }
    return EASE_LINEAR;
}

const char *easing_name(EaseType e)
{
    for (size_t i = 0; i < sizeof kEaseNames / sizeof kEaseNames[0]; i++) {
        if (kEaseNames[i].type == e) {
            return kEaseNames[i].name;
        }
    }
    return "linear";
}

/* ------------------------------------------------------------------------- */
/* მრუდები                                                                    */
/* ------------------------------------------------------------------------- */

static float clamp01(float p)
{
    return (p < 0.0f) ? 0.0f : (p > 1.0f) ? 1.0f : p;
}

/* bounce — გეომეტრიული პროგრესიით მცირდება ხტუნვის სიმაღლე. */
static float bounce_out(float p)
{
    const float n = 7.5625f;
    const float d = 2.75f;

    if (p < 1.0f / d) {
        return n * p * p;
    } else if (p < 2.0f / d) {
        p -= 1.5f / d;
        return n * p * p + 0.75f;
    } else if (p < 2.5f / d) {
        p -= 2.25f / d;
        return n * p * p + 0.9375f;
    }
    p -= 2.625f / d;
    return n * p * p + 0.984375f;
}

float easing_apply(EaseType e, float p)
{
    p = clamp01(p);

    /* back-ის სტანდარტული ზედმეტობის კოეფიციენტი (≈10% გადაჭარბება). */
    const float c1 = 1.70158f;
    const float c2 = c1 * 1.525f;
    const float c3 = c1 + 1.0f;

    switch (e) {
        case EASE_LINEAR:      return p;

        case EASE_IN:          return p * p;
        case EASE_OUT:         return 1.0f - (1.0f - p) * (1.0f - p);
        case EASE_INOUT:       return (p < 0.5f) ? 2.0f * p * p
                                                 : 1.0f - 2.0f * (1.0f - p) * (1.0f - p);

        case EASE_CUBIC_IN:    return p * p * p;
        case EASE_CUBIC_OUT: {
            float q = 1.0f - p;
            return 1.0f - q * q * q;
        }
        case EASE_CUBIC_INOUT: {
            if (p < 0.5f) {
                return 4.0f * p * p * p;
            }
            float q = -2.0f * p + 2.0f;
            return 1.0f - q * q * q / 2.0f;
        }

        case EASE_EXPO_IN:     return (p <= 0.0f) ? 0.0f : powf(2.0f, 10.0f * p - 10.0f);
        case EASE_EXPO_OUT:    return (p >= 1.0f) ? 1.0f : 1.0f - powf(2.0f, -10.0f * p);
        case EASE_EXPO_INOUT:
            if (p <= 0.0f) return 0.0f;
            if (p >= 1.0f) return 1.0f;
            return (p < 0.5f) ? powf(2.0f, 20.0f * p - 10.0f) / 2.0f
                              : (2.0f - powf(2.0f, -20.0f * p + 10.0f)) / 2.0f;

        case EASE_BACK_IN:     return c3 * p * p * p - c1 * p * p;
        case EASE_BACK_OUT: {
            float q = p - 1.0f;
            return 1.0f + c3 * q * q * q + c1 * q * q;
        }
        case EASE_BACK_INOUT:
            if (p < 0.5f) {
                float q = 2.0f * p;
                return (q * q * ((c2 + 1.0f) * q - c2)) / 2.0f;
            } else {
                float q = 2.0f * p - 2.0f;
                return (q * q * ((c2 + 1.0f) * q + c2) + 2.0f) / 2.0f;
            }

        case EASE_ELASTIC_OUT: {
            if (p <= 0.0f) return 0.0f;
            if (p >= 1.0f) return 1.0f;
            const float c4 = (2.0f * 3.14159265358979323846f) / 3.0f;
            return powf(2.0f, -10.0f * p) * sinf((p * 10.0f - 0.75f) * c4) + 1.0f;
        }

        case EASE_BOUNCE_OUT:  return bounce_out(p);

        case EASE_SMOOTH:
        default:               return p * p * (3.0f - 2.0f * p);
    }
}

/* ------------------------------------------------------------------------- */
/* Track                                                                      */
/* ------------------------------------------------------------------------- */

void track_set_constant(Track *tr, float value)
{
    if (tr == NULL) {
        return;
    }
    tr->keys     = NULL;
    tr->count    = 0;
    tr->constant = value;
}

bool track_is_animated(const Track *tr)
{
    return tr != NULL && tr->keys != NULL && tr->count > 0;
}

float track_sample(const Track *tr, float t)
{
    if (tr == NULL) {
        return 0.0f;
    }
    if (tr->keys == NULL || tr->count <= 0) {
        return tr->constant;
    }
    if (tr->count == 1 || t <= tr->keys[0].t) {
        return tr->keys[0].v;
    }
    if (t >= tr->keys[tr->count - 1].t) {
        return tr->keys[tr->count - 1].v;
    }

    /*
     * წრფივი ძებნა: საკვანძო კადრები ერთეულებია (ტიპურად 2-5), ამიტომ ორობითი
     * ძებნა აქ მხოლოდ კოდს ართულებდა და არაფერს მოიგებდა.
     */
    int i = 0;
    while (i + 1 < tr->count && tr->keys[i + 1].t <= t) {
        i++;
    }

    const Keyframe *a = &tr->keys[i];
    const Keyframe *b = &tr->keys[i + 1];

    float span = b->t - a->t;
    if (span <= 0.0f) {
        return b->v; /* დამთხვეული ან შებრუნებული გასაღებები — ვიღებთ მეორეს */
    }

    /* easing ეკუთვნის იმ გასაღებს, რომლისკენაც მივდივართ (b). */
    float p = easing_apply(b->ease, (t - a->t) / span);
    return a->v + (b->v - a->v) * p;
}

void track_free(Track *tr)
{
    if (tr == NULL) {
        return;
    }
    free(tr->keys);
    tr->keys  = NULL;
    tr->count = 0;
}
