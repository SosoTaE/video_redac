# Algorithms

The non-obvious parts, and why they are the way they are. Several of these
sections describe bugs that actually occurred — those are the parts worth
reading before changing the code.

---

## Text rasterization

`raster_styled_lines()` runs in four stages.

**1 — Measure on a 1×1 probe surface.** A one-pixel Cairo surface is created
purely to obtain a `cairo_t` and query font extents. For every line, logical
width is

```
max(x_advance, width + x_bearing)
```

because some glyphs (italic *f*) extend past their advance. The minimum
`x_bearing` across lines is tracked too — it can be negative, meaning ink
sticking out to the left.

**2 — Size the texture.**

```
line_height = font_extents.height × line_spacing
tex_w = ceil(max_width − min_x_bearing) + 2·padding
tex_h = ceil(fe.height + line_height·(n−1)) + 2·padding
```

Padding is tied to font size (`size/4 + 4` for text widgets) so large glyphs
don't get their antialiased edges clipped.

**3 — Draw.** Cairo places text on the **baseline**, hence `+ascent`:

```
x = padding − min_x_bearing        (+ alignment offset)
y = padding + fe.ascent + line_height·i
```

Alignment shifts each line by `(max_width − line_advance) × align`.

**4 — Repack.** `surface_to_rgba` converts Cairo's aligned ARGB32 into a packed
`width*4` RGBA buffer, reading `uint32_t` and shifting so the code is
endian-independent.

### Word wrapping

Greedy: append words while they fit, then break. A single word wider than the
limit is left whole — breaking mid-word damages URLs and code more often than it
helps.

Wrapping runs **before** tokenizing, and only for text widgets. Code is never
wrapped; reflowing source would be worse than a horizontal overflow.

---

## Glyph metrics and the per-glyph typewriter

The naive typewriter clips a fraction of the texture's width. For monospaced
code that looks almost right, but for proportional text it cuts mid-glyph.

Instead, rasterization records where every character ends.

**Measuring.** `measure_char_advances` measures **prefixes**, not individual
characters:

```c
for each codepoint boundary b:
    save = scratch[b]; scratch[b] = '\0';
    cairo_text_extents(cr, scratch, &te);
    scratch[b] = save;
    out_x[k] = base_x + te.x_advance;
```

Summing per-character advances would ignore kerning — "AV" is narrower than
"A" + "V". This is O(n²) shaping, but it happens once, and lines are short.

Boundaries are found with `utf8_is_lead()` (`(b & 0xC0) != 0x80`), so multi-byte
characters count as one.

**Per frame,** `compute_reveal_cutoffs` converts a 0..1 progress into one x
threshold per line:

```c
budget = floor(reveal · total_chars)
for each line l:
    n = chars_in_line(l)
    if      (budget <= 0) out[l] = -1e30    /* line not started */
    else if (budget >= n) out[l] = +1e30    /* line complete */
    else                  out[l] = char_x[line_start[l] + budget]
    budget -= n
```

That array is uploaded to VRAM and the compositing kernel discards any pixel
past its line's threshold.

**Why the code panel is a separate texture.** If the panel were the glyph
texture's background, per-line clipping would carve a ragged edge out of it.
Split into two layers, the panel is composited unclipped and only the glyphs are
cut.

**Buffer slots.** `h_cutoff`/`d_cutoff` hold `VR_PIPELINE_DEPTH` sets. With two
frames in flight, frame *i* writing slot *i mod 2* cannot disturb frame *i−1*.
The safety argument is the same as for frame buffers: slot *s* was last used by
frame *i−2*, whose event was awaited on the previous iteration.

---

## Syntax highlighting

Line-by-line scanner with carried state, in this priority order:

1. continuation of a block comment or triple-quoted string from the previous line;
2. a `#` preprocessor line (C family), consuming the whole line;
3. whitespace runs;
4. line comment → rest of line;
5. block comment start — closes on this line, or sets `mode = 1`;
6. triple-quoted string (Python) — likewise `mode = 2`;
7. quoted string or char literal, honouring `\` escapes;
8. Go backtick raw string (no escapes);
9. number — digits, or `.` followed by a digit; consumes identifier characters
   and `.`, with special handling for `e+`/`e-`;
10. identifier → keyword table → type table → constant table → else the
    `(`-lookahead heuristic marks it a function call;
11. operator run from `+-*/%=<>!&|^~?:`;
12. punctuation from `(){}[];,.`;
13. anything else, one byte, as `TOK_TEXT`.

Tokens cover every byte, so rendering is a straight walk. Bytes ≥ 0x80 count as
identifier characters, which keeps UTF-8 words intact.

Theme: Catppuccin Mocha, chosen for contrast on a dark panel.

---

## Layout resolution

Two phases, because a position can depend on the object's own size, which only
exists after rasterization.

**Parse time** — a string `x`/`y` is stored verbatim in `x_expr`/`y_expr`; a
number goes to `base->x`; an array becomes a track.

**After rasterization** — `media_prepare_textures` evaluates:

```c
layout_eval(expr, canvas, axis, &pos, &default_anchor);
b->x = pos;
if (!b->has_anchor_x) b->anchor_x = default_anchor;
b->anchor_off_x = b->anchor_x * b->base_w;
```

**At render time** the offset is subtracted, and — importantly — it applies to
tracks as well as static values:

```c
rt.x = (has_track_x ? track_sample(&tr_x, t) : b->x) - b->anchor_off_x;
```

so `x` in a keyframe animation also means "the anchor point".

> **Consequence.** After layout, `base->x` is the anchor point, not the top-left
> corner. Anything reporting real bounds must subtract `anchor_off_*` —
> forgetting that made `--check` report right-anchored objects as off-canvas.

### The string-expression versus track bug

`has_track_x` was originally set from `json_has("x")`. With a string expression
that made the renderer read `tr_x`, whose constant was 0 (a string yields the
numeric fallback), so every object collapsed into the top-left corner.

The revealing detail was that `--dump` printed **correct** positions — it reads
`b->x` directly — while only the render was wrong. The fix:

```c
base->has_track_x = json_has(obj, "x") && base->x_expr == NULL;
```

---

## The constant-property trap

`parse_track()` returns true only for arrays. Setting

```c
base->has_track_opacity = parse_track(...);   /* wrong */
```

meant a constant `"opacity": 0.0` was discarded and the object fell back to the
default of 1.0 — an attempt to hide something made it fully visible instead.

`has_track_*` therefore means **"the JSON specified this property"**, not "it is
animated":

```c
base->has_track_opacity = json_has(obj, "opacity");
```

The same applied to `scale` and `rotation`, which have no separate static field.

---

## Cover fitting and Ken Burns

`height = canvas_height` with `x` omitted reproduces CSS `object-fit: cover`:
the width is derived from the aspect ratio (typically wider than the canvas) and
auto-centring crops it symmetrically.

Ken Burns is a `scale` track. Because scale pivots about the **base centre**,
a pure scale animation is a centred push; adding `x`/`y` tracks turns it into a
pan.

### Scale pivot

The centre is computed from `base_w`/`base_h`, **not** from the scaled size:

```c
cx = rt->x + b->base_w * 0.5f;
```

Using the scaled size would anchor growth at the top-left, and a centred title
would visibly drift right as it grew. This was a real bug.

---

## Transition presets

All formulas take progress `p ∈ [0,1]`; translations are canvas fractions.
`from` is drawn first, `to` on top, which is why most presets only move `to`.

| Preset | from | to |
|---|---|---|
| `crossfade` | — | `opacity = p` |
| `fade` | `opacity = 1 − min(1, 2p)` | `opacity = max(0, 2p − 1)` |
| `slide_left` | — | `x = 1 − p` |
| `slide_right` | — | `x = −(1 − p)` |
| `slide_up` / `down` | — | `y = ±(1 − p)` |
| `push_left` | `x = −p` | `x = 1 − p` |
| `push_right/up/down` | mirrored | mirrored |
| `zoom_in` | — | `opacity = p`, `scale = 0.7 + 0.3p` |
| `zoom_out` | `opacity = 1 − p`, `scale = 1 + 0.35p` | `opacity = p` |
| `spin` | — | `opacity = p`, `scale = 0.2 + 0.8p`, `rotate = 180(1 − p)` |
| `wipe_right` | — | rect mask `x=0, w=p` |
| `wipe_left` | — | rect mask `x=1−p, w=p` |
| `iris` | — | circle mask `r = 0.78p` |

The iris radius reaches 0.78 because covering the corners needs
`√(0.5² + 0.5²) ≈ 0.71` in normalised units.

`from`/`to` blocks in JSON, and `fromMask`/`toMask`, override these.

---

## The colour-tagging bug

After moving RGB→YUV onto the GPU, solid colour patches came back wrong by up to
**15/255**.

The BT.709 matrix was verified correct against the specification, so the fault
was elsewhere. ffmpeg's verbose log gave it away:

```
[swscaler] YUV color matrix differs for YUV->YUV, using intermediate RGB to convert
```

Raw input carries no colour tags, so ffmpeg assumed a different matrix and
silently round-tripped every frame through RGB — corrupting colour *and* burning
CPU. The fix is to tag the **input** as well as the output, and to hand NVENC
its native `nv12` so no conversion is inserted at all:

```
-colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv -i -
-c:v h264_nvenc … -pix_fmt nv12 -colorspace bt709 …
```

Error dropped to ≤2/255, which is ordinary H.264 quantization.

---

## Verifying `--range`

Rendering a sub-range must produce exactly the frames the full render would.
The first comparison showed `max Δ = 56`, and a control of two full renders gave
`Δ = 0`, which seemed to prove a real bug.

**The control was wrong.** It compared two *identical* encodes, which proves
determinism but not that a 240-frame and a 1440-frame encode compress the same
frame identically. Frame 0 of the preview is an IDR; the same frame in the full
render is a P-frame.

Comparing uncompressed output instead (`VIDEO_REDAC_ENCODER=rawvideo`) gave
**max Δ = 0** — bit-identical. The renderer was correct; the measurement was not.

The lesson generalises: when comparing renders, compare before the encoder or
not at all.

---

## Scene selection during a transition

```c
size_t si = scene_count - 1;
for (i = 0; i < scene_count; i++)
    if (time_ms < scenes[i].start_ms + scenes[i].duration_ms) { si = i; break; }
```

"First unfinished", not "last started". During the overlap both neighbours are
active and `from` is the earlier one; the other rule selects the following pair
and no transition is ever drawn.

---

## Per-scene effects and the ping-pong

Scene effects apply **before** the transition, so a clip's look carries into it.
Global effects apply to the finished frame.

Because the ping-pong may leave the result in the scratch buffer,
`scene_effects()` copies it back device-to-device so callers can keep using the
same pointer:

```c
uchar4 *out = apply_effect_list(scene->effects, …, buffer, …);
if (out != buffer) cudaMemcpyAsync(buffer, out, …, cudaMemcpyDeviceToDevice, stream);
```

One extra 8 MB copy, and only when a scene actually has effects.

> In flat mode the scene node **is** the root, so reading root `effects` as
> scene effects applied every effect twice. Guarded by an `is_root` flag; the
> byte-identity regression test is what caught it.

---

## Audio mixing

Video renders silently to a temporary file, then a second ffmpeg pass mixes
audio and muxes with `-c:v copy` — the video is never re-encoded.

Per track:

```
[N:a] aresample=48000
    → aformat=fltp:stereo
    → atrim=0:DUR
    → volume=V
    → afade=t=in:st=0:d=FI
    → afade=t=out:st=DUR-FO:d=FO
    → adelay=delays=START:all=1
```

then `amix=inputs=K:normalize=0` and `alimiter=limit=0.95`.

Order matters: trim, then gain, then fades, and only then position. Any other
order puts the fades in the wrong place.

Duration resolution:

```
explicit duration          → use it
loop and no duration       → to the end of the video
otherwise                  → ffprobe the source, minus the in-point
finally clamp to (video_duration − start)
```

If the mux fails the silent video is **kept** and its path printed — discarding
a long render because of an audio problem would be the wrong trade.

---

## Shell-command safety

Both ffmpeg invocations are built as strings and run through `popen`/`system`,
so every externally supplied value is single-quoted, with `'` escaped as `'\''`.

Verified: `-o "x'; touch pwned.txt; echo '.mp4"` creates a file with that literal
name and executes nothing.
