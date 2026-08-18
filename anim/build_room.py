#!/usr/bin/env python3
"""
Generates anim/room.json — a storage room, furnished with real scanned models.

Every prop is a CC0 glTF from Poly Haven, loaded exactly as downloaded: the
`.gltf`, its `.bin`, and its diffuse texture, with the node hierarchy and UVs
the scanner produced. Nothing was converted or re-exported. The shell of the
room — floor, walls, ceiling — is the engine's own boxes, because a wall is a
box and importing one would be silly.

Two things about scale are worth stating, because they decide every number here:

  * an imported model is normalised so its *longest* axis spans the unit cube,
    so a single `size` means "how long is the longest side" and preserves the
    model's own proportions. Per-axis sizes are for the shell, never the props.
  * the room is built at 200 units per metre, and Poly Haven publishes real
    dimensions, so each prop's size is its true length times two hundred. That
    is what stops a crate from ending up the size of a desk.

Credit, though CC0 requires none: models by Poly Haven (polyhaven.com).
"""

import json
import math
import os

W, H = 1920, 1080
CX, CY = W // 2, H // 2
FPS = 60
FOCAL = 1150.0
DUR = 16.0

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PROPS = os.path.join(ROOT, "assets", "props")
TEX = os.path.join(ROOT, "assets", "city")

M = 200.0                     # units per metre

# The room, in metres, then in units. y grows downward, so the floor is +y.
RW, RH, RD = 6.0 * M, 3.2 * M, 9.0 * M
FLOOR = 300.0                 # the floor's y
CEIL = FLOOR - RH

FONT_GE = "DejaVu Sans"
FONT_GE_BOLD = "DejaVu Sans-Bold"


def track(points, ease="cubicinout"):
    out = []
    for i, (t, v) in enumerate(points):
        k = {"t": round(t, 4), "v": round(v, 4)}
        if i > 0:
            k["ease"] = ease
        out.append(k)
    return out


def prop(oid, name, x, y, z, metres, **kw):
    """A downloaded model. `metres` is its real longest dimension."""
    o = {
        "id": oid, "type": "mesh",
        "path": os.path.join(PROPS, name, f"{name}_1k.gltf"),
        "x": CX + x, "y": CY + y, "z_depth": z,
        "anchor": "center", "size": metres * M,
        "color": "#FFFFFF", "ambient": 0.16,
    }
    o.update(kw)
    return o


def slab(oid, x, y, z, size, color, **kw):
    o = {
        "id": oid, "type": "mesh", "shape": "box",
        "x": CX + x, "y": CY + y, "z_depth": z,
        "anchor": "center", "size": size,
        "color": color, "ambient": 0.10,
    }
    o.update(kw)
    return o


objects = []
lights = []

# --- the shell -------------------------------------------------------------
# Thin boxes rather than planes: a plane is one-sided and infinitely thin, so it
# shows nothing at a grazing angle and nothing at all from behind. A wall with
# depth catches the light on its edge, which is what makes a corner a corner.
T = 24.0
objects += [
    slab("floor", 0, FLOOR, RD * 0.5, [RW, T, RD], "#FFFFFF",
         ambient=0.12, texture=os.path.join(TEX, "concrete.png")),
    slab("ceil", 0, CEIL, RD * 0.5, [RW, T, RD], "#8C8F98", ambient=0.10),
    slab("wall_l", -RW * 0.5, FLOOR - RH * 0.5, RD * 0.5, [T, RH, RD], "#A8ABB4",
         ambient=0.10),
    slab("wall_r", RW * 0.5, FLOOR - RH * 0.5, RD * 0.5, [T, RH, RD], "#A8ABB4",
         ambient=0.10),
    slab("wall_back", 0, FLOOR - RH * 0.5, RD, [RW, RH, T], "#B0B3BB",
         ambient=0.11),
]

# --- the lamps, and the light that belongs to each -------------------------
# The lamp model hangs from the ceiling; the light sits just below its shade,
# with a range of about two metres so each one owns a pool of floor rather than
# washing the whole room flat.
for i, lz in enumerate((RD * 0.28, RD * 0.72)):
    ly = CEIL + 130.0
    objects.append(prop(f"lamp{i}", "industrial_pipe_lamp", 0.0, ly, lz, 0.55,
                        ambient=0.55, rotate_x=180))
    lights.append({"x": 0.0, "y": ly + 90.0, "z": lz,
                   "intensity": 1.5, "range": 3.4 * M})

# a dim cool fill so the corners are not voids
lights.append({"x": 0.0, "y": CEIL, "z": RD * 0.5,
               "intensity": 0.30, "range": 14.0 * M})

# --- the furniture ---------------------------------------------------------
# Sizes are the models' real dimensions. Everything sits ON the floor, so each
# y is the floor minus half the prop's height — which for a normalised model is
# half its longest side only when that side is the vertical one, hence the
# per-prop offsets rather than one formula.
objects += [
    prop("rack", "worn_metal_rack", -RW * 0.5 + 240, FLOOR - 190, RD * 0.62, 1.9,
         rotate_y=90),
    prop("desk", "metal_office_desk", RW * 0.5 - 300, FLOOR - 78, RD * 0.34, 1.6,
         rotate_y=-90),
    prop("ladder", "wooden_ladder", -RW * 0.5 + 300, FLOOR - 200, RD * 0.22, 2.0,
         rotate_y=24),
    prop("tank", "small_lpg_tank", RW * 0.5 - 200, FLOOR - 58, RD * 0.78, 0.58,
         rotate_y=-30),

    prop("crate_a", "plastic_crate_01", -140, FLOOR - 40, RD * 0.46, 0.40,
         rotate_y=18),
    prop("crate_b", "plastic_crate_01", -120, FLOOR - 118, RD * 0.44, 0.40,
         rotate_y=-9),
    prop("crate_c", "plastic_crate_02", 190, FLOOR - 38, RD * 0.55, 0.38,
         rotate_y=-34),
    prop("crate_d", "plastic_crate_02", 205, FLOOR - 110, RD * 0.57, 0.38,
         rotate_y=12),

    prop("tin_a", "oil_tin", 60, FLOOR - 36, RD * 0.30, 0.34, rotate_y=40),
    prop("tin_b", "oil_tin", 320, FLOOR - 36, RD * 0.24, 0.34, rotate_y=-15),
]

# --- the move --------------------------------------------------------------
# In through the door end and down the room, drifting sideways so the props
# pass each other. Slow: an interior is read by what slides past, not by speed.
STEPS = 48
px, py, pz, tx, ty, tz = [], [], [], [], [], []
for k in range(STEPS + 1):
    f = k / STEPS
    t = DUR * f
    px.append((t, -260.0 + 500.0 * f))
    py.append((t, FLOOR - 300.0 - 120.0 * math.sin(f * math.pi)))
    pz.append((t, -520.0 + f * (RD * 0.62)))
    tx.append((t, -80.0 + 300.0 * math.sin(f * math.pi * 0.9)))
    ty.append((t, FLOOR - 260.0))
    tz.append((t, RD * 0.55 + f * (RD * 0.35)))

camera = {
    "perspective": FOCAL,
    "px": track(px, "linear"), "py": track(py, "linear"), "pz": track(pz, "linear"),
    "tx": track(tx, "linear"), "ty": track(ty, "linear"), "tz": track(tz, "linear"),
}


def fade(t_in, hold, t_out=0.9, delay=0.0):
    return [{"t": round(v[0], 4), "v": v[1], "ease": "cubicinout"} for v in [
        (delay, 0.0), (delay + t_in, 1.0),
        (delay + t_in + hold, 1.0), (delay + t_in + hold + t_out, 0.0)]]


objects += [
    {"id": "h1", "type": "text", "content": "საწყობი", "font": FONT_GE_BOLD,
     "size": 74, "color": "#EEF1F7", "x": 140, "y": 820,
     "opacity": fade(1.0, 3.2, 1.0, delay=1.2)},
    {"id": "h2", "type": "text", "content": "ნამდვილი მოდელები — Poly Haven, CC0",
     "font": FONT_GE, "size": 32, "color": "#A8B2C8", "x": 144, "y": 922,
     "opacity": fade(1.0, 3.2, 1.0, delay=1.6)},
]

project = {
    "_comment": "A storage room furnished with CC0 glTF models from Poly Haven. "
                "Generated by anim/build_room.py.",
    "_credit": "Models: Poly Haven (polyhaven.com), CC0",
    "project": {"width": W, "height": H, "fps": FPS, "bg_color": "#05060A"},
    "output": {"encoder": "h264_nvenc", "preset": "p6", "cq": 20},
    "scenes": [{
        "id": "room",
        "duration_ms": int(DUR * 1000),
        "camera": camera,
        "light": lights,
        "effects": [{"type": "vignette", "amount": 0.40, "radius": 0.68,
                     "softness": 0.6}],
        "objects": objects,
        "timeline": [],
    }],
}

out = os.path.join(HERE, "room.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(project, f, indent=1, ensure_ascii=False)
props = sum(1 for o in objects if o.get("type") == "mesh" and "path" in o)
print(f"wrote {out} — {props} imported props, {len(lights)} lights, {DUR:g}s")
