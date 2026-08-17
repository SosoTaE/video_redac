#!/usr/bin/env python3
"""
Generates anim/mic_ad.json — a product film for the GXL 066 microphone.

The model is a real glTF import (assets/microphone/scene.gltf), so this doubles
as the end-to-end proof of the importer: 17 primitives, a 36-node hierarchy,
an external .bin and a JPEG base colour map, all read straight from what
Sketchfab shipped.

The film is three shots, each one camera move:

  1. reveal  — a slow dolly in while the microphone turns
  2. detail  — an orbit past it at a low angle, with the feature captions
  3. close   — pull back to a centred hero frame and the product name

Written as a generator because the camera work is arithmetic: an orbit is a
circle, and typing out a circle's keyframes by hand is how you end up with a
camera that drifts.

Attribution, required by the model's licence (CC-BY-NC-SA-4.0), is burnt into
the final shot and repeated in the JSON. Non-commercial use only — this is a
demo, not an actual advertisement.
"""

import json
import math
import os

W, H = 1920, 1080
CX, CY = W // 2, H // 2
FPS = 60
FOCAL = 1500.0

CREDIT = ('"Microphone GXL 066 Bafhcteks" by Gistold — CC-BY-NC-SA-4.0')

HERE = os.path.dirname(os.path.abspath(__file__))
MODEL = os.path.join(os.path.dirname(HERE), "assets", "microphone", "scene.gltf")

# Palette: a cold studio dark, with one warm accent so the beige body of the
# microphone reads as the warmest thing in frame.
INK = "#05060A"
HAZE = "#161E33"
TEXT = "#ECEFF6"
DIM = "#A9B4CC"
ACCENT = "#E8B060"

FONT = "Noto Sans"
FONT_BOLD = "Noto Sans-Bold"

# DejaVu, not Noto Sans Georgian, for the Georgian lines.
#
# Cairo is asked for one family per text object and does not fall back per
# glyph, so the family has to cover everything in the string. Noto Sans
# Georgian covers the Georgian and nothing else: a comma, an em dash, digits
# and parentheses all come out as empty boxes, which is easy to miss when you
# are checking that the Georgian itself looks right. DejaVu covers all of it.
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


def fade(t_in, hold, t_out=0.5, delay=0.0):
    """An opacity track: up, hold, down. The commonest thing in the film."""
    return track([
        (delay, 0.0),
        (delay + t_in, 1.0),
        (delay + t_in + hold, 1.0),
        (delay + t_in + hold + t_out, 0.0),
    ])


def mic(rot_from, rot_to, dur, **kw):
    o = {
        "id": "mic",
        "type": "mesh",
        "path": MODEL,
        "x": CX,
        "y": CY,
        "anchor": "center",
        "size": 900,
        "color": "#FFFFFF",
        "ambient": 0.30,
        "rotate_y": track([(0, rot_from), (dur, rot_to)], ease="linear"),
    }
    o.update(kw)
    return o


def backdrop():
    """A radial wash behind everything, so the frame is not flat black.

    Drawn first because it is listed first. Painter order follows the array,
    and an explicit "z" would be worse than useless here: z is numbered across
    the whole file, so giving each scene its own 0/10/100 sorts objects into
    the neighbouring scene's slice — the backdrop of shot two lands in shot one
    and paints over its captions.

    A mesh is lit from the camera, which means the microphone's own falloff is
    the only gradient in shot; without something behind it the body floats in a
    void and the silhouette stops reading.
    """
    return {
        "id": "bg",
        "type": "rect",
        "x": CX, "y": CY, "anchor": "center",
        "w": W, "h": H,
        "gradient": {"kind": "radial", "from": HAZE, "to": INK},
    }


def text(oid, content, x, y, size, color, font=FONT, align="left", **kw):
    o = {
        "id": oid,
        "type": "text",
        "content": content,
        "font": font,
        "size": size,
        "color": color,
        "x": x,
        "y": y,
        "align": align,
    }
    o.update(kw)
    return o


# --------------------------------------------------------------------------
# Shot 1 — reveal
# --------------------------------------------------------------------------
D1 = 6.0

scene1 = {
    "id": "reveal",
    "duration_ms": int(D1 * 1000),
    "camera": {
        "perspective": FOCAL,
        # Straight in, slightly above, easing to a stop — the whole shot is one
        # move, so the microphone appears to grow into the frame rather than
        # being pushed at the viewer.
        "px": track([(0, 0), (D1, 0)]),
        "py": track([(0, -520), (D1, -90)]),
        "pz": track([(0, -3400), (D1, -2050)]),
        "tx": track([(0, 0), (D1, 0)]),
        "ty": track([(0, -40), (D1, -20)]),
        "tz": track([(0, 0), (D1, 0)]),
    },
    "effects": [{"type": "vignette", "amount": 0.42, "radius": 0.62, "softness": 0.6}],
    "objects": [
        backdrop(),
        mic(-28, 14, D1, opacity=track([(0, 0.0), (0.9, 1.0)])),
        text("t1", "GXL 066", 150, 700, 132, TEXT, FONT_BOLD,
             opacity=track([(0, 0), (1.1, 0), (2.0, 1.0)])),
        text("t2", "STUDIO CONDENSER", 158, 898, 40, ACCENT, FONT,
             opacity=track([(0, 0), (1.8, 0), (2.7, 1.0)])),
    ],
    "timeline": [],
}

# --------------------------------------------------------------------------
# Shot 2 — detail orbit, with captions
# --------------------------------------------------------------------------
D2 = 7.5
ORBIT_R = 1900.0
STEPS = 30

px, py, pz = [], [], []
for k in range(STEPS + 1):
    f = k / STEPS
    t = f * D2
    ang = math.radians(-46.0 + 92.0 * f)
    px.append((t, ORBIT_R * math.sin(ang)))
    pz.append((t, -ORBIT_R * math.cos(ang)))
    # Rises through the middle of the move, so the orbit is not a flat turntable.
    py.append((t, -60.0 - 360.0 * math.sin(math.pi * f)))

# Short lines on purpose. The captions sit in the left third and the
# microphone holds the centre for the whole orbit, so anything much longer
# runs onto its pale body — where a dim grey loses almost all its contrast and
# the tail of the sentence simply stops being readable.
FEATURES = [
    ("ორმაგი დიაფრაგმა", "34 მმ ოქროს კაფსულა"),
    ("ჩაშენებული ამორტიზატორი", "ვიბრაცია არ გადადის"),
    ("ფოლადის ორმაგი გისოსი", "სუფთა მაღალი სიხშირეები"),
]

feature_objs = []
for i, (title, sub) in enumerate(FEATURES):
    at = 0.8 + i * 2.1
    feature_objs.append(text(f"f{i}a", title, 140, 300 + 0, 52, TEXT, FONT_GE_BOLD,
                             opacity=fade(0.45, 1.25, 0.45, delay=at)))
    feature_objs.append(text(f"f{i}b", sub, 142, 382, 34, DIM, FONT_GE,
                             opacity=fade(0.45, 1.25, 0.45, delay=at + 0.15)))

scene2 = {
    "id": "detail",
    "duration_ms": int(D2 * 1000),
    "camera": {
        "perspective": FOCAL,
        "px": track(px, ease="linear"),
        "py": track(py, ease="linear"),
        "pz": track(pz, ease="linear"),
        "tx": track([(0, 0)]),
        "ty": track([(0, -30)]),
        "tz": track([(0, 0)]),
    },
    "effects": [{"type": "vignette", "amount": 0.45, "radius": 0.6, "softness": 0.6}],
    "objects": [backdrop(), mic(14, 74, D2)] + feature_objs,
    "timeline": [],
}

# --------------------------------------------------------------------------
# Shot 3 — hero and sign-off
# --------------------------------------------------------------------------
D3 = 6.5

scene3 = {
    "id": "close",
    "duration_ms": int(D3 * 1000),
    "camera": {
        "perspective": FOCAL,
        "px": track([(0, 1200), (D3, 0)]),
        "py": track([(0, -300), (D3, -110)]),
        "pz": track([(0, -1500), (D3, -2600)]),
        "tx": track([(0, 0)]),
        "ty": track([(0, -30)]),
        "tz": track([(0, 0)]),
    },
    "effects": [{"type": "vignette", "amount": 0.5, "radius": 0.58, "softness": 0.62}],
    "objects": [
        backdrop(),
        mic(74, 116, D3, y=CY + 80),
        text("c1", "GXL 066", CX, 128, 116, TEXT, FONT_BOLD, align="center",
             anchor="top",
             opacity=track([(0, 0), (1.0, 0), (2.0, 1.0)])),
        text("c2", "ხმა ისეთი, როგორიც არის", CX, 286, 44, ACCENT, FONT_GE,
             align="center", anchor="top",
             opacity=track([(0, 0), (1.6, 0), (2.6, 1.0)])),
        text("credit", CREDIT, CX, H - 74, 22, "#5C647A", FONT,
             align="center", anchor="top",
             opacity=track([(0, 0), (3.2, 0), (4.2, 0.9)])),
    ],
    "timeline": [],
}

project = {
    "_comment": "GXL 066 product film — a glTF import driven by camera moves. "
                "Generated by anim/build_mic_ad.py.",
    "_credit": CREDIT,
    "project": {"width": W, "height": H, "fps": FPS, "bg_color": INK},
    "output": {"encoder": "h264_nvenc", "preset": "p6", "cq": 19},
    "transition": {"use": "crossfade", "duration": 0.7},
    "scenes": [scene1, scene2, scene3],
}

out = os.path.join(HERE, "mic_ad.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(project, f, indent=1, ensure_ascii=False)
total = (D1 + D2 + D3)
print(f"wrote {out} — 3 shots, {total:g}s @ {FPS}fps")
