#!/usr/bin/env python3
"""
Generates anim/materials.json — a short film about surface material.

Three shots, each one making a single point that a still frame cannot:

  1. bumps    — a flat panel of a few hundred triangles turning under a fixed
                light, carrying a normal map. Something has to move relative to
                the light or the shading is indistinguishable from shading
                painted into the albedo; the panel turns because lights in this
                format do not animate.
  2. props    — two Poly Haven models turning, so their normal maps read
                against a changing view rather than as painted-on shading.
  3. glow     — the same lamp with the lights taken away, so what is left is
                only what the surface emits.

The bump map is generated here rather than downloaded: a grid of hemispheres is
the one pattern where "is that a bump or a dent?" has an unambiguous answer, and
an unambiguous answer is what a test of a normal map needs.
"""

import json
import math
import os
import struct
import zlib

W, H = 1600, 900
CX, CY = W // 2, H // 2
FPS = 60
FOCAL = 1400.0

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GEN = os.path.join(ROOT, "assets", "gen")

PROPS = os.path.join(ROOT, "assets", "props")
LAMP = os.path.join(PROPS, "industrial_pipe_lamp", "industrial_pipe_lamp_1k.gltf")
CRATE = os.path.join(PROPS, "plastic_crate_01", "plastic_crate_01_1k.gltf")

INK = "#05060A"
TEXT = "#ECEFF6"
DIM = "#93A0BC"
FONT = "DejaVu Sans"
FONT_BOLD = "DejaVu Sans-Bold"

CREDIT = "Models by Poly Haven — CC0"


def png(path, size, pixel):
    """Writes an RGB PNG. Small enough to do by hand and avoids a dependency."""
    raw = bytearray()
    for y in range(size):
        raw.append(0)                      # filter: none
        for x in range(size):
            raw.extend(pixel(x, y))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    hdr = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    blob = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", hdr)
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(blob)


def write_maps():
    os.makedirs(GEN, exist_ok=True)
    n, cells = 512, 8
    step = n / cells
    r = step / 2.0

    def normal(x, y):
        # The centre of the cell this texel falls in.
        cx = (x // step) * step + r
        cy = (y // step) * step + r
        dx, dy = (x - cx) / r, (y - cy) / r
        d2 = dx * dx + dy * dy
        if d2 < 1.0:
            # The normal of a hemisphere sitting on the surface.
            nx, ny, nz = dx, dy, math.sqrt(1.0 - d2)
        else:
            nx, ny, nz = 0.0, 0.0, 1.0
        return (int((nx * 0.5 + 0.5) * 255),
                int((ny * 0.5 + 0.5) * 255),
                int((nz * 0.5 + 0.5) * 255))

    def albedo(x, y):
        # Deliberately almost uniform: a flat colour leaves the normal map as
        # the only thing in frame that can produce a shape.
        v = 196 + ((x // 64 + y // 64) % 2) * 6
        return (v, v - 6, v - 14)

    nrm = os.path.join(GEN, "bumps_nor.png")
    alb = os.path.join(GEN, "bumps_albedo.png")
    png(nrm, n, normal)
    png(alb, n, albedo)
    return nrm, alb


def track(points, ease="cubicinout"):
    out = []
    for i, (t, v) in enumerate(points):
        k = {"t": round(t, 4), "v": round(v, 4)}
        if i > 0:
            k["ease"] = ease
        out.append(k)
    return out


def text(oid, content, x, y, size, color, font=FONT, **kw):
    o = {"id": oid, "type": "text", "content": content, "font": font,
         "size": size, "color": color, "x": x, "y": y, "align": "left"}
    o.update(kw)
    return o


def fade(t_in, hold, t_out=0.6, delay=0.0):
    return track([(delay, 0.0), (delay + t_in, 1.0),
                  (delay + t_in + hold, 1.0), (delay + t_in + hold + t_out, 0.0)])


nrm_path, alb_path = write_maps()
rel_nrm = os.path.relpath(nrm_path, HERE)
rel_alb = os.path.relpath(alb_path, HERE)

# --------------------------------------------------------------------------
# Shot 1 — a flat plane that does not look flat
# --------------------------------------------------------------------------
D1 = 7.0

# The panel turns; the light stays put.
#
# Something has to move relative to the light, or a normal map is
# indistinguishable from shading painted into the albedo — both give a still
# frame with light and shade in it. Turning the surface is what separates them:
# painted shading turns with the surface, while a normal map's highlights slide
# across it and stay on the side the light is on.
#
# Moving the light would be the more natural way to say that, and is exactly
# what this format does not do: a light is resolved once per scene, and a
# keyframe array in its position reads as zero. See the warning the parser now
# prints if you try.
scene1 = {
    "id": "bumps",
    "duration_ms": int(D1 * 1000),
    "camera": {
        "perspective": FOCAL,
        "px": track([(0, -70), (D1, 70)]),
        "py": track([(0, -60), (D1, -20)]),
        "pz": track([(0, -1500), (D1, -1380)]),
        "tx": track([(0, 0)]), "ty": track([(0, 0)]), "tz": track([(0, 0)]),
    },
    "light": [
        {"x": -1050.0, "y": -560.0, "z": -820.0, "intensity": 1.3, "range": 3400.0},
    ],
    "effects": [{"type": "vignette", "amount": 0.4, "radius": 0.66, "softness": 0.6}],
    "objects": [
        {"id": "panel", "type": "mesh", "shape": "plane",
         "x": CX, "y": CY, "z_depth": 0, "anchor": "center",
         "size": 760, "color": "#FFFFFF",
         "texture": rel_alb, "normal_map": rel_nrm,
         "cull": False,
         "ambient": 0.07, "specular": 0.40, "shininess": 30,
         "rotate_x": track([(0, -58), (D1, -26)], ease="linear"),
         "rotate_y": track([(0, -32), (D1, 32)], ease="linear")},
        text("s1a", "NORMAL MAP", 110, H - 210, 60, TEXT, FONT_BOLD,
             opacity=fade(0.7, 4.8, 0.6, delay=0.4)),
        text("s1b", "two triangles per quad — the relief is entirely in the light",
             114, H - 128, 28, DIM, FONT,
             opacity=fade(0.7, 4.5, 0.6, delay=0.9)),
    ],
    "timeline": [],
}

# --------------------------------------------------------------------------
# Shot 2 — the same map on real models, turning
# --------------------------------------------------------------------------
D2 = 7.0


def prop(oid, path, x, spin):
    return {
        "id": oid, "type": "mesh", "path": path,
        "x": x, "y": CY + 10, "z_depth": 0, "anchor": "center",
        "size": 420, "color": "#FFFFFF",
        "ambient": 0.10, "specular": 0.40, "shininess": 44,
        "rotate_y": track([(0, spin), (D2, spin + 300)], ease="linear"),
    }


scene2 = {
    "id": "props",
    "duration_ms": int(D2 * 1000),
    "camera": {
        "perspective": FOCAL,
        "px": track([(0, -170), (D2, 170)]),
        "py": track([(0, -140), (D2, -90)]),
        "pz": track([(0, -1320), (D2, -1200)]),
        "tx": track([(0, 0)]), "ty": track([(0, 30)]), "tz": track([(0, 0)]),
    },
    "light": [
        {"x": -900.0, "y": -620.0, "z": -520.0, "intensity": 1.15, "range": 3200.0},
        {"x": 850.0, "y": -160.0, "z": 260.0, "intensity": 0.45, "range": 2400.0},
    ],
    "effects": [{"type": "vignette", "amount": 0.42, "radius": 0.62, "softness": 0.6}],
    "objects": [
        prop("crate", CRATE, CX - 330, 24),
        prop("lamp", LAMP, CX + 330, -40),
        text("s2a", "IMPORTED MATERIALS", 110, H - 210, 60, TEXT, FONT_BOLD,
             opacity=fade(0.7, 4.9, 0.6, delay=0.3)),
        text("s2b", "normal, occlusion and specular, read from the glTF",
             114, H - 128, 28, DIM, FONT,
             opacity=fade(0.7, 4.6, 0.6, delay=0.8)),
    ],
    "timeline": [],
}

# --------------------------------------------------------------------------
# Shot 3 — take the lights away
# --------------------------------------------------------------------------
D3 = 6.0

# Poly Haven's lamp emits from the filament and nowhere else — a few hundred
# texels of a 1k map — which is honest but far too small to carry a shot. So the
# glTF-driven case sits behind, at its true size, and the foreground makes the
# argument with three spheres of the same albedo and three emissive strengths:
# in near-darkness the albedo is the same dark blue on all three, and everything
# that separates them is the added term.
GLOW = [("#FF6A2C", 0.45), ("#7EE787", 0.85), ("#39E0FF", 1.35)]

glow_objs = []
for i, (col, strength) in enumerate(GLOW):
    glow_objs.append({
        "id": f"g{i}", "type": "mesh", "shape": "sphere",
        "x": CX + (i - 1) * 330, "y": CY + 110, "z_depth": -60,
        "anchor": "center", "size": 190, "color": "#243050",
        "ambient": 0.04, "specular": 0.45, "shininess": 60,
        "emissive": col, "emissive_strength": strength,
    })

scene3 = {
    "id": "glow",
    "duration_ms": int(D3 * 1000),
    "camera": {
        "perspective": FOCAL,
        "px": track([(0, 200), (D3, -140)]),
        "py": track([(0, -140), (D3, -90)]),
        "pz": track([(0, -1400), (D3, -1280)]),
        "tx": track([(0, 0)]), "ty": track([(0, 20)]), "tz": track([(0, 0)]),
    },
    # One dim light from far off, so there is just enough to see that the
    # spheres share an albedo and the lamp is made of metal.
    "light": [
        {"x": -1200.0, "y": -820.0, "z": -1000.0, "intensity": 0.75, "range": 4000.0},
    ],
    "effects": [{"type": "vignette", "amount": 0.42, "radius": 0.66, "softness": 0.6}],
    "objects": glow_objs + [
        {"id": "lamp2", "type": "mesh", "path": LAMP,
         "x": CX, "y": CY - 160, "z_depth": 300, "anchor": "center",
         "size": 360, "color": "#FFFFFF",
         "ambient": 0.05, "specular": 0.35, "shininess": 40,
         "emissive_strength": 3.0,
         "rotate_y": track([(0, -28), (D3, 16)], ease="linear")},
        text("s3a", "EMISSIVE", 150, 168, 60, TEXT, FONT_BOLD,
             opacity=fade(0.7, 3.9, 0.6, delay=0.3)),
        text("s3b", "added after the shading — one albedo, three strengths",
             154, 250, 28, DIM, FONT,
             opacity=fade(0.7, 3.6, 0.6, delay=0.8)),
        text("credit", CREDIT, W - 420, H - 60, 22, "#5C647A", FONT,
             opacity=track([(0, 0), (2.6, 0), (3.6, 0.9)])),
    ],
    "timeline": [],
}

project = {
    "_comment": "Surface material — normal maps and emission. "
                "Generated by anim/build_materials.py.",
    "_credit": CREDIT,
    "project": {"width": W, "height": H, "fps": FPS, "bg_color": INK},
    "output": {"encoder": "h264_nvenc", "preset": "p6", "cq": 20},
    # A crossfade overlaps the scenes it joins, so the film is shorter than the
    # sum of its shots and each shot starts earlier than that sum suggests:
    # 0.0, 6.4 and 12.8 rather than 0, 7 and 14. Worth writing down — a caption
    # timed against the wrong start looks like an opacity bug.
    "transition": {"use": "crossfade", "duration": 0.6},
    "scenes": [scene1, scene2, scene3],
}

out = os.path.join(HERE, "materials.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(project, f, indent=1, ensure_ascii=False)
print(f"wrote {out} — 3 shots, {D1 + D2 + D3:g}s @ {FPS}fps")
print(f"wrote {nrm_path}")
print(f"wrote {alb_path}")
