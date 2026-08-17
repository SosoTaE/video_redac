/*
 * render_common.c — the backend-independent half of the renderer.
 *
 * See render_common.h for the division of labour. In short: this file decides
 * *what* a frame contains; a backend decides *how* the pixels get written.
 *
 * Nothing here allocates a frame buffer, touches a device, or knows about
 * threads. The one exception is vr_open_ffmpeg_pipe(), which starts the encoder
 * — shared because both backends need exactly the same colour tagging, and
 * getting that wrong silently corrupts colour (see the comment there).
 */

#include "render_common.h"

#include "anim.h"
#include "audio.h"   /* vr_shell_quote() */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

float vr_clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

double vr_seconds_since(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) + (double)(now.tv_nsec - start->tv_nsec) * 1e-9;
}

/* ------------------------------------------------------------------------- */
/* Timeline evaluation                                                        */
/* ------------------------------------------------------------------------- */

/* The easing curves live in anim.c — see easing_apply(). */

/* Whether a widget has an event of the given type (used to pick its initial state). */
static bool widget_has_action(const Scene *scene, int widget_index, ActionType action)
{
    for (size_t i = 0; i < scene->event_count; i++) {
        if (scene->events[i].target_index == widget_index &&
            scene->events[i].action == action) {
            return true;
        }
    }
    return false;
}

/*
 * Events are applied in JSON order: MOVE deltas accumulate, while
 * fade/scale/rotate overwrite each other (the last event has the final say).
 */
void vr_evaluate_scene(const EditorContext *ctx, const Scene *scene,
                       WidgetRuntime *rt, int local_ms)
{
    /* Time inside a scene is local — the tracks use the same origin. */
    float t_sec = (float)local_ms * 0.001f;

    for (size_t i = 0; i < scene->widget_count; i++) {
        const WidgetBase *b = ctx->widgets[scene->first_widget + i];

        /*
         * Base values. If a property has a keyframe track, the track dictates
         * an absolute value; otherwise the object's static field is used.
         * Timeline events are layered on top *afterwards*.
         */
        /* The anchor offset applies to both paths — static and track alike. */
        rt[i].x        = (b->has_track_x ? track_sample(&b->tr_x, t_sec) : b->x)
                         - b->anchor_off_x;
        rt[i].y        = (b->has_track_y ? track_sample(&b->tr_y, t_sec) : b->y)
                         - b->anchor_off_y;
        rt[i].scale    = b->has_track_scale ? track_sample(&b->tr_scale, t_sec) : 1.0f;
        rt[i].visible  = true;

        /* In a track the angle is in degrees (as in the JSON); internally radians. */
        rt[i].rotation = b->has_track_rotation
                             ? track_sample(&b->tr_rotation, t_sec) * (float)(M_PI / 180.0)
                             : 0.0f;
        /* A line's own angle; zero for everything else. */
        rt[i].rotation += b->base_rotation;

        if (b->has_track_opacity) {
            rt[i].opacity = track_sample(&b->tr_opacity, t_sec);
        } else {
            /* With a fade_in the widget starts invisible; otherwise visible from t=0. */
            rt[i].opacity = widget_has_action(scene, (int)i, ACTION_FADE_IN) ? 0.0f : 1.0f;
        }
        /*
         * `reveal` doubles as a line's `trim`: both mean "show this fraction of
         * the content", and for a line rasterized along +x the compositor's
         * cutoff already does the right thing.
         */
        if (b->has_track_trim) {
            rt[i].reveal = track_sample(&b->tr_trim, t_sec);
        } else {
            rt[i].reveal = widget_has_action(scene, (int)i, ACTION_TYPEWRITE) ? 0.0f : 1.0f;
        }

        /*
         * Destination size. Untracked, it is the object's own size and the
         * precomputed anchor offset applies unchanged — which is what keeps
         * every existing project byte-for-byte identical.
         *
         * Tracked, the anchor has to be recomputed from the *current* size, or
         * a bar with "anchor": "bottom" would slide as it grew instead of
         * rising from a fixed baseline.
         */
        rt[i].w = b->has_track_w ? track_sample(&b->tr_w, t_sec) : b->base_w;
        rt[i].h = b->has_track_h ? track_sample(&b->tr_h, t_sec) : b->base_h;
        if (rt[i].w < 0.0f) rt[i].w = 0.0f;
        if (rt[i].h < 0.0f) rt[i].h = 0.0f;

        if (b->has_track_w && !b->auto_center_x) {
            rt[i].x += b->anchor_off_x - b->anchor_x * rt[i].w;
        }
        if (b->has_track_h) {
            rt[i].y += b->anchor_off_y - b->anchor_y * rt[i].h;
        }

        rt[i].tint       = b->has_track_tint ? track_sample(&b->tr_tint, t_sec) : 0.0f;
        rt[i].tint_color = b->tint_color;

        /* No band unless a highlight event says otherwise. */
        rt[i].hl_alpha = 0.0f;
        rt[i].hl_from  = 0;
        rt[i].hl_to    = 0;
    }

    /*
     * --- pass 1: group transforms -----------------------------------------
     *
     * Groups are resolved before their members so a member's own animation can
     * be layered on top of the parent's rather than fighting it. Events are
     * read twice, which is cheaper than it looks: a scene has a handful of
     * events, and the alternative is a second pointer array per frame.
     */
    GroupRuntime gr[VR_MAX_GROUPS];
    size_t       ngroups = (scene->group_count < VR_MAX_GROUPS)
                               ? scene->group_count : VR_MAX_GROUPS;

    for (size_t g = 0; g < ngroups; g++) {
        gr[g].x = gr[g].y = 0.0f;
        gr[g].scale       = 1.0f;
        gr[g].rotation    = 0.0f;
        gr[g].opacity     = 1.0f;
    }

    for (size_t e = 0; e < scene->event_count; e++) {
        const TimelineEvent *ev = &scene->events[e];
        if (ev->target_group < 0 || (size_t)ev->target_group >= ngroups) {
            continue;
        }

        int ev_ms = local_ms - ev->time_ms;
        if (ev_ms < 0) {
            continue;
        }
        float p = (ev->duration_ms > 0)
                      ? vr_clampf((float)ev_ms / (float)ev->duration_ms, 0.0f, 1.0f)
                      : 1.0f;
        float eased = easing_apply(ev->ease, p);

        GroupRuntime *g = &gr[ev->target_group];

        switch (ev->action) {
            case ACTION_FADE_IN:  g->opacity  = eased;                     break;
            case ACTION_FADE_OUT: g->opacity  = 1.0f - eased;              break;
            case ACTION_MOVE:     g->x       += ev->value_x * eased;
                                  g->y       += ev->value_y * eased;       break;
            case ACTION_SCALE:    g->scale    = 1.0f + (ev->value - 1.0f) * eased; break;
            case ACTION_ROTATE:   g->rotation = ev->value * eased * (float)(M_PI / 180.0); break;
            case ACTION_ANIMATE: {
                if (!ev->has_keys) {
                    break;
                }
                float v = track_sample(&ev->anim_track, (float)ev_ms * 0.001f);
                switch (ev->anim_prop) {
                    case PROP_X:        g->x        = v; break;
                    case PROP_Y:        g->y        = v; break;
                    case PROP_OPACITY:  g->opacity  = v; break;
                    case PROP_SCALE:    g->scale    = v; break;
                    case PROP_ROTATION: g->rotation = v * (float)(M_PI / 180.0); break;
                    default:            break;   /* size and tint are per-object */
                }
                break;
            }

            case ACTION_ORBIT: {
                /* A group has no size, so its centre *is* the orbit point. */
                float ang = (ev->orbit_a0 + ev->orbit_sweep * eased) * (float)(M_PI / 180.0);
                float rad = ev->orbit_r0 + (ev->orbit_r1 - ev->orbit_r0) * eased;
                g->x = ev->orbit_cx + rad * cosf(ang) - scene->groups[ev->target_group].pivot_x;
                g->y = ev->orbit_cy + rad * sinf(ang) - scene->groups[ev->target_group].pivot_y;
                if (ev->orbit_orient) {
                    g->rotation = ang + (float)(M_PI * 0.5);
                }
                break;
            }
            default: break;   /* typewrite/highlight are per-object concepts */
        }
    }

    /* --- pass 2: per-object events ---------------------------------------- */
    for (size_t e = 0; e < scene->event_count; e++) {
        const TimelineEvent *ev = &scene->events[e];

        if (ev->target_index < 0 || (size_t)ev->target_index >= scene->widget_count) {
            continue; /* unresolved target, a group event, or an unknown action */
        }

        int ev_ms = local_ms - ev->time_ms;
        if (ev_ms < 0) {
            continue; /* the event has not started yet */
        }

        /* p — linear progress 0..1; a duration of 0 means an instant switch. */
        float p = (ev->duration_ms > 0)
                      ? vr_clampf((float)ev_ms / (float)ev->duration_ms, 0.0f, 1.0f)
                      : 1.0f;
        /* Every event has its own curve ("ease"); smoothstep is the default. */
        float eased = easing_apply(ev->ease, p);

        WidgetRuntime *r = &rt[ev->target_index];

        switch (ev->action) {
            case ACTION_FADE_IN:
                r->opacity = eased;
                break;
            case ACTION_FADE_OUT:
                r->opacity = 1.0f - eased;
                break;
            case ACTION_MOVE:
                r->x += ev->value_x * eased;
                r->y += ev->value_y * eased;
                break;
            case ACTION_TYPEWRITE:
                r->reveal = p; /* typing is linear — easing would look unnatural */
                break;
            case ACTION_SCALE:
                /* Interpolate from 1.0 to the target. */
                r->scale = 1.0f + (ev->value - 1.0f) * eased;
                break;
            case ACTION_ROTATE:
                r->rotation = ev->value * eased * (float)(M_PI / 180.0);
                break;
            case ACTION_ORBIT: {
                /*
                 * Circular (or spiral) travel, evaluated in closed form.
                 *
                 * This is the whole reason the action exists: the same motion
                 * expressed as keyframes is a polygon, and a smooth-looking one
                 * costs dozens of keys per revolution. Here it is exact at every
                 * timestamp, and costs one sin and one cos.
                 */
                float ang = (ev->orbit_a0 + ev->orbit_sweep * eased) * (float)(M_PI / 180.0);
                float rad = ev->orbit_r0 + (ev->orbit_r1 - ev->orbit_r0) * eased;

                /*
                 * `rt->x` is the object's top-left, so the half-size is taken
                 * off to put its *centre* on the orbit — which is what "this
                 * object goes round that point" has to mean.
                 */
                const WidgetBase *ob = ctx->widgets[scene->first_widget + ev->target_index];
                r->x = ev->orbit_cx + rad * cosf(ang) - ob->base_w * 0.5f;
                r->y = ev->orbit_cy + rad * sinf(ang) - ob->base_h * 0.5f;

                /* Optionally turn the object to face along its direction of
                 * travel — the tangent, a quarter turn ahead of the radius. */
                if (ev->orbit_orient) {
                    r->rotation = ang + (float)(M_PI * 0.5);
                }
                break;
            }

            case ACTION_ANIMATE: {
                /*
                 * A track sampled at the event's own local time, assigned
                 * absolutely. Past the end the last key holds — an animation
                 * that finishes should stay where it finished, not snap back.
                 */
                if (!ev->has_keys) {
                    break;
                }
                float v = track_sample(&ev->anim_track, (float)ev_ms * 0.001f);

                switch (ev->anim_prop) {
                    case PROP_X:        r->x        = v; break;
                    case PROP_Y:        r->y        = v; break;
                    case PROP_OPACITY:  r->opacity  = v; break;
                    case PROP_SCALE:    r->scale    = v; break;
                    case PROP_ROTATION: r->rotation = v * (float)(M_PI / 180.0); break;
                    case PROP_W:        r->w        = v; break;
                    case PROP_H:        r->h        = v; break;
                    case PROP_TINT:     r->tint     = v; break;
                    case PROP_TRIM:     r->reveal   = v; break;
                    case PROP_NONE:
                    default:            break;
                }
                break;
            }

            case ACTION_EMIT: {
                /*
                 * A particle's whole life, in closed form.
                 *
                 * `p` is already clamped to [0,1] over the lifetime, so the
                 * elapsed seconds come from the event's duration rather than
                 * from p — a particle must stop at its end, not keep coasting.
                 */
                float dt = (float)ev_ms * 0.001f;
                r->x += ev->emit_vx * dt;
                r->y += ev->emit_vy * dt + 0.5f * ev->emit_gravity * dt * dt;

                if (ev->emit_spin != 0.0f) {
                    r->rotation += ev->emit_spin * dt * (float)(M_PI / 180.0);
                }

                /*
                 * Alpha over the life: a quick rise, then a fade whose length
                 * is `fade`. Past the end the particle is gone — otherwise a
                 * burst would leave its debris frozen on screen for ever.
                 */
                float fade = vr_clampf(ev->emit_fade, 0.0f, 1.0f);
                float a;
                if (p >= 1.0f) {
                    a = 0.0f;
                } else if (p < 0.06f) {
                    a = p / 0.06f;
                } else if (fade > 0.0f && p > 1.0f - fade) {
                    a = (1.0f - p) / fade;
                } else {
                    a = 1.0f;
                }
                r->opacity *= a;
                break;
            }

            case ACTION_HIGHLIGHT:
                /*
                 * The band fades in over the event's duration and then stays.
                 * A later highlight on the same widget simply replaces this one
                 * — the same "last event wins" rule fade and scale follow.
                 */
                r->hl_from  = ev->hl_from;
                r->hl_to    = ev->hl_to;
                r->hl_color = ev->hl_color;
                r->hl_alpha = (float)ev->hl_color.a / 255.0f * eased;
                break;

            case ACTION_UNKNOWN:
            default:
                break;
        }
    }

    /*
     * --- composing the parent transform ------------------------------------
     *
     * The member's centre is taken through the group's scale and rotation about
     * the pivot, then translated. Working on the centre rather than the
     * top-left is what makes a rotating group turn as a rigid body instead of
     * shearing: every child keeps its offset from the pivot.
     *
     * Scale and rotation are *composed* with the member's own (multiplied and
     * added), so a spinning child inside a spinning group does both.
     */
    for (size_t i = 0; i < scene->widget_count; i++) {
        const WidgetBase *b = ctx->widgets[scene->first_widget + i];
        if (b->group_index < 0 || (size_t)b->group_index >= ngroups) {
            continue;
        }
        const GroupRuntime *g  = &gr[b->group_index];
        const GroupDef     *gd = &scene->groups[b->group_index];

        float cx = rt[i].x + rt[i].w * 0.5f;
        float cy = rt[i].y + rt[i].h * 0.5f;

        float dx = (cx - gd->pivot_x) * g->scale;
        float dy = (cy - gd->pivot_y) * g->scale;

        float cs = cosf(g->rotation), sn = sinf(g->rotation);
        float rx = dx * cs - dy * sn;
        float ry = dx * sn + dy * cs;

        cx = gd->pivot_x + rx + g->x;
        cy = gd->pivot_y + ry + g->y;

        rt[i].x        = cx - rt[i].w * 0.5f;
        rt[i].y        = cy - rt[i].h * 0.5f;
        rt[i].scale   *= g->scale;
        rt[i].rotation += g->rotation;
        rt[i].opacity *= g->opacity;
    }

    for (size_t i = 0; i < scene->widget_count; i++) {
        rt[i].opacity = vr_clampf(rt[i].opacity, 0.0f, 1.0f);
        rt[i].reveal  = vr_clampf(rt[i].reveal, 0.0f, 1.0f);
        if (rt[i].scale < 0.0f) {
            rt[i].scale = 0.0f;
        }
        /* Coverage below 1/255 is invisible → skip the layer entirely. */
        rt[i].visible = (rt[i].opacity > 0.002f) && (rt[i].reveal > 0.0f) &&
                        (rt[i].scale > 0.0001f);
    }
}

/*
 * The logic: work out how many characters are visible in total, then "spend"
 * that budget line by line. A fully typed line gets an infinite threshold, a
 * line not yet reached gets a negative one (i.e. entirely hidden).
 */
void vr_compute_reveal_cutoffs(const GlyphMetrics *g, float reveal, float *out)
{
    int budget = (int)floorf(reveal * (float)g->total_chars + 1e-4f);

    for (int l = 0; l < g->line_count; l++) {
        int chars_in_line = g->line_start[l + 1] - g->line_start[l] - 1;
        if (chars_in_line < 0) {
            chars_in_line = 0;
        }

        if (budget <= 0) {
            out[l] = -1.0e30f; /* this line has not started yet */
        } else if (budget >= chars_in_line) {
            out[l] = 1.0e30f;  /* the line is fully visible */
        } else {
            out[l] = g->char_x[g->line_start[l] + budget];
        }
        budget -= chars_in_line;
    }
}

/* ------------------------------------------------------------------------- */
/* Scene selection                                                            */
/* ------------------------------------------------------------------------- */

/*
 * We look for the *first* scene that has not finished yet. This matters:
 * inside an overlap window both neighbours are active, and "from" is precisely
 * the earlier one. Taking "the last scene that started" would pick the next
 * pair here and the transition would never be drawn.
 */
void vr_select_scenes(const EditorContext *ctx, int time_ms,
                      size_t *out_index, const Scene **out_a, const Scene **out_b,
                      float *out_p)
{
    size_t si = ctx->scene_count - 1;
    for (size_t i = 0; i < ctx->scene_count; i++) {
        const Scene *sc = &ctx->scenes[i];
        if (time_ms < sc->start_ms + sc->duration_ms) {
            si = i;
            break;
        }
    }

    const Scene *A = &ctx->scenes[si];
    const Scene *B = NULL;
    float        p = 0.0f;

    if (si + 1 < ctx->scene_count) {
        const Scene *next = &ctx->scenes[si + 1];
        int          tdur = (si < ctx->transition_count)
                                ? ctx->transitions[si].duration_ms : 0;

        if (time_ms >= next->start_ms) {
            if (tdur > 0) {
                B = next;
                p = vr_clampf((float)(time_ms - next->start_ms) / (float)tdur, 0.0f, 1.0f);
            } else {
                A = next;      /* a cut — simply the next scene */
                si = si + 1;
            }
        }
    }

    *out_index = si;
    *out_a     = A;
    *out_b     = B;
    *out_p     = p;
}

/* ------------------------------------------------------------------------- */
/* Compositing geometry                                                       */
/* ------------------------------------------------------------------------- */

bool vr_composite_setup(int fb_w, int fb_h, int tex_w, int tex_h,
                        const WidgetBase *b, const WidgetRuntime *rt,
                        CompositeParams *out)
{
    if (tex_w <= 0 || tex_h <= 0) {
        return false;
    }

    /* Destination size: the (possibly animated) size × the animated scale. */
    float dst_w = rt->w * rt->scale;
    float dst_h = rt->h * rt->scale;
    if (dst_w < 0.5f || dst_h < 0.5f) {
        return false;
    }

    /*
     * Both rotation and scale happen around the object's *base* centre.
     *
     * The centre is deliberately computed from base_w/base_h and not from
     * dst_w/dst_h: otherwise a growing object would stay "pinned" to its
     * top-left corner and drift down and to the right — a centred title would
     * visibly slide off centre.
     */
    float cx = rt->x + rt->w * 0.5f;
    float cy = rt->y + rt->h * 0.5f;

    float cs = cosf(rt->rotation);
    float sn = sinf(rt->rotation);

    /* Texture pixels → destination pixels (the per-axis scales). */
    float sx = dst_w / (float)tex_w;
    float sy = dst_h / (float)tex_h;

    out->fb_w  = fb_w;
    out->fb_h  = fb_h;
    out->tex_w = tex_w;
    out->tex_h = tex_h;
    out->cx    = cx;
    out->cy    = cy;

    /* t = S⁻¹ · R(-θ) · d  →  see the comment on CompositeParams. */
    out->inv_a =  cs / sx;
    out->inv_b =  sn / sx;
    out->inv_c = -sn / sy;
    out->inv_d =  cs / sy;

    /* The bounding box of the rotated rectangle. */
    float abs_cs   = fabsf(cs);
    float abs_sn   = fabsf(sn);
    float half_bbw = (abs_cs * dst_w + abs_sn * dst_h) * 0.5f;
    float half_bbh = (abs_sn * dst_w + abs_cs * dst_h) * 0.5f;

    int x0 = (int)floorf(cx - half_bbw);
    int y0 = (int)floorf(cy - half_bbh);
    int x1 = (int)ceilf(cx + half_bbw) + 1;
    int y1 = (int)ceilf(cy + half_bbh) + 1;

    /* Clip to the screen — no work spent on what would not be drawn anyway. */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb_w) x1 = fb_w;
    if (y1 > fb_h) y1 = fb_h;

    if (x1 <= x0 || y1 <= y0) {
        return false; /* entirely off-screen */
    }

    out->bb_x  = x0;
    out->bb_y  = y0;
    out->bb_w  = x1 - x0;
    out->bb_h  = y1 - y0;
    out->alpha = rt->opacity;

    const float inv255 = 1.0f / 255.0f;
    out->tint_amount = vr_clampf(rt->tint, 0.0f, 1.0f);
    out->tint_r      = rt->tint_color.r * inv255;
    out->tint_g      = rt->tint_color.g * inv255;
    out->tint_b      = rt->tint_color.b * inv255;

    out->pad_y       = b->glyphs.pad_y;
    out->line_height = (b->glyphs.line_height > 0.0f) ? b->glyphs.line_height : 1.0f;
    out->line_count  = b->glyphs.line_count;
    return true;
}

bool vr_highlight_setup(const CompositeParams *geom, const WidgetBase *b,
                        const WidgetRuntime *rt, HighlightParams *out)
{
    if (rt->hl_alpha <= 0.002f || b->glyphs.line_count <= 0) {
        return false;
    }

    int last = b->glyphs.line_count - 1;
    if (rt->hl_from > last) {
        return false; /* the range starts past the end of the text */
    }

    out->geom       = *geom;
    out->first_line = (rt->hl_from < 0) ? 0 : rt->hl_from;
    out->last_line  = (rt->hl_to > last) ? last : rt->hl_to;

    /* The panel doubles as the band's stencil — see HighlightParams. */
    out->plate_w = 0;
    out->plate_h = 0;
    if (b->kind == WIDGET_CODE) {
        const CodeWidget *cw = (const CodeWidget *)b;
        out->plate_w = cw->plate.width;
        out->plate_h = cw->plate.height;
    }

    const float inv = 1.0f / 255.0f;
    out->r = rt->hl_color.r * inv;
    out->g = rt->hl_color.g * inv;
    out->b = rt->hl_color.b * inv;

    /*
     * The band's own alpha is folded together with the widget's fade, so a code
     * block fading out takes its highlight with it instead of leaving a
     * coloured rectangle hanging in mid-air.
     */
    out->alpha = rt->hl_alpha * rt->opacity;

    /* Premultiply, matching the convention every other layer follows. */
    out->r *= out->alpha;
    out->g *= out->alpha;
    out->b *= out->alpha;

    return out->last_line >= out->first_line;
}

/* ------------------------------------------------------------------------- */
/* Effects                                                                    */
/* ------------------------------------------------------------------------- */

EffectGPU vr_effect_sample(const Effect *fx, float t)
{
    EffectGPU g;
    memset(&g, 0, sizeof g);

    g.type = (int)fx->type;
    for (int i = 0; i < FXP_MAX; i++) {
        g.p[i] = track_sample(&fx->param[i], t);
    }

    const float inv = 1.0f / 255.0f;
    g.ca[0] = fx->color_a.r * inv; g.ca[1] = fx->color_a.g * inv;
    g.ca[2] = fx->color_a.b * inv; g.ca[3] = fx->color_a.a * inv;
    g.cb[0] = fx->color_b.r * inv; g.cb[1] = fx->color_b.g * inv;
    g.cb[2] = fx->color_b.b * inv; g.cb[3] = fx->color_b.a * inv;
    return g;
}

unsigned int vr_effect_seed(long long frame)
{
    return (unsigned int)(frame * 2654435761ULL);
}

int vr_blur_radius(const EffectGPU *g)
{
    int radius = (int)lrintf(g->p[FXP_RADIUS]);
    if (radius < 1) {
        return 0;    /* a zero radius — the pass is skipped entirely */
    }
    if (radius > 128) {
        radius = 128; /* a ceiling, so no backend is handed an unbounded loop */
    }
    return radius;
}

/* ------------------------------------------------------------------------- */
/* Transitions                                                                */
/* ------------------------------------------------------------------------- */

void vr_trans_side_set(TransSide *s, float opacity, float tx, float ty,
                       float scale, float rot_deg, int w, int h)
{
    if (scale < 0.001f) {
        scale = 0.001f;
    }
    float r  = rot_deg * (float)(M_PI / 180.0);
    float cs = cosf(r), sn = sinf(r);

    s->opacity = opacity;
    s->tx      = tx * (float)w;      /* translation is in fractions of the frame */
    s->ty      = ty * (float)h;
    s->ia      =  cs / scale;
    s->ib      =  sn / scale;
    s->ic      = -sn / scale;
    s->id      =  cs / scale;
    s->mask    = 0;
    s->m0 = s->m1 = s->m2 = s->m3 = 0.0f;
}

/*
 * `from` is drawn first and `to` on top, so many transitions are just a motion
 * of `to`. The full list lives in TransitionType in types.h.
 */
void vr_transition_preset(TransitionType type, float p, int w, int h,
                          TransSide *from, TransSide *to)
{
    vr_trans_side_set(from, 1.0f, 0, 0, 1.0f, 0, w, h);
    vr_trans_side_set(to,   1.0f, 0, 0, 1.0f, 0, w, h);

    switch (type) {
        case TRANS_CROSSFADE:
            to->opacity = p;
            break;

        case TRANS_FADE:                       /* crossfade */
            from->opacity = 1.0f - vr_clampf(p * 2.0f, 0.0f, 1.0f);
            to->opacity   = vr_clampf(p * 2.0f - 1.0f, 0.0f, 1.0f);
            break;

        case TRANS_SLIDE_LEFT:  vr_trans_side_set(to, 1.0f,  (1.0f - p), 0, 1, 0, w, h); break;
        case TRANS_SLIDE_RIGHT: vr_trans_side_set(to, 1.0f, -(1.0f - p), 0, 1, 0, w, h); break;
        case TRANS_SLIDE_UP:    vr_trans_side_set(to, 1.0f, 0,  (1.0f - p), 1, 0, w, h); break;
        case TRANS_SLIDE_DOWN:  vr_trans_side_set(to, 1.0f, 0, -(1.0f - p), 1, 0, w, h); break;

        case TRANS_PUSH_LEFT:
            vr_trans_side_set(from, 1.0f, -p, 0, 1, 0, w, h);
            vr_trans_side_set(to,   1.0f, (1.0f - p), 0, 1, 0, w, h);
            break;
        case TRANS_PUSH_RIGHT:
            vr_trans_side_set(from, 1.0f, p, 0, 1, 0, w, h);
            vr_trans_side_set(to,   1.0f, -(1.0f - p), 0, 1, 0, w, h);
            break;
        case TRANS_PUSH_UP:
            vr_trans_side_set(from, 1.0f, 0, -p, 1, 0, w, h);
            vr_trans_side_set(to,   1.0f, 0, (1.0f - p), 1, 0, w, h);
            break;
        case TRANS_PUSH_DOWN:
            vr_trans_side_set(from, 1.0f, 0, p, 1, 0, w, h);
            vr_trans_side_set(to,   1.0f, 0, -(1.0f - p), 1, 0, w, h);
            break;

        case TRANS_ZOOM_IN:
            vr_trans_side_set(to, p, 0, 0, 0.7f + 0.3f * p, 0, w, h);
            break;
        case TRANS_ZOOM_OUT:
            vr_trans_side_set(from, 1.0f - p, 0, 0, 1.0f + 0.35f * p, 0, w, h);
            vr_trans_side_set(to,   p, 0, 0, 1.0f, 0, w, h);
            break;

        case TRANS_SPIN:
            vr_trans_side_set(to, p, 0, 0, 0.2f + 0.8f * p, 180.0f * (1.0f - p), w, h);
            break;

        case TRANS_WIPE_RIGHT:                 /* opens from left to right */
            to->mask = 2; to->m0 = 0.0f; to->m1 = 0.0f; to->m2 = p; to->m3 = 1.0f;
            break;
        case TRANS_WIPE_LEFT:
            to->mask = 2; to->m0 = 1.0f - p; to->m1 = 0.0f; to->m2 = p; to->m3 = 1.0f;
            break;

        case TRANS_IRIS:
            /* A radius up to 0.75 — enough to cover the corners (√2/2 ≈ 0.71). */
            to->mask = 1; to->m0 = 0.5f; to->m1 = 0.5f; to->m2 = p * 0.78f;
            break;

        case TRANS_CUT:
        default:
            to->opacity = (p >= 0.5f) ? 1.0f : 0.0f;
            break;
    }
}

void vr_transition_apply_inline(const Transition *tr, float p, int w, int h,
                                TransSide *from, TransSide *to)
{
    if (tr->has_from) {
        vr_trans_side_set(from, track_sample(&tr->from_opacity, p),
                          track_sample(&tr->from_x, p), track_sample(&tr->from_y, p),
                          track_sample(&tr->from_scale, p),
                          track_sample(&tr->from_rotate, p), w, h);
    }
    if (tr->has_to) {
        vr_trans_side_set(to, track_sample(&tr->to_opacity, p),
                          track_sample(&tr->to_x, p), track_sample(&tr->to_y, p),
                          track_sample(&tr->to_scale, p),
                          track_sample(&tr->to_rotate, p), w, h);
    }

    /* A mask also overrides the preset's mask — the JSON has the final say. */
    if (tr->from_mask_shape != 0) {
        from->mask = tr->from_mask_shape;
        from->m0 = track_sample(&tr->from_mask[0], p);
        from->m1 = track_sample(&tr->from_mask[1], p);
        from->m2 = track_sample(&tr->from_mask[2], p);
        from->m3 = track_sample(&tr->from_mask[3], p);
    }
    if (tr->to_mask_shape != 0) {
        to->mask = tr->to_mask_shape;
        to->m0 = track_sample(&tr->to_mask[0], p);
        to->m1 = track_sample(&tr->to_mask[1], p);
        to->m2 = track_sample(&tr->to_mask[2], p);
        to->m3 = track_sample(&tr->to_mask[3], p);
    }
}

/* ------------------------------------------------------------------------- */
/* Output                                                                     */
/* ------------------------------------------------------------------------- */

/* Shell quoting is shared with the audio mixer — see vr_shell_quote() in audio.c. */

const char *vr_software_encoder(const char *encoder)
{
    static const struct {
        const char *hardware;
        const char *software;
    } kFallback[] = {
        { "h264_nvenc", "libx264"   },
        { "hevc_nvenc", "libx265"   },
        { "av1_nvenc",  "libsvtav1" },
    };

    if (encoder == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof kFallback / sizeof kFallback[0]; i++) {
        if (strcmp(encoder, kFallback[i].hardware) == 0) {
            return kFallback[i].software;
        }
    }
    return NULL;
}

FILE *vr_open_ffmpeg_pipe(const EditorContext *ctx, const char *output_file,
                          const char *default_encoder, bool allow_hardware)
{
    /* The environment variable outranks the JSON "output" block — handy for quick experiments. */
    const char *encoder  = getenv("VIDEO_REDAC_ENCODER");
    bool        explicit_choice = (encoder != NULL && encoder[0] != '\0');

    if (!explicit_choice) {
        encoder = (ctx->output.encoder != NULL) ? ctx->output.encoder : default_encoder;
    }

    /*
     * A project written for the GPU should still render on a machine without
     * one. Substituting silently would be worse than the failure it prevents,
     * though — the output would quietly take a different code path — so the
     * swap is always announced.
     */
    bool was_nvenc = (strstr(encoder, "nvenc") != NULL);

    if (!allow_hardware && !explicit_choice) {
        const char *software = vr_software_encoder(encoder);
        if (software != NULL) {
            fprintf(stderr, "note: '%s' needs an NVIDIA GPU — encoding with %s instead.\n",
                    encoder, software);
            encoder = software;
        }
    }

    bool is_nvenc = (strstr(encoder, "nvenc") != NULL);

    /*
     * The preset namespaces do not overlap: NVENC speaks p1..p7, x264 speaks
     * ultrafast..placebo. Handing one to the other is not a slow encode but a
     * hard ffmpeg error ("invalid preset 'p5'").
     *
     * So the project's preset is honoured only while the encoder stays in the
     * same family. The moment we fall back from NVENC to software, its preset
     * has to go with it — it was written for an encoder that is no longer in
     * use.
     */
    const char *preset = ctx->output.preset;
    if (preset != NULL && was_nvenc != is_nvenc) {
        fprintf(stderr, "note: preset '%s' belongs to the previous encoder — using the default.\n",
                preset);
        preset = NULL;
    }
    if (preset == NULL) {
        preset = is_nvenc ? "p5" : "medium";
    }

    char quoted_out[2048];
    char quoted_enc[128];
    char quoted_preset[64];
    if (!vr_shell_quote(output_file, quoted_out, sizeof quoted_out) ||
        !vr_shell_quote(encoder, quoted_enc, sizeof quoted_enc) ||
        !vr_shell_quote(preset, quoted_preset, sizeof quoted_preset)) {
        fprintf(stderr, "error: the output file name is too long.\n");
        return NULL;
    }

    /*
     * A bitrate given → constant bitrate; otherwise constant quality. The
     * quality knob is spelled differently per encoder family: -cq for NVENC,
     * -crf for x264/x265.
     */
    char rate[192];
    if (ctx->output.bitrate != NULL) {
        char quoted_br[64];
        if (!vr_shell_quote(ctx->output.bitrate, quoted_br, sizeof quoted_br)) {
            return NULL;
        }
        snprintf(rate, sizeof rate, "-b:v %s", quoted_br);
    } else if (is_nvenc) {
        snprintf(rate, sizeof rate, "-rc vbr -cq %d -b:v 0", ctx->output.cq);
    } else {
        snprintf(rate, sizeof rate, "-crf %d", ctx->output.cq);
    }

    /* -tune hq is NVENC-only; x264's tunes mean something else entirely. */
    const char *tune = is_nvenc ? "-tune hq " : "";

    char cmd[4096];
    int  n = snprintf(cmd, sizeof cmd,
                      "ffmpeg -hide_banner -loglevel error -y "
                      "-f rawvideo -pixel_format nv12 -video_size %dx%d -framerate %d "
                      /*
                       * Tagging the *input* stream's colour properties is
                       * mandatory. Without it ffmpeg cannot know the NV12 is
                       * already BT.709 limited range, falls back to its own
                       * assumption and silently inserts swscale: "YUV color
                       * matrix differs for YUV->YUV, using intermediate RGB".
                       * That both corrupted colours (up to 15/255 of error)
                       * and burned CPU on every frame.
                       */
                      "-colorspace bt709 -color_primaries bt709 -color_trc bt709 "
                      "-color_range tv -i - "
                      /* nv12 is NVENC's native format → no conversion at all. */
                      "-c:v %s -preset %s %s%s -pix_fmt nv12 "
                      "-colorspace bt709 -color_primaries bt709 -color_trc bt709 "
                      "-color_range tv -movflags +faststart %s",
                      ctx->config.width, ctx->config.height, ctx->config.fps,
                      quoted_enc, quoted_preset, tune, rate, quoted_out);

    if (n < 0 || (size_t)n >= sizeof cmd) {
        fprintf(stderr, "error: the ffmpeg command did not fit in the buffer.\n");
        return NULL;
    }

    fprintf(stderr, "FFmpeg: %s\n", cmd);

    FILE *pipe = popen(cmd, "w");
    if (pipe == NULL) {
        fprintf(stderr, "error: could not start ffmpeg (is it on PATH?).\n");
    }
    return pipe;
}

void vr_frame_range(const EditorContext *ctx, long long *out_first,
                    long long *out_last, long long *out_total)
{
    /* The frame count — rounded up, so a final partial frame is included too. */
    long long total_frames =
        ((long long)ctx->config.duration_ms * ctx->config.fps + 999) / 1000;
    if (total_frames < 1) {
        total_frames = 1;
    }

    long long first = 0;
    long long last  = total_frames - 1;

    if (ctx->range_end_sec > ctx->range_start_sec) {
        first = (long long)floor(ctx->range_start_sec * ctx->config.fps);
        last  = (long long)ceil(ctx->range_end_sec * ctx->config.fps) - 1;

        if (first < 0) first = 0;
        if (last > total_frames - 1) last = total_frames - 1;
        if (last < first) last = first;
    }

    *out_first = first;
    *out_last  = last;
    *out_total = total_frames;
}
