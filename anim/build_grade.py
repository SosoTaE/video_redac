#!/usr/bin/env python3
"""
Generates anim/grade.json — a reel of the grading and motion features.

Four shots, each isolating one thing that a still frame or a global correction
cannot show:

  1. light    — a lamp travelling across a normal-mapped panel. Painted-in
                shading and a real normal map are indistinguishable until the
                light moves, so moving it is the demonstration.
  2. blur     — shapes crossing the frame fast enough for a 180-degree shutter
                to smear them. Slow motion would prove nothing: the blur is
                physical, so its length follows the speed.
  3. window   — two power windows on two effects, one inverted. Sharp and
                lifted inside, blurred and crushed outside — the ordinary
                shape of a secondary grade.
  4. key      — a green plate composited over a gradient, keyed.

The green plate and the bump map are generated here rather than downloaded: a
grid of hemispheres is the one relief pattern where "bump or dent?" has an
unambiguous answer, and a backdrop with a deliberate brightness gradient is what
proves the key ignores luma.
"""

import json
import math
import os
import struct
import zlib

W, H = 1600, 900
CX, CY = W // 2, H // 2
FPS = 60

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GEN = os.path.join(ROOT, "assets", "gen")

INK = "#05060A"
TEXT = "#ECEFF6"
DIM = "#93A0BC"
FONT = "DejaVu Sans"
FONT_BOLD = "DejaVu Sans-Bold"


def png(path, w, h, pixel):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            raw.extend(pixel(x, y))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    hdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", hdr)
                + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def write_assets():
    os.makedirs(GEN, exist_ok=True)

    # A green plate: an unevenly lit backdrop with a subject on it. The vertical
    # brightness ramp is the point — it is what a luma-based key gets wrong.
    gw, gh = 720, 540

    def green(x, y):
        t = y / gh
        base = (int(6 + 12 * (1 - t)), int(150 + 74 * (1 - t)), int(56 + 26 * (1 - t)))
        cx, cy, r = gw * 0.5, gh * 0.54, 176.0
        d = math.hypot(x - cx, y - cy)
        a = 1.0 if d < r - 7 else ((r + 7 - d) / 14.0 if d < r + 7 else 0.0)
        subj = (236, 126, 48)
        return tuple(int(base[k] + (subj[k] - base[k]) * a) for k in range(3))

    plate = os.path.join(GEN, "green_plate.png")
    png(plate, gw, gh, green)
    return plate


def track(points, ease="cubicinout"):
    out = []
    for i, (t, v) in enumerate(points):
        k = {"t": round(t, 4), "v": round(v, 4)}
        if i > 0:
            k["ease"] = ease
        out.append(k)
    return out


def caption(oid, title, sub, delay=0.3, hold=4.4):
    def fade(d, hd):
        return track([(d, 0.0), (d + 0.6, 1.0), (d + 0.6 + hd, 1.0),
                      (d + 0.6 + hd + 0.5, 0.0)])
    return [
        {"id": oid + "a", "type": "text", "content": title, "font": FONT_BOLD,
         "size": 58, "color": TEXT, "x": 110, "y": H - 205, "align": "left",
         "opacity": fade(delay, hold)},
        {"id": oid + "b", "type": "text", "content": sub, "font": FONT,
         "size": 27, "color": DIM, "x": 114, "y": H - 126, "align": "left",
         "opacity": fade(delay + 0.4, hold - 0.4)},
    ]


plate_path = write_assets()
rel_plate = os.path.relpath(plate_path, HERE)
rel_nrm = os.path.relpath(os.path.join(GEN, "bumps_nor.png"), HERE)
rel_alb = os.path.relpath(os.path.join(GEN, "bumps_albedo.png"), HERE)

# --------------------------------------------------------------------------
# 1 — a travelling light
# --------------------------------------------------------------------------
D1 = 6.0
STEPS = 30
lx, lz = [], []
for k in range(STEPS + 1):
    f = k / STEPS
    a = math.radians(-105.0 + 210.0 * f)
    lx.append((f * D1, math.sin(a) * 1250.0))
    lz.append((f * D1, -300.0 + math.cos(a) * 800.0))

scene1 = {
    "id": "light",
    "duration_ms": int(D1 * 1000),
    "camera": {"perspective": 1400,
               "px": track([(0, -50), (D1, 50)]), "py": track([(0, -60), (D1, -30)]),
               "pz": track([(0, -1450), (D1, -1330)]),
               "tx": track([(0, 0)]), "ty": track([(0, 0)]), "tz": track([(0, 0)])},
    "light": [{"x": track(lx, ease="linear"), "y": -580.0,
               "z": track(lz, ease="linear"), "intensity": 1.3, "range": 3600.0}],
    "effects": [{"type": "vignette", "amount": 0.4, "radius": 0.66, "softness": 0.6}],
    "objects": [
        {"id": "panel", "type": "mesh", "shape": "plane",
         "x": CX, "y": CY - 40, "z_depth": 0, "anchor": "center",
         "size": 820, "color": "#FFFFFF", "cull": False,
         "texture": rel_alb, "normal_map": rel_nrm,
         "ambient": 0.06, "specular": 0.40, "shininess": 30,
         "rotate_x": track([(0, -34), (D1, -22)], ease="linear")},
    ] + caption("s1", "MOVING LIGHT", "the relief is in the light, not the geometry"),
    "timeline": [],
}

# --------------------------------------------------------------------------
# 2 — motion blur
# --------------------------------------------------------------------------
D2 = 6.0
PAL = ["#FF7043", "#42A5F5", "#7EE787", "#FFD166", "#D2A8FF"]

# Twelve crossings rather than five, staggered so the frame is never empty.
# Each takes 0.42 s to cross 1,900 px — about 4,500 px/s, which a 180-degree
# shutter at 60 fps smears roughly 37 px. Slower and there would be nothing to
# see; the blur is physical, not an amount to be dialled up.
streaks = []
for i in range(12):
    t0 = 0.15 + i * 0.44
    y = 150 + (i % 5) * 130
    streaks.append({
        "id": f"k{i}", "type": "circle" if i % 2 == 0 else "rect",
        "x": track([(t0, -160), (t0 + 0.42, W + 160)], ease="linear"),
        "y": y, "anchor": "center", "w": 120, "h": 120, "color": PAL[i % len(PAL)],
    })

scene2 = {
    "id": "blur",
    "duration_ms": int(D2 * 1000),
    "effects": [{"type": "vignette", "amount": 0.38, "radius": 0.68, "softness": 0.6}],
    "objects": streaks + caption("s2", "MOTION BLUR",
                                 "16 sub-frames across a 180-degree shutter"),
    "timeline": [],
}

# --------------------------------------------------------------------------
# 3 — power windows
# --------------------------------------------------------------------------
D3 = 6.0
# The window drifts across the frame, which is the whole reason its parameters
# are tracks: a window that cannot follow its subject only works locked off.
wcx = track([(0, 0.30), (D3, 0.68)], ease="linear")

scene3 = {
    "id": "window",
    "duration_ms": int(D3 * 1000),
    # The captions sit in this scene too and are graded along with everything
    # else — there is no per-object exemption, and a caption burned in before
    # the grade genuinely would be graded. So the outside correction is kept
    # mild enough to read through rather than pretending the problem away.
    "effects": [
        {"type": "color_grade", "exposure": -0.28, "saturation": 0.40},
        {"type": "blur", "radius": 6,
         "window": {"shape": "ellipse", "cx": wcx, "cy": 0.40,
                    "rx": 0.17, "ry": 0.30, "feather": 0.05, "invert": True}},
        {"type": "color_grade", "exposure": 0.62, "saturation": 1.45,
         "window": {"shape": "ellipse", "cx": wcx, "cy": 0.40,
                    "rx": 0.17, "ry": 0.30, "feather": 0.05}},
    ],
    "objects": [
        {"id": "bg", "type": "rect", "x": CX, "y": CY, "anchor": "center",
         "w": W, "h": H, "gradient": {"kind": "linear", "from": "#16233A", "to": "#3A2418"}},
    ] + [
        {"id": f"d{i}", "type": "circle", "x": 210 + i * 300, "y": 360,
         "anchor": "center", "w": 220, "h": 300, "color": PAL[i]}
        for i in range(5)
    ] + caption("s3", "POWER WINDOW", "one grade inside, another outside — and it moves"),
    "timeline": [],
}

# --------------------------------------------------------------------------
# 4 — chroma key
# --------------------------------------------------------------------------
D4 = 5.5

scene4 = {
    "id": "key",
    "duration_ms": int(D4 * 1000),
    "effects": [{"type": "vignette", "amount": 0.4, "radius": 0.66, "softness": 0.6}],
    "objects": [
        {"id": "bg", "type": "rect", "x": CX, "y": CY, "anchor": "center",
         "w": W, "h": H,
         "gradient": {"kind": "radial", "from": "#3C5A99", "to": "#0B0F1A"}},
        {"id": "raw", "type": "image", "path": rel_plate, "width": 520,
         "x": 210, "y": 150},
        {"id": "cut", "type": "image", "path": rel_plate, "width": 520,
         "x": 880, "y": 150,
         "key": {"color": "#0EA53F", "tolerance": 0.12, "softness": 0.06, "spill": 0.8}},
    ] + caption("s4", "CHROMA KEY", "the backdrop is a gradient; the key ignores it",
                hold=3.9),
    "timeline": [],
}

project = {
    "_comment": "Grading and motion — moving lights, motion blur, power windows, "
                "chroma key. Generated by anim/build_grade.py.",
    "project": {"width": W, "height": H, "fps": FPS, "bg_color": INK,
                # 16 samples: enough that the fastest shape in shot 2 smears
                # smoothly rather than in steps, and past which the difference
                # stops being visible.
                "motion_blur": {"samples": 16, "shutter": 0.5}},
    "output": {"encoder": "h264_nvenc", "preset": "p6", "cq": 20},
    "transition": {"use": "crossfade", "duration": 0.6},
    "scenes": [scene1, scene2, scene3, scene4],
}

out = os.path.join(HERE, "grade.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(project, f, indent=1, ensure_ascii=False)
print(f"wrote {out} — 4 shots, {D1 + D2 + D3 + D4:g}s @ {FPS}fps")
print(f"wrote {plate_path}")
