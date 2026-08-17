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

## Depth — perspective-projected layers

Any object may sit at a depth and turn out of the screen plane. It stays a flat
quad — this is not a mesh renderer — but the quad is placed in space and
projected, which is enough for a carousel, a receding tunnel, parallax, or a
card that turns to face away.

```json
"camera": { "perspective": 1500 },
"objects": [
  { "id": "card", "type": "rect", "w": 210, "h": 300, "x": "center", "y": "center",
    "anchor": "center",
    "z_depth":  [ {"t":0,"v":-300}, {"t":3,"v":900} ],
    "rotate_y": [ {"t":0,"v":0},    {"t":3,"v":360} ] }
]
```

| Key | Type | Meaning |
|---|---|---|
| `camera.perspective` | float | focal length in pixels; **0 (default) = no projection** |
| `z_depth` / `z` | float or track | depth; positive is away from the viewer |
| `rotate_x`, `rotate_y` | float or track | degrees out of the screen plane |

All three are also `animate` properties (`z`, `rotate_x`, `rotate_y`).

**Focal length** controls how strong the perspective is. Around twice the canvas
width is a natural look; smaller exaggerates depth. Anything smaller than the
scene's z range puts objects at or behind the viewer, where they are clipped.

**Depth sorting** happens automatically: when any object in a scene has a
non-zero `z`, that scene's layers are drawn farthest-first each frame, so nearer
ones occlude. The sort is stable, so layers at equal depth keep their authored
z-order. A scene with no depth skips the sort entirely.

**The projection is anchored at the canvas centre**, not each object's own
centre. That is the difference between a camera and a per-object trick: a layer
moving away both shrinks *and* drifts toward the middle of the frame, which is
what makes parallax work.

### Shading and backfaces

A flat quad carries no lighting of its own, so a card in mid-turn reads as a
shape that merely got narrower. Two keys fix that:

| Key | Type | Meaning |
|---|---|---|
| `shading` | float 0..1 | darkens the layer as it turns away; 0 (default) is off |
| `backface` | `show` \| `hide` \| `dim` | what to do when the far side faces you |

```json
{ "type": "rect", "rotate_y": [...], "shading": 0.75, "backface": "hide" }
```

Both derive from the quad's normal after rotation, whose z component is simply
`cos(ry)·cos(rx)` — its sign says which side you are looking at, its magnitude
how square-on the surface is.

**They answer different questions**, which is easy to conflate: `shading` is
about *angle*, `backface` is about *side*. At exactly 180° a layer's back is
square-on, so `shading` leaves it at full brightness — only `backface` can tell
you that you are looking at the reverse.

`hide` is what a carousel wants: the cards on the far side of the ring face away
and should not be seen through the near ones.

### Two things worth knowing

**A quad seen exactly edge-on disappears.** At `rotate_y` 90° or 270° there is
nothing to draw. That is the geometry, not a bug.

**Cross-backend agreement is looser here.** The affine path matches CPU and GPU
to within one ulp; the perspective path divides, which amplifies that rounding.
Measured on a frame of `anim/space3d.json`: 7 differing pixels out of 921,600,
max RGB delta 4 — against 0 or 1 for the same scene without depth. Still
invisible, but a regression test across backends needs a tolerance here.

**Existing projects are untouched.** With no `perspective` and no depth, the
compositor takes exactly the affine route it always did — verified
bit-identical, not merely equivalent.

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
| `perspective` / `focal` | float | focal length in pixels; enables depth (see above) |

Composed onto every object exactly as a group is, with the canvas centre as the
pivot, and applied after groups — so a camera move layers on top of a scene's
own hierarchy instead of being overwritten by it.

### Moving the camera through the scene

The keys above move the *picture*. These move the *viewpoint*, which is a
different thing: they change what is in front of what, so nearer objects slide
across farther ones and the geometry turns as you pass it. Only meshes see this
— projected layers are flat cards and stay square to the lens.

```json
"camera": {
  "perspective": 1500,
  "px": [ {"t":0,"v":-900}, {"t":3,"v":0}, {"t":6,"v":900,"ease":"cubicinout"} ],
  "py": -400,
  "tx": 0, "ty": 0, "tz": 0
}
```

| Key | Type | Meaning |
|---|---|---|
| `px`, `py`, `pz` | track | where the eye is, in world units |
| `tx`, `ty`, `tz` | track | what it looks at |
| `roll` | track | spin about the view axis, degrees |

The world is the canvas: x runs right, y runs **down**, and z runs away from the
viewer, so the units are the same pixels an object's `x`/`y` are given in.

**Naming any one of these switches the camera on.** Leave them all out and the
eye sits at `(0, 0, -perspective)` looking at the origin, which is the fixed
viewpoint every earlier project was authored against — and reaches it through
the same arithmetic, so those projects render byte-for-byte as before. Name even
one and the rest take their defaults from that same position, so `"px": 400` is
a step to the right and nothing else.

A degenerate pair — eye and target at the same point, or looking straight down
the up axis — falls back to a sane basis rather than producing NaNs that would
poison every vertex in the scene.

**Shake is a hash of the timestamp, not a random number.** A frame has to stay
a pure function of time, or `--range` and the two backends would each produce a
different judder.

---

## `light`

A point light for the scene, in world units. Meshes only; flat layers are not
shaded.

```json
"light": { "x": 0, "y": 0, "z": 0 }
```

Without it, meshes are lit from the camera: whatever faces the viewer is bright
and nothing is ever in shadow. That is the right default for a single object on
a title card, where the subject can never hide itself, and quite wrong for a
scene that is *about* where the light comes from — a solar system, where half of
each body must be dark, or a product turning under a fixed key light.

`ambient` sets the floor, so a body's night side is not pure black; a light
source itself takes `"ambient": 1.0`, which is how the Sun in
`anim/solar.json` stays bright while lighting everything else.

**Two-sided geometry is lit two-sided.** A surface with `cull` off is being
shown from both faces and so has no outward direction for light to fall on;
clamping at zero there would black out the whole thing whenever the light
happened to be on the far side. Those surfaces use the magnitude of the term
instead. It is the reason a ring lit by a star in its own plane is visible at
all rather than arithmetically, uselessly correct.

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

### `type: "video"`

A clip, decoded to frames when the project loads.

```json
{ "type": "video", "path": "clip.mp4", "width": 480,
  "x": "center", "y": 40, "anchor": "top",
  "start": 2.0, "speed": 1.0, "loop": false }
```

| Key | Type | Default |
|---|---|---|
| `path` / `src` | string | required, relative to the JSON |
| `width`, `height` | int | the source's own size; one alone keeps the aspect |
| `start` | float | 0 — in-point within the source, seconds |
| `speed` | float | 1 — 2 is twice as fast |
| `loop` | bool | false — otherwise the last frame holds |

**How it works, and what that costs.** Every frame is decoded up front and
stacked into one texture; the compositor is handed a slice of it. That keeps the
pipeline's central assumption intact — a texture is uploaded once, never per
frame — and with it everything that depends on the assumption: a frame stays a
pure function of time, `--range` renders the same pixels as a full run
(*verified: 0 differing bytes*), and both backends work unchanged.

The cost is memory, so **frames are decoded at the size they will be shown at**,
not the source's: a 640×360 frame is 0.9 MB where 1080p is 8.3 MB. There is a
512 MB budget per clip; beyond it the clip is truncated, with a warning. A
four-second 480×270 clip is about 59 MB.

`speed` is applied by the decoder, so the frames arrive already retimed rather
than being picked — and picked frames inherit the rounding.

**Shadow and glow are not supported on video** and are ignored with a warning.
The padding would wrap the whole stacked strip rather than each frame, so every
frame after the first would be read from the wrong offset.

### `type: "mesh"`

A triangle mesh — an OBJ file, or a named primitive.

```json
{ "type": "mesh", "shape": "torus", "size": 240, "x": "center", "y": "center",
  "anchor": "center", "color": "#D2A8FF",
  "rotate_x": [ {"t":0,"v":0}, {"t":4,"v":360} ] }

{ "type": "mesh", "path": "assets/model.obj", "size": 300, "ambient": 0.2 }
```

| Key | Type | Default |
|---|---|---|
| `path` / `src` | string | an OBJ file, relative to the JSON |
| `shape` | `box` \| `sphere` \| `torus` \| `cylinder` \| `plane` \| `ring` | `box` when no path |
| `size` | float | 300 — the mesh's longest axis, in pixels |
| `color` | colour | `#7EE787` |
| `ambient` | float 0..1 | 0.25 — the floor for faces turned away |
| `cull` | bool | true for closed shapes, false for `plane` |
| `smooth` | bool | true for curved primitives and imported models |
| `texture` | string | an image to wrap onto the surface, relative to the JSON |

Position, rotation, depth, `scale`, `opacity` and `blend` all work as on any
other object. `rotate_x` / `rotate_y` / `rotation` turn the model in space.

**This is the one widget that is not a texture.** Everything else is rasterized
once and composited many times; a mesh is transformed, projected and filled
every frame, because its silhouette changes with every degree of rotation.

**Imported models are normalised** into a unit cube centred on the origin before
anything else happens. Without that, `size` would mean something different for
every file — an OBJ in millimetres and one in metres differ by a thousand — and
models authored far from the origin would rotate about a point outside
themselves.

**Meshes occlude each other properly.** They share one depth buffer per scene,
so two solids that pass through one another meet along the right curve instead
of one being drawn wholly on top of the other. Ordinary layers are still painted
back to front; only meshes are depth-tested.

### `smooth` — flat facets or a curved surface

Flat shading gives every triangle one colour, taken from the face's own normal.
Smooth shading interpolates the *vertices'* normals across the face, so a
sphere reads as a sphere rather than as a heap of visible facets.

The default follows the shape because the right answer does: averaging normals
across a cube's corner rounds its edges into something soft and wrong, while a
sphere without it is unmistakably faceted. So `sphere`, `torus`, `cylinder` and
imported models smooth by default; `box` and `plane` do not. `"smooth": false`
or `true` overrides.

A model that arrives without normals gets them derived from its faces, weighted
by area so a sliver triangle does not skew the surface as much as a large one.

### `ring` — a flat annulus

A disc with a hole, lying in the XZ plane: a planetary ring, a halo, an orbit
marker. Its UVs run *radially* — u from the inner edge to the outer, v around
the circumference — because ring textures are radial strips, one row of pixels
repeated all the way round. A square plane's corner-to-corner UVs would smear
that strip across the disc instead.

Like `plane` it is two-sided: `cull` defaults to false, and its lighting uses
the magnitude of the light term rather than clamping at zero (see `light`).

### `texture` — wrapping an image onto the surface

```json
{ "type": "mesh", "shape": "sphere", "texture": "assets/earth.png" }
```

The image is multiplied by `color`, so leave the colour white to see the texture
untouched and tint it otherwise. Lighting still applies.

**Transparent texels are not surface.** A texture with an alpha channel cuts the
shape out: fully transparent pixels are rejected before the depth buffer is
written, so the hole in a ring lets whatever is behind it through instead of
being an invisible disc that occludes. Partially transparent texels blend
normally.

A sphere's UVs are laid out for equirectangular maps — u is longitude, v is
latitude with v = 0 at the top — so a planet or globe texture can be used as
downloaded. Sampling is
perspective-correct — interpolating UVs directly across a steeply angled face is
the classic swimming-texture artefact — and coordinates outside 0..1 wrap, so
tiled UVs behave as expected.

Every primitive carries UVs: the box and cylinder give each face the whole
image, the sphere and torus wrap it around their parameterisation, and the
plane maps it corner to corner.

### File formats

**OBJ** — `v`, `vt`, `vn` and `f`. A face may reference any combination of the
three (`f 1/2/3`, `f 1//3`, `f 1`); distinct combinations become distinct
vertices, which is what lets a cube keep hard edges. Polygons are triangulated
as a fan. Faces referencing undefined vertices are dropped with a warning rather
than read out of bounds.

**glTF 2.0** — both `.gltf` (with an external `.bin` or an embedded base64
buffer) and `.glb`. Positions, normals, texture coordinates, indices and the
whole node hierarchy's transforms are read, along with the material's base
colour texture when it is a file next to the model.

**Two conventions are converted on the way in**, and both are invisible until
they bite. glTF is Y-up with +Z toward the viewer, while this renderer's world
is Y-down with +Z going away, so models are given a half turn about X —
otherwise every import arrives upside down, and since a lone model still looks
like a model, it reads as the artist's choice rather than as a bug. And glTF
calls a face front-facing when its normal points at the viewer, where this
renderer keeps the opposite (see `smooth` above for why y-down inverts the
test), so triangle winding is reversed as well. Without that, an imported model
with `cull` at its default vanishes completely.

Deliberately not read: animation, skinning, morph targets and the rest of the
PBR material model. A file using them still imports — in its bind pose, with
flat colour — and says so, rather than silently producing something that looks
almost right. Non-triangle primitives are counted and reported; sparse accessors
are refused rather than read as their dense base, which would be wrong geometry
presented as correct.

**Shading is flat, lit from the camera.** It is the cheapest thing that makes a
solid read as a solid — without it every face of a cube is the same colour and
the shape collapses into a silhouette. `ambient` sets how dark a face turned
fully away becomes.

#### What this is not

There is no z-buffer. The rasterizer loops over *triangles inside each pixel*
rather than pixels inside each triangle, so the depth test is a local variable
and needs no shared buffer or atomics — which is what lets one implementation
serve both backends. The cost is **O(triangles) per pixel**, so this suits
models of hundreds to a few thousand faces, not scanned assets.

There is also no texturing, no per-vertex normals (so no smooth shading), and no
scene lighting.

Cross-backend agreement is looser than for 2D: measured at 3 differing bytes in
23 million, max delta 10.

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

### Shadow and glow

```json
"shadow": { "dx": 0, "dy": 10, "blur": 22, "color": "#00000099" }
"glow":   { "blur": 34, "color": "#7EE787AA" }
```

Both keys drive the same thing — a blurred, tinted copy of the object's own
alpha, drawn underneath. A glow is a shadow with no offset and a bright colour,
so the defaults differ and nothing else does.

| Key | Type | Default (shadow / glow) |
|---|---|---|
| `dx`, `dy` | float | 0, 8 / 0, 0 |
| `blur` | float | 16 / 24 — capped at 128 |
| `color` | colour | `#00000096` / `#FFFFFF8C` |

Works on every object type, including text and code blocks (where the shadow
belongs to the panel, not the glyphs).

Applied to the **texture**, not by the compositor, so it costs nothing per frame
— the same reasoning as gradients. The texture grows to make room and the
object's position is pulled back by the same amount, so the content does not
move. The blur is three box passes, which is O(1) per pixel: a 40-pixel blur
costs the same as a 4-pixel one.

### Blend modes

```json
"blend": "add"     // or "screen", or "normal" (the default)
```

| Mode | Formula | Use |
|---|---|---|
| `normal` | source-over | everything ordinary |
| `add` | `src + dst` | particles, glows, anything that should read as *light* |
| `screen` | `1-(1-src)(1-dst)` | the gentler version — saturates instead of clipping |

Additive is what makes a mass of particles look like light rather than paint:
overlapping sprites brighten toward white instead of each hiding the one behind
it. With three overlapping coloured discs, `normal` muddies them, `add` gives
red+green→yellow and all three→white.

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

## `labels`

Named instants for a scene, in milliseconds:

```json
"labels": { "beat": 1500, "drop": 3200 }
```

Any event's `time` may refer to one. An audio marker is just a label whose value
came off a waveform — there is no separate mechanism for it.

---

## `timeline[]`

| Key | Type | Default |
|---|---|---|
| `id` / `label` | string | names this event so others can hang off it |
| `time` | number or string | seconds, or an expression (see below) |
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

### Relative event times

```json
{ "id": "intro", "action": "fade_in", "target": "t", "time_ms": 0, "duration_ms": 600 },
{ "action": "fade_in", "target": "b", "time": "intro.end + 150" },
{ "action": "fade_in", "target": "c", "time": "beat + 100" }
```

`time` accepts `<name>.start`, `<name>.end`, a `labels` entry, or a plain
number — plus any number of signed terms. **Numbers are milliseconds.**

Absolute times make every later event wrong the moment an earlier one changes
length; a named event that others hang off survives the edit.

Resolved iteratively, so an event may hang off another that is itself relative.
A cycle or an unknown name warns and leaves the event at its default time.

Resolution happens before a scene's duration is derived from its last event, so
an auto-length scene measures the real times rather than a row of zeros.

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

### Bindings — a position relative to another object

A value starting with `=` is resolved against other objects, after every size
and position is known:

```json
{ "id": "label", "type": "text", "content": "220", "anchor": "bottom",
  "x": "=bar1.cx", "y": "=bar1.top - 14" }
```

| Edge | Meaning |
|---|---|
| `left` `right` `top` `bottom` | the object's box |
| `cx` `cy` | its centre |
| `w` `h` | its size |

Grammar: a term, then any number of signed terms — `=bar3.right + 24`. It is
deliberately the same shape as an event's `time`, so there is one thing to
learn rather than two.

The value becomes the object's **anchor point**, exactly as a layout expression
does. So `"anchor": "bottom"` with `"x": "=bar.cx"` centres the label on the
bar, which is what writing that means.

Layout expressions answer "where on the canvas"; bindings answer "where
relative to that". Chains work (`a → b → c`), and a cycle or an unknown name
warns and leaves the object at its default rather than failing the parse.

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
