# Host-side modules

Every function outside the rendering backends. Kernels are in
[05-cuda-kernels.md](05-cuda-kernels.md), the backends themselves and the code
they share in [09-backends.md](09-backends.md).

Signatures are abbreviated; `static` marks a file-local helper.

---

## `arena.c` — bump allocator

```c
bool        arena_init(Arena*, size_t capacity);
void        arena_init_from_buffer(Arena*, void *buffer, size_t capacity);
void       *arena_alloc(Arena*, size_t size, size_t align);
void       *arena_alloc_zero(Arena*, size_t size, size_t align);
void        arena_reset(Arena*);
ArenaMarker arena_mark(const Arena*);
void        arena_release(Arena*, ArenaMarker);
void        arena_destroy(Arena*);

static size_t align_up(size_t n, size_t align);
static bool   is_power_of_two(size_t v);
```

`ARENA_NEW(arena, T, n)` wraps `arena_alloc_zero` with the right alignment.
`VR_ALIGNOF` picks `_Alignof` in C and `alignof` in C++, since `renderer.cu` is
compiled as C++.

**`arena_alloc` overflow handling.** Two checks, both necessary:

```c
size_t offset = align_up(a->used, align);
if (offset < a->used || offset > a->capacity) return NULL;  /* align_up wrapped */
if (size > a->capacity - offset)              return NULL;  /* offset+size would wrap */
```

Writing `offset + size > capacity` instead would itself overflow. The subtraction
form cannot.

`arena_release` refuses to *grow* `used`; a marker from the future indicates a
logic error, not a valid request.

---

## `anim.c` — easing and tracks

```c
EaseType    easing_from_name(const char *name);
const char *easing_name(EaseType);
float       easing_apply(EaseType, float p);

void  track_set_constant(Track*, float value);
bool  track_is_animated(const Track*);
float track_sample(const Track*, float t);
void  track_free(Track*);

static bool  loose_equal(const char *a, const char *b);
static float clamp01(float);
static float bounce_out(float p);
```

`loose_equal` ignores case, `_`, `-` and spaces, so `easeInOut`, `ease_in_out`
and `easeinout` are the same name. The JSON author should not have to remember
our spelling.

### `easing_apply`

`p` is clamped to [0,1]; the **output is not**. `backin/out/inout` and
`elasticout` deliberately leave the range — that overshoot is what makes motion
read as alive. Formulas: quadratic, cubic, `2^(10p-10)` for expo, the standard
`c1 = 1.70158` back constant, `2^(-10p)·sin((10p-0.75)·2π/3)+1` for elastic, and
the four-segment geometric `bounce_out`.

### `track_sample`

```c
if (!keys)                 return constant;
if (count == 1 || t <= keys[0].t)          return keys[0].v;
if (t >= keys[count-1].t)                  return keys[count-1].v;
find i with keys[i].t <= t < keys[i+1].t;
p = easing_apply(keys[i+1].ease, (t - keys[i].t) / (keys[i+1].t - keys[i].t));
return lerp(keys[i].v, keys[i+1].v, p);
```

Linear search: tracks hold a handful of keys, so a binary search would only add
code. Coincident keys (`span <= 0`) return the second value rather than dividing
by zero.

---

## `layout.c` — position expressions

```c
bool layout_anchor_from_name(const char *name, float *ax, float *ay);
bool layout_eval(const char *expr, float canvas, LayoutAxis axis,
                 float *out_pos, float *out_anchor);
static bool read_term(const char **p, float canvas, LayoutAxis,
                      float *value, float *anchor, bool *has_anchor);
```

A deliberately small parser — expressions are short (`bottom-160`, `50%+40`) and
a full expression language would only add failure modes.

`read_term` handles one term: a keyword (`left`/`center`/`right`, or
`top`/`middle`/`bottom`) or a number with an optional `%`. The **first** keyword
seen also sets the default anchor, which is what makes `"bottom-160"` mean "160
px above the bottom edge" rather than "top edge at height−160".

Resolution happens in `media_prepare_textures()`, because it needs the object's
size, which only exists after rasterization.

---

## `effects.c` — effect names and lifetime

```c
EffectType  effect_from_name(const char *name);
const char *effect_name(EffectType);
bool        effect_needs_neighbors(EffectType);
void        effect_free(Effect*);
```

Only the host-side bookkeeping; the pixel maths is in `include/pixel_ops.h`.
`effect_needs_neighbors` is true for `bloom`, `blur`, `pixelate`, `rgb_split`
and `glitch` — the effects that read pixels other than their own and therefore
cannot run in place.

`effect_free` also releases the colour cube a `lut` effect owns and the tracks
its power window uses. It deliberately does **not** free `d_lut`: that is device
memory the renderer allocated and frees with the rest of its own, and freeing it
here would be a double free on the CUDA backend.

---

## `lut.c` — Adobe/Iridas `.cube` files

```c
bool lut_load_cube(const char *path, float **out, int *size);
```

Reads a colour lookup table into a cube of `size³` RGB triples, red varying
fastest — the order the file itself stores.

**1D tables are expanded into a cube on load.** A 1D LUT means the three axes
are independent, so the cube is separable and the expansion is exact rather than
an approximation; the pixel loop then has one case instead of two.

What makes a reader non-trivial is that files in the wild are sloppy: Windows
line endings, tabs, comments after data, keywords this reader does not know, a
declared size that disagrees with the row count. Each is handled rather than
producing a plausible-looking but wrong image — **a LUT read with the wrong
stride still renders, in wrong colour**, which is why a size mismatch is a hard
error and not a warning.

A `DOMAIN_MIN`/`DOMAIN_MAX` other than 0..1 is reported and then used as-is:
such files expect log footage, which nothing here produces.

---

## `mesh.c` — primitives, OBJ, and the shared geometry helpers

```c
bool        mesh_load(MeshWidget*);
const char *mesh_shape_name(size_t i);
void        mesh_free(MeshWidget*);

/* src/mesh_internal.h — shared with the format readers */
bool vr_mesh_push_vert(MeshWidget*, size_t *cap, float x, float y, float z);
bool vr_mesh_push_tri(MeshWidget*, size_t *cap, int a, int b, int c);
bool vr_mesh_ensure_attribs(MeshWidget*, size_t cap);
void vr_mesh_set_attrib(MeshWidget*, float nx, float ny, float nz, float u, float v);
bool vr_mesh_ensure_tangents(MeshWidget*, size_t cap);
void vr_mesh_set_tangent(MeshWidget*, float x, float y, float z, float w);
```

`mesh_load` dispatches on the path's extension or, with no path, on `shape`
through a table shared with `--list shapes` so the two cannot disagree.

Everything is normalised into a unit cube about the origin afterwards. Without
that, `size` would mean something different for every file — an OBJ in
millimetres and one in metres differ by a thousand — and models built far from
the origin would rotate about a point outside themselves.

Two derivation passes run after normalisation, in that order because each is the
next one's input:

* `derive_normals` — area-weighted, so a sliver does not skew a surface.
* `derive_tangents` — **not** area-weighted, deliberately. See
  [06-algorithms.md](06-algorithms.md#tangent-frames-for-normal-mapping). Only
  built for a mesh that actually has a normal map.

---

## `gltf.c` — glTF 2.0

```c
bool vr_mesh_load_gltf(MeshWidget*, size_t *vc, size_t *tc);
```

Both `.gltf` (external or base64 buffers) and `.glb`. Everything reads through
`acc_read`, which turns "component c of element i of accessor a" into a float
regardless of how it was packed — reading buffers directly works for one
exporter's files and breaks on the next.

Geometry, the node hierarchy's transforms, and from the material: base colour,
occlusion, normal and emissive textures plus `normalTexture.scale` and
`emissiveFactor`. Animation, skinning and morph targets are out of scope and
reported rather than silently dropped.

**Two conventions are converted on the way in**, and both are invisible until
they bite — a half turn about X, and reversed winding. See
[03-json-reference.md](03-json-reference.md#file-formats--obj-and-gltf).

`TANGENT` is read only when *every* primitive supplies it; otherwise the partial
array is discarded and `mesh.c` derives the whole set from the UVs. One
consistent frame beats a marginally better one on half the object.

---

## `highlighter.c` — the lexer

```c
Language highlighter_language_from_name(const char *name);
size_t   highlighter_tokenize_line(const char *line, Language, HighlightState*,
                                   Token *out, size_t max);
Color    highlighter_class_color(TokenClass);

static LangSpec lang_spec(Language);
static bool     is_ident_start(char);
static bool     is_ident_char(char);
static bool     is_digit(char);
static bool     in_word_list(const char *const *list, const char *word, size_t len);
static void     emit(Emitter*, size_t start, size_t len, TokenClass);
static size_t   scan_string(const char *line, size_t i, size_t len,
                            char quote, bool allow_escape);
```

Not a parser and not trying to be one: "visually correct" colouring is all a
video needs, so this is a classic scanner over keyword tables.

`LangSpec` describes a language: keyword/type/constant tables, the line-comment
marker, and flags for block comments, preprocessor lines, triple-quoted strings
and Go's backtick raw strings.

**State across lines.** `HighlightState.mode` is 0 (normal), 1 (inside a block
comment) or 2 (inside a triple-quoted string), which is how multi-line
constructs survive line-by-line tokenizing without building a whole-file tree.

`emit()` has two useful behaviours:

- adjacent tokens of the same class are **merged**, reducing Cairo calls;
- if the caller's buffer runs out, the last token is **extended** instead of
  dropping text. Colouring degrades; text never disappears.

Details and the full grammar: [06-algorithms.md](06-algorithms.md#syntax-highlighting).

---

## `media_loader.c` — Cairo rasterization

### Public

```c
bool media_render_text_rgba(const char *utf8, const char *font, int size,
                            Color, float line_spacing, int padding,
                            float max_width, float align,
                            Texture *out, GlyphMetrics *metrics);
bool media_render_text_widget(TextWidget*);
bool media_render_code_widget(CodeWidget*);      /* produces glyphs + plate */
bool media_render_shape_widget(ShapeWidget*);
bool media_load_image_rgba(const char *path, Texture *out);
bool media_prepare_textures(EditorContext*);
void texture_free(Texture*);
void glyph_metrics_free(GlyphMetrics*);
void media_shutdown(void);
```

### Internal

```c
static bool    texture_alloc(Texture*, int w, int h);       /* cap 16384 */
static bool    utf8_is_lead(unsigned char);
static size_t  utf8_count(const char*, size_t);
static int     ascii_casecmp(const char*, const char*);
static void    split_font_spec(const char *spec, char *family, size_t,
                               cairo_font_weight_t*, cairo_font_slant_t*);
static size_t  count_lines(const char*);
static char  **split_lines(const char*, size_t *out_count);   /* expands tabs */
static void    free_lines(char**, size_t);
static bool    surface_to_rgba(cairo_surface_t*, Texture*);
static void    measure_char_advances(cairo_t*, char *scratch, const char *s,
                                     size_t len, double base_x,
                                     float *out_x, size_t *out_count);
static char  **wrap_lines(char**, size_t*, const char *family, int size,
                          cairo_font_weight_t, cairo_font_slant_t, float max_w);
static bool    raster_styled_lines(StyledLine*, size_t, const char *font, int size,
                                   Color, float spacing, int padding, float align,
                                   Texture*, GlyphMetrics*);
static void    rounded_rect_path(cairo_t*, double x, double y, double w, double h, double r);
static bool    raster_plate(int w, int h, Color, int radius, Texture*);
```

`raster_styled_lines` is the core: one function serves both plain text and
highlighted code, the only difference being whether a line carries tokens.

`surface_to_rgba` reads Cairo's ARGB32 as a `uint32_t` and shifts out the
channels rather than indexing bytes — that is endian-safe — and repacks the
aligned Cairo stride into a packed `width*4` buffer.

`media_shutdown` calls `cairo_debug_reset_static_data()`. Cairo and fontconfig
keep process-lifetime font caches which LeakSanitizer counts as leaks; clearing
them keeps the report clean so *real* leaks stay visible.

### `media_prepare_textures`

The cache-filling pass, and the only place that knows both the object's size and
the canvas:

1. rasterize by kind (text / code / image / shape);
2. set `base_w`/`base_h` — for images honouring `width`/`height` and preserving
   aspect when only one is given;
3. resolve `auto_center_x`;
4. evaluate `x_expr`/`y_expr` through `layout_eval`;
5. compute `anchor_off_x/y = anchor × size`.

---

## `parser.c` — JSON to `EditorContext`

The largest module. Three principles: it only *reads*; every allocation is
recorded so one `editor_context_free()` cleans up; and a missing field is a
default, not an error.

### Public

```c
EditorContext *parse_video_project(const char *filepath);
EditorContext *parse_video_project_ex(const char *filepath,
                                      char **defines, int define_count);
void  editor_context_free(EditorContext*);
void  editor_context_dump(const EditorContext*);
int   editor_context_check(const EditorContext*);   /* returns problem count */
char *read_file_to_string(const char *filename, size_t *out_len);
bool  parse_hex_color(const char *hex, Color *out);
```

### JSON helpers

```c
static int         json_int(const cJSON*, const char *key, int fallback);
static float       json_float(const cJSON*, const char *key, float fallback);
static const char *json_str(const cJSON*, const char *key, const char *fallback);
static bool        json_has(const cJSON*, const char *key);
static Color       json_color(const cJSON*, const char *key, Color fallback);
static float       json_length(const cJSON*, const char *key, float canvas, float fallback);
static float       json_align(const cJSON*, const char *key, float fallback);
```

All are `NULL`-safe because cJSON's lookup is. `json_length` accepts a number or
an expression string, so `max_width` can be `"80%"`.

### Templating

```c
static char *expand_vars(const char *src, const cJSON *vars);
static void  substitute_vars(cJSON *node, const cJSON *vars);
static void  apply_style_chain(cJSON *obj, const cJSON *styles, const char *name, int depth);
static void  apply_styles(cJSON *obj, const cJSON *styles);
```

`substitute_vars` walks the parsed tree and rewrites string values in place —
never the raw text. See [03-json-reference.md](03-json-reference.md#vars-and-substitution).

### Object parsing

```c
static void *array_push(void **items, size_t *count, size_t *cap, size_t elem_size);
static bool  parse_track(const cJSON*, Track*, float fallback);
static void  parse_widget_base(WidgetBase*, const cJSON*, WidgetKind, int z);
static bool  parse_text_object (EditorContext*, const cJSON*, int z);
static bool  parse_code_object (EditorContext*, const cJSON*, int z);
static bool  parse_image_object(EditorContext*, const cJSON*, int z, const char *base_file);
static bool  parse_shape_object(EditorContext*, const cJSON*, int z, WidgetKind);
static char *resolve_relative_path(const char *base_file, const char *path);
```

`array_push` is one generic geometric-growth routine (`void**`) shared by all
dynamic arrays. It checks `new_cap > SIZE_MAX / elem_size` before multiplying,
and on `realloc` failure leaves the old array intact.

`parse_track` returns true **only for arrays**. `parse_widget_base` therefore
sets `has_track_*` from `json_has()` instead — see
[06-algorithms.md](06-algorithms.md#the-constant-property-trap).

### Scenes, transitions, effects, audio

```c
static bool           parse_scene(EditorContext*, const cJSON *node, const cJSON *styles,
                                  const char *filepath, int *z, bool is_root);
static ActionType     action_from_string(const char*);
static bool           parse_timeline_event(Scene*, const cJSON*);
static TransitionType transition_from_name(const char*);
static bool           parse_transition_side(const cJSON *side, Track *op, Track *x,
                                            Track *y, Track *sc, Track *rot,
                                            float d_op, float d_sc);
static int            parse_mask(const cJSON*, Track slots[4]);   /* 0|1|2 */
static bool           parse_transitions(EditorContext*, const cJSON*);
static void           effect_set_defaults(Effect*);
static bool           parse_effects_into(struct Effect **list, size_t *count,
                                         size_t *cap, const cJSON*);
static bool           parse_audio(EditorContext*, const cJSON*, const char *base_file);
```

`parse_effects_into` takes an arbitrary list so the same code serves both the
root stack and per-scene stacks.

`parse_audio` opens each file to verify it exists. Audio is the last stage of
rendering, so a typo would otherwise surface only after thousands of frames.

### Index and timing

```c
static int  compare_by_z(const void*, const void*);
static bool ctx_build_widget_index(EditorContext*);
static void resolve_timeline_targets(EditorContext*);
static void compute_scene_times(EditorContext*);
```

`ctx_build_widget_index` must run **after** all parsing: it stores pointers into
the widget arrays, which `realloc` invalidates.

`resolve_timeline_targets` searches within each scene and stores a scene-local
index, which is what allows the same `id` in different scenes.

`compute_scene_times` accumulates start times and clamps any transition longer
than its neighbouring scenes.

### Diagnostics

```c
static float       widget_left(const WidgetBase*);   /* x - anchor_off_x */
static float       widget_top (const WidgetBase*);
static const char *kind_name(WidgetKind);
static const char *action_name(ActionType);
```

`widget_left/top` exist because after layout `base->x` is the **anchor point**;
reporting it directly made right-anchored objects look off-canvas.

`editor_context_check` reports: zero duration, no objects, duplicate ids
**within a scene**, zero-size objects, fully off-canvas objects, clipped *text*
(clipping is normal for cover images, so those are notes), un-rasterized
textures, unknown actions, unresolved targets, events past the scene end, and
audio starting past the video end. Returns the count; `main` maps non-zero to
exit status 1.

---

## `audio.c` — the second pass

```c
bool  vr_shell_quote(const char *in, char *out, size_t out_size);
char *audio_make_silent_path(const char *output_file);
bool  audio_mux(const EditorContext*, const char *silent_video, const char *output_file);

static void  sb_free(StrBuf*);
static void  sb_addf(StrBuf*, const char *fmt, ...);
static float probe_duration(const char *path);
```

`vr_shell_quote` lives here but is shared with the renderer: both build ffmpeg
commands, and one implementation means injection safety is reviewed in one
place. It single-quotes and escapes `'` as `'\''`.

`probe_duration` shells out to `ffprobe`. It is needed only to place `fade_out`,
whose start is measured from the clip's end.

`audio_mux` builds a `filter_complex` per track:

```
[N:a] aresample=48000 → aformat → atrim → volume → afade(in) → afade(out) → adelay
```

then sums with `amix=normalize=0` and a limiter.

The chain order is a mixing desk's: **trim → volume → eq → compressor → pan →
fades → timeline delay**. A compressor has to see the level it will actually act
on, so it sits after the fader and the EQ; the fades and the delay come last
because they are about *when*, not what.

`duck` is the exception and cannot be a link in that chain — it needs the *other*
track's signal — so the graph is built in three parts: the chains, then an
`asplit` of each key into one copy per ducker plus one for the mix, then the
`sidechaincompress` stages.

---

## `main.c`

```c
static void print_usage(const char *prog);
static bool parse_range(const char *spec, double *start, double *end);
int  main(int argc, char **argv);
```

Orchestration only: parse arguments → `parse_video_project_ex` →
`media_prepare_textures` → optional `--dump` / `--check` → `render_video` →
`editor_context_free` → `media_shutdown`.

`ctx` is created and destroyed here; every other module receives it by pointer.
