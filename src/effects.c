/*
 * effects.c — the effect name table and lifetime management.
 *
 * The pixel maths itself lives in renderer.cu (in the kernels); this file holds
 * only the host-side part that needs no CUDA.
 */

#include "effects.h"

#include <string.h>

/* Comparison that tolerates case and separators: "rgb_split", "rgbSplit" and
 * "RGB-SPLIT" are all the same name. */
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

static const struct {
    const char *name;
    EffectType  type;
} kNames[] = {
    { "grayscale",    FX_GRAYSCALE    },
    { "greyscale",    FX_GRAYSCALE    },
    { "invert",       FX_INVERT       },
    { "sepia",        FX_SEPIA        },
    { "posterize",    FX_POSTERIZE    },
    { "threshold",    FX_THRESHOLD    },
    { "vignette",     FX_VIGNETTE     },
    { "grain",        FX_GRAIN        },
    { "noise",        FX_GRAIN        },
    { "scanlines",    FX_SCANLINES    },
    { "color_grade",  FX_COLOR_GRADE  },
    { "grade",        FX_COLOR_GRADE  },
    { "vibrance",     FX_VIBRANCE     },
    { "split_tone",   FX_SPLIT_TONE   },
    { "gradient_map", FX_GRADIENT_MAP },
    { "duotone",      FX_GRADIENT_MAP },
    { "blur",         FX_BLUR         },
    { "pixelate",     FX_PIXELATE     },
    { "rgb_split",    FX_RGB_SPLIT    },
    { "chromatic",    FX_RGB_SPLIT    },
    { "glitch",       FX_GLITCH       },
};

EffectType effect_from_name(const char *name)
{
    if (name == NULL) {
        return FX_NONE;
    }
    for (size_t i = 0; i < sizeof kNames / sizeof kNames[0]; i++) {
        if (loose_equal(name, kNames[i].name)) {
            return kNames[i].type;
        }
    }
    return FX_NONE;
}

const char *effect_name(EffectType t)
{
    for (size_t i = 0; i < sizeof kNames / sizeof kNames[0]; i++) {
        if (kNames[i].type == t) {
            return kNames[i].name;
        }
    }
    return "none";
}

bool effect_needs_neighbors(EffectType t)
{
    switch (t) {
        case FX_BLUR:
        case FX_PIXELATE:
        case FX_RGB_SPLIT:
        case FX_GLITCH:
            return true;
        default:
            return false;
    }
}

void effect_free(Effect *fx)
{
    if (fx == NULL) {
        return;
    }
    for (int i = 0; i < FXP_MAX; i++) {
        track_free(&fx->param[i]);
    }
}
