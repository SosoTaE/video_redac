#!/usr/bin/env python3
"""
Generates anim/solar.json — a 3D solar system.

Every body is the renderer's own `sphere` primitive wearing an equirectangular
map from Solar System Scope (CC BY 4.0). That is deliberate, and worth saying
plainly because the obvious move is to download a planet *model*: a planet model
is a sphere plus exactly these maps, so importing one would mean re-deriving a
sphere we can already tessellate to any density, at a fixed resolution someone
else chose. The texture is the part that carries the information.

Three things make it read as a solar system rather than as balls in a row:

  * a point light at the Sun, so every body has a terminator and a night side
  * a shared depth buffer, so a moon passes behind its planet and Saturn's
    rings cross in front of and behind the globe in the same frame
  * orbits generated as keyframes, because an ellipse typed by hand drifts

Scale is compressed, as it must be: at true proportions Neptune's orbit is
about 6000 times the Sun's radius and every planet is a subpixel. Relative
*order* is kept — the sizes and orbits rank correctly even though the ratios do
not.

Attribution, required by the textures' licence:
  Solar textures by Solar System Scope (solarsystemscope.com), CC BY 4.0.
"""

import json
import math
import os

W, H = 1920, 1080
CX, CY = W // 2, H // 2
FPS = 60
FOCAL = 1500.0

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TEX = os.path.join(ROOT, "assets", "planets")
GEN = os.path.join(TEX, "gen")

CREDIT = "Solar textures — Solar System Scope (CC BY 4.0)"

FONT = "Noto Sans"
FONT_BOLD = "Noto Sans-Bold"
FONT_GE = "DejaVu Sans"          # covers Georgian *and* punctuation; Noto's does not
FONT_GE_BOLD = "DejaVu Sans-Bold"

INK = "#02030A"
TEXT = "#EDF1F8"
DIM = "#94A0BC"
ACCENT = "#F0B357"


# --------------------------------------------------------------------------
# Textures
# --------------------------------------------------------------------------
def prepare(src, width):
    """Downscales a map to `width` and returns the path to the copy.

    Sampling is bilinear now, which fixes magnification, but there are still no
    mipmaps — so a 2048-wide map on a 160-pixel planet is being *minified* by a
    factor of thirteen, and bilinear only ever averages four texels of the
    thirteen-by-thirteen block it should. Pre-scaling to roughly the size the
    body is actually drawn at is what covers the rest, and it stays until
    mipmaps exist.
    """
    from PIL import Image

    os.makedirs(GEN, exist_ok=True)
    base = os.path.splitext(os.path.basename(src))[0]
    out = os.path.join(GEN, f"{base}_{width}.png")

    if not os.path.exists(out):
        im = Image.open(os.path.join(TEX, src))
        if im.width > width:
            im = im.resize((width, max(1, im.height * width // im.width)),
                           Image.LANCZOS)
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


# --------------------------------------------------------------------------
# The system
# --------------------------------------------------------------------------
#  name        texture                    orbit  size  period  spin  tilt  texw
BODIES = [
    ("mercury", "2k_mercury.jpg",           980,   82,   6.0,   9.0,   0,   512),
    ("venus",   "2k_venus_atmosphere.jpg", 1320,  140,   9.5,  16.0,  -3,   512),
    ("earth",   "2k_earth_daymap.jpg",     1760,  152,  13.0,   5.0, -23,   768),
    ("mars",    "2k_mars.jpg",             2260,  108,  18.0,   5.5, -25,   512),
    ("jupiter", "2k_jupiter.jpg",          3080,  360,  30.0,   3.0,  -3,  1024),
    ("saturn",  "2k_saturn.jpg",           3860,  300,  44.0,   3.4, -27,  1024),
    ("uranus",  "2k_uranus.jpg",           4560,  208,  60.0,   6.0, -82,   512),
    ("neptune", "2k_neptune.jpg",          5180,  200,  76.0,   6.4, -28,   512),
]

SUN_SIZE = 760
STEPS_PER_ORBIT = 48


def orbit_tracks(radius, period, duration, phase, centre=(0.0, 0.0)):
    """Circular orbit in the XZ plane, as x / z_depth keyframes.

    Sampled rather than expressed as a formula because the renderer's tracks are
    piecewise interpolations of keyframes — a circle has to be *drawn* into
    them. Enough steps per revolution that the polygon is not visible.
    """
    turns = max(1.0, duration / period)
    n = max(8, int(STEPS_PER_ORBIT * turns))

    xs, zs = [], []
    for k in range(n + 1):
        t = duration * k / n
        a = phase + 2.0 * math.pi * t / period
        xs.append((t, centre[0] + radius * math.cos(a)))
        zs.append((t, centre[1] + radius * math.sin(a)))
    return track(xs), track(zs)


def body(oid, tex, size, x, z, *, ambient=0.08, spin=None, tilt=0.0,
         shape="sphere", duration=1.0, **kw):
    o = {
        "id": oid,
        "type": "mesh",
        "shape": shape,
        "anchor": "center",
        "y": CY,
        "size": size,
        "color": "#FFFFFF",
        "ambient": ambient,
        "texture": tex,
    }
    # x and z_depth arrive either as a constant or as a whole track.
    o["x"] = ([{"t": p["t"], "v": CX + p["v"], "ease": p.get("ease", "linear")}
               for p in x] if isinstance(x, list) else CX + x)
    o["z_depth"] = z
    if spin is not None:
        o["rotate_y"] = track([(0, 0), (duration, 360.0 * duration / spin)])
    if tilt:
        # The axial tilt goes on `rotation`, not `rotate_x`. The model matrix is
        # Rz·Ry·Rx, so a tilt in Rx is applied *before* the spin and the planet
        # precesses instead of turning; in Rz it is applied after, and the spin
        # axis tilts with it, which is what an axial tilt actually is.
        o["rotation"] = tilt
    o.update(kw)
    return o


def sky(duration):
    """A star sphere enclosing everything.

    The camera sits inside it, so what is visible is the *far* wall — the near
    wall falls behind the eye and is rejected before it is ever rasterized.
    `cull` therefore has to be off: the inward-facing side is exactly the side
    being looked at.
    """
    return {
        "id": "sky",
        "type": "mesh",
        "shape": "sphere",
        "x": CX, "y": CY, "z_depth": 0,
        "anchor": "center",
        "size": 26000,
        "color": "#FFFFFF",
        "ambient": 1.0,          # emissive: starlight is not lit by the Sun
        "cull": False,
        "smooth": True,
        "texture": prepare("2k_stars_milky_way.jpg", 2048),
        "opacity": 0.55,
    }


def sun(duration, ambient=1.0):
    return {
        "id": "sun",
        "type": "mesh",
        "shape": "sphere",
        "x": CX, "y": CY, "z_depth": 0,
        "anchor": "center",
        "size": SUN_SIZE,
        "color": "#FFFFFF",
        "ambient": ambient,      # the light source cannot be lit by itself
        "texture": prepare("2k_sun.jpg", 1024),
        "rotate_y": track([(0, 0), (duration, 360.0 * duration / 40.0)]),
    }


def planets(duration, only=None, scale=1.0):
    """Every body's orbit, spin and tilt, plus Saturn's rings and Earth's moon."""
    out = []
    for i, (name, tex, orbit, size, period, spin, tilt, texw) in enumerate(BODIES):
        if only is not None and name not in only:
            continue

        r = orbit * scale
        # Spread the starting angles so the planets never line up into a row.
        phase = 0.9 * i + 0.4
        xs, zs = orbit_tracks(r, period, duration, phase)

        out.append(body(name, prepare(tex, texw), size, xs, zs,
                        spin=spin, tilt=tilt, duration=duration))

        if name == "saturn":
            # The rings share the globe's orbit exactly, so they take the same
            # tracks. No spin: the map is a radial strip, identical all the way
            # round, so turning it would cost work and change nothing.
            out.append(body("saturn_rings",
                            prepare("2k_saturn_ring_alpha.png", 1024),
                            size * 2.35, xs, zs,
                            shape="ring", ambient=0.16, tilt=tilt,
                            duration=duration))

        if name == "earth":
            # Just clear of the globe: any wider and the moon swings out of
            # frame in the close shot, where the camera sits only a few planet
            # radii away and a wide orbit is mostly spent off-screen.
            mxs, mzs = orbit_tracks(size * 1.15, 5.5, duration, 0.0)
            moon_x = [{"t": a["t"], "v": a["v"] + b["v"] - CX, "ease": "linear"}
                      for a, b in zip(xs, mxs)] if len(xs) == len(mxs) else xs
            # The moon's own circle is added to the planet's, so it orbits the
            # Earth while the Earth orbits the Sun. Sampling both at the same
            # instants is what lets the two be summed keyframe by keyframe.
            n = max(len(xs), len(mxs))
            mx = resample(xs, n)
            mz = resample(zs, n)
            ox = resample(mxs, n)
            oz = resample(mzs, n)
            moon_x = [{"t": a["t"], "v": a["v"] + b["v"] - CX, "ease": "linear"}
                      for a, b in zip(mx, ox)]
            moon_z = [{"t": a["t"], "v": a["v"] + b["v"], "ease": "linear"}
                      for a, b in zip(mz, oz)]
            out.append(body("moon", prepare("2k_moon.jpg", 512), 54,
                            [{"t": p["t"], "v": p["v"] - CX} for p in moon_x],
                            moon_z, spin=5.5, duration=duration))
    return out


def resample(kf, n):
    """Re-samples a keyframe list onto n+1 evenly spaced times."""
    if len(kf) == n + 1:
        return kf
    dur = kf[-1]["t"]
    out = []
    for k in range(n + 1):
        t = dur * k / n
        # linear search is fine: these lists are a few hundred entries at most
        j = 0
        while j + 2 < len(kf) and kf[j + 1]["t"] < t:
            j += 1
        a, b = kf[j], kf[min(j + 1, len(kf) - 1)]
        f = 0.0 if b["t"] == a["t"] else (t - a["t"]) / (b["t"] - a["t"])
        out.append({"t": round(t, 4), "v": a["v"] + (b["v"] - a["v"]) * f,
                    "ease": "linear"})
    return out


def text(oid, content, x, y, size, color, font=FONT, align="left", **kw):
    o = {"id": oid, "type": "text", "content": content, "font": font,
         "size": size, "color": color, "x": x, "y": y, "align": align}
    o.update(kw)
    return o


def fade(t_in, hold, t_out=0.6, delay=0.0):
    return [{"t": round(v[0], 4), "v": v[1], "ease": "cubicinout"} for v in [
        (delay, 0.0), (delay + t_in, 1.0),
        (delay + t_in + hold, 1.0), (delay + t_in + hold + t_out, 0.0)]]


def arc(duration, radius, y_from, y_to, a_from, a_to, steps=40,
        centre=(0.0, 0.0)):
    """A camera arc at fixed radius about `centre` — framing holds while the
    viewpoint moves. The centre has to be the same point the camera looks at,
    or the subject swings across the frame as the arc goes round."""
    px, py, pz = [], [], []
    for k in range(steps + 1):
        f = k / steps
        t = duration * f
        a = math.radians(a_from + (a_to - a_from) * f)
        px.append((t, centre[0] + radius * math.sin(a)))
        pz.append((t, centre[1] - radius * math.cos(a)))
        py.append((t, y_from + (y_to - y_from) * f))
    return {"px": track(px), "py": track(py), "pz": track(pz)}


def follow(duration, xs, zs, radius, y_from, y_to, a_from, a_to):
    """A camera that rides along with a moving body.

    The look-at is the body's own orbit track and the eye is that same track
    plus a slowly turning offset, so the subject sits still in frame while the
    background — here, the Sun — sweeps behind it. Sampling the eye at exactly
    the body's keyframe times is what keeps the two locked together; interpolate
    them on different grids and the subject drifts.
    """
    n = len(xs) - 1
    px, py, pz, tx, tz = [], [], [], [], []
    for k, (kx, kz) in enumerate(zip(xs, zs)):
        f = k / n
        t = kx["t"]
        a = math.radians(a_from + (a_to - a_from) * f)
        px.append((t, kx["v"] + radius * math.sin(a)))
        pz.append((t, kz["v"] - radius * math.cos(a)))
        py.append((t, y_from + (y_to - y_from) * f))
        tx.append((t, kx["v"]))
        tz.append((t, kz["v"]))
    return {"px": track(px), "py": track(py), "pz": track(pz),
            "tx": track(tx), "ty": track([(0, 0)]), "tz": track(tz)}


# --------------------------------------------------------------------------
# Shot 1 — the whole system, from above the plane
# --------------------------------------------------------------------------
D1 = 16.0
cam1 = arc(D1, 8200.0, -4200.0, -1500.0, -34.0, 26.0)
scene1 = {
    "id": "system",
    "duration_ms": int(D1 * 1000),
    "camera": dict({"perspective": FOCAL,
                    "tx": track([(0, 0)]), "ty": track([(0, 0)]),
                    "tz": track([(0, 0)])}, **cam1),
    "light": {"x": 0, "y": 0, "z": 0},
    "objects": [sky(D1), sun(D1)] + planets(D1) + [
        text("h1", "მზის სისტემა", 140, 120, 96, TEXT, FONT_GE_BOLD,
             opacity=[{"t": 0, "v": 0}, {"t": 1.2, "v": 0, "ease": "cubicinout"},
                      {"t": 2.4, "v": 1.0, "ease": "cubicinout"},
                      {"t": 11.0, "v": 1.0, "ease": "cubicinout"},
                      {"t": 12.6, "v": 0.0, "ease": "cubicinout"}]),
        text("h2", "რვა პლანეტა, ერთი ვარსკვლავი", 146, 246, 40, ACCENT, FONT_GE,
             opacity=[{"t": 0, "v": 0}, {"t": 2.0, "v": 0, "ease": "cubicinout"},
                      {"t": 3.2, "v": 1.0, "ease": "cubicinout"},
                      {"t": 11.0, "v": 1.0, "ease": "cubicinout"},
                      {"t": 12.6, "v": 0.0, "ease": "cubicinout"}]),
    ],
    "timeline": [],
}

# --------------------------------------------------------------------------
# Shot 2 — riding with the Earth
# --------------------------------------------------------------------------
D2 = 11.0
E_SCALE = 0.62
E_ORBIT, E_SIZE, E_PERIOD = 1760 * E_SCALE, 152, 13.0
e_xs, e_zs = orbit_tracks(E_ORBIT, E_PERIOD, D2, 0.9 * 2 + 0.4)
cam2 = follow(D2, e_xs, e_zs, 520.0, -260.0, -120.0, 24.0, 128.0)

scene2 = {
    "id": "inner",
    "duration_ms": int(D2 * 1000),
    "camera": dict({"perspective": FOCAL}, **cam2),
    "light": {"x": 0, "y": 0, "z": 0},
    "objects": [sky(D2), sun(D2)]
               + planets(D2, only={"earth", "venus", "mars"}, scale=E_SCALE)
               + [
        text("i1", "დედამიწა", 140, 790, 68, TEXT, FONT_GE_BOLD,
             opacity=fade(0.7, 3.0, 0.7, delay=0.9)),
        text("i2", "ერთადერთი, სადაც ვიცით რომ სიცოცხლეა", 144, 886, 34, DIM,
             FONT_GE, opacity=fade(0.7, 3.0, 0.7, delay=1.2)),
        text("i3", "მთვარე — ჩვენი ერთადერთი თანამგზავრი", 140, 886, 34, DIM,
             FONT_GE, opacity=fade(0.7, 2.4, 0.7, delay=6.6)),
    ],
    "timeline": [],
}

# --------------------------------------------------------------------------
# Shot 3 — Saturn
# --------------------------------------------------------------------------
D3 = 10.0
# The elevation stays well above the ring plane for the whole move. Drop toward
# it and the rings close to a hairline — technically correct, and it throws away
# the only thing the shot is about.
cam3 = arc(D3, 1560.0, -900.0, -560.0, -40.0, 30.0)
sat_x, sat_z = orbit_tracks(0.0, 60.0, D3, 0.0)
scene3 = {
    "id": "saturn",
    "duration_ms": int(D3 * 1000),
    "camera": dict({"perspective": FOCAL,
                    "tx": track([(0, 0)]), "ty": track([(0, 0)]),
                    "tz": track([(0, 0)])}, **cam3),
    "light": {"x": -2000, "y": -1300, "z": -3000},
    "objects": [
        sky(D3),
        body("saturn", prepare("2k_saturn.jpg", 1024), 460, 0.0, 0,
             spin=14.0, tilt=-27, duration=D3, ambient=0.10),
        body("saturn_rings", prepare("2k_saturn_ring_alpha.png", 1024),
             460 * 2.35, 0.0, 0, shape="ring", ambient=0.18, tilt=-27,
             duration=D3),
        text("s1", "სატურნი", CX, 120, 84, TEXT, FONT_GE_BOLD,
             align="center", anchor="top",
             opacity=fade(0.8, 5.0, 0.8, delay=0.9)),
        text("s2", "რგოლები — ყინული და ქვა", CX, 240, 36, ACCENT, FONT_GE,
             align="center", anchor="top",
             opacity=fade(0.8, 4.6, 0.8, delay=1.3)),
        text("credit", CREDIT, CX, H - 70, 22, "#5A6178", FONT,
             align="center", anchor="top",
             opacity=[{"t": 0, "v": 0}, {"t": 6.0, "v": 0, "ease": "cubicinout"},
                      {"t": 7.4, "v": 0.9, "ease": "cubicinout"}]),
    ],
    "timeline": [],
}

project = {
    "_comment": "A 3D solar system: textured spheres, a point light at the Sun, "
                "and a depth buffer. Generated by anim/build_solar.py.",
    "_credit": CREDIT,
    "project": {"width": W, "height": H, "fps": FPS, "bg_color": INK},
    "output": {"encoder": "h264_nvenc", "preset": "p6", "cq": 20},
    "transition": {"use": "crossfade", "duration": 0.8},
    "scenes": [scene1, scene2, scene3],
}

out = os.path.join(HERE, "solar.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(project, f, indent=1, ensure_ascii=False)

total = D1 + D2 + D3
n = sum(len(s["objects"]) for s in project["scenes"])
print(f"wrote {out} — 3 shots, {total:g}s @ {FPS}fps, {n} objects")
