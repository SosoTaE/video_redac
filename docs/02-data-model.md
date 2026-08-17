# Data model

Every struct, enum and field. Unless stated otherwise these live in
`include/types.h`, which is included from both C11 and CUDA C++ and therefore
contains only plain-old-data — no functions.

## Compile-time constants

| Macro | Default | Meaning |
|---|---|---|
| `VR_PIPELINE_DEPTH` | 2 | Frames in flight. Defined in `types.h` because both the renderer (frame buffers) and the media loader (typewriter cutoff slots) size arrays by it; a mismatch would be a silent race. Override with `make CPPFLAGS_EXTRA=-DVR_PIPELINE_DEPTH=1`. |

---

## Colour

```c
typedef struct { uint8_t r, g, b, a; } Color;
```

RGBA8, exactly the layout the framebuffer and `ffmpeg -pix_fmt rgba` expect.
Parsed from `"#RRGGBB"` or `"#RRGGBBAA"`.

---

## Arena allocator

```c
typedef struct {
    uint8_t *base;      /* block start (one malloc)          */
    size_t   capacity;  /* total size in bytes               */
    size_t   used;      /* bytes handed out right now        */
    size_t   peak;      /* high-water mark, for diagnostics  */
    bool     owns_base; /* true → arena_destroy() frees base */
} Arena;

typedef struct { size_t used; } ArenaMarker;
```

Bump allocator for one frame's lifetime. Memory taken from it is **never freed
individually** — `arena_reset()` releases everything at once.

`owns_base` distinguishes `arena_init()` (mallocs) from
`arena_init_from_buffer()` (wraps someone else's memory).

---

## Animation core

```c
typedef enum {
    EASE_LINEAR = 0,
    EASE_IN, EASE_OUT, EASE_INOUT,                    /* quadratic */
    EASE_CUBIC_IN, EASE_CUBIC_OUT, EASE_CUBIC_INOUT,
    EASE_EXPO_IN, EASE_EXPO_OUT, EASE_EXPO_INOUT,
    EASE_BACK_IN, EASE_BACK_OUT, EASE_BACK_INOUT,     /* overshoots, then settles */
    EASE_ELASTIC_OUT,                                 /* spring wobble */
    EASE_BOUNCE_OUT,
    EASE_SMOOTH                                       /* smoothstep — historical default */
} EaseType;

typedef struct {
    float    t;     /* seconds (or progress 0..1 inside a transition) */
    float    v;
    EaseType ease;  /* the curve used to reach *this* key */
} Keyframe;

typedef struct {
    Keyframe *keys;     /* NULL → constant */
    int       count;
    float     constant;
} Track;
```

These sit in `types.h` rather than `anim.h` for the same reason `Arena` does:
`WidgetBase` stores `Track` **by value**, so the full definition is required.
`anim.h` declares only the functions.

Easing belongs to the key you are moving **toward**; interpolation between keys
`i` and `i+1` uses `keys[i+1].ease`.

---

## Texture

```c
typedef struct {
    uint8_t *pixels;        /* host RGBA8, premultiplied; NULL if not rasterized */
    int      width, height;
    size_t   stride;        /* bytes per row — always width*4 (packed)   */
    void    *d_pixels;      /* device copy (uchar4*), owned by renderer  */
    bool     premultiplied; /* invariant: always true in this pipeline   */
} Texture;
```

`d_pixels` is `void*` so the header stays readable from plain C; `renderer.cu`
casts it to `uchar4*`.

`stride` exists for generality but is always packed — Cairo's own stride is
aligned and gets repacked during conversion, because `cudaMemcpy` and the
kernels prefer a uniform layout.

---

## Glyph metrics

```c
typedef struct {
    int    line_count;
    int   *line_start;   /* [line_count + 1] indices into char_x */
    float *char_x;       /* character boundaries in texture X coords */
    int    total_chars;
    float  pad_y;        /* top edge of the first line inside the texture */
    float  line_height;  /* line step in pixels */

    float *h_cutoff;     /* host scratch, VR_PIPELINE_DEPTH slots */
    void  *d_cutoff;     /* device copy (float*), owned by renderer */
} GlyphMetrics;
```

Recorded during rasterization so the typewriter can cut exactly on a character
boundary. Layout is flat (one `malloc` per array) to keep freeing simple:

```
line l occupies char_x[line_start[l] .. line_start[l+1]-1]
        which is (N_l + 1) values: left edge + right edge of each character
chars_in_line(l) = line_start[l+1] - line_start[l] - 1
```

`h_cutoff` and `d_cutoff` hold `VR_PIPELINE_DEPTH` slots so frame *i* cannot
overwrite what frame *i−1* is still reading.

---

## Widgets

```c
typedef enum {
    WIDGET_TEXT = 0, WIDGET_CODE, WIDGET_IMAGE, WIDGET_RECT, WIDGET_CIRCLE
} WidgetKind;
```

### WidgetBase

Deliberately the **first field** of every concrete widget, so a `TextWidget*`
can be safely cast to `WidgetBase*` (C's common-initial-sequence rule). This is
what lets the renderer iterate over all objects uniformly.

```c
typedef struct {
    WidgetKind kind;
    char      *id;
    float      x, y;            /* after layout: the ANCHOR POINT, not top-left */
    bool       auto_center_x;   /* x omitted → centre horizontally */
    int        z_order;         /* JSON order → painter's algorithm */
    Texture    tex;
    GlyphMetrics glyphs;        /* zeroed for images and shapes */

    float      base_w, base_h;  /* destination size at scale = 1 */

    Track      tr_x, tr_y, tr_opacity, tr_scale, tr_rotation;
    bool       has_track_x, has_track_y, has_track_opacity;
    bool       has_track_scale, has_track_rotation;

    char      *x_expr, *y_expr; /* "center", "bottom-160" — resolved later */
    float      anchor_x, anchor_y;         /* 0 | 0.5 | 1 */
    bool       has_anchor_x, has_anchor_y;
    float      anchor_off_x, anchor_off_y; /* anchor × size, in pixels */
} WidgetBase;
```

Three fields deserve care:

- **`base_w`/`base_h`** is the destination size, which is *not* the texture size
  for images (JSON may request a different one). The kernel receives both and
  scales.
- **`has_track_*`** means "the JSON specified this property", **not** "it is
  animated". A constant `"opacity": 0.0` must still win over the default of 1.0.
  Getting this wrong made explicit zeros invisible — see
  [06-algorithms.md](06-algorithms.md#the-constant-property-trap).
- **`anchor_off_*`** is stored separately so the offset also applies to keyframe
  tracks, not just static positions.

> After layout resolution, `x`/`y` hold the **anchor point**. Anything reporting
> real bounds (the validator, `--dump`) must subtract `anchor_off_*`.

### TextWidget

```c
typedef struct {
    WidgetBase base;
    char  *content;      /* '\n' starts a new line */
    char  *font;         /* family name, e.g. "FiraCode-Bold" */
    int    size;
    Color  color;
    float  line_spacing; /* typically 1.25 */
    float  max_width;    /* pixels; 0 = no wrapping */
    float  align;        /* 0 left | 0.5 centre | 1 right */
} TextWidget;
```

### CodeWidget

```c
typedef struct {
    WidgetBase base;
    char  *code, *language, *font;
    int    size;
    Color  fg, bg;
    int    padding, corner_radius;
    float  line_spacing;
    bool   highlight;    /* false → single colour */
    Texture plate;       /* the rounded backing panel — a SEPARATE layer */
} CodeWidget;
```

`plate` is separate because of the typewriter: only the glyphs may be clipped
while the panel stays whole. In one texture the two could not be told apart.

### ImageWidget

```c
typedef struct {
    WidgetBase base;
    char *path;
    int   request_w, request_h;  /* 0 → native size; one axis → keep aspect */
} ImageWidget;
```

### ShapeWidget

```c
typedef struct {
    WidgetBase base;   /* kind = WIDGET_RECT or WIDGET_CIRCLE */
    Color  color;
    float  w, h;       /* for a circle: the bounding box */
    int    corner_radius;
} ShapeWidget;
```

Both shapes share a struct because they differ only by `base.kind`. They are
drawn with Cairo rather than a dedicated kernel, which gives antialiasing and
rounded corners for free and lets the whole existing pipeline apply unchanged.

---

## Timeline

```c
typedef enum {
    ACTION_UNKNOWN = 0,
    ACTION_FADE_IN, ACTION_FADE_OUT,
    ACTION_MOVE,       /* delta on value_x / value_y */
    ACTION_TYPEWRITE,
    ACTION_SCALE,      /* value → target scale */
    ACTION_ROTATE,     /* value → target angle, degrees */
    ACTION_HIGHLIGHT   /* reserved */
} ActionType;

typedef struct {
    int        time_ms, duration_ms;
    ActionType action;
    char      *target_id;
    int        target_index;   /* SCENE-LOCAL index; -1 if unresolved */
    float      value, value_x, value_y;
    EaseType   ease;           /* default EASE_SMOOTH */
} TimelineEvent;
```

`target_index` is local to the scene, which is why the same `id` may appear in
different scenes.

### WidgetRuntime

One widget's computed state for one frame. Derived data, rebuilt from scratch in
the arena every frame — never stored.

```c
typedef struct {
    float x, y;
    float opacity;   /* 0..1 */
    float scale;
    float rotation;  /* radians, about the base centre */
    float reveal;    /* typewriter progress 0..1, in characters */
    bool  visible;
} WidgetRuntime;
```

---

## Scenes and transitions

```c
typedef struct {
    char          *id;
    int            duration_ms;
    int            start_ms;      /* computed, overlap-aware */
    size_t         first_widget, widget_count;
    TimelineEvent *events;
    size_t         event_count, event_cap;
    Color          bg_color;
    bool           has_bg;
    struct Effect *effects;       /* applied BEFORE the transition */
    size_t         effect_count, effect_cap;
} Scene;
```

```c
typedef enum {
    TRANS_CUT = 0, TRANS_CROSSFADE, TRANS_FADE,
    TRANS_SLIDE_LEFT, TRANS_SLIDE_RIGHT, TRANS_SLIDE_UP, TRANS_SLIDE_DOWN,
    TRANS_PUSH_LEFT,  TRANS_PUSH_RIGHT,  TRANS_PUSH_UP,  TRANS_PUSH_DOWN,
    TRANS_ZOOM_IN, TRANS_ZOOM_OUT, TRANS_SPIN,
    TRANS_WIPE_LEFT, TRANS_WIPE_RIGHT, TRANS_IRIS
} TransitionType;

typedef struct {
    TransitionType type;
    int            duration_ms;
    bool           has_from, has_to;
    Track from_opacity, from_x, from_y, from_scale, from_rotate;
    Track to_opacity,   to_x,   to_y,   to_scale,   to_rotate;
    int   from_mask_shape, to_mask_shape;  /* 0 none, 1 circle, 2 rect */
    Track from_mask[4], to_mask[4];        /* circle: cx,cy,r | rect: x,y,w,h */
} Transition;
```

Inside a transition a `Track`'s "time" is **progress in [0,1]**, not seconds.
All mask parameters are canvas fractions.

---

## Effects

`include/effects.h`

```c
typedef enum {
    FX_NONE = 0,
    /* pointwise — no neighbour reads */
    FX_GRAYSCALE, FX_INVERT, FX_SEPIA, FX_POSTERIZE, FX_THRESHOLD,
    FX_VIGNETTE, FX_GRAIN, FX_SCANLINES, FX_COLOR_GRADE, FX_VIBRANCE,
    FX_SPLIT_TONE, FX_GRADIENT_MAP,
    /* neighbourhood — needs a ping-pong buffer */
    FX_BLUR, FX_PIXELATE, FX_RGB_SPLIT, FX_GLITCH
} EffectType;

typedef enum {
    FXP_AMOUNT = 0, FXP_RADIUS, FXP_SOFTNESS, FXP_ANGLE, FXP_COUNT,
    FXP_SIZE, FXP_LEVEL, FXP_LEVELS, FXP_BALANCE, FXP_EXPOSURE,
    FXP_BRIGHTNESS, FXP_CONTRAST, FXP_GAMMA, FXP_SATURATION,
    FXP_VIBRANCE, FXP_HUE, FXP_TEMPERATURE, FXP_TINT, FXP_MAX
} EffectParam;

typedef struct Effect {
    EffectType type;
    Track      param[FXP_MAX];
    Color      color_a, color_b;
} Effect;
```

One shared parameter table for all effects; which slots are read depends on the
type. Every parameter is a `Track`, so a blur radius can ramp from 18 to 0.

`struct Effect` carries an explicit tag because `types.h` forward-declares it —
that is how `EditorContext` can hold a pointer without including `effects.h`.

---

## Syntax highlighting

`include/highlighter.h`

```c
typedef enum { LANG_NONE = 0, LANG_C, LANG_GO, LANG_RUST, LANG_PYTHON } Language;

typedef enum {
    TOK_TEXT = 0, TOK_KEYWORD, TOK_TYPE, TOK_STRING, TOK_NUMBER,
    TOK_COMMENT, TOK_FUNCTION, TOK_OPERATOR, TOK_PUNCT, TOK_PREPROC
} TokenClass;

typedef struct { size_t start, len; TokenClass cls; } Token;

typedef struct {
    int  mode;          /* 0 normal, 1 block comment, 2 triple-quoted string */
    char triple_quote;  /* which quote opened it */
} HighlightState;
```

Tokens are **contiguous** — they cover every byte of the line, with whitespace
as `TOK_TEXT`. That makes rendering a straight walk with no gaps to handle.

---

## Audio

```c
typedef struct {
    char  *path;      /* file only — TTS is deliberately out of scope */
    float  start;     /* position on the video timeline, seconds */
    float  in;        /* in-point within the source */
    float  duration;  /* 0 → to source end */
    float  volume;    /* linear gain */
    float  fade_in, fade_out;
    bool   loop;
} AudioTrack;
```

---

## Project and output

```c
typedef struct {
    int   width, height, fps;
    Color bg_color;
    int   duration_ms;   /* 0 → derived from the timeline */
} ProjectConfig;

typedef struct {
    char *encoder;   /* h264_nvenc | hevc_nvenc | av1_nvenc | libx264 … */
    char *preset;    /* p1..p7, or x264 presets */
    int   cq;        /* quality: lower is better */
    char *bitrate;   /* e.g. "12M"; NULL → constant quality */
} OutputConfig;
```

---

## EditorContext

The single piece of state, passed by pointer everywhere.

```c
typedef struct {
    ProjectConfig config;
    OutputConfig  output;

    TextWidget    *texts;    size_t text_count,  text_cap;
    CodeWidget    *codes;    size_t code_count,  code_cap;
    ImageWidget   *images;   size_t image_count, image_cap;
    ShapeWidget   *shapes;   size_t shape_count, shape_cap;

    WidgetBase   **widgets;  size_t widget_count;   /* sorted by z_order */

    Scene         *scenes;       size_t scene_count, scene_cap;
    Transition    *transitions;  size_t transition_count, transition_cap;
    struct Effect *effects;      size_t effect_count, effect_cap;
    AudioTrack    *audio;        size_t audio_count, audio_cap;

    Arena  frame_arena;
    void  *gpu;                  /* opaque RenderResources* */

    double range_start_sec, range_end_sec;   /* --range */
} EditorContext;
```

> **`widgets` holds pointers into the `texts`/`codes`/… arrays**, so it is
> invalidated by every `realloc`. It is therefore built only after all parsing
> is complete (`ctx_build_widget_index()`), and never before.

---

## Renderer-private structs

Defined inside `renderer.cu`; not visible to the rest of the program.

```c
struct FrameSlot {
    uchar4      *d_frame;     /* RGBA compositing target */
    uchar4      *d_fx;        /* effect ping-pong */
    uchar4      *d_scene[2];  /* two scenes during a transition */
    uint8_t     *d_nv12;      /* only this is copied to the host */
    uint8_t     *h_frame;     /* pinned staging */
    cudaStream_t stream;
    cudaEvent_t  done;
};

struct RenderResources {
    FrameSlot slot[VR_PIPELINE_DEPTH];
    size_t    frame_bytes;    /* RGBA size in VRAM */
    size_t    nv12_bytes;     /* w*h*3/2 — what actually crosses PCIe */
    int       width, height;
    size_t    texture_bytes;
    int       texture_count;
};
```

`d_fx` is allocated only if any effect exists; `d_scene[]` only if there is more
than one scene.
