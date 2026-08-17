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

    /*
     * `t` was written as a percentage ("50%") and is still a fraction of the
     * owning scene's duration, not seconds.
     *
     * It cannot be resolved during parsing: in flat mode a scene's duration
     * only becomes known after the whole project has been read. So the flag
     * survives until resolve_relative_times() runs, and is false everywhere
     * afterwards — nothing downstream of the parser ever sees it set.
     */
    bool     t_relative;
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
    WIDGET_CIRCLE,  /* circle/ellipse — accents, pulses               */
    WIDGET_LINE,    /* straight segment — connectors, edges, arrows   */
    WIDGET_PATH,    /* polyline or bezier — curves, plots, glyphs      */
    WIDGET_VIDEO,   /* a decoded clip — one texture per frame           */
    WIDGET_MESH     /* a triangle mesh, rasterized with per-pixel depth  */
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

    /*
     * Position in parse order, which `z_order` starts equal to but may be
     * overridden away from. Scenes own a contiguous run of parse positions, so
     * this is what lets the index check that sorting by z has not moved an
     * object into a neighbouring scene's slice.
     */
    size_t     seq;
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
     * Animated destination size.
     *
     * Distinct from `scale`, and needed because of it: a bar growing from
     * nothing has to start at h = 0, and `scale` multiplies — 0 × anything
     * stays 0, so the bar never appears. These set the size outright.
     *
     * No re-rasterization is involved: the compositor already derives an
     * independent x and y scale from destination ÷ texture size, so animating
     * the destination costs nothing per frame.
     */
    Track      tr_w, tr_h;
    bool       has_track_w, has_track_h;

    /*
     * 2.5D: depth, and rotation about the X and Y axes.
     *
     * A layer stays a flat quad — this is not a mesh renderer — but the quad is
     * placed in space and projected, which is what gives cards that turn to
     * face away, parallax between depths, and a carousel that actually recedes.
     *
     * `z` is positive *away* from the viewer. Zero everywhere means the whole
     * 3D path is skipped and compositing takes exactly the affine route it
     * always did — which is what keeps existing projects byte-identical.
     */
    Track      tr_z, tr_rx, tr_ry;
    bool       has_track_z, has_track_rx, has_track_ry;

    /*
     * How a turned layer is treated.
     *
     * `shading` 0..1 darkens it as it turns away from the viewer — a flat quad
     * has no lighting of its own, so without this a card in mid-turn reads as
     * a shape that merely got narrower.
     *
     * `backface` 0 = show (the historical behaviour), 1 = hide, 2 = dim. Hiding
     * is what a carousel wants: the cards on the far side face away and should
     * not be seen through the near ones.
     */
    float      shading;
    int        backface;

    /* `trim` 0..1 — how much of a line is drawn. Shares the compositor's
     * typewriter cutoff, so it needs no pixel code of its own. */
    Track      tr_trim;
    bool       has_track_trim;

    /*
     * Tint: blend the whole layer toward `tint_color` by an animated amount.
     *
     * A cheaper answer than animating the colour itself, which for text would
     * mean re-rasterizing every frame. Enough for the case that actually comes
     * up — something lighting up, or going red on failure.
     */
    Color      tint_color;
    Track      tr_tint;
    bool       has_track_tint;

    /*
     * Relative layout (see layout.h).
     *
     * The expression ("center", "bottom-160", "50%+40") is stored as text,
     * because translating it into pixels needs the object's size — which only
     * exists after rasterization.
     */
    char      *x_expr, *y_expr;

    /*
     * A binding: `"y": "=title.bottom + 24"`, resolved against other objects
     * once every size and position is known.
     *
     * Layout expressions ("center", "bottom-160") answer "where on the
     * canvas"; a binding answers "where relative to that other thing" — which
     * is what captions, callouts and stacked panels actually need, and what
     * otherwise gets hand-computed and then silently goes stale.
     */
    char      *x_bind, *y_bind;
    float      anchor_x, anchor_y;     /* 0 = edge, 0.5 = centre, 1 = far edge */
    bool       has_anchor_x, has_anchor_y;

    /*
     * The offset the anchor introduces, in pixels (anchor × size).
     * Stored separately so it applies to keyframe tracks too, not just to
     * static positions.
     */
    float      anchor_off_x, anchor_off_y;

    /*
     * Rotation baked in at parse time, added to whatever the timeline supplies.
     *
     * A line is rasterized horizontally and then turned to its real angle. That
     * is what lets `trim` reuse the typewriter's cutoff untouched: in the
     * texture the segment always runs along +x, so "reveal up to here" is a
     * plain x threshold no matter which way the line points on screen.
     */
    float      base_rotation;

    /*
     * Displacement contributed by a `repeat` block (see parser.c).
     *
     * Kept apart from `x`/`y` rather than folded into them, because a repeated
     * object may still position itself with an expression ("center") — and that
     * only becomes pixels after rasterization. Adding the offset there keeps
     * both features working together instead of one overwriting the other.
     */
    float      repeat_dx, repeat_dy;

    /*
     * Group membership. The name is what the JSON wrote; the index is resolved
     * once the widget array exists (see resolve_timeline_targets) and the name
     * is kept only for the error message.
     */
    char      *group_name;
    int        group_index;   /* scene-local index into Scene::groups, or -1 */

    /*
     * Clip mask, in the object's own texture space as fractions 0..1.
     * shape 0 = none, 1 = circle (cx, cy, r), 2 = rectangle (x, y, w, h).
     *
     * Fractions rather than pixels so a mask survives scaling: "the left half"
     * stays the left half whatever size the object animates to.
     */
    int        mask_shape;
    float      mask[4];
    bool       mask_invert;

    /*
     * Drop shadow / glow.
     *
     * Both are the same operation: blur the object's own alpha, tint it, and
     * draw it underneath. A glow is simply a shadow with no offset and a bright
     * colour, so one set of fields covers both.
     *
     * Applied to the *texture* rather than by the compositor, so it costs
     * nothing per frame — the same reasoning as gradients. The texture grows by
     * `tex_pad` on every side to make room, and the object's position is pulled
     * back by the same amount so the content does not move.
     */
    bool       shadow_on;
    float      shadow_dx, shadow_dy;
    float      shadow_blur;
    Color      shadow_color;
    int        tex_pad;

    /* Blend mode: 0 = normal, 1 = additive, 2 = screen. See pixel_ops.h. */
    int        blend;
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

    /*
     * Outline. `stroke_width` 0 means no outline, which is the historical
     * behaviour; `filled` false draws the outline alone.
     *
     * Both exist because a ring cannot be expressed by a fill: the workaround
     * was a fully transparent fill colour, which simply produced nothing.
     */
    Color  stroke_color;
    float  stroke_width;
    bool   filled;

    /*
     * An optional gradient fill, drawn by Cairo at rasterization time.
     * kind 0 = none (flat `color`), 1 = linear, 2 = radial.
     *
     * Two stops covers what motion graphics actually use; more would mean a
     * variable-length array in a struct that is otherwise plain data.
     */
    int    grad_kind;
    Color  grad_from, grad_to;
    float  grad_angle;      /* degrees, linear only */
} ShapeWidget;

/*
 * A straight segment.
 *
 * It exists because the alternative was a rotated rectangle whose length and
 * angle the author computed by hand with hypot() and atan2() — the arithmetic
 * that produced most of the visual bugs it was meant to avoid.
 *
 * Stored as its two endpoints; the parser derives the rotation and the
 * rasterizer draws it horizontally.
 */
typedef struct {
    WidgetBase base;         /* base.kind = WIDGET_LINE */

    float  x1, y1, x2, y2;
    float  width;
    Color  color;
    int    cap;              /* 0 = butt, 1 = round, 2 = square */
} LineWidget;

/*
 * One step of a path.
 *
 * Quadratics are converted to cubics while parsing, so there is exactly one
 * curve type here — the rasterizer and the bounding-box pass each handle two
 * cases instead of three.
 */
typedef struct {
    uint8_t op;      /* 0 = move, 1 = line, 2 = cubic, 3 = close */
    float   c[6];    /* line/move: x,y | cubic: c1x,c1y,c2x,c2y,x,y */
} PathSeg;

/*
 * A polyline or bezier path.
 *
 * The alternative in a project file was forty-six points and forty-five tiny
 * rectangles per curve — which is not only verbose but visibly faceted, and
 * makes the curve impossible to restyle.
 *
 * Coordinates are stored relative to the path's own bounding box, so the widget
 * behaves like every other one: a texture with a position, free to be moved,
 * scaled and rotated by the timeline.
 */
typedef struct {
    WidgetBase base;         /* base.kind = WIDGET_PATH */

    PathSeg *segs;
    size_t   seg_count, seg_cap;

    float    width;          /* stroke width; 0 → fill only */
    Color    color;          /* stroke colour  */
    Color    fill_color;
    bool     filled;
    bool     closed;
    int      cap, join;      /* 0 = butt/miter, 1 = round, 2 = square/bevel */
} PathWidget;

/*
 * A video clip, decoded to frames at load time.
 *
 * This is the one widget that breaks the pipeline's central assumption — a
 * texture drawn once and composited many times. The resolution is to decode
 * every frame up front, stacked into a single texture, and pick the right slice
 * per frame at composite time. That keeps everything else intact: the frame is
 * still a pure function of time, `--range` still works, and both backends need
 * only an offset rather than a decoder.
 *
 * The cost is memory, which is why frames are decoded at the *destination*
 * size rather than the source's: a 640x360 clip is 0.9 MB a frame, a 1080p one
 * is 8.3 MB. See `mem_budget_mb`.
 */
typedef struct {
    WidgetBase base;         /* base.kind = WIDGET_VIDEO */

    char  *path;
    float  start;            /* in-point within the source, seconds        */
    float  speed;            /* 1 = natural; 2 = twice as fast             */
    bool   loop;             /* repeat instead of holding the last frame   */

    int    frame_w, frame_h; /* one frame; base.tex holds them stacked     */
    int    frame_count;
    float  src_fps;

    int    request_w, request_h;  /* 0 = the source's own size             */
} VideoWidget;

/* ------------------------------------------------------------------------- */
/* Meshes                                                                     */
/* ------------------------------------------------------------------------- */

/* One triangle, as indices into the vertex array. */
typedef struct {
    int v[3];
} MeshTri;

/*
 * A triangle mesh.
 *
 * This is the one widget that is not a texture. Everything else in the project
 * is rasterized once and composited many times; a mesh has to be transformed,
 * projected and filled every frame, because its silhouette changes with every
 * degree of rotation.
 *
 * Vertices are stored in the mesh's own space, centred on the origin and scaled
 * so the longest axis spans 1.0. That normalisation is what lets `size` mean
 * the same thing whatever model is loaded — an OBJ exported in metres and one
 * exported in millimetres both arrive as unit cubes.
 */
typedef struct {
    WidgetBase base;         /* base.kind = WIDGET_MESH */

    float   *verts;          /* 3 floats each, model space */
    float   *norms;          /* 3 floats each; NULL when the mesh has none */
    float   *uvs;            /* 2 floats each; NULL when the mesh has none */
    size_t   vert_count;
    MeshTri *tris;
    size_t   tri_count;

    /*
     * Smooth (Gouraud) shading, interpolating per-vertex normals.
     *
     * Defaults per shape rather than globally: averaging face normals across a
     * cube's corner rounds it visibly, while a sphere without it is a heap of
     * visible facets. The right answer depends on whether the surface is meant
     * to be curved, which only the shape knows.
     */
    bool     smooth;

    Texture  tex;            /* optional surface texture; pixels NULL if none */
    char    *tex_path;

    /*
     * Where this mesh's triangles live inside the renderer's shared staging
     * buffer. Each mesh owns a disjoint region, because the host writes the
     * next mesh's triangles while the previous one's DMA may still be in
     * flight — a single shared region would be overwritten underneath it.
     */
    size_t   tri_base;

    char    *path;           /* NULL for a procedural primitive */
    char    *shape;          /* "box" | "sphere" | "torus" | "cylinder" | "plane" */

    float    size;           /* the unit mesh is scaled to this, in pixels */
    Color    color;
    float    ambient;        /* 0..1 floor, so faces turned away are not black */
    bool     cull;           /* drop back-facing triangles (true for closed shapes) */
    bool     wire;           /* draw edges rather than filled faces */
} MeshWidget;

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
    ACTION_HIGHLIGHT,
    ACTION_ORBIT,      /* position swept around a centre — see the orbit_* fields */
    ACTION_EMIT,       /* one particle: ballistic motion + a lifetime */
    ACTION_ANIMATE     /* a keyframe track on any named property */
} ActionType;

/*
 * The property an `animate` event drives.
 *
 * Having one action able to address every property is what removes the older
 * awkwardness: `scale` and `rotate` were separate actions carrying a single
 * tween each, so two overlapping scales on one object fought, and a property
 * without its own action (a bar's height) had no way to be animated at all.
 */
typedef enum {
    PROP_NONE = 0,
    PROP_X, PROP_Y,
    PROP_OPACITY,
    PROP_SCALE,
    PROP_ROTATION,   /* degrees in the JSON, radians inside */
    PROP_W, PROP_H,
    PROP_TINT,
    PROP_TRIM,
    PROP_Z, PROP_RX, PROP_RY    /* 2.5D: depth and the out-of-plane rotations */
} AnimProp;

/* Per-event easing — see anim.h. */

typedef struct {
    int        time_ms;      /* start, measured from the scene's start        */
    int        duration_ms;  /* duration; 0 → instantaneous                   */
    ActionType action;
    /*
     * A name for this event, and a start time expressed against another.
     *
     * Timelines are edited by inserting and retiming, and absolute
     * milliseconds make every later event wrong the moment an earlier one
     * changes length. A named event that others hang off survives the edit.
     *
     * `time_expr` is resolved after the whole scene is read — an event may
     * reference one written later — and is NULL from then on.
     */
    char      *label;
    char      *time_expr;

    char      *target_id;    /* the widget id exactly as written in the JSON  */
    int        target_index; /* resolved SCENE-LOCAL index; -1 if not found        */
    int        target_group; /* resolved group index, or -1 — targets are either  */
    float      value;        /* generic scalar (scale, rotate, …)             */
    float      value_x;      /* MOVE delta on X                               */
    float      value_y;      /* MOVE delta on Y                               */
    EaseType   ease;         /* "ease": "backout" — defaults to EASE_SMOOTH   */

    /* HIGHLIGHT: an inclusive line range and the band's colour. Lines are
     * 1-based in the JSON, as an editor shows them; stored 0-based. */
    int        hl_from, hl_to;
    Color      hl_color;

    /*
     * ORBIT: the object's centre is swept along a circle (or a spiral, when the
     * two radii differ).
     *
     * This exists because `rotate` spins an object about its *own* centre,
     * which is a different thing entirely — expressing circular travel with it
     * is impossible, and the alternative was a polyline of keyframes
     * approximating the arc. The centre is stored in pixels: the JSON may write
     * it as a layout expression ("center", "50%+40"), which is resolved during
     * parsing, since it depends on the canvas rather than on the object.
     *
     * Angles are degrees. 0° points right (+x); because y grows downwards, a
     * positive sweep reads as clockwise on screen.
     */
    /*
     * EMIT: one particle's flight, evaluated in closed form.
     *
     * Generated by an `emitter` block, never written by hand. Storing velocity
     * rather than keyframes is what keeps a 700-particle burst cheap: the
     * position at any instant is p0 + v·t + ½g·t², so a particle needs no
     * keyframe array and any frame can still be rendered on its own.
     */
    /*
     * ANIMATE: a keyframe track and the property it drives.
     *
     * Key times are seconds *from the event's start*, and values are absolute.
     * Absolute rather than relative is the point: two overlapping tracks on one
     * property no longer accumulate into something neither author intended —
     * the later event simply wins, the same rule fade and scale already follow.
     */
    AnimProp   anim_prop;
    Track      anim_track;
    bool       has_keys;

    float      emit_vx, emit_vy;      /* px per second                         */
    float      emit_gravity;          /* px per second², downward              */
    float      emit_fade;             /* fraction of the life spent fading out */
    float      emit_spin;             /* degrees per second                    */

    float      orbit_cx, orbit_cy;    /* centre, in pixels                     */
    float      orbit_r0, orbit_r1;    /* start and end radius (equal = circle)  */
    float      orbit_a0, orbit_sweep; /* start angle and total travel, degrees  */
    bool       orbit_orient;          /* also turn the object along the tangent */
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

    /*
     * HIGHLIGHT: a band drawn behind a range of lines, the way an editor marks
     * the current line. `hl_alpha` 0 means no band — which is the state of
     * every widget that has no highlight event, so the extra layer costs
     * nothing when unused.
     */
    int   hl_from, hl_to;    /* inclusive, 0-based line indices */
    float hl_alpha;
    Color hl_color;

    /* Destination size for this frame, before `scale` is applied. */
    float w, h;

    /* 2.5D state: depth, the two out-of-plane rotations (radians), and the
     * camera's focal length — carried per widget so the compositor needs
     * nothing but the runtime record. 0 focal = no projection. */
    float z, rx, ry, focal;

    /* Tint strength 0..1, and the colour to blend toward. */
    float tint;
    Color tint_color;
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
/*
 * A parent transform shared by several objects.
 *
 * Groups are not a container: widgets stay in one flat array and merely record
 * which group they belong to. That keeps the renderer's iteration, z-ordering
 * and scene-slicing exactly as they were, while still letting one `rotate`
 * drive sixty-four children.
 *
 * The pivot is a point on the canvas, resolved during parsing like an orbit
 * centre — a group has no size of its own to derive one from.
 */
typedef struct {
    char  *id;
    float  pivot_x, pivot_y;
} GroupDef;

/* Per-frame state of one group. Composed onto every member. */
typedef struct {
    float x, y;        /* translation, pixels                */
    float scale;
    float rotation;    /* radians                            */
    float opacity;     /* multiplies each member's own       */
} GroupRuntime;

/*
 * Upper bound on groups per scene.
 *
 * A fixed array keeps vr_evaluate_scene allocation-free — it runs per frame and
 * has no arena of its own. Sixty-four is far past any sane hand-authored scene;
 * the parser rejects more with a clear message rather than overflowing.
 */
#define VR_MAX_GROUPS 64

/*
 * A whole-scene transform: zoom, pan, roll and shake.
 *
 * It is composed onto every object exactly as a group is, and for the same
 * reason — the alternative is animating each object identically, which is both
 * verbose and impossible to keep in sync. A camera push replaces what would
 * otherwise be a `scale` on every layer in the scene.
 */
/*
 * A camera in the scene's 3D space.
 *
 * World space is the canvas: the origin sits at the centre of the frame, x runs
 * right, y runs *down* (as everywhere else in this project) and z runs away
 * from the viewer. Keeping y down rather than flipping to a maths convention
 * means a 3D position and a 2D one mean the same thing, which is what lets a
 * mesh and a text layer be placed against each other.
 *
 * The default — eye at (0, 0, -focal) looking at the origin — reproduces the
 * fixed viewpoint exactly, so a scene that never mentions a camera position
 * renders as it always did.
 */
typedef struct {
    bool  moving;               /* any of the tracks below were given */
    Track px, py, pz;           /* eye */
    Track tx, ty, tz;           /* what it looks at */
    Track roll;                 /* degrees about the view axis */
} Camera3D;

typedef struct {
    bool  present;

    Camera3D eye;

    /*
     * Focal length in pixels: how strong the perspective is.
     *
     * 0 disables projection entirely (the historical behaviour). A useful
     * value is around twice the canvas width — smaller exaggerates depth, and
     * anything smaller than the scene's own z range puts objects behind the
     * viewer.
     */
    float focal;

    Track zoom;        /* 1 = neutral                                   */
    Track x, y;        /* pan, in pixels; positive x moves content left */
    Track rotation;    /* roll, degrees                                 */
    Track shake;       /* amplitude in pixels; 0 = still               */
} Camera;

typedef struct {
    char          *id;
    int            duration_ms;
    int            start_ms;      /* computed, accounting for overlaps        */

    size_t         first_widget;  /* index into ctx->widgets                  */
    size_t         widget_count;

    TimelineEvent *events;        /* scene-local; target_index is local too   */
    size_t         event_count, event_cap;

    GroupDef      *groups;        /* scene-local, like everything else here    */
    size_t         group_count, group_cap;

    /*
     * Named instants: `"labels": { "beat1": 1200 }` in milliseconds.
     * Audio cues live here too — a marker is just a label whose value the
     * author took from a waveform.
     */
    char         **label_names;
    int           *label_times;
    size_t         label_count, label_cap;

    Camera         camera;

    Color          bg_color;
    bool           has_bg;

    /*
     * A point light, in world units.
     *
     * Without one, meshes are lit from the camera: every surface facing the
     * viewer is bright and nothing is ever in shadow. That is the right default
     * for a single object on a title card — it can never hide its own subject —
     * and quite wrong for a scene that is *about* where the light comes from,
     * where the whole point is that half of each body is dark.
     *
     * Absent by default, so every existing project keeps the camera-mounted
     * shading it was authored against.
     */
    bool           has_light;
    float          light[3];

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

    LineWidget    *lines;
    size_t         line_count, line_cap;

    PathWidget    *paths;
    size_t         path_count, path_cap;

    VideoWidget   *videos;
    size_t         video_count, video_cap;

    MeshWidget    *meshes;
    size_t         mesh_count, mesh_cap;

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
