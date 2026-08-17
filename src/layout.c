/*
 * layout.c — პოზიციის გამოსახულებების გამომთვლელი.
 *
 * მიზანმიმართულად პატარა რეკურსიის გარეშე პარსერია: გამოსახულებები მოკლეა
 * ("bottom-160", "50%+40") და სრული გამოთვლითი ენა აქ მხოლოდ ხარვეზების
 * წყარო იქნებოდა.
 */

#include "layout.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* მიმაგრების სახელები                                                        */
/* ------------------------------------------------------------------------- */

bool layout_anchor_from_name(const char *name, float *out_ax, float *out_ay)
{
    if (name == NULL || out_ax == NULL || out_ay == NULL) {
        return false;
    }

    /* 9 წერტილი, როგორც გრაფიკულ რედაქტორებში. */
    static const struct {
        const char *name;
        float       ax, ay;
    } kAnchors[] = {
        { "topleft",     0.0f, 0.0f }, { "top",    0.5f, 0.0f }, { "topright",     1.0f, 0.0f },
        { "left",        0.0f, 0.5f }, { "center", 0.5f, 0.5f }, { "right",        1.0f, 0.5f },
        { "bottomleft",  0.0f, 1.0f }, { "bottom", 0.5f, 1.0f }, { "bottomright",  1.0f, 1.0f },
        { "middle",      0.5f, 0.5f },
    };

    for (size_t i = 0; i < sizeof kAnchors / sizeof kAnchors[0]; i++) {
        const char *a = name, *b = kAnchors[i].name;
        while (*a && *b) {
            /* '-', '_' და ხარეები იგნორირდება: "bottom-right" == "bottomright" */
            while (*a == '-' || *a == '_' || *a == ' ') a++;
            int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
            if (ca != *b) break;
            a++; b++;
        }
        while (*a == '-' || *a == '_' || *a == ' ') a++;
        if (*a == '\0' && *b == '\0') {
            *out_ax = kAnchors[i].ax;
            *out_ay = kAnchors[i].ay;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* გამოსახულების გამოთვლა                                                     */
/* ------------------------------------------------------------------------- */

/* ერთი "term"-ის წაკითხვა. აბრუნებს false-ს, თუ ვერაფერი ამოიცნო. */
static bool read_term(const char **p, float canvas, LayoutAxis axis,
                      float *out_value, float *out_anchor, bool *out_has_anchor)
{
    const char *s = *p;
    while (isspace((unsigned char)*s)) {
        s++;
    }

    /* --- საკვანძო სიტყვა --- */
    if (isalpha((unsigned char)*s)) {
        const char *start = s;
        while (isalpha((unsigned char)*s)) {
            s++;
        }

        size_t len = (size_t)(s - start);
        char   word[24];
        if (len >= sizeof word) {
            return false;
        }
        for (size_t i = 0; i < len; i++) {
            word[i] = (char)tolower((unsigned char)start[i]);
        }
        word[len] = '\0';

        float frac;
        if (axis == LAYOUT_AXIS_X) {
            if      (strcmp(word, "left")   == 0) frac = 0.0f;
            else if (strcmp(word, "center") == 0) frac = 0.5f;
            else if (strcmp(word, "middle") == 0) frac = 0.5f;
            else if (strcmp(word, "right")  == 0) frac = 1.0f;
            else return false;
        } else {
            if      (strcmp(word, "top")    == 0) frac = 0.0f;
            else if (strcmp(word, "center") == 0) frac = 0.5f;
            else if (strcmp(word, "middle") == 0) frac = 0.5f;
            else if (strcmp(word, "bottom") == 0) frac = 1.0f;
            else return false;
        }

        *out_value = canvas * frac;

        /* პირველი საკვანძო სიტყვა კარნახობს ნაგულისხმევ მიმაგრებას:
         * "bottom-160" ბუნებრივად ნიშნავს "ქვედა კიდიდან 160px", ანუ ობიექტის
         * ქვედა კიდე უნდა აითვალოს და არა ზედა. */
        if (!*out_has_anchor) {
            *out_anchor     = frac;
            *out_has_anchor = true;
        }

        *p = s;
        return true;
    }

    /* --- რიცხვი, შესაძლოა პროცენტით --- */
    char  *end = NULL;
    double v   = strtod(s, &end);
    if (end == s) {
        return false;
    }
    s = end;

    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '%') {
        v = (double)canvas * v / 100.0;
        s++;
    }

    *out_value = (float)v;
    *p = s;
    return true;
}

bool layout_eval(const char *expr, float canvas, LayoutAxis axis,
                 float *out_pos, float *out_anchor)
{
    if (expr == NULL || out_pos == NULL || out_anchor == NULL) {
        return false;
    }

    const char *p          = expr;
    float       total      = 0.0f;
    float       anchor     = 0.0f;
    bool        has_anchor = false;
    float       term       = 0.0f;

    if (!read_term(&p, canvas, axis, &term, &anchor, &has_anchor)) {
        return false;
    }
    total = term;

    for (;;) {
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (*p != '+' && *p != '-') {
            return false; /* უცნობი სიმბოლო */
        }

        float sign = (*p == '+') ? 1.0f : -1.0f;
        p++;

        if (!read_term(&p, canvas, axis, &term, &anchor, &has_anchor)) {
            return false;
        }
        total += sign * term;
    }

    *out_pos    = total;
    *out_anchor = anchor;
    return true;
}
