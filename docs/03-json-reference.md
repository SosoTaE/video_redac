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
  "transition":  { },   // the default transition for every gap
  "transitions": [ ],   // per-gap overrides
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

## `transition` and `transitions[]`

Entry `i` of `transitions` plays between scene `i` and `i+1`.

```json
{ "use": "slide_left", "duration": 0.6 }
```

**`transition`** (singular) sets the default for *every* gap, so a film whose
clips all share one transition needs it written once rather than once per gap:

```json
"transition": { "use": "crossfade", "duration": 0.7 }
```

The three forms combine, most specific winning:

| Written | Effect |
|---|---|
| `"transition": {…}` alone | every gap uses it |
| `"transitions": [ {…}, … ]` | that gap, explicitly |
| `"transitions": [ …, null ]` | that gap falls back to `transition` |
| neither | every gap is a hard cut (the historical default) |

A `transitions` array shorter than the number of gaps is padded with the
default. Without a `transition` key, unlisted gaps stay cuts — so existing
projects are unaffected.

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

## `repeat` — one description, many objects

Any object may carry a `repeat` block. The parser expands it into `count`
siblings *before* anything else sees the array, so the renderer only ever
handles ordinary objects.

```json
{ "id": "dot", "type": "circle", "radius": 14, "x": "center", "y": "center",
  "repeat": { "count": 16, "layout": "radial", "radius": 240,
              "stagger_ms": 30, "color_cycle": ["#7EE787", "#79C0FF", "#D2A8FF"] } }
```

| Key | Type | Default |
|---|---|---|
| `count` | int | required; capped at 4096 |
| `layout` | `radial` \| `grid` \| `line` \| `stack` | `radial` |
| `radius`, `angle_start`, `sweep` | float | radial: 200, 0, 360 |
| `cols`, `spacing_x`, `spacing_y` | int/float | grid: 4, 100, 100 |
| `rotate_step` | float (deg) | 0 — per-copy rotation |
| `orient` | bool | `false` — turn each copy to face its own radial angle |
| `stagger_ms` | int | 0 — per-copy delay on timeline events |
| `color_cycle` | array of colours | cycled across the copies |

**Ids.** Copies are named `dot#0` … `dot#15`, so a single one can still be
targeted directly.

**The timeline expands too.** An event aimed at `dot` becomes one event per
copy, each delayed by `stagger_ms × i`. Without that a `repeat` would be useless
for anything animated — the author would have to name ids the expansion
invented. A staggered `scale` on a radial repeat is how a ring becomes a wave.

**Layouts produce displacements, not positions**, so they compose with the
template's own position: `"x": "center"` plus a radial repeat gives a ring
centred on the canvas, and the offset is applied after `"center"` resolves.

`stack` places every copy at the template's position — useful when the spread
comes from the timeline, or from `rotate_step`.

**Rotation pivots about each copy's own centre**, not the ring's. A fan of
spokes is therefore built from `stack` + `rotate_step` on a line whose midpoint
is the hub (a diameter), not from twelve half-length lines — those would fan
about their own midpoints, off to one side. See `anim/lines.json`.

---

## `camera`

A whole-scene transform, given per scene (or at the root in flat mode).

```json
"camera": {
  "zoom":  [ {"t":0,"v":1}, {"t":1.5,"v":1.9,"ease":"cubicinout"}, {"t":3,"v":1} ],
  "x":     [ {"t":0,"v":0}, {"t":1.5,"v":120} ],
  "shake": [ {"t":3,"v":0}, {"t":3.2,"v":14}, {"t":3.8,"v":0} ]
}
```

| Key | Type | Meaning |
|---|---|---|
| `zoom` | track | 1 is neutral |
| `x`, `y` | track | pan in pixels; the content moves the opposite way |
| `rotation` | track | roll, degrees |
| `shake` | track | amplitude in pixels; 0 is still |

Composed onto every object exactly as a group is, with the canvas centre as the
pivot, and applied after groups — so a camera move layers on top of a scene's
own hierarchy instead of being overwritten by it.

**Shake is a hash of the timestamp, not a random number.** A frame has to stay
a pure function of time, or `--range` and the two backends would each produce a
different judder.

---

## `groups[]`

A parent transform shared by several objects.

```json
"groups":  [ { "id": "kaleido", "pivot_x": "center", "pivot_y": "center" } ],
"objects": [ { "id": "petal", "type": "rect", "group": "kaleido", "repeat": { "count": 24 } } ],
"timeline":[ { "action": "rotate", "target": "kaleido", "value": 360, "duration_ms": 6000 } ]
```

| Key | Type | Default |
|---|---|---|
| `id` | string | — |
| `pivot_x`, `pivot_y` | number or layout expression | canvas centre |

An object joins a group with `"group": "<id>"`. A timeline event may target a
group id, and then drives every member at once: `fade_in`/`fade_out`, `move`,
`scale`, `rotate` and `orbit` all apply. `typewrite` and `highlight` do not —
they are per-object ideas.

**Composition.** The member's centre is taken through the group's scale and
rotation about the pivot, then translated; scale multiplies and rotation adds
onto the member's own. So a spinning object inside a spinning group does both,
and a rotating group turns as a rigid body rather than shearing.

Groups are not containers: widgets stay in one flat array and merely record
which group they belong to, so z-ordering and scene slicing are unchanged. A
scene may hold up to 64 groups.

Twenty-four petals and a whole kaleidoscope come to **three** timeline events —
see `anim/kaleido.json`.

---

## `counter` — a number that changes

```json
{ "type": "text", "x": "center", "y": 70, "size": 54, "color": "#F85149",
  "counter": { "from": 0.842, "to": 0.041, "decimals": 3,
               "prefix": "loss  ", "duration_ms": 2600, "rate": 14 } }
```

| Key | Type | Default |
|---|---|---|
| `from`, `to` | float | 0, 100 |
| `decimals` | int | 0 |
| `prefix`, `suffix` | string | "" |
| `duration_ms` / `duration` | int / float | 1000 ms |
| `start_ms` | int | 0 |
| `rate` | int | 12 updates per second |
| `ease` | string | `cubicout` |
| `thousands` | bool | false — group the integer part |

The parser expands this into one text object per displayed value, each visible
for its own slice of the timeline. Re-rasterizing the text every frame would
break the invariant the renderer is built on — a texture is drawn once and
composited many times — so the values are enumerated instead.

That is affordable because a counter does not need to change every frame. At the
default rate a five-second count is about sixty small textures. Steps that
render the same string are merged, so a low `decimals` costs far fewer.

Values switch cleanly rather than crossfading: two numbers dissolving through
each other reads as a blur, not as a count.

---

## `emitter` — a burst of particles

Like `repeat`, but for particles: the parser produces `count` objects and one
`emit` event each.

```json
{ "type": "circle", "radius": 4, "color": "#FFD166",
  "emitter": { "count": 500, "seed": 7, "cx": "center", "cy": "center",
               "spread": 14, "speed": [130, 560], "angle": [0, 360],
               "life": [0.9, 2.1], "emit_ms": 260, "gravity": 300,
               "size_jitter": [0.5, 1.6], "fade": 0.5,
               "color_cycle": ["#FFD166", "#F85149"] } }
```

| Key | Type | Default |
|---|---|---|
| `count` | int | required; capped at 4096 |
| `seed` | int | 1 |
| `cx`, `cy` | number or layout expression | canvas centre |
| `spread` | float | 0 — spawn disc radius |
| `spread_x`, `spread_y` | float | `spread` — per-axis, for a strip rather than a disc |
| `speed` | float or `[min,max]` | `[100,300]` px/s |
| `angle` | float or `[min,max]` | `[0,360]` degrees |
| `life` | float or `[min,max]` | `[1,2]` seconds |
| `emit_ms`, `start_ms` | int | 0 — the spawning window |
| `gravity` | float | 0 — px/s², downward; negative rises |
| `spin` | float or `[min,max]` | 0 — degrees/second |
| `size_jitter` | `[min,max]` | none |
| `fade` | float 0..1 | 0.35 — share of the life spent fading |
| `color_cycle` | array of colours | picked at random per particle |

Any numeric field may be a `[min, max]` pair, sampled per particle.

**Closed form, not keyframes.** A ballistic path is `p₀ + v·t + ½g·t²`, so each
particle needs five numbers rather than a keyframe array — a 700-particle burst
costs about 28 KB of events, and any frame in the middle of it still renders on
its own.

**Deterministic.** The randomness comes from a fixed hash seeded by `seed`, not
`rand()` — which would make output depend on libc and on whatever else had
called it. Verified: the same seed renders byte-identically twice, a different
seed renders differently, and a `--range` window matches the corresponding
frames of a full render exactly.

Particles disappear at the end of their life rather than freezing in place.

---

## `objects[]`

### Common to every object

Two keys every object accepts, beyond position and the property tracks:

| Key | Type | Meaning |
|---|---|---|
| `z` | int | explicit draw order, overriding array position |
| `clip` / `mask` | object | clip the object to a shape |

```json
"clip": { "shape": "circle", "cx": 0.5, "cy": 0.5, "r": 0.45 }
"clip": { "shape": "rect", "x": 0, "y": 0, "w": 1, "h": 0.5, "invert": true }
```

Mask coordinates are **fractions of the object's own box**, not pixels, so a
mask survives scaling: "the left half" stays the left half at any size the
object animates to. It rotates with the object too.


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

### `type: "line"`

A straight segment, given by its endpoints.

```json
{ "type": "line", "x1": 330, "y1": 428, "x2": 750, "y2": 176,
  "width": 2, "cap": "round", "color": "#79C0FF",
  "trim": [ { "t": 0, "v": 0 }, { "t": 1.5, "v": 1, "ease": "cubicinout" } ] }
```

| Key | Type | Default |
|---|---|---|
| `x1`, `y1`, `x2`, `y2` | float | 0,0 → 100,0 |
| `width` | float | 2 |
| `cap` | `butt` \| `round` \| `square` | `butt` |
| `color` | colour | white |
| `trim` | float or track, 0..1 | 1 |

`x`/`y` are ignored — a line's position is its endpoints.

**`trim`** draws the segment progressively, which is what makes a line appear to
be *drawn* rather than merely faded in.

How it works, since it explains the one limitation: the segment is rasterized
horizontally and its real angle becomes a baked-in rotation. In the texture the
line always runs along +x, so "reveal up to here" is a plain x threshold —
exactly the cutoff the typewriter already uses. `trim` therefore needed no new
kernel and no new pixel code at all.

The threshold is quantised, so the segment is divided into one sub-step per
pixel of length (capped at 1024). A single step would make `trim` binary.

### `type: "path"` / `"polyline"`

A polyline or bezier curve, given either as SVG path data or as a list of points.

```json
{ "type": "path", "d": "M 100 400 C 300 400 300 120 500 120 S 700 400 840 380",
  "stroke": "#F0B239", "width": 5, "cap": "round", "fill": "none" }

{ "type": "path", "points": [[60, 420], [67, 418], [74, 415]],
  "stroke": "#79C0FF", "width": 4, "join": "round" }
```

| Key | Type | Default |
|---|---|---|
| `d` | SVG path string | — |
| `points` | `[[x,y], …]` or `[{"x":…,"y":…}, …]` | — |
| `stroke` / `color` | colour | white |
| `width` | float | 2 — 0 means fill only |
| `fill` | colour or `"none"` | `"none"` |
| `cap` | `butt` \| `round` \| `square` | `butt` |
| `join` | `miter` \| `round` \| `bevel` | `miter` |
| `closed` | bool | `false` |

**Supported `d` commands:** `M L H V C S Q T Z`, absolute and relative, with the
usual implicit repetition (a bare coordinate pair continues the previous
command, and repeats after `M` mean `L`). **Arcs (`A`) are not supported** —
they need a parameter conversion of their own and nothing has called for one.

Quadratics are converted to cubics while parsing, so the rest of the pipeline
handles exactly one curve type.

Coordinates are absolute canvas positions and `x`/`y` are ignored: the widget is
placed at its own bounding box, which is derived from the control points. A
cubic never leaves the convex hull of its four points, so that bound is correct
(if slightly generous).

A malformed path warns and renders as nothing rather than failing the parse —
one bad curve should not cost the whole render.

**No animated `trim` yet.** A line's `trim` works by rasterizing the segment
horizontally, which a curve cannot be. Doing it properly needs either
per-frame re-rasterization or arc-length encoded into the texture; until then,
animate a path's `opacity` instead. See `anim/paths.json`.

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

### Gradients (`rect` / `circle`)

```json
"gradient": { "kind": "linear", "from": "#7EE787", "to": "#79C0FF", "angle": 90 }
"gradient": { "kind": "radial", "from": "#FFD166", "to": "#F85149" }
```

Replaces the flat `color`. Two stops, evaluated by Cairo when the shape is
rasterized — so a gradient costs nothing per frame, like everything else about
a shape's appearance.

### Outlines (`rect` / `circle`)

| Key | Type | Default |
|---|---|---|
| `stroke` | colour | none |
| `stroke_width` | float | 2 when `stroke` is given, else 0 |
| `fill` | `"none"` | filled |

`fill: "none"` with a `stroke` gives a ring or an outlined box. Before this
existed the only way to attempt one was a fully transparent fill colour, which
simply drew nothing.

The outline is inset by half its width so it stays inside the object's box —
Cairo centres a stroke on its path, so without the inset the outer half would be
clipped.

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
`scale`, `zoom`, `rotate`, `highlight`, `orbit`, `animate`, `emit`.

`animate` supersedes `scale`/`rotate`/`fade_*` for anything non-trivial — see
below. `emit` is generated by an `emitter` block and is not written by hand.

`move_x`/`move_y` use `value` on that axis; plain `move` uses `value_x`/`value_y`.

### `animate` — a track on any property

One action that can drive any property from a keyframe track, rather than a
separate action per property each carrying a single tween.

```json
{ "time_ms": 0, "action": "animate", "target": "bar", "property": "h",
  "keys": [ { "t": 0, "v": 0 }, { "t": 0.8, "v": 300, "ease": "backout" },
            { "t": 1.6, "v": 120 }, { "t": 2.4, "v": 260, "ease": "bounceout" } ] }
```

| Key | Type | Meaning |
|---|---|---|
| `property` | string | `x` `y` `opacity` `scale` `rotation` `w` `h` `tint` `trim` |
| `keys` | array | `t` in **seconds from the event's start**, `v` absolute |
| `from`, `to` | float | two-key shorthand; uses `duration_ms` and `ease` |

`duration_ms` is optional with `keys` — the last key ends the event. Past the
end the last value holds, so a finished animation stays where it finished.

**Why this exists.** Two overlapping `scale` events on one object used to fight:
each computes its value relative to 1.0, so the second one snapped the object
back to its original size the instant it began. Measured on a deliberate case,
that was a **28 px jump in a single frame**, against 8 px for the ordinary
motion around it. Describing the whole movement as one `animate` with three keys
removes the conflict by construction — there is only one event.

It is also the only way to reach properties that never had an action of their
own, such as a bar's `h`.

A group may be the target too; `x` `y` `opacity` `scale` `rotation` apply there
(size and tint are per-object ideas).

### `orbit`

Sweeps the object's **centre** around a point — a circle, or a spiral when the
two radii differ.

```json
{ "time_ms": 0, "duration_ms": 5000, "action": "orbit", "target": "moon",
  "cx": "center", "cy": "center", "radius": 360,
  "from_angle": 42, "sweep": 540 }
```

| Key | Type | Default |
|---|---|---|
| `cx`, `cy` | number or layout expression | canvas centre |
| `radius` | float | 100 |
| `radius_to` | float | `radius` — differ for a spiral |
| `from_angle` | float (deg) | 0 |
| `sweep` | float (deg) | 360 — total travel, may exceed a turn |
| `to_angle` | float (deg) | alternative to `sweep` (single turn) |
| `orient` | bool | `false` — also turn the object along the tangent |

**Why this is not `rotate`.** `rotate` spins an object about its *own* centre;
it cannot make one object travel around another. Without `orbit` the only
option was approximating the arc with keyframes, which is both verbose and
inexact — see the measurements below.

Notes:

- `sweep`, not an end angle, because "one and a half turns" is `540`, whereas
  `to_angle: 180` would not say how many turns to take getting there.
- Angles are degrees; 0° points right (+x). Because y grows downwards, a
  positive sweep reads as **clockwise** on screen.
- `cx`/`cy` accept layout expressions (`"center"`, `"60%-20"`). They are
  resolved during parsing, since a point on the canvas — unlike a widget's
  position — does not depend on the object's size.
- The event's `ease` shapes the travel, so `cubicout` gives an arc that starts
  fast and settles.
- `orbit` **overwrites** position, like `fade`/`scale`/`rotate` — it does not
  accumulate the way `move` does. With `orient: true` it overwrites rotation too.

**Accuracy.** A 360° orbit at r=300 was rendered and the object's centroid
compared against the analytic circle, alongside keyframe approximations of the
same motion:

| Source | Keyframes | Max radius error |
|---|---|---|
| `orbit` | **0** | **0.17 px** |
| keyframes, 12 segments | 26 | 10.26 px |
| keyframes, 24 segments | 50 | 2.61 px |
| keyframes, 60 segments | 122 | 0.42 px |

Zero keyframes is more accurate than a 122-keyframe polygon, because the
position is evaluated in closed form at every timestamp rather than interpolated
between samples.

See `anim/orbit.json`.

### `highlight`

Draws a band behind a range of lines, the way an editor marks the current line.

```json
{ "time_ms": 500, "duration_ms": 400, "action": "highlight",
  "target": "snippet", "from": 5, "to": 6, "color": "#3FB95040" }
```

| Key | Type | Default |
|---|---|---|
| `line` | int | a single line; shorthand for `from` = `to` |
| `from`, `to` | int | inclusive range |
| `color` | colour | `#FFD16640` — soft amber, 25 % opaque |

**Lines are 1-based**, matching an editor's gutter. The alpha channel of
`color` sets the band's strength, and the event's `duration_ms` fades it in.

Behaviour worth knowing:

- The band inherits the widget's position, scale and rotation, because it is
  back-projected through the same matrix as the text — it cannot drift out of
  alignment with the lines it marks.
- On a code block it is clipped to the panel's own shape, so it follows rounded
  corners instead of poking out of them.
- It fades with the widget: a block fading out takes its highlight with it.
- Like `fade` and `scale`, a later `highlight` on the same target **replaces**
  the earlier one.
- A range beyond the end of the text is clamped; one starting beyond it draws
  nothing.

### Custom easing curves

A project-level `eases` block defines curves usable anywhere `ease` is accepted:

```json
"eases": {
  "snappy":  [0.9, 0.0, 0.1, 1.0],
  "springy": { "type": "spring", "bounces": 3, "damping": 0.35 }
}
```

A four-number array is a CSS-style cubic-bezier. An object with
`"type": "spring"` takes `bounces` (frequency) and `damping` 0..1 — smaller is
bouncier. Measured overshoot: 44 % at `damping` 0.2, 24 % at 0.45 (the default),
7 % at 1.0; every setting settles exactly on the target.

A segment using a custom curve is **resampled into linear sub-keys during
parsing**, so the renderer never learns that custom curves exist — no per-key
parameters to carry and no lookup table to reach from a sampler that has no
context. Twenty-four samples per segment puts the error well below a pixel.

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

### Animating size and colour

Two properties beyond the usual five:

```json
{ "id": "bar", "type": "rect", "w": 80, "x": 80, "y": 340, "anchor": "bottomleft",
  "h": [ { "t": 0, "v": 0 }, { "t": 1.2, "v": 300, "ease": "backout" } ] }
```

| Property | Meaning |
|---|---|
| `w`, `h` as an **array** | animate the destination size |
| `tint` | a colour to blend toward |
| `tint_amount` | 0..1, animatable — the blend strength |

**`w`/`h` versus `scale`.** `scale` multiplies, so a bar starting at `h: 0`
stays at 0 forever — the usual workaround was a fake starting height. An
animated `h` sets the size outright, and the object's **anchor decides what
stays put**: `"anchor": "bottomleft"` grows a bar upward from a fixed baseline.

A plain number keeps its old meaning (the object's own size); only an array is
treated as an animation. When `w`/`h` is animated the shape is rasterized at the
animation's **peak** value, so it is only ever scaled down — sharp at full size,
with slightly squashed corners on a rounded rectangle at smaller ones.

Nothing is re-rasterized per frame: the compositor already derives independent x
and y scales from destination ÷ texture size.

**`tint`** blends the whole layer toward a colour. It is the cheap answer to
"this element lights up" — animating the real colour would mean re-rasterizing
text every frame. The blend happens in premultiplied space, so it tints the
glyphs and not the transparent space around them.

```json
{ "id": "neuron", "type": "circle", "radius": 60, "color": "#30363D",
  "tint": "#FFD166",
  "tint_amount": [ { "t": 0, "v": 0 }, { "t": 0.8, "v": 1 }, { "t": 1.6, "v": 0 } ] }
```

### Relative times

`t` may be a percentage string instead of seconds:

```json
"opacity": [ { "t": "0%", "v": 0 }, { "t": "50%", "v": 1 }, { "t": "100%", "v": 0 } ]
```

The percentage is of the **owning scene's** duration (of the whole film for
project-level `effects`). This is what makes a clip retimeable: changing a
scene from 6 s to 8 s rescales every percentage key, where absolute times would
all have to be edited by hand.

The two forms mix freely inside one track — `{"t": 1.0}` and `{"t": "75%"}` in
the same array are resolved and then sorted together.

Inside a transition's `from`/`to` blocks `t` is already progress in `[0,1]`, so
`"50%"` there simply means `0.5`.

Sampling rules:

- before the first key → the first value;
- after the last key → the last value;
- between keys → interpolate using the **second** key's `ease`.

Keys are sorted by `t` at parse time, so out-of-order input is tolerated —
including a mix of absolute and percentage times, which can only be ordered
once the percentages have become seconds.

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
