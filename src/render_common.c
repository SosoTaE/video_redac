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
        /*
         * Three offsets apply to both paths — static position and track alike:
         *
         *   anchor_off_*  where the object's origin sits inside its box
         *   repeat_dx/dy  the displacement a `repeat` block gave this copy
         *   tex_pad       the margin a shadow added around the texture
         *
         * They belong here rather than folded into `b->x` during layout,
         * because `b->x` is only read when there is no track — and a plain
         * `"x": 240` is stored as a track with a constant, so anything folded
         * in earlier would be silently skipped for most objects.
         */
        /*
         * The shadow's margin has to be undone by however much the anchor did
         * NOT already account for it.
         *
         * `anchor_off` is computed from the *padded* size, so an object
         * anchored at its centre is already right — the padding is symmetric,
         * so the padded centre and the content centre coincide. Only a
         * top-left anchor needs the full margin removed, and a bottom-right
         * one needs it added back. Subtracting the whole margin regardless
         * pushed every centred object up and to the left by the blur radius.
         */
        float pad_x = (float)b->tex_pad * (1.0f - 2.0f * b->anchor_x);
        float pad_y = (float)b->tex_pad * (1.0f - 2.0f * b->anchor_y);

        rt[i].x        = (b->has_track_x ? track_sample(&b->tr_x, t_sec) : b->x)
                         - b->anchor_off_x + b->repeat_dx - pad_x;
        rt[i].y        = (b->has_track_y ? track_sample(&b->tr_y, t_sec) : b->y)
                         - b->anchor_off_y + b->repeat_dy - pad_y;
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

        /* 2.5D. Untracked these are zero, which turns the projection off. */
        rt[i].z     = b->has_track_z  ? track_sample(&b->tr_z,  t_sec) : 0.0f;
        rt[i].rx    = b->has_track_rx ? track_sample(&b->tr_rx, t_sec) * (float)(M_PI / 180.0) : 0.0f;
        rt[i].ry    = b->has_track_ry ? track_sample(&b->tr_ry, t_sec) * (float)(M_PI / 180.0) : 0.0f;
        rt[i].focal = scene->camera.present ? scene->camera.focal : 0.0f;

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
                    case PROP_Z:        r->z        = v; break;
                    case PROP_RX:       r->rx       = v * (float)(M_PI / 180.0); break;
                    case PROP_RY:       r->ry       = v * (float)(M_PI / 180.0); break;
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

    /*
     * --- the camera ---------------------------------------------------------
     *
     * The same composition as a group, but the pivot is the canvas centre and
     * every object is a member. Applied after groups, so a camera move layers
     * on top of whatever the scene's own hierarchy did rather than being
     * overwritten by it.
     */
    if (scene->camera.present) {
        const Camera *cam = &scene->camera;

        float zoom = track_sample(&cam->zoom, t_sec);
        float rot  = track_sample(&cam->rotation, t_sec) * (float)(M_PI / 180.0);
        float panx = track_sample(&cam->x, t_sec);
        float pany = track_sample(&cam->y, t_sec);
        float amp  = track_sample(&cam->shake, t_sec);

        if (zoom < 0.001f) {
            zoom = 0.001f;
        }

        /*
         * Shake is a hash of the millisecond, not a random number: the frame
         * has to stay a pure function of time or --range and the two backends
         * would each produce a different judder.
         */
        float sx = 0.0f, sy = 0.0f;
        if (amp > 0.0f) {
            unsigned int h = (unsigned int)local_ms * 2654435761u;
            h ^= h >> 15; h *= 0x2c1b3c6du;
            unsigned int h2 = h ^ 0x9e3779b9u;
            h2 ^= h2 >> 13; h2 *= 0x297a2d39u;

            sx = ((float)(h  & 0xFFFF) / 32768.0f - 1.0f) * amp;
            sy = ((float)(h2 & 0xFFFF) / 32768.0f - 1.0f) * amp;
        }

        float ccx = (float)ctx->config.width  * 0.5f;
        float ccy = (float)ctx->config.height * 0.5f;
        float cs  = cosf(rot), sn = sinf(rot);

        for (size_t i = 0; i < scene->widget_count; i++) {
            float cx = rt[i].x + rt[i].w * 0.5f;
            float cy = rt[i].y + rt[i].h * 0.5f;

            /* Panning moves the camera, so the content moves the other way. */
            float dx = (cx - ccx - panx) * zoom;
            float dy = (cy - ccy - pany) * zoom;

            cx = ccx + dx * cs - dy * sn + sx;
            cy = ccy + dx * sn + dy * cs + sy;

            rt[i].x         = cx - rt[i].w * 0.5f;
            rt[i].y         = cy - rt[i].h * 0.5f;
            rt[i].scale    *= zoom;
            rt[i].rotation += rot;
        }
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

/*
 * Builds the perspective form of the transform.
 *
 * The layer is a flat quad, so the map from texture coordinates to the screen
 * is a homography. Deriving it rather than projecting per pixel is what keeps
 * the pixel loop cheap: three multiply-adds and one divide, against a full
 * rotate-and-project.
 *
 *   P(u,v) = o + u·e1 + v·e2          a point on the quad, in camera space
 *   screen = C + f·(P.x, P.y)/(f + P.z)
 *
 * which in homogeneous form is
 *
 *        [ f·e1.x  f·e2.x  f·o.x ]
 *   H =  [ f·e1.y  f·e2.y  f·o.y ]
 *        [   e1.z    e2.z  f+o.z ]
 *
 * and the pixel loop uses H⁻¹. Returns false if the quad is degenerate or
 * entirely behind the viewer.
 */
static bool build_homography(float focal, float sx, float sy,
                             float rx, float ry, float rz,
                             float tex_w, float tex_h, float z,
                             float off_x, float off_y,
                             CompositeParams *out, float corners[4][2])
{
    float cx_ = cosf(rx), sx_ = sinf(rx);
    float cy_ = cosf(ry), sy_ = sinf(ry);
    float cz_ = cosf(rz), sz_ = sinf(rz);

    /* R = Rz · Ry · Rx, applied to the quad's two in-plane basis vectors. */
    float r00 =  cz_ * cy_;
    float r01 =  cz_ * sy_ * sx_ - sz_ * cx_;
    float r10 =  sz_ * cy_;
    float r11 =  sz_ * sy_ * sx_ + cz_ * cx_;
    float r20 = -sy_;
    float r21 =  cy_ * sx_;

    /* One texel step in u and in v, after scaling into destination pixels. */
    float e1x = r00 * sx, e1y = r10 * sx, e1z = r20 * sx;
    float e2x = r01 * sy, e2y = r11 * sy, e2z = r21 * sy;

    /*
     * The quad's (0,0) texel, in camera space — relative to the *canvas*
     * centre, not the object's own.
     *
     * That distinction is the difference between a camera and a per-object
     * trick: with the object's own centre as origin, a layer moving away
     * shrinks in place. With the canvas centre as origin it also drifts toward
     * the middle of the frame, which is what depth actually looks like and
     * what makes parallax work.
     */
    float ox = -(tex_w * 0.5f) * e1x - (tex_h * 0.5f) * e2x + off_x;
    float oy = -(tex_w * 0.5f) * e1y - (tex_h * 0.5f) * e2y + off_y;
    float oz = -(tex_w * 0.5f) * e1z - (tex_h * 0.5f) * e2z + z;

    float f = focal;
    float h[9] = {
        f * e1x, f * e2x, f * ox,
        f * e1y, f * e2y, f * oy,
        e1z,     e2z,     f + oz,
    };

    float det = h[0] * (h[4] * h[8] - h[5] * h[7])
              - h[1] * (h[3] * h[8] - h[5] * h[6])
              + h[2] * (h[3] * h[7] - h[4] * h[6]);
    if (fabsf(det) < 1e-9f) {
        return false;      /* edge-on, or scaled to nothing */
    }
    float id = 1.0f / det;

    out->hinv[0] =  (h[4] * h[8] - h[5] * h[7]) * id;
    out->hinv[1] = -(h[1] * h[8] - h[2] * h[7]) * id;
    out->hinv[2] =  (h[1] * h[5] - h[2] * h[4]) * id;
    out->hinv[3] = -(h[3] * h[8] - h[5] * h[6]) * id;
    out->hinv[4] =  (h[0] * h[8] - h[2] * h[6]) * id;
    out->hinv[5] = -(h[0] * h[5] - h[2] * h[3]) * id;
    out->hinv[6] =  (h[3] * h[7] - h[4] * h[6]) * id;
    out->hinv[7] = -(h[0] * h[7] - h[1] * h[6]) * id;
    out->hinv[8] =  (h[0] * h[4] - h[1] * h[3]) * id;

    /* The four projected corners give the bounding box to iterate over. */
    const float uv[4][2] = { {0,0}, {tex_w,0}, {0,tex_h}, {tex_w,tex_h} };
    for (int i = 0; i < 4; i++) {
        float U = uv[i][0], V = uv[i][1];
        float X = h[0] * U + h[1] * V + h[2];
        float Y = h[3] * U + h[4] * V + h[5];
        float W = h[6] * U + h[7] * V + h[8];
        if (W <= 1e-6f) {
            return false;  /* a corner is at or behind the viewer */
        }
        corners[i][0] = X / W;
        corners[i][1] = Y / W;
    }
    return true;
}

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

    int x0, y0, x1, y1;

    /*
     * Perspective only when the layer actually uses it. The affine branch below
     * is the original code, untouched — the homography reduces to it
     * algebraically but not bit-for-bit, and there is no reason to move every
     * existing project by a rounding step.
     */
    out->persp = (rt->focal > 0.0f) &&
                 (rt->z != 0.0f || rt->rx != 0.0f || rt->ry != 0.0f);

    out->shade = 1.0f;

    if (out->persp) {
        /*
         * The quad's normal after rotation. For R = Rz·Ry·Rx applied to
         * (0,0,1) the z component is simply cos(ry)·cos(rx) — positive when the
         * front faces the viewer, negative when the back does, and its
         * magnitude is how square-on the surface is.
         */
        float nz = cosf(rt->ry) * cosf(rt->rx);

        if (b->backface == 1 && nz < 0.0f) {
            return false;                     /* hidden: cull it entirely */
        }

        float facing = fabsf(nz);
        if (b->shading > 0.0f) {
            out->shade = 1.0f - b->shading * (1.0f - facing);
        }
        if (b->backface == 2 && nz < 0.0f) {
            out->shade *= 0.45f;              /* dimmed: still drawn, clearly behind */
        }

        /* Everything is expressed against the canvas centre, so that is where
         * the projection is anchored and where the bounding box is measured
         * from. */
        float ccx = (float)fb_w * 0.5f;
        float ccy = (float)fb_h * 0.5f;

        float corners[4][2];
        if (!build_homography(rt->focal, sx, sy, rt->rx, rt->ry, rt->rotation,
                              (float)tex_w, (float)tex_h, rt->z,
                              cx - ccx, cy - ccy, out, corners)) {
            return false;
        }
        out->cx = ccx;
        out->cy = ccy;
        cx = ccx;
        cy = ccy;

        float minx = corners[0][0], maxx = corners[0][0];
        float miny = corners[0][1], maxy = corners[0][1];
        for (int i = 1; i < 4; i++) {
            if (corners[i][0] < minx) minx = corners[i][0];
            if (corners[i][0] > maxx) maxx = corners[i][0];
            if (corners[i][1] < miny) miny = corners[i][1];
            if (corners[i][1] > maxy) maxy = corners[i][1];
        }
        x0 = (int)floorf(cx + minx);
        y0 = (int)floorf(cy + miny);
        x1 = (int)ceilf(cx + maxx) + 1;
        y1 = (int)ceilf(cy + maxy) + 1;
    } else {
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

        x0 = (int)floorf(cx - half_bbw);
        y0 = (int)floorf(cy - half_bbh);
        x1 = (int)ceilf(cx + half_bbw) + 1;
        y1 = (int)ceilf(cy + half_bbh) + 1;
    }

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

    out->blend       = b->blend;
    out->mask_shape  = b->mask_shape;
    out->mask_invert = b->mask_invert ? 1 : 0;
    for (int m = 0; m < 4; m++) {
        out->mask[m] = b->mask[m];
    }

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

bool vr_video_slice(const WidgetBase *b, int local_ms, size_t *out_offset)
{
    if (b->kind != WIDGET_VIDEO) {
        return false;
    }

    const VideoWidget *v = (const VideoWidget *)b;
    if (v->frame_count <= 0 || v->frame_h <= 0) {
        return false;
    }

    /*
     * The clip's own time base, not the film's. `speed` is already folded into
     * src_fps by the decoder, so this is a plain multiply — and because it
     * depends only on `local_ms`, a clip stays a pure function of time like
     * everything else.
     */
    long idx = (long)((float)local_ms * 0.001f * v->src_fps);

    if (idx < 0) {
        idx = 0;
    }
    if (idx >= v->frame_count) {
        /* Past the end: loop, or hold the last frame. Holding is the safer
         * default — a clip that vanishes mid-scene reads as a bug. */
        idx = v->loop ? (idx % v->frame_count) : (v->frame_count - 1);
    }

    *out_offset = (size_t)idx * (size_t)v->frame_w * (size_t)v->frame_h * 4u;
    return true;
}

bool vr_depth_order(const Scene *scene, const WidgetRuntime *rt, int *order)
{
    bool any = false;
    for (size_t i = 0; i < scene->widget_count; i++) {
        if (rt[i].z != 0.0f) {
            any = true;
            break;
        }
    }
    if (!any) {
        return false;
    }

    for (size_t i = 0; i < scene->widget_count; i++) {
        order[i] = (int)i;
    }

    /*
     * Insertion sort, descending by z — farthest drawn first.
     *
     * Stable, which matters: layers at equal depth must keep their authored
     * z-order rather than being shuffled by the sort. It is also O(n) on the
     * common case where depths barely change between frames, which is exactly
     * what an animated scene does.
     */
    for (size_t i = 1; i < scene->widget_count; i++) {
        int   cur = order[i];
        float zc  = rt[cur].z;
        size_t j  = i;
        while (j > 0 && rt[order[j - 1]].z < zc) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = cur;
    }
    return true;
}

static int b_blend_of(const MeshWidget *m) { return m->base.blend; }

/*
 * Inverse-square falloff, softened so it never blows up at the source.
 *
 * `range` is where the light is down to half. Zero means no falloff, which is
 * what a sun wants and what every scene written before this existed assumes —
 * so it is also the default.
 */
static float vr_falloff(float dist, float range)
{
    if (range <= 0.0f) {
        return 1.0f;
    }
    float t = dist / range;
    return 1.0f / (1.0f + t * t);
}

int vr_mesh_project(const MeshWidget *m, const WidgetRuntime *rt,
                    int fb_w, int fb_h, const float view[12],
                    const Light *lights, int light_count,
                    ScreenTri *out, int cap, MeshParams *mp)
{
    if (m->tri_count == 0 || m->vert_count == 0) {
        return 0;
    }

    /* R = Rz · Ry · Rx, the same convention the 2.5D layers use. */
    float cx_ = cosf(rt->rx), sx_ = sinf(rt->rx);
    float cy_ = cosf(rt->ry), sy_ = sinf(rt->ry);
    float cz_ = cosf(rt->rotation), sz_ = sinf(rt->rotation);

    float r00 = cz_ * cy_, r01 = cz_ * sy_ * sx_ - sz_ * cx_, r02 = cz_ * sy_ * cx_ + sz_ * sx_;
    float r10 = sz_ * cy_, r11 = sz_ * sy_ * sx_ + cz_ * cx_, r12 = sz_ * sy_ * cx_ - cz_ * sx_;
    float r20 = -sy_,      r21 = cy_ * sx_,                   r22 = cy_ * cx_;

    /* The unit mesh spans -1..1 on every axis, so half the requested extent is
     * the factor. Per-axis, which is what lets a cylinder be a lamp post. */
    float ax_ = m->size[0] * rt->scale * 0.5f;
    float ay_ = m->size[1] * rt->scale * 0.5f;
    float az_ = m->size[2] * rt->scale * 0.5f;

    /*
     * Normals do not scale the way positions do.
     *
     * Squash a sphere into a disc and its surface normals must splay outward,
     * not squash with it — the correct transform is the inverse transpose,
     * which for a pure axis scale is the reciprocal of each factor. Using the
     * position scale instead would tilt every normal the wrong way and light a
     * stretched object as though it were still round.
     */
    float nsx = 1.0f / ax_, nsy = 1.0f / ay_, nsz = 1.0f / az_;
    float ccx   = (float)fb_w * 0.5f;
    float ccy   = (float)fb_h * 0.5f;

    /* World → view. With no camera tracks this is a translation of `focal`
     * along z, which is exactly the fixed viewpoint. */
    const float *V = view;

    /* The object's centre, relative to the canvas centre — the same anchoring
     * the projected layers use, so a mesh and a card agree about where "far
     * away" is. */
    float ox = rt->x + rt->w * 0.5f - ccx;
    float oy = rt->y + rt->h * 0.5f - ccy;

    /*
     * Focal length 0 means the scene has no camera perspective. A mesh still
     * has to be projected — it is a solid, not a flat card — so it falls back
     * to a long lens, which is very nearly orthographic and never divides by
     * something near zero.
     */
    float f = (rt->focal > 0.0f) ? rt->focal : (float)fb_w * 8.0f;

    const float inv255 = 1.0f / 255.0f;
    float base_r = m->color.r * inv255;
    float base_g = m->color.g * inv255;
    float base_b = m->color.b * inv255;
    float amb    = vr_clampf(m->ambient, 0.0f, 1.0f);

    /*
     * Smooth shading replaces the flat term rather than compounding it. Leaving
     * the face's own `lit` in the vertex colour and then multiplying by the
     * interpolated one shades everything twice and — because the flat term is
     * constant per face — the facet edges stay perfectly visible, which defeats
     * the entire point of interpolating.
     */
    bool smooth = (m->smooth && m->norms != NULL);

    /*
     * A surface that is not culled is being shown from both sides, so it has no
     * "outward" direction for the light to fall on — and Lambert's clamp, which
     * exists to stop a face being lit from behind, instead makes the whole thing
     * black whenever the light happens to be on the other side. A planetary ring
     * lit by a star in its own plane is exactly that case: physically edge-on,
     * arithmetically zero, and simply absent from the picture.
     *
     * So two-sided geometry takes the magnitude of the dot product. It is what
     * the surface means: whichever face you are looking at is the lit one.
     */
    bool two_sided = !m->cull;

    /*
     * The light is moved into view space once, here, rather than moving every
     * surface point back into world space. Both give the same dot product —
     * the view transform is rigid — and this way the per-vertex cost stays what
     * it was.
     */
    float Lv[VR_MAX_LIGHTS][3];
    for (int li = 0; li < light_count; li++) {
        const Light *L = &lights[li];
        Lv[li][0] = view[0]*L->x + view[1]*L->y + view[2]*L->z  + view[3];
        Lv[li][1] = view[4]*L->x + view[5]*L->y + view[6]*L->z  + view[7];
        Lv[li][2] = view[8]*L->x + view[9]*L->y + view[10]*L->z + view[11];
    }

    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    int   n = 0;

    for (size_t t = 0; t < m->tri_count; t++) {
        const MeshTri *tri = &m->tris[t];

        float wx[3], wy[3], wz[3];      /* view space: x, y, and depth */
        float vlit[3], vu[3], vv[3];

        for (int k = 0; k < 3; k++) {
            const float *v = &m->verts[(size_t)tri->v[k] * 3];

            /* model → rotated → scaled → world */
            float mx_ = v[0] * ax_, my_ = v[1] * ay_, mz_ = v[2] * az_;
            float Wx = (r00 * mx_ + r01 * my_ + r02 * mz_) + ox;
            float Wy = (r10 * mx_ + r11 * my_ + r12 * mz_) + oy;
            float Wz = (r20 * mx_ + r21 * my_ + r22 * mz_) + rt->z;

            /* world → view */
            float X = V[0]*Wx + V[1]*Wy + V[2]*Wz  + V[3];
            float Y = V[4]*Wx + V[5]*Wy + V[6]*Wz  + V[7];
            float Z = V[8]*Wx + V[9]*Wy + V[10]*Wz + V[11];

            wx[k] = X; wy[k] = Y; wz[k] = Z;

            /* Per-vertex shading and texture coordinates, carried through the
             * same loop so the vertex is touched once. */
            if (m->norms != NULL) {
                const float *nv = &m->norms[(size_t)tri->v[k] * 3];
                float na = nv[0] * nsx, nb = nv[1] * nsy, nc = nv[2] * nsz;
                float NX = r00*na + r01*nb + r02*nc;
                float NY = r10*na + r11*nb + r12*nc;
                float NZ = r20*na + r21*nb + r22*nc;
                float len2 = NX*NX + NY*NY + NZ*NZ;
                float lam;

                if (light_count > 0) {
                    /* Vertex normals point outward, so no sign correction is
                     * needed here — unlike the face normals below. Clamped at
                     * zero, which is what draws the terminator: past ninety
                     * degrees the surface simply faces away from the light.
                     * Several lights add up, then saturate. */
                    float VNx = V[0]*NX + V[1]*NY + V[2]*NZ;
                    float VNy = V[4]*NX + V[5]*NY + V[6]*NZ;
                    float VNz = V[8]*NX + V[9]*NY + V[10]*NZ;
                    float nl = sqrtf(VNx*VNx + VNy*VNy + VNz*VNz);

                    lam = 0.0f;
                    for (int li = 0; li < light_count; li++) {
                        float lx = Lv[li][0] - X, ly = Lv[li][1] - Y, lz = Lv[li][2] - Z;
                        float ll = sqrtf(lx*lx + ly*ly + lz*lz);
                        float d = (ll > 1e-9f && nl > 1e-9f)
                            ? (VNx*lx + VNy*ly + VNz*lz) / (ll * nl) : 1.0f;
                        if (two_sided) d = fabsf(d);
                        if (d > 0.0f) {
                            lam += d * lights[li].intensity * vr_falloff(ll, lights[li].range);
                        }
                    }
                    if (lam > 1.0f) lam = 1.0f;
                } else {
                    float VZ = V[8]*NX + V[9]*NY + V[10]*NZ;   /* rotation only */
                    lam = (len2 > 1e-12f) ? fabsf(VZ) / sqrtf(len2) : 1.0f;
                }
                vlit[k] = amb + (1.0f - amb) * lam;
            } else {
                vlit[k] = 1.0f;
            }
            if (m->uvs != NULL) {
                vu[k] = m->uvs[(size_t)tri->v[k] * 2];
                vv[k] = m->uvs[(size_t)tri->v[k] * 2 + 1];
            } else {
                vu[k] = 0.0f; vv[k] = 0.0f;
            }

        }

        /*
         * Flat shading from the face's own normal.
         *
         * The light sits at the camera, so the term is just how much the face
         * turns away from the viewer. It is the cheapest thing that makes a
         * solid read as a solid — without it every face of a cube is the same
         * colour and the shape disappears into a silhouette.
         */
        float ax = wx[1] - wx[0], ay = wy[1] - wy[0], az = wz[1] - wz[0];
        float bx = wx[2] - wx[0], by = wy[2] - wy[0], bz = wz[2] - wz[0];
        float nx = ay * bz - az * by;
        float ny = az * bx - ax * bz;
        float nz = ax * by - ay * bx;

        float len = sqrtf(nx * nx + ny * ny + nz * nz);
        float lam;

        if (light_count > 0) {
            /*
             * Negated: this renderer keeps the face whose cross product points
             * *away* from the viewer (y-down flips the projected winding), so
             * the outward normal — the one the light actually strikes — is the
             * other one. Without the sign every lit body would come out
             * inside-out, bright exactly where it should be dark.
             */
            float cx_w = (wx[0] + wx[1] + wx[2]) * (1.0f / 3.0f);
            float cy_w = (wy[0] + wy[1] + wy[2]) * (1.0f / 3.0f);
            float cz_w = (wz[0] + wz[1] + wz[2]) * (1.0f / 3.0f);

            lam = 0.0f;
            for (int li = 0; li < light_count; li++) {
                float lx = Lv[li][0] - cx_w, ly = Lv[li][1] - cy_w, lz = Lv[li][2] - cz_w;
                float ll = sqrtf(lx*lx + ly*ly + lz*lz);
                float d = (ll > 1e-9f && len > 1e-9f)
                    ? -(nx*lx + ny*ly + nz*lz) / (ll * len) : 1.0f;
                if (two_sided) d = fabsf(d);
                if (d > 0.0f) {
                    lam += d * lights[li].intensity * vr_falloff(ll, lights[li].range);
                }
            }
            if (lam > 1.0f) lam = 1.0f;
        } else {
            lam = (len > 1e-9f) ? fabsf(nz) / len : 1.0f;
        }
        float lit = smooth ? 1.0f : (amb + (1.0f - amb) * lam);

        /*
         * Clip against the near plane before projecting.
         *
         * Perspective division is meaningless at or behind the eye — z goes to
         * zero and the projected point flies to infinity — so a triangle that
         * straddles the eye plane cannot simply be projected. Discarding it
         * whole is the cheap answer and it is invisible as long as the camera
         * stays outside its subject: an object you walk around is never cut by
         * the eye plane. Put the camera *inside* something and it is fatal —
         * the walls of a room are single large boxes, every one of them crosses
         * the eye plane, and every one of them vanishes entirely, leaving the
         * furniture floating in a void.
         *
         * Sutherland–Hodgman against the single plane z = NEAR. Three vertices
         * in gives three or four out, so a clipped triangle becomes one or two.
         */
        const float NEAR = 1.0f;
        float px4[4], py4[4], pz4[4], pl4[4], pu4[4], pv4[4];
        int nc = 0;

        for (int a = 0; a < 3; a++) {
            int b = (a + 1) % 3;
            bool ina = (wz[a] > NEAR), inb = (wz[b] > NEAR);

            if (ina) {
                px4[nc] = wx[a]; py4[nc] = wy[a]; pz4[nc] = wz[a];
                pl4[nc] = vlit[a]; pu4[nc] = vu[a]; pv4[nc] = vv[a];
                nc++;
            }
            if (ina != inb) {
                /* Attributes interpolate linearly in view space, which is why
                 * the split happens here and not after the divide. */
                float ct = (NEAR - wz[a]) / (wz[b] - wz[a]);
                px4[nc] = wx[a] + (wx[b] - wx[a]) * ct;
                py4[nc] = wy[a] + (wy[b] - wy[a]) * ct;
                pz4[nc] = NEAR;
                pl4[nc] = vlit[a] + (vlit[b] - vlit[a]) * ct;
                pu4[nc] = vu[a] + (vu[b] - vu[a]) * ct;
                pv4[nc] = vv[a] + (vv[b] - vv[a]) * ct;
                nc++;
            }
        }
        if (nc < 3) {
            continue;               /* wholly behind the eye */
        }

        /* Fan the polygon back into triangles and project each. */
        for (int fan = 2; fan < nc; fan++) {
            const int fi[3] = { 0, fan - 1, fan };

            float sxp[3], syp[3], szp[3], flit[3], fu[3], fv[3];
            for (int k = 0; k < 3; k++) {
                int q = fi[k];
                float sc = f / pz4[q];
                sxp[k] = ccx + px4[q] * sc;
                syp[k] = ccy + py4[q] * sc;
                szp[k] = pz4[q];
                flit[k] = pl4[q];
                fu[k] = pu4[q];
                fv[k] = pv4[q];
            }

            /* Signed area in screen space: its sign is the winding, which is
             * how a back face is recognised after projection. */
            float area = (sxp[1] - sxp[0]) * (syp[2] - syp[0])
                       - (sxp[2] - sxp[0]) * (syp[1] - syp[0]);

            if (fabsf(area) < 1e-6f) {
                continue;           /* edge-on: nothing to fill */
            }
            if (m->cull && area < 0.0f) {
                continue;
            }
            if (n >= cap) {
                break;              /* the staging buffer is full */
            }

            /*
             * The rasterizer accepts one winding only, so a front face that
             * projected the other way round is reversed here rather than
             * complicating the pixel loop with a sign test.
             */
            int i0 = 0, i1 = 1, i2 = 2;
            if (area < 0.0f) {
                i1 = 2; i2 = 1;
            }

        ScreenTri *o = &out[n++];
        o->x0 = sxp[i0]; o->y0 = syp[i0]; o->z0 = szp[i0];
        o->x1 = sxp[i1]; o->y1 = syp[i1]; o->z1 = szp[i1];
        o->x2 = sxp[i2]; o->y2 = syp[i2]; o->z2 = szp[i2];
        o->r = base_r * lit;
        o->g = base_g * lit;
        o->b = base_b * lit;

        o->l0 = flit[i0]; o->l1 = flit[i1]; o->l2 = flit[i2];

        /*
         * u/z and 1/z, not u and v directly: those interpolate linearly in
         * screen space where u does not, which is the difference between a
         * texture that lies flat on a tilted face and one that visibly swims.
         */
        float iz0 = 1.0f / szp[i0];
        float iz1 = 1.0f / szp[i1];
        float iz2 = 1.0f / szp[i2];

        o->u0 = fu[i0] * iz0; o->v0 = fv[i0] * iz0; o->w0 = iz0;
        o->u1 = fu[i1] * iz1; o->v1 = fv[i1] * iz1; o->w1 = iz1;
        o->u2 = fu[i2] * iz2; o->v2 = fv[i2] * iz2; o->w2 = iz2;

        /* Reciprocal edge lengths, so the rasterizer can turn its edge
         * functions into pixel distances without three square roots per pixel.
         * A degenerate edge would divide by zero; it gets a huge reciprocal,
         * which reads as "always far outside" and is harmless because a
         * triangle with a zero-length edge has no area to fill anyway. */
        float ex0 = o->x1 - o->x0, ey0 = o->y1 - o->y0;
        float ex1 = o->x2 - o->x1, ey1 = o->y2 - o->y1;
        float ex2 = o->x0 - o->x2, ey2 = o->y0 - o->y2;
        float ln0 = sqrtf(ex0*ex0 + ey0*ey0);
        float ln1 = sqrtf(ex1*ex1 + ey1*ey1);
        float ln2 = sqrtf(ex2*ex2 + ey2*ey2);
        o->rlen0 = (ln0 > 1e-6f) ? 1.0f / ln0 : 1e6f;
        o->rlen1 = (ln1 > 1e-6f) ? 1.0f / ln1 : 1e6f;
        o->rlen2 = (ln2 > 1e-6f) ? 1.0f / ln2 : 1e6f;

        for (int k = 0; k < 3; k++) {
            if (sxp[k] < minx) minx = sxp[k];
            if (sxp[k] > maxx) maxx = sxp[k];
            if (syp[k] < miny) miny = syp[k];
            if (syp[k] > maxy) maxy = syp[k];
        }
        }
    }

    if (getenv("VR_MESH_DEBUG")) {
        /* View-space depth range, which is what the z-buffer compares — the
         * quickest way to tell a culling problem from an ordering one. */
        float zlo = 1e30f, zhi = -1e30f;
        for (int q = 0; q < n; q++) {
            float zz[3] = { out[q].z0, out[q].z1, out[q].z2 };
            for (int k = 0; k < 3; k++) {
                if (zz[k] < zlo) zlo = zz[k];
                if (zz[k] > zhi) zhi = zz[k];
            }
        }
        fprintf(stderr, "[mesh] id=%s viewz %.1f..%.1f\n",
                m->base.id ? m->base.id : "?", (double)zlo, (double)zhi);
        fprintf(stderr, "[mesh] tris=%zu kept=%d  bbox x %.0f..%.0f y %.0f..%.0f  "
                        "size=%.0f scale=%.2f focal=%.0f ox=%.0f oy=%.0f z=%.0f\n",
                m->tri_count, n, (double)minx, (double)maxx, (double)miny, (double)maxy,
                (double)m->size[1], (double)rt->scale, (double)f,
                (double)ox, (double)oy, (double)rt->z);
    }
    if (n == 0) {
        return 0;
    }

    int x0 = (int)floorf(minx), y0 = (int)floorf(miny);
    int x1 = (int)ceilf(maxx) + 1, y1 = (int)ceilf(maxy) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb_w) x1 = fb_w;
    if (y1 > fb_h) y1 = fb_h;
    if (x1 <= x0 || y1 <= y0) {
        return 0;
    }

    mp->fb_w = fb_w;  mp->fb_h = fb_h;
    mp->bb_x = x0;    mp->bb_y = y0;
    mp->bb_w = x1 - x0;
    mp->bb_h = y1 - y0;
    mp->tri_count = n;
    mp->alpha  = rt->opacity;
    mp->blend  = b_blend_of(m);
    mp->smooth = smooth ? 1 : 0;
    /* Half the stroke: the rasterizer measures from the edge in both
     * directions, so a 2px line reaches 1px either side of it. */
    mp->wire   = m->wire * 0.5f;
    mp->aa     = m->antialias ? 1 : 0;
    mp->tex_w  = (m->tex.pixels != NULL) ? m->tex.width  : 0;
    mp->tex_h  = (m->tex.pixels != NULL) ? m->tex.height : 0;
    return n;
}

void vr_camera_view(const Scene *scene, float t_sec, float focal, float view[12])
{
    const Camera3D *c = &scene->camera.eye;

    /* The fixed viewpoint: back along -z by the focal length, looking at the
     * origin. Chosen so that the moving and non-moving cases share one code
     * path and agree exactly where they overlap. */
    float ex = 0.0f, ey = 0.0f, ez = -focal;
    float tx = 0.0f, ty = 0.0f, tz = 0.0f;
    float roll = 0.0f;

    if (c->moving) {
        ex = track_sample(&c->px, t_sec);
        ey = track_sample(&c->py, t_sec);
        ez = track_sample(&c->pz, t_sec);
        tx = track_sample(&c->tx, t_sec);
        ty = track_sample(&c->ty, t_sec);
        tz = track_sample(&c->tz, t_sec);
        roll = track_sample(&c->roll, t_sec) * (float)(M_PI / 180.0);
    }

    /* Forward. A degenerate eye/target pair falls back to looking down +z
     * rather than producing NaNs that would poison every vertex. */
    float fx = tx - ex, fy = ty - ey, fz = tz - ez;
    float fl = sqrtf(fx * fx + fy * fy + fz * fz);
    if (fl < 1e-6f) {
        fx = 0.0f; fy = 0.0f; fz = 1.0f; fl = 1.0f;
    }
    fx /= fl; fy /= fl; fz /= fl;

    /*
     * Up is (0,-1,0) because y runs down: "up" on screen is negative y. If the
     * camera looks straight up or down that is parallel to the forward axis and
     * the cross product collapses, so a different reference is used there.
     */
    float ux = 0.0f, uy = -1.0f, uz = 0.0f;
    if (fabsf(fy) > 0.999f) {
        ux = 0.0f; uy = 0.0f; uz = 1.0f;
    }

    /*
     * right = forward x up, then down = forward x right.
     *
     * The order matters and is easy to get backwards: with the default view
     * (forward +z, up (0,-1,0)) this gives right = (1,0,0) and down = (0,1,0),
     * i.e. the identity — screen x to the right and screen y down, exactly the
     * axes every existing scene was authored against. Taking up x forward
     * instead negates both and silently mirrors the whole world.
     */
    float rx = fy * uz - fz * uy;
    float ry = fz * ux - fx * uz;
    float rz = fx * uy - fy * ux;
    float rl = sqrtf(rx * rx + ry * ry + rz * rz);
    if (rl < 1e-6f) {
        rx = 1.0f; ry = 0.0f; rz = 0.0f; rl = 1.0f;
    }
    rx /= rl; ry /= rl; rz /= rl;

    float vx = fy * rz - fz * ry;
    float vy = fz * rx - fx * rz;
    float vz = fx * ry - fy * rx;

    /* Roll spins the basis about the view axis. */
    if (roll != 0.0f) {
        float cs = cosf(roll), sn = sinf(roll);
        float nrx = rx * cs + vx * sn, nry = ry * cs + vy * sn, nrz = rz * cs + vz * sn;
        float nvx = vx * cs - rx * sn, nvy = vy * cs - ry * sn, nvz = vz * cs - rz * sn;
        rx = nrx; ry = nry; rz = nrz;
        vx = nvx; vy = nvy; vz = nvz;
    }

    /* Rows of the view rotation, then the translation that puts the eye at the
     * origin. A world point p becomes (R·p + T). */
    view[0] = rx; view[1] = ry; view[2]  = rz; view[3]  = -(rx * ex + ry * ey + rz * ez);
    view[4] = vx; view[5] = vy; view[6]  = vz; view[7]  = -(vx * ex + vy * ey + vz * ez);
    view[8] = fx; view[9] = fy; view[10] = fz; view[11] = -(fx * ex + fy * ey + fz * ez);
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

    /*
     * A still image: --frame writes a .png, which needs a different tail.
     * -movflags belongs to the mp4 muxer, and -pix_fmt nv12 would ask the PNG
     * encoder for a format it does not have; rgb24 is what it wants.
     */
    size_t olen = strlen(output_file);
    bool   is_png = (olen > 4 && strcmp(output_file + olen - 4, ".png") == 0);

    if (is_png) {
        char cmd_png[4096];
        int  m = snprintf(cmd_png, sizeof cmd_png,
                          "ffmpeg -hide_banner -loglevel error -y "
                          "-f rawvideo -pixel_format nv12 -video_size %dx%d -framerate %d "
                          "-colorspace bt709 -color_primaries bt709 -color_trc bt709 "
                          "-color_range tv -i - "
                          "-frames:v 1 -pix_fmt rgb24 %s",
                          ctx->config.width, ctx->config.height, ctx->config.fps,
                          quoted_out);
        if (m < 0 || (size_t)m >= sizeof cmd_png) {
            fprintf(stderr, "error: the ffmpeg command did not fit in the buffer.\n");
            return NULL;
        }
        fprintf(stderr, "FFmpeg: %s\n", cmd_png);
        FILE *pipe_png = popen(cmd_png, "w");
        if (pipe_png == NULL) {
            fprintf(stderr, "error: could not start ffmpeg (is it on PATH?).\n");
        }
        return pipe_png;
    }

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
