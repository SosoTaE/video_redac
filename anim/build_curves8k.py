#!/usr/bin/env python3
"""
Generates anim/curves8k.json — six scenes of parametric curves and particles.

Authored against a 1280x720 reference and multiplied by SCALE, so one design
renders at any resolution.  That matters more than it sounds: at 8K a 2px stroke
is invisible and a 26px caption is a smudge, so every *length* has to scale, not
only the coordinates.

    python3 anim/build_curves8k.py 6     # 7680x4320
    python3 anim/build_curves8k.py 1     # 1280x720, for quick previews
"""
import json, math, sys

SCALE = float(sys.argv[1]) if len(sys.argv) > 1 else 6.0
BW, BH = 1280, 720
W, H = int(BW * SCALE), int(BH * SCALE)
CX, CY = W / 2, H / 2

def s(v):
    return round(v * SCALE, 2)

GREEN, BLUE, PURPLE = "#7EE787", "#79C0FF", "#D2A8FF"
AMBER, RED, ORANGE  = "#FFD166", "#F85149", "#FFA657"
TEAL, PINK, DIM     = "#39D3BB", "#FF7B72", "#8B949E"

def caption(cid, txt, t0, t1):
    return {"id": cid, "type": "text", "content": txt,
            "x": "center", "y": "bottom-%d" % int(s(46)),
            "size": int(s(26)), "color": DIM, "font": "Inter", "anchor": "bottom",
            "opacity": [{"t": t0, "v": 0}, {"t": t0 + 0.7, "v": 1},
                        {"t": t1 - 0.7, "v": 1}, {"t": t1, "v": 0}]}

def tracks(fn, dur, n=150):
    kx, ky = [], []
    for i in range(n + 1):
        u = i / n
        x, y = fn(u)
        kx.append({"t": round(u * dur, 4), "v": round(x, 1), "ease": "linear"})
        ky.append({"t": round(u * dur, 4), "v": round(y, 1), "ease": "linear"})
    return kx, ky

def points(fn, n=600):
    return [[round(v, 1) for v in fn(i / n)] for i in range(n + 1)]

scenes = []

# ------------------------------------------------------------- 1. harmonograph
D = 8.0
def harmo(f1, f2, f3, f4, p1, p2, A, B, damp):
    def fn(u):
        t   = u * 2 * math.pi * 3
        dec = math.exp(-damp * u * 3)
        return (CX + A * math.sin(f1 * t + p1) * dec + A * 0.45 * math.sin(f2 * t) * dec,
                CY + B * math.sin(f3 * t + p2) * dec + B * 0.45 * math.sin(f4 * t) * dec)
    return fn

objs, tl = [], []
for i, (pars, cols) in enumerate([
        ((2, 3, 3, 2, 0.0,        math.pi / 2, s(430), s(250), 0.16), [GREEN,  "#3FB950"]),
        ((3, 4, 2, 5, math.pi / 3, 0.0,        s(390), s(225), 0.20), [BLUE,   "#1F6FEB"]),
        ((5, 2, 4, 3, math.pi / 5, math.pi / 4, s(340), s(200), 0.24), [PURPLE, "#A371F7"])]):
    fn = harmo(*pars)
    objs.append({"id": "hc%d" % i, "type": "path", "points": points(fn),
                 "stroke": cols[0] + "44", "width": s(1.6), "fill": "none", "cap": "round",
                 "trim": [{"t": 0.3 + 0.3 * i, "v": 0},
                          {"t": 5.0 + 0.3 * i, "v": 1, "ease": "cubicinout"}]})
    objs.append({"id": "hd%d" % i, "type": "circle", "radius": s(8 - i * 1.4),
                 "x": CX, "y": CY, "color": cols[0], "blend": "add",
                 "repeat": {"count": 20, "layout": "stack", "stagger_ms": 34,
                            "color_cycle": cols}})
    kx, ky = tracks(fn, D)
    tl += [{"action": "animate", "target": "hd%d" % i, "property": "x",
            "time_ms": int(280 * i), "keys": kx},
           {"action": "animate", "target": "hd%d" % i, "property": "y",
            "time_ms": int(280 * i), "keys": ky}]
objs.append(caption("c1", "harmonograph", 0.2, D - 0.1))
scenes.append({"id": "harmonograph", "duration_ms": int(D * 1000),
               "objects": objs, "timeline": tl,
               "camera": {"zoom": [{"t": 0, "v": 1.10}, {"t": D, "v": 1.0, "ease": "cubicinout"}],
                          "rotation": [{"t": 0, "v": -3}, {"t": D, "v": 3, "ease": "easeinout"}]}})

# ------------------------------------------------------------- 2. phyllotaxis
D = 7.0
N = 560
GOLD = 137.507764
objs, tl = [], []
pal = [AMBER, ORANGE, GREEN, TEAL, BLUE, PURPLE]
for n in range(N):
    r = s(14.6) * math.sqrt(n)
    objs.append({"id": "p%d" % n, "type": "circle",
                 "radius": s(2.0 + 4.2 * (n / N) ** 0.6),
                 "x": CX, "y": CY, "anchor": "center", "blend": "add",
                 "color": pal[n % len(pal)],
                 "opacity": [{"t": 0.007 * n, "v": 0}, {"t": 0.007 * n + 0.4, "v": 1}]})
    # each seed spirals out to its own place on the golden-angle lattice
    tl.append({"action": "orbit", "target": "p%d" % n, "cx": "center", "cy": "center",
               "radius": 0, "radius_to": r, "from_angle": (n * GOLD) - 220,
               "sweep": 220, "time_ms": int(7 * n), "duration_ms": 2600,
               "ease": "cubicout"})
objs.append(caption("c2", "phyllotaxis · golden angle", 0.2, D - 0.1))
scenes.append({"id": "phyllotaxis", "duration_ms": int(D * 1000),
               "objects": objs, "timeline": tl,
               "camera": {"zoom": [{"t": 0, "v": 1.35}, {"t": 4.5, "v": 1.0, "ease": "cubicinout"}],
                          "rotation": [{"t": 0, "v": 0}, {"t": D, "v": 14, "ease": "easeinout"}]}})

# ------------------------------------------------------------- 3. rose curves
D = 7.0
def rose(k, a, phase=0.0):
    def fn(u):
        th = u * 2 * math.pi * (2 if (k % 2 == 0) else 1)
        rr = a * math.cos(k * th + phase)
        return (CX + rr * math.cos(th), CY + rr * math.sin(th) * (BH / BW))
    return fn

objs, tl = [], []
for i, (k, a, col) in enumerate([(3, s(400), GREEN), (5, s(340), BLUE),
                                 (7, s(280), PURPLE), (4, s(210), AMBER)]):
    fn = rose(k, a)
    objs.append({"id": "r%d" % i, "type": "path", "points": points(fn),
                 "stroke": col, "width": s(2.2), "fill": "none", "cap": "round",
                 "blend": "add", "group": "roses",
                 "trim": [{"t": 0.2 + 0.5 * i, "v": 0},
                          {"t": 3.4 + 0.5 * i, "v": 1, "ease": "cubicinout"}]})
    objs.append({"id": "rd%d" % i, "type": "circle", "radius": s(7 - i),
                 "x": CX, "y": CY, "color": col, "blend": "add", "group": "roses",
                 "repeat": {"count": 14, "layout": "stack", "stagger_ms": 30,
                            "color_cycle": [col]}})
    kx, ky = tracks(fn, D - 0.6)
    tl += [{"action": "animate", "target": "rd%d" % i, "property": "x",
            "time_ms": int(400 * i), "keys": kx},
           {"action": "animate", "target": "rd%d" % i, "property": "y",
            "time_ms": int(400 * i), "keys": ky}]
tl.append({"action": "rotate", "target": "roses", "value": 40,
           "time_ms": 0, "duration_ms": int(D * 1000), "ease": "easeinout"})
objs.append(caption("c3", "rose curves  r = a·cos(kθ)", 0.2, D - 0.1))
scenes.append({"id": "roses", "duration_ms": int(D * 1000),
               "groups": [{"id": "roses", "pivot_x": "center", "pivot_y": "center"}],
               "objects": objs, "timeline": tl})

# ------------------------------------------------------------- 4. torus knot
D = 7.0
def knot(p, q, R, r):
    def fn(u):
        th = u * 2 * math.pi * q
        rad = R + r * math.cos(p * th / q)
        return (CX + rad * math.cos(th), CY + rad * math.sin(th) * 0.62)
    return fn

objs, tl = [], []
for i, (p, q, col) in enumerate([(3, 7, TEAL), (2, 5, PINK), (5, 8, AMBER)]):
    fn = knot(p, q, s(300 - i * 55), s(120 - i * 22))
    objs.append({"id": "k%d" % i, "type": "path", "points": points(fn, 900),
                 "stroke": col, "width": s(2.0), "fill": "none", "cap": "round",
                 "blend": "add",
                 "trim": [{"t": 0.3 + 0.6 * i, "v": 0},
                          {"t": 4.6 + 0.6 * i, "v": 1, "ease": "cubicinout"}]})
    objs.append({"id": "kd%d" % i, "type": "circle", "radius": s(6),
                 "x": CX, "y": CY, "color": col, "blend": "add",
                 "repeat": {"count": 16, "layout": "stack", "stagger_ms": 26,
                            "color_cycle": [col, "#FFFFFF"]}})
    kx, ky = tracks(fn, D - 0.5, n=220)
    tl += [{"action": "animate", "target": "kd%d" % i, "property": "x",
            "time_ms": int(300 * i), "keys": kx},
           {"action": "animate", "target": "kd%d" % i, "property": "y",
            "time_ms": int(300 * i), "keys": ky}]
objs.append(caption("c4", "torus knots  (p,q)", 0.2, D - 0.1))
scenes.append({"id": "knots", "duration_ms": int(D * 1000), "objects": objs, "timeline": tl,
               "camera": {"rotation": [{"t": 0, "v": -8}, {"t": D, "v": 8, "ease": "easeinout"}],
                          "zoom": [{"t": 0, "v": 1.0}, {"t": D, "v": 1.06, "ease": "easeinout"}]}})

# ------------------------------------------------------------- 5. converge + burst
D = 6.5
objs, tl = [], []
cols = [AMBER, ORANGE, RED, PINK, BLUE, GREEN, PURPLE, TEAL]
for i, col in enumerate(cols):
    objs.append({"id": "sp%d" % i, "type": "rect", "w": s(54), "h": s(5),
                 "corner_radius": s(3), "x": CX, "y": CY, "anchor": "center",
                 "blend": "add", "color": col,
                 "repeat": {"count": 12, "layout": "stack", "stagger_ms": 32,
                            "color_cycle": [col]}})
    tl.append({"action": "orbit", "target": "sp%d" % i, "cx": "center", "cy": "center",
               "radius": s(540), "radius_to": s(38), "from_angle": 360.0 * i / len(cols),
               "sweep": 780, "orient": True, "time_ms": 0, "duration_ms": 4200,
               "ease": "cubicinout"})
objs.append({"id": "core", "type": "circle", "radius": s(30), "x": "center", "y": "center",
             "anchor": "center", "fill": "none", "stroke": AMBER, "stroke_width": s(3),
             "glow": {"blur": s(30), "color": AMBER + "88"},
             "opacity": [{"t": 0, "v": 0}, {"t": 3.6, "v": 0}, {"t": 4.2, "v": 1}],
             "scale": [{"t": 3.9, "v": 0.2}, {"t": 4.5, "v": 1.6, "ease": "backout"},
                       {"t": 5.3, "v": 1.0}]})
objs.append({"id": "burst", "type": "circle", "radius": s(4), "color": AMBER, "blend": "add",
             "emitter": {"count": 900, "seed": 11, "cx": "center", "cy": "center",
                         "spread": s(10), "speed": [s(150), s(700)], "angle": [0, 360],
                         "life": [0.7, 1.9], "emit_ms": 200, "start_ms": 4200,
                         "gravity": s(40), "size_jitter": [0.4, 1.6], "fade": 0.55,
                         "color_cycle": [AMBER, ORANGE, RED, "#FFFFFF"]}})
objs.append(caption("c5", "spiral · converge", 0.2, D - 0.1))
scenes.append({"id": "converge", "duration_ms": int(D * 1000), "objects": objs, "timeline": tl,
               "camera": {"zoom": [{"t": 0, "v": 1.0}, {"t": 4.2, "v": 1.0},
                                   {"t": 4.5, "v": 1.14, "ease": "cubicout"},
                                   {"t": 5.7, "v": 1.0, "ease": "cubicinout"}],
                          "shake": [{"t": 4.2, "v": 0}, {"t": 4.35, "v": s(9)},
                                    {"t": 5.1, "v": 0}]}})

# ------------------------------------------------------------- 6. kaleidoscope
D = 7.5
def leaf(R, K):
    return ("M %.0f %.0f C %.0f %.0f %.0f %.0f %.0f %.0f C %.0f %.0f %.0f %.0f %.0f %.0f Z"
            % (CX - R, CY, CX - R * .35, CY - K, CX + R * .35, CY - K, CX + R, CY,
               CX + R * .35, CY + K, CX - R * .35, CY + K, CX - R, CY))

objs = [
    {"id": "outer", "type": "path", "d": leaf(s(300), s(112)), "group": "kalA",
     "stroke": GREEN, "width": s(2.2), "fill": "none", "cap": "round", "blend": "add",
     "trim": [{"t": 0, "v": 0}, {"t": 1.8, "v": 1, "ease": "cubicinout"}],
     "repeat": {"count": 14, "layout": "stack", "rotate_step": 180.0 / 14,
                "color_cycle": [GREEN, BLUE, PURPLE, ORANGE]}},
    {"id": "inner", "type": "path", "d": leaf(s(180), s(66)), "group": "kalB",
     "stroke": AMBER, "width": s(1.8), "fill": "none", "cap": "round", "blend": "add",
     "trim": [{"t": 0.6, "v": 0}, {"t": 2.6, "v": 1, "ease": "cubicinout"}],
     "repeat": {"count": 10, "layout": "stack", "rotate_step": 180.0 / 10,
                "color_cycle": [AMBER, PINK, TEAL]}},
    {"id": "mote", "type": "circle", "radius": s(3), "color": "#FFFFFF", "blend": "add",
     "emitter": {"count": 420, "seed": 5, "cx": "center", "cy": "center",
                 "spread": s(330), "speed": [s(8), s(55)], "angle": [0, 360],
                 "life": [2.2, 4.5], "emit_ms": 3200, "fade": 0.7,
                 "size_jitter": [0.4, 1.7],
                 "color_cycle": ["#FFFFFF", GREEN, BLUE, PURPLE]}},
    caption("c6", "kaleidoscope", 0.3, D - 0.1),
]
tl = [
    {"action": "rotate", "target": "kalA", "value":  320, "time_ms": 0,
     "duration_ms": int(D * 1000), "ease": "cubicinout"},
    {"action": "rotate", "target": "kalB", "value": -420, "time_ms": 0,
     "duration_ms": int(D * 1000), "ease": "cubicinout"},
    {"action": "scale",  "target": "kalA", "value": 1.12, "time_ms": 0,
     "duration_ms": 3400, "ease": "easeinout"},
]
scenes.append({"id": "kaleidoscope", "duration_ms": int(D * 1000),
               "groups": [{"id": "kalA", "pivot_x": "center", "pivot_y": "center"},
                          {"id": "kalB", "pivot_x": "center", "pivot_y": "center"}],
               "objects": objs, "timeline": tl})

doc = {
    "_comment": "Six scenes of parametric curves. Generated by anim/build_curves8k.py.",
    "project": {"width": W, "height": H, "fps": 60, "bg_color": "#05060A"},
    # H.264 cannot encode 8K on NVENC ("No capable devices found"); HEVC can.
    "output": {"encoder": "hevc_nvenc", "preset": "p6", "cq": 22},
    "transition": {"use": "crossfade", "duration": 0.8},
    "scenes": scenes,
}
out = "anim/curves8k.json" if SCALE > 1.5 else "anim/curves_preview.json"
json.dump(doc, open(out, "w"), indent=1)
print("%s  %dx%d  scenes=%d" % (out, W, H, len(scenes)))
