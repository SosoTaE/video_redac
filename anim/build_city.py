#!/usr/bin/env python3
"""
Generates anim/city.json — a night street you fly down.

An environment rather than an object: the camera is *inside* it, and everything
in shot is placed to be passed rather than examined. That inverts the demands on
the renderer. A product turntable needs a clean silhouette; a street needs depth
you can feel, which comes from three things this file leans on:

  * lights with falloff, so a lamp lights the pavement under it and not the
    whole block — without that, "lamp" reads as "direction" and the space flattens
  * a shared depth buffer, so buildings occlude each other as you pass between them
  * facade textures whose lit windows carry the sense of scale, since the engine
    has no shadows to do it

Built from boxes. The point is not that a box is a building; it is that eighty of
them at the right heights, with the right things glowing, read as a street.

All textures are generated (see the docstring in this repo's assets/city), so
nothing here depends on an outside download.
"""

import json
import math
import os
import random

W, H = 1920, 1080
CX, CY = W // 2, H // 2
FPS = 60
FOCAL = 1300.0

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TEX = os.path.join(ROOT, "assets", "city")
STARS = os.path.join(ROOT, "assets", "planets", "gen", "2k_stars_milky_way_2048.png")

FONT = "Noto Sans"
FONT_GE = "DejaVu Sans"
FONT_GE_BOLD = "DejaVu Sans-Bold"

INK = "#04050A"
TEXT = "#E8ECF5"
WARM = "#FFC978"

random.seed(11)

# Storey height in world units. Every facade is generated with a row count taken
# from the building it will wrap, so a window is the same shape on a six-storey
# block as on a thirty-storey tower. One shared texture cannot do that: a box
# face takes the whole 0..1 UV range, so a single image stretched over a face
# seven times taller than it is wide turns its windows into vertical stripes.
STOREY = 150.0


def facade(rows, cols=6):
    """Generates (once) a facade with `rows` storeys and returns its path."""
    from PIL import Image, ImageDraw

    gen = os.path.join(TEX, "gen")
    os.makedirs(gen, exist_ok=True)
    out = os.path.join(gen, f"facade_{cols}x{rows}.png")
    if os.path.exists(out):
        return out

    rng = random.Random(1000 + rows * 31 + cols)
    ww, wh, mx, my = 44, 34, 22, 18
    W = cols * (ww + mx) + mx
    H = rows * (wh + my) + my

    im = Image.new("RGB", (W, H), (16, 18, 26))
    d = ImageDraw.Draw(im)
    for r in range(rows):
        for c in range(cols):
            x = mx + c * (ww + mx)
            y = my + r * (wh + my)
            roll = rng.random()
            if roll < 0.42:
                k = rng.uniform(0.65, 1.0)
                col = (int(255 * k), int(214 * k), int(150 * k))
            elif roll < 0.52:
                k = rng.uniform(0.5, 0.85)
                col = (int(150 * k), int(200 * k), int(255 * k))
            else:
                col = (26, 29, 40)
            d.rectangle([x, y, x + ww, y + wh], fill=col)
            d.line([x, y + wh, x + ww, y + wh], fill=(10, 11, 16))
    im.save(out)
    return out


def track(points, ease="linear"):
    out = []
    for i, (t, v) in enumerate(points):
        k = {"t": round(t, 4), "v": round(v, 4)}
        if i > 0:
            k["ease"] = ease
        out.append(k)
    return out


def box(oid, x, y, z, size, color, **kw):
    """A solid, positioned in world units about the canvas centre."""
    o = {
        "id": oid, "type": "mesh", "shape": "box",
        "x": CX + x, "y": CY + y, "z_depth": z,
        "anchor": "center", "size": size,
        "color": color, "ambient": 0.10,
    }
    o.update(kw)
    return o


# --------------------------------------------------------------------------
# The street
# --------------------------------------------------------------------------
# A canyon: two rows of towers with a road between them, running away from the
# camera down +z. Each tower takes its own width, height and depth, staggered so
# the eye reads a row of separate buildings rather than one long fence.
ROAD_HALF = 620.0        # half the street's width
Z_NEAR, Z_FAR = -900.0, 12000.0
STEP = 780.0

objects = []
lights = []

# Sky first, so everything else paints over it.
objects.append({
    "id": "sky", "type": "mesh", "shape": "sphere",
    "x": CX, "y": CY, "z_depth": 0, "anchor": "center",
    "size": 40000, "color": "#FFFFFF", "ambient": 1.0,
    "cull": False, "smooth": True, "opacity": 0.30,
    "texture": STARS,
})

# The road: one long plane. `plane` lies in XZ, which is exactly the ground.
objects.append({
    "id": "road", "type": "mesh", "shape": "plane",
    "x": CX, "y": CY + 300, "z_depth": (Z_NEAR + Z_FAR) * 0.5,
    "anchor": "center", "size": Z_FAR - Z_NEAR,
    "color": "#FFFFFF", "ambient": 0.16,
    "texture": os.path.join(TEX, "road.png"),
})

n = 0
z = Z_NEAR
while z < Z_FAR:
    for side in (-1, 1):
        # Proportions vary so the skyline is not a comb. Depth matters as much
        # as height: buildings of one depth line up into a wall, and the street
        # loses the staggered recession that makes it feel deep.
        w = random.uniform(380.0, 700.0)
        h = random.uniform(1000.0, 3000.0)
        dep = random.uniform(460.0, 900.0)
        offset = ROAD_HALF + w * 0.5 + random.uniform(20.0, 200.0)

        objects.append(box(
            f"t{n}", side * offset, 300.0 - h * 0.5, z + random.uniform(-120.0, 120.0),
            [w, h, dep], "#FFFFFF", ambient=0.13,
            texture=facade(max(4, int(round(h / STOREY)))),
            rotation=random.uniform(-0.8, 0.8),
        ))
        n += 1
    z += STEP

# Street lamps: a small emissive head, and a light at the same place with a
# range short enough that each one owns its own pool of pavement.
LAMP_Z = [600.0, 2100.0, 3600.0, 5100.0]
for i, lz in enumerate(LAMP_Z):
    side = -1 if i % 2 == 0 else 1
    lx = side * (ROAD_HALF - 90.0)
    ly = 300.0 - 620.0

    objects.append({
        "id": f"lamp{i}", "type": "mesh", "shape": "sphere",
        "x": CX + lx, "y": CY + ly, "z_depth": lz,
        "anchor": "center", "size": 62, "color": WARM,
        "ambient": 1.0,                       # the source cannot light itself
    })
    objects.append({
        "id": f"post{i}", "type": "mesh", "shape": "cylinder",
        "x": CX + lx, "y": CY + 300 - 310, "z_depth": lz,
        "anchor": "center", "size": [26, 620, 26],
        "color": "#3A3F4C", "ambient": 0.25,
    })
    lights.append({"x": lx, "y": ly, "z": lz, "intensity": 1.0, "range": 900.0})

# A cool fill from high above the street, with a long range: the sky itself.
# Without it the tops of the towers, far from every lamp, fall to pure ambient
# and the skyline stops existing.
lights.append({"x": 0.0, "y": -6000.0, "z": 4000.0, "intensity": 0.45, "range": 9000.0})

# --------------------------------------------------------------------------
# The move: down the street, then a lift to see over it
# --------------------------------------------------------------------------
DUR = 18.0
STEPS = 60

px, py, pz, tx, ty, tz = [], [], [], [], [], []
for k in range(STEPS + 1):
    f = k / STEPS
    t = DUR * f
    # A drift along the road at eye level, then a climb clear of the rooftops.
    # The climb has to actually clear them — the tallest tower is 3000 units, so
    # a lift of 1500 just puts the camera among the upper storeys, where there
    # are no lamps and nothing reads. Rising past them turns the shot from a
    # dark corridor into a skyline.
    lift = max(0.0, (f - 0.55) / 0.45) ** 1.7
    px.append((t, 210.0 * math.sin(f * math.pi * 1.2)))
    py.append((t, 300.0 - 380.0 - 4200.0 * lift))
    pz.append((t, Z_NEAR - 700.0 + f * 5200.0))
    # Look down the street, then tilt down onto the city as the camera rises.
    tx.append((t, 220.0 * math.sin(f * math.pi * 1.2 + 0.9)))
    ty.append((t, 300.0 - 700.0 + 900.0 * lift))
    tz.append((t, Z_NEAR + 3000.0 + f * 5200.0))

camera = {
    "perspective": FOCAL,
    "px": track(px), "py": track(py), "pz": track(pz),
    "tx": track(tx), "ty": track(ty), "tz": track(tz),
}


def text(oid, content, x, y, size, color, font=FONT, **kw):
    o = {"id": oid, "type": "text", "content": content, "font": font,
         "size": size, "color": color, "x": x, "y": y}
    o.update(kw)
    return o


def fade(t_in, hold, t_out=0.8, delay=0.0):
    return [{"t": round(v[0], 4), "v": v[1], "ease": "cubicinout"} for v in [
        (delay, 0.0), (delay + t_in, 1.0),
        (delay + t_in + hold, 1.0), (delay + t_in + hold + t_out, 0.0)]]


objects += [
    text("h1", "ღამის ქუჩა", 140, 820, 76, TEXT, FONT_GE_BOLD,
         opacity=fade(1.0, 3.4, 1.0, delay=1.0)),
    text("h2", "რვა სინათლე, მილევით", 144, 926, 34, WARM, FONT_GE,
         opacity=fade(1.0, 3.4, 1.0, delay=1.4)),
]

project = {
    "_comment": "A night street: an environment the camera moves through. "
                "Generated by anim/build_city.py.",
    "project": {"width": W, "height": H, "fps": FPS, "bg_color": INK},
    "output": {"encoder": "h264_nvenc", "preset": "p6", "cq": 20},
    "scenes": [{
        "id": "street",
        "duration_ms": int(DUR * 1000),
        "camera": camera,
        "light": lights,
        "effects": [{"type": "vignette", "amount": 0.42, "radius": 0.66,
                     "softness": 0.6}],
        "objects": objects,
        "timeline": [],
    }],
}

out = os.path.join(HERE, "city.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(project, f, indent=1, ensure_ascii=False)
print(f"wrote {out} — {len(objects)} objects, {len(lights)} lights, {DUR:g}s @ {FPS}fps")
