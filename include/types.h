#ifndef VIDEO_REDAC_TYPES_H
#define VIDEO_REDAC_TYPES_H

/*
 * types.h — the project's central data structures.
 *
 * Strict rule: no global variables. All state lives in `EditorContext`, which
 * is passed to functions by pointer.
 *
 * This header is read from both C11 (host) and CUDA C++ (device), so it holds
 * only plain-old-data types — no functions.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * How many frames are in flight in the render pipeline.
 *
 * It lives here because both sides need it: renderer.cu creates this many frame
 * buffers, and media_loader.c reserves this many typewriter-cutoff slots. A
 * mismatch would be a silent race — frame i overwriting what frame i-1 is still
 * reading.
 *
 * Override with: make CPPFLAGS_EXTRA=-DVR_PIPELINE_DEPTH=1 — useful for
 * measurement, since 1 means a fully serial pipeline with no overlap.
 */
#ifndef VR_PIPELINE_DEPTH
#  define VR_PIPELINE_DEPTH 2
#endif

/* ------------------------------------------------------------------------- */
/* Animation core: easing + keyframe tracks                                   */
/* ------------------------------------------------------------------------- */

/*
 * These structs live here rather than in anim.h for the same reason Arena
 * does: WidgetBase stores them *by value*, so types.h needs the full
 * definition. anim.h declares only functions — the same split arena.h uses.
 */
typedef enum {
    EASE_LINEAR = 0,
    EASE_IN,          EASE_OUT,          EASE_INOUT,        /* quadratic */
    EASE_CUBIC_IN,    EASE_CUBIC_OUT,    EASE_CUBIC_INOUT,
    EASE_EXPO_IN,     EASE_EXPO_OUT,     EASE_EXPO_INOUT,
    EASE_BACK_IN,     EASE_BACK_OUT,     EASE_BACK_INOUT,   /* overshoots, then settles */
    EASE_ELASTIC_OUT, /* spring-like wobble */
    EASE_BOUNCE_OUT,  /* bounces to rest    */
    EASE_SMOOTH       /* smoothstep — the historical default */
} EaseType;

typedef struct {
    float    t;     /* time in seconds */
    float    v;
    EaseType ease;  /* the curve used to reach *this* key */
} Keyframe;

/*
 * Either a constant (keys == NULL) or an ascending sequence of keyframes.
 * Sampling rules: see anim.h / track_sample().
 */
typedef struct {
    Keyframe *keys;
    int       count;
    float     constant;
} Track;

/* Effects are only ever held by pointer, so an incomplete type is enough
 * (the full definition is in effects.h). */
struct Effect;

/* ------------------------------------------------------------------------- */
/* Colour                                                                     */
/* ------------------------------------------------------------------------- */

/* RGBA8 — exactly the layout the GPU framebuffer and ffmpeg's
 * `-pix_fmt rgba` expect. */
typedef struct {
    uint8_t r, g, b, a;
} Color;

/* ------------------------------------------------------------------------- */
/* Arena Allocator                                                            */
/* ------------------------------------------------------------------------- */

/*
 * A simple bump allocator for memory that lives exactly one frame.
 *
 * Rationale: at 60 fps we cannot call malloc/free per frame — that means
 * fragmentation and unpredictable latency. Instead one large block is taken
 * once, `used` is bumped during the frame, and at the end `arena_reset()`
 * releases everything with a single assignment (O(1)).
 *
 * Consequence: memory taken from the arena is *never* freed individually.
 */
typedef struct {
    uint8_t *base;      /* start of the block (malloc'd once)                 */
    size_t   capacity;  /* total block size in bytes                          */
    size_t   used;      /* bytes handed out right now                         */
    size_t   peak;      /* high-water mark of `used`, for diagnostics         */
    bool     owns_base; /* true → arena_destroy() frees `base`                */
} Arena;

/* Arena marker: lets a scoped "sub-allocation" be rolled back. */
typedef struct {
    size_t used;
} ArenaMarker;

/* ------------------------------------------------------------------------- */
/* Texture (pixels rasterized on the CPU + their VRAM copy)                   */
/* ------------------------------------------------------------------------- */

/*
 * The heart of the hybrid design.
 *
 * `pixels` — an RGBA8 buffer in host RAM, filled once at init by Cairo (text)
 * or stb_image (pictures). This is the cache: the same string is *not*
 * rasterized again on every frame.
 *
 * `d_pixels` — a copy of that buffer in VRAM. The type is `void*` because this
 * header is also read from plain C; renderer.cu casts it to `uchar4*`.
 *
 * ALPHA: `pixels` is stored PREMULTIPLIED (r,g,b already multiplied by a).
 * That is not an arbitrary choice:
 *   1. Cairo's ARGB32 is premultiplied already — un-premultiplying loses
 *      precision on antialiased semi-transparent edges (dark "fringing"
 *      around glyph outlines).
 *   2. Bilinear interpolation and fading are only correct in premultiplied
 *      space.
 * The kernel uses the matching source-over formula: dst = src + dst*(1-src.a).
 */
typedef struct {
    uint8_t *pixels;        /* host RGBA8, premultiplied; NULL if not yet drawn    */
    int      width;
    int      height;
    size_t   stride;        /* bytes per row (packed → width * 4)              */
    void    *d_pixels;      /* device copy (uchar4*), owned by the renderer    */
    bool     premultiplied; /* invariant — always true in this pipeline        */
} Texture;

/* ------------------------------------------------------------------------- */
/* Widgets                                                                    */
/* ------------------------------------------------------------------------- */

typedef enum {
    WIDGET_TEXT = 0,
    WIDGET_CODE,
    WIDGET_IMAGE,
    WIDGET_RECT,    /* rectangle — scrims, lower thirds, divider bars */
    WIDGET_CIRCLE   /* circle/ellipse — accents, pulses               */
} WidgetKind;

/* ------------------------------------------------------------------------- */
/* Glyph metrics (for a true per-glyph TYPEWRITE)                             */
/* ------------------------------------------------------------------------- */

/*
 * During rasterization we record where every character ends in the texture.
 * Without this, "typing" could only be a crude column wipe; with it the kernel
 * cuts exactly on a character boundary — the way a real editor would type.
 *
 * The layout is flat (one malloc per array) to keep freeing simple:
 *   line l occupies char_x[line_start[l] .. line_start[l+1]-1],
 *   which is (N_l + 1) values: the left edge plus each character's right edge.
 */
typedef struct {
    int    line_count;
    int   *line_start;   /* [line_count + 1] indices into char_x */
    float *char_x;       /* character boundaries in texture X coordinates */
    int    total_chars;  /* total characters (spaces included, '\n' excluded)  */
    float  pad_y;        /* top edge of the first line inside the texture     */
    float  line_height;  /* line step in pixels                               */

    /* One float per line — the cutoff; uploaded to VRAM every frame. */
    float *h_cutoff;     /* host scratch buffer                               */
    void  *d_cutoff;     /* device copy (float*), owned by the renderer       */
} GlyphMetrics;

/*
 * The header every widget shares.
 *
 * It is deliberately the *first* field of every concrete widget struct, which
 * (by C's common-initial-sequence rule) makes casting `TextWidget*` to
 * `WidgetBase*` safe. That is what lets the renderer iterate over all objects
 * uniformly, without knowing their concrete type.
 */
typedef struct {
    WidgetKind kind;
    char      *id;           /* unique name, e.g. "main_title"                */
    float      x, y;         /* base position; after layout: the anchor point */
    bool       auto_center_x;/* true → x derived from the texture width       */
    int        z_order;      /* order in the JSON → painter's algorithm       */
    Texture    tex;          /* the cached pixels                             */
    GlyphMetrics glyphs;     /* text only; zeroed for images and shapes       */

    /*
     * Destination size at scale = 1.0. For text that is the texture size, but
     * an image may request a different one in the JSON — hence a separate
     * field, handed to the kernel alongside the real texture size so it can
     * scale.
     */
    float      base_w, base_h;

    /*
     * Optional keyframe tracks for properties.
     *
     * If the JSON says e.g. "opacity": [{"t":0,"v":0},{"t":1.2,"v":1}], the
     * track sets the value *directly* before timeline events are layered on
     * top. Both styles coexist: a track supplies an absolute value, an event
     * then replaces or adds to it.
     */
    Track      tr_x, tr_y, tr_opacity, tr_scale, tr_rotation;
    bool       has_track_x, has_track_y, has_track_opacity;
    bool       has_track_scale, has_track_rotation;

    /*
     * Relative layout (see layout.h).
     *
     * The expression ("center", "bottom-160", "50%+40") is stored as text,
     * because translating it into pixels needs the object's size — which only
     * exists after rasterization.
     */
    char      *x_expr, *y_expr;
    float      anchor_x, anchor_y;     /* 0 = edge, 0.5 = centre, 1 = far edge */
    bool       has_anchor_x, has_anchor_y;

    /*
     * The offset the anchor introduces, in pixels (anchor × size).
     * Stored separately so it applies to keyframe tracks too, not just to
     * static positions.
     */
    float      anchor_off_x, anchor_off_y;
} WidgetBase;

/* A plain text object — titles, captions, formulae. */
typedef struct {
    WidgetBase base;         /* must be the first field! */

    char  *content;          /* the text itself; '\n' starts a new line       */
    char  *font;             /* family name, e.g. "FiraCode-Bold"             */
    int    size;             /* font size in pixels                           */
    Color  color;
    float  line_spacing;     /* line-spacing factor (typically 1.25)          */

    /*
     * Automatic wrapping and alignment.
     *
     * max_width > 0 → the text wraps on word boundaries within that width.
     * Without it, the only remedy for a long caption was shrinking the font.
     */
    float  max_width;        /* in pixels; 0 = no wrapping                    */
    float  align;            /* 0 = left, 0.5 = centre, 1 = right             */
} TextWidget;

/* A code block — monospace, a backing panel, and syntax highlighting. */
typedef struct {
    WidgetBase base;         /* must be the first field! */

    char  *code;             /* multi-line source                             */
    char  *language;         /* "go", "rust", "python" — for the highlighter  */
    char  *font;
    int    size;
    Color  fg;               /* base colour, used when highlighting is off    */
    Color  bg;               /* panel colour (a=0 → no panel)                 */
    int    padding;          /* inner padding of the panel, in pixels         */
    float  line_spacing;
    bool   highlight;        /* false → single-colour text                    */
    int    corner_radius;    /* corner rounding of the panel                  */

    /*
     * The panel is its own texture, not the glyphs' background.
     *
     * The reason is TYPEWRITE: while typing, *only* the letters may be clipped
     * while the panel stays whole. In a single texture the two layers could no
     * longer be told apart.
     */
    Texture plate;
} CodeWidget;

/*
 * A geometric shape: rectangle or circle.
 *
 * Both share one struct because they differ only by base.kind — separate
 * arrays would just multiply boilerplate.
 *
 * They are drawn into a texture with Cairo rather than by a dedicated kernel.
 * That gives antialiasing and rounded corners for free and — more importantly —
 * lets the entire existing pipeline (fade, move, scale, rotate, effects) apply
 * unchanged.
 */
typedef struct {
    WidgetBase base;         /* base.kind = WIDGET_RECT or WIDGET_CIRCLE */

    Color  color;
    float  w, h;             /* for a circle: the bounding box               */
    int    corner_radius;    /* rectangles only */
} ShapeWidget;

/* A raster image (PNG/JPG), loaded with stb_image. */
typedef struct {
    WidgetBase base;         /* must be the first field! */

    char  *path;             /* path, resolved against the JSON's directory   */
    int    request_w;        /* 0 → native size; otherwise the requested one  */
    int    request_h;
} ImageWidget;

/* ------------------------------------------------------------------------- */
/* Timeline                                                                   */
/* ------------------------------------------------------------------------- */

typedef enum {
    ACTION_UNKNOWN = 0,
    ACTION_FADE_IN,
    ACTION_FADE_OUT,
    ACTION_MOVE,       /* delta on the value_x / value_y axes */
    ACTION_TYPEWRITE,  /* characters appearing one by one     */
    ACTION_SCALE,      /* value → target scale (1.0 = original)       */
    ACTION_ROTATE,     /* value → target angle in degrees     */
    ACTION_HIGHLIGHT
} ActionType;

/* Per-event easing — see anim.h. */

typedef struct {
    int        time_ms;      /* start, measured from the scene's start        */
    int        duration_ms;  /* duration; 0 → instantaneous                   */
    ActionType action;
    char      *target_id;    /* the widget id exactly as written in the JSON  */
    int        target_index; /* resolved SCENE-LOCAL index; -1 if not found        */
    float      value;        /* generic scalar (scale, rotate, …)             */
    float      value_x;      /* MOVE delta on X                               */
    float      value_y;      /* MOVE delta on Y                               */
    EaseType   ease;         /* "ease": "backout" — defaults to EASE_SMOOTH   */
} TimelineEvent;

/*
 * One widget's computed state for one frame.
 *
 * This is *derived* data — recomputed from scratch in the arena every frame and
 * never stored. That gives full determinism: any frame can be rendered
 * independently, which is what makes --range possible.
 */
typedef struct {
    float x, y;
    float opacity;   /* 0.0 .. 1.0 */
    float scale;
    float rotation;  /* radians, about the base centre */
    /*
     * TYPEWRITE progress 0..1 — the fraction of *characters* revealed.
     * Using the glyph metrics it becomes an exact pixel threshold per line
     * (see GlyphMetrics), so the cut always lands on a character boundary and
     * never mid-glyph.
     */
    float reveal;
    bool  visible;
} WidgetRuntime;

/* ------------------------------------------------------------------------- */
/* Audio track                                                                */
/* ------------------------------------------------------------------------- */

/*
 * One audio clip on the video timeline. All times are in seconds.
 *
 * Files only — speech synthesis (TTS) is deliberately not part of the project.
 */
typedef struct {
    char  *path;      /* audio (or video) file to take the sound from       */
    float  start;     /* when it starts on the video timeline               */
    float  in;        /* in-point *within the source* (skip the first N s)      */
    float  duration;  /* how long it plays; 0 → to the source's end         */
    float  volume;    /* linear gain: 0.5 = −6 dB, 2.0 = +6 dB              */
    float  fade_in;   /* fade-in length at the clip's start                 */
    float  fade_out;  /* fade-out length at the clip's end                  */
    bool   loop;      /* repeat the source until `duration` is filled       */
} AudioTrack;

/* ------------------------------------------------------------------------- */
/* Scenes and transitions                                                     */
/* ------------------------------------------------------------------------- */

/*
 * A scene = a clip with its own duration, objects and timeline.
 *
 * Time inside a scene is *local*: t=0 means the scene's start, not the film's.
 * Moving a scene along the timeline therefore requires no recomputation of its
 * contents.
 *
 * Objects live in the shared arrays (texts/codes/…); a scene stores only the
 * contiguous span of `ctx->widgets` that belongs to it. Contiguity is
 * guaranteed by z_order increasing globally across scenes.
 */
typedef struct {
    char          *id;
    int            duration_ms;
    int            start_ms;      /* computed, accounting for overlaps        */

    size_t         first_widget;  /* index into ctx->widgets                  */
    size_t         widget_count;

    TimelineEvent *events;        /* scene-local; target_index is local too   */
    size_t         event_count, event_cap;

    Color          bg_color;
    bool           has_bg;

    /*
     * The scene's own effects — applied *before* the transition, so a clip's
     * look carries into it. The global stack works on the finished frame.
     */
    struct Effect *effects;
    size_t         effect_count, effect_cap;
} Scene;

typedef enum {
    TRANS_CUT = 0,      /* no transition */
    TRANS_CROSSFADE,
    TRANS_FADE,         /* through the background */
    TRANS_SLIDE_LEFT,  TRANS_SLIDE_RIGHT,  TRANS_SLIDE_UP,  TRANS_SLIDE_DOWN,
    TRANS_PUSH_LEFT,   TRANS_PUSH_RIGHT,   TRANS_PUSH_UP,   TRANS_PUSH_DOWN,
    TRANS_ZOOM_IN,     TRANS_ZOOM_OUT,     TRANS_SPIN,
    TRANS_WIPE_LEFT,   TRANS_WIPE_RIGHT,   TRANS_IRIS,
    TRANS_TYPE_COUNT
} TransitionType;

/*
 * A transition between clips.
 *
 * The built-in presets derive their parameters from progress `p ∈ [0,1]`. If
 * the JSON supplies `from`/`to` blocks, those tracks override the preset — so a
 * custom transition can be described as data, with no code.
 *
 * Note: here a Track's "time" is progress in [0,1], not seconds.
 */
typedef struct {
    TransitionType type;
    int            duration_ms;

    bool           has_from, has_to;
    Track          from_opacity, from_x, from_y, from_scale, from_rotate;
    Track          to_opacity,   to_x,   to_y,   to_scale,   to_rotate;

    /*
     * Masks: the clip is revealed from inside a shape.
     * 0 = no mask, 1 = circle (cx, cy, r), 2 = rectangle (x, y, w, h).
     * All parameters are canvas fractions and, like the other channels, are
     * interpolated over progress [0,1].
     */
    int            from_mask_shape, to_mask_shape;
    Track          from_mask[4], to_mask[4];
} Transition;

/* ------------------------------------------------------------------------- */
/* Project configuration                                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    int   width;
    int   height;
    int   fps;
    Color bg_color;
    int   duration_ms;   /* 0 → derived from the timeline */
} ProjectConfig;

/*
 * Encoding parameters. These used to be hard-coded into the ffmpeg command,
 * so changing quality meant editing source.
 */
typedef struct {
    char *encoder;    /* h264_nvenc | hevc_nvenc | av1_nvenc | libx264 … */
    char *preset;     /* p1..p7 (nvenc) or ultrafast..veryslow (x264)    */
    int   cq;         /* quality: lower is better                        */
    char *bitrate;    /* e.g. "12M"; NULL → constant quality (cq)        */
} OutputConfig;

/* ------------------------------------------------------------------------- */
/* EditorContext — the single piece of "global" state, allocated on the heap  */
/* ------------------------------------------------------------------------- */

typedef struct {
    ProjectConfig config;
    OutputConfig  output;

    /* Dynamic arrays (geometric growth, see parser.c) */
    TextWidget    *texts;
    size_t         text_count, text_cap;

    CodeWidget    *codes;
    size_t         code_count, code_cap;

    ImageWidget   *images;
    size_t         image_count, image_cap;

    ShapeWidget   *shapes;
    size_t         shape_count, shape_cap;

    /* Post-processing stack — applied to the whole frame, in order. */
    struct Effect *effects;
    size_t         effect_count, effect_cap;

    /*
     * The sequence of scenes. In flat mode (no "scenes" in the JSON) one
     * implicit scene is created holding every object — that way the renderer
     * has a single code path and older projects keep working unchanged.
     */
    Scene         *scenes;
    size_t         scene_count, scene_cap;

    Transition    *transitions;   /* [i] — between scenes i and i+1 */
    size_t         transition_count, transition_cap;

    /* Audio tracks; empty → the video stays silent and the second pass is skipped. */
    AudioTrack    *audio;
    size_t         audio_count, audio_cap;

    /*
     * Render range in seconds (--range). end <= start → the whole video.
     *
     * This works because timeline evaluation is a pure function: a frame
     * depends only on its own timestamp, never on the previous frame. Starting
     * in the middle is therefore exactly as correct as starting at zero.
     */
    double         range_start_sec, range_end_sec;

    /*
     * A uniform index over every widget.
     *
     * Careful: these pointers point *inside* the `texts`/`codes` arrays, so
     * every realloc invalidates them. That is exactly why the index is built
     * only after parsing has finished (ctx_build_widget_index).
     */
    WidgetBase   **widgets;
    size_t         widget_count;

    Arena          frame_arena;  /* scratch memory for one frame */
    void          *gpu;          /* opaque RenderResources*, owned by renderer.cu       */
} EditorContext;

#endif /* VIDEO_REDAC_TYPES_H */
