# JSON reference

Every key the parser understands. **A missing key is never an error** — each has
a default. Unknown keys are ignored, which is why `"_comment"` and `"_note"`
work as comments.

## Top level

```json
{
  "vars":        { },
  "styles":      { },
  "project":     { },
  "output":      { },
  "objects":     [ ],   // flat mode
  "timeline":    [ ],   // flat mode
  "scenes":      [ ],   // scene mode (replaces objects/timeline)
  "transitions": [ ],
  "effects":     [ ],
  "audio":       [ ]
}
```

`objects`/`timeline` and `scenes` are mutually exclusive: if `scenes` is
present, it wins.

---

## `project`

| Key | Type | Default | Notes |
|---|---|---|---|
| `width` | int | 1920 | 16–16384; rounded **down to even** (4:2:0 requires it) |
| `height` | int | 1080 | same |
| `fps` | int | 60 | 1–480 |
| `bg_color` | colour | `#000000FF` | shows through slides and zooms |
| `duration_ms` | int | 0 | 0 → derived. In scene mode it is *computed* from the scenes |

Invalid resolution or fps is a hard error — a corrupt config would otherwise ask
for absurd amounts of VRAM.

## `output`

| Key | Type | Default |
|---|---|---|
| `encoder` | string | `h264_nvenc` |
| `preset` | string | `p5` |
| `cq` | int | 21 (clamped 0–51) |
| `bitrate` | string | *(none)* — constant-quality mode |

`VIDEO_REDAC_ENCODER` overrides `encoder` for quick experiments.

## `vars` and `${substitution}`

```json
"vars": { "title": "Hello", "accent": "#F9E2AF" }
```

`${name}` is replaced in **every string value** anywhere in the document —
including colours and paths. `--set KEY=VALUE` overrides the block.

Substitution runs on the **parsed tree**, not the raw text. That is deliberate:
a quote or backslash inside a value would break the document if substituted
textually; on the tree it can only ever stay inside one string node.

Undefined variables expand to `""` with a warning.

## `styles`

```json
"styles": {
  "base": { "font": "DejaVu Sans-Bold", "color": "#FFFFFF" },
  "head": { "style": "base", "size": 62, "x": "center" }
}
```

Any object may reference one (`"style": "head"`) or several
(`"style": ["base", "head"]`). Rules:

- the object's **own fields always win**; a style only fills gaps;
- a style may itself have `"style"`, and the parent is applied **after**, so the
  more specific style wins;
- cycles stop at depth 8 with a warning;
- a style may carry `type`, positions and even animation tracks — not just
  typography.

---

## `scenes[]`

| Key | Type | Default |
|---|---|---|
| `id` | string | *(none)* — used in diagnostics |
| `duration` | float (s) | derived from the timeline, else 3.0 |
| `duration_ms` | int | takes precedence over `duration` |
| `objects` / `layers` | array | — |
| `timeline` | array | — |
| `bg_color` | colour | inherits `project.bg_color` |
| `effects` | array | applied **before** the transition |

Time inside a scene is local: `t = 0` is the scene's start.

## `transitions[]`

Entry `i` plays between scene `i` and `i+1`.

```json
{ "use": "slide_left", "duration": 0.6 }
```

| Key | Type | Default |
|---|---|---|
| `use` / `type` | string | `cut` |
| `duration` | float (s) | 0.6 |
| `duration_ms` | int | overrides `duration` |
| `from`, `to` | object | per-channel tracks; override the preset |
| `fromMask`, `toMask` | object | shape mask |

**Presets:** `cut`, `crossfade`, `fade`, `slide_left/right/up/down`,
`push_left/right/up/down`, `zoom_in`, `zoom_out`, `spin`, `wipe_left`,
`wipe_right`, `iris`.

A transition longer than either neighbouring scene is clamped, with a warning.

### Custom transitions

`from`/`to` channels, sampled over progress **p ∈ [0,1]** — not seconds:

| Channel | Units |
|---|---|
| `opacity` | 0..1 |
| `x`, `y` | fraction of the canvas |
| `scale` | multiplier |
| `rotate` | degrees |

```json
{ "duration": 0.8,
  "from": { "opacity": [{"t":0,"v":1},{"t":1,"v":0}] },
  "to":   { "x":      [{"t":0,"v":0.6},{"t":1,"v":0,"ease":"backout"}],
            "rotate": [{"t":0,"v":22}, {"t":1,"v":0,"ease":"backout"}] } }
```

### Masks

```json
"toMask": { "shape": "circle", "cx": 0.5, "cy": 0.5,
            "r": [{"t":0,"v":0},{"t":1,"v":0.8,"ease":"easeout"}] }

"toMask": { "shape": "rect", "x": 0, "y": 0, "h": 1,
            "w": [{"t":0,"v":0},{"t":1,"v":1}] }
```

All parameters are canvas fractions and may be tracks. A mask given in JSON
overrides the preset's own mask.

---

## `objects[]`

### Common to every object

| Key | Type | Default | Notes |
|---|---|---|---|
| `id` | string | `"unnamed"` | timeline target; **scene-local** |
| `type` | string | `"text"` | `text` `code` `image` `rect` `circle` |
| `style` | string / array | — | see above |
| `x`, `y` | number \| string \| track | 0 | number = pixels, string = expression, array = keyframes |
| `x_pos`, `y_pos` | number | — | accepted synonyms |
| `anchor` | string | `topleft` | 9-point anchor |
| `opacity` | number \| track | 1 | |
| `scale` | number \| track | 1 | about the base centre |
| `rotation` | number \| track | 0 | degrees, about the base centre |

Omitting `x` entirely centres the object horizontally.

### Position expressions

```
expr  := term (('+'|'-') term)*
term  := number | number'%' | keyword
keyword (x): left | center | middle | right
keyword (y): top  | center | middle | bottom
```

Examples: `"center"`, `"bottom-160"`, `"50%+40"`, `"right-24"`.

A keyword also sets the **default anchor** — `"bottom-160"` naturally means "160
px above the bottom edge", measuring the object's bottom. An explicit `anchor`
overrides that.

`anchor` accepts: `topleft` `top` `topright` `left` `center` `right`
`bottomleft` `bottom` `bottomright` `middle`. Hyphens, underscores and spaces
are ignored, so `bottom-right` works.

### `type: "text"`

| Key | Type | Default |
|---|---|---|
| `content` | string | `""` — `\n` starts a new line |
| `font` | string | `"Sans"` — e.g. `"FiraCode-Bold"` |
| `size` | int | 48 |
| `color` | colour | white |
| `line_spacing` | float | 1.25 |
| `max_width` | number \| `"80%"` | 0 = no wrapping |
| `align` | string | `left` \| `center` \| `right` |

A `-Bold`, `-Italic`, `-BoldItalic` or `-Regular` suffix on the font name is
split off into Cairo's weight/slant. An unrecognised suffix is treated as part
of the family, so `"Noto-Sans"` still works.

### `type: "code"`

| Key | Type | Default |
|---|---|---|
| `code` / `content` | string | `""` |
| `language` | string | `"text"` — `c` `cpp` `go` `rust` `python` … |
| `highlight` | bool | `true` |
| `font` | string | `"monospace"` |
| `size` | int | 32 |
| `color` | colour | `#E6E6E6` — base colour when highlighting is off |
| `bg_color` | colour | `#181825DC` — the panel; alpha 0 disables it |
| `padding` | int | 24 |
| `corner_radius` | int | 12 |
| `line_spacing` | float | 1.35 |

Tabs expand to four spaces. The panel is a **separate layer**, so the typewriter
clips only the glyphs.

### `type: "image"`

| Key | Type | Default |
|---|---|---|
| `path` / `src` | string | **required** |
| `width` | int | 0 → native |
| `height` | int | 0 → native |

Relative paths resolve against the **JSON file's directory**, not the working
directory. Giving only one axis preserves aspect ratio — that plus omitting `x`
produces `object-fit: cover`:

```json
{ "type": "image", "path": "photo.jpg", "height": 1920, "y": 0 }
```

### `type: "rect"` / `"circle"`

| Key | Type | Default |
|---|---|---|
| `color` | colour | white |
| `w`, `h` / `width`, `height` | number | 100 |
| `radius` | number | 50 — circle only; sets `w`/`h` |
| `cx`, `cy` | number | circle only — centre, converted to top-left |
| `corner_radius` | int | 0 — rect only |

---

## `timeline[]`

| Key | Type | Default |
|---|---|---|
| `time_ms` | int | 0 |
| `duration_ms` | int | 0 → instant |
| `action` | string | — |
| `target` | string | — |
| `value` | float | 0 (1.0 for `scale`) |
| `value_x`, `value_y` | float | 0 |
| `ease` | string | `smooth` |

**Actions:** `fade_in`, `fade_out`, `move`, `move_x`, `move_y`, `typewrite`,
`scale`, `zoom`, `rotate`, `highlight` *(reserved)*.

`move_x`/`move_y` use `value` on that axis; plain `move` uses `value_x`/`value_y`.

**Easing names:** `linear`, `easein`, `easeout`, `easeinout`, `cubicin`,
`cubicout`, `cubicinout`, `expoin`, `expoout`, `expoinout`, `backin`, `backout`,
`backinout`, `elasticout`, `bounceout`, `smooth`. Case, `_` and `-` are ignored.

### How events combine

- `move` deltas **accumulate**;
- `fade`, `scale`, `rotate` **overwrite** — the last matching event in JSON order
  wins;
- an object with any `fade_in` starts invisible; otherwise it is visible from
  `t = 0`;
- an object with any `typewrite` starts with nothing revealed.

---

## Tracks

Any numeric property may be a keyframe array instead of a number:

```json
"opacity": [ { "t": 0, "v": 0 }, { "t": 1.2, "v": 1, "ease": "backout" } ]
```

Sampling rules:

- before the first key → the first value;
- after the last key → the last value;
- between keys → interpolate using the **second** key's `ease`.

Keys are sorted by `t` at parse time, so out-of-order input is tolerated.

---

## `effects[]`

Run in array order, after compositing. Every numeric parameter is a track.

```json
{ "type": "vignette", "amount": 0.35, "radius": 0.6, "softness": 0.55 }
```

| Effect | Parameters | Defaults |
|---|---|---|
| `grayscale` | `amount` | 1 |
| `invert` | `amount` | 1 |
| `sepia` | `amount` | 1 |
| `posterize` | `levels` | 6 |
| `threshold` | `level` | 0.5 |
| `vignette` | `amount`, `radius`, `softness`, `color` | 0.35, 0.6, 0.55, black |
| `grain` | `amount` | 0.04 |
| `scanlines` | `amount`, `count` | 0.35, 240 |
| `vibrance` | `amount` | 0.3 |
| `split_tone` | `shadows`, `highlights`, `balance`, `amount` | blue/orange, 0, 0.25 |
| `gradient_map` | `shadow`, `highlight`, `amount` | black/white, 1 |
| `color_grade` | `exposure`, `brightness`, `contrast`, `gamma`, `saturation`, `vibrance`, `hue`, `temperature`, `tint` | 0,0,1,1,1,0,0,0,0 |
| `blur` | `radius` | 8 (capped at 128) |
| `pixelate` | `size` | 8 |
| `rgb_split` | `amount`, `angle` | 3, 0 |
| `glitch` | `amount` | 0.05 |

Aliases: `greyscale`, `noise` → `grain`, `grade` → `color_grade`, `chromatic` →
`rgb_split`, `duotone` → `gradient_map`.

---

## `audio[]`

```json
{ "path": "music.mp3", "volume": 0.35, "loop": true,
  "fade_in": 1.0, "fade_out": 2.0 }
```

| Key | Type | Default |
|---|---|---|
| `path` / `file` | string | **required** |
| `start` | float (s) | 0 — position on the video timeline |
| `in` | float (s) | 0 — in-point within the source |
| `duration` | float (s) | 0 → to source end |
| `volume` | float | 1.0 |
| `fade_in`, `fade_out` | float (s) | 0 |
| `loop` | bool | false |

Paths resolve against the JSON's directory. **Existence is checked at parse
time**, so a typo fails in the first second instead of after rendering
thousands of frames.

There is no text-to-speech; a track with `"text"` is refused with an explanatory
message.
