#!/usr/bin/env python3
"""
Generates anim/space3d.json — perspective-projected layers.

Every layer is still a flat quad; what is new is that the quad is placed in
space and projected.  That is enough for a carousel, a receding tunnel and a
depth-sorted particle field, and it composes with everything else — the cards
below carry gradients, the motes use additive blend, the whole thing sits under
a camera.

    python3 anim/build_3d.py 1     # 1280x720
    python3 anim/build_3d.py 3     # 4K
"""
import json, math, sys

SCALE = float(sys.argv[1]) if len(sys.argv) > 1 else 1.0
BW, BH = 1280, 720
W, H = int(BW * SCALE), int(BH * SCALE)
CX, CY = W / 2, H / 2
def s(v): return round(v * SCALE, 2)

GREEN, BLUE, PURPLE = "#7EE787", "#79C0FF", "#D2A8FF"
AMBER, RED, TEAL, PINK = "#FFD166", "#F85149", "#39D3BB", "#FF7B72"
DIM = "#8B949E"
FOCAL = s(1500)

def caption(cid, txt, t0, t1):
    return {"id": cid, "type": "text", "content": txt, "x": "center",
            "y": "bottom-%d" % int(s(40)), "size": int(s(26)), "color": DIM,
            "font": "Inter", "anchor": "bottom",
            "opacity": [{"t": t0, "v": 0}, {"t": t0 + .6, "v": 1},
                        {"t": t1 - .6, "v": 1}, {"t": t1, "v": 0}]}

def track(vals, dur, n):
    return [{"t": round(i * dur / n, 4), "v": round(vals(i / n), 2), "ease": "linear"}
            for i in range(n + 1)]

scenes = []

# ------------------------------------------------------------ 1. carousel
D, N, R = 9.0, 14, s(430)
objs, tl = [], []
pal = [GREEN, BLUE, PURPLE, AMBER, TEAL, PINK]
for i in range(N):
    base = 2 * math.pi * i / N
    # A card orbits the vertical axis: x and z trace a circle, and rotate_y
    # turns it to keep facing outward.  Three tracks, no new machinery.
    fx = lambda u, b=base: CX + R * math.sin(b + 2 * math.pi * u)
    fz = lambda u, b=base:      R * math.cos(b + 2 * math.pi * u)
    fr = lambda u, b=base: -math.degrees(b + 2 * math.pi * u)
    objs.append({"id": "card%d" % i, "type": "rect",
                 "w": s(210), "h": s(300), "corner_radius": s(14),
                 "x": CX, "y": CY, "anchor": "center",
                 "gradient": {"kind": "linear", "from": pal[i % len(pal)],
                              "to": pal[(i + 3) % len(pal)], "angle": 70},
                 # the far side of the ring faces away; hiding it stops those
                 # cards showing through the near ones, and `shading` gives the
                 # turn the falloff a flat quad has none of
                 "backface": "hide", "shading": 0.75})
    tl += [{"action": "animate", "target": "card%d" % i, "property": "x",
            "keys": track(fx, D, 90)},
           {"action": "animate", "target": "card%d" % i, "property": "z",
            "keys": track(fz, D, 90)},
           # rotate_y, not rotation: a carousel card turns about the vertical
           # axis to face outward. `rotation` spins it in the screen plane,
           # which leaves the surface normal pointing straight at the viewer —
           # so nothing ever faces away and both culling and shading are inert.
           {"action": "animate", "target": "card%d" % i, "property": "rotate_y",
            "keys": track(fr, D, 90)}]
objs.append(caption("c1", "carousel · 14 layers in depth", .2, D - .1))
scenes.append({"id": "carousel", "duration_ms": int(D * 1000),
               "camera": {"perspective": FOCAL,
                          "zoom": [{"t": 0, "v": 1.18}, {"t": D, "v": 1.0, "ease": "cubicinout"}]},
               "objects": objs, "timeline": tl})

# ------------------------------------------------------------ 2. tunnel
D, RINGS = 8.0, 22
Z_FAR, Z_NEAR = s(2800), s(-260)
PERIOD = 3.4                     # seconds for one ring to traverse the tunnel
objs, tl = [], []

def sawtooth(phase):
    """
    z as a repeating ramp from Z_FAR to Z_NEAR.

    Each wrap is written as two keys a millisecond apart, so the ring jumps back
    to the far end instead of streaking across the frame — a single key would be
    interpolated like any other move.
    """
    kz, ka, t = [], [], 0.0
    u = phase                    # progress through this ring's current pass
    while t <= D + 0.01:
        z = Z_FAR + (Z_NEAR - Z_FAR) * u
        kz.append({"t": round(t, 4), "v": round(z, 1), "ease": "linear"})
        # fade in at the far end, out as it sweeps past the viewer
        ka.append({"t": round(t, 4),
                   "v": round(min(1.0, u * 6.0) * min(1.0, (1.0 - u) * 5.0), 3),
                   "ease": "linear"})
        step = min(0.05, (1.0 - u) * PERIOD)
        if step <= 1e-4:
            kz.append({"t": round(t, 4), "v": round(Z_NEAR, 1), "ease": "linear"})
            ka.append({"t": round(t, 4), "v": 0.0, "ease": "linear"})
            t += 0.001
            u = 0.0
            kz.append({"t": round(t, 4), "v": round(Z_FAR, 1), "ease": "linear"})
            ka.append({"t": round(t, 4), "v": 0.0, "ease": "linear"})
            continue
        t += step
        u += step / PERIOD
    return kz, ka

for i in range(RINGS):
    col = pal[i % len(pal)]
    kz, ka = sawtooth(i / RINGS)
    objs.append({"id": "ring%d" % i, "type": "circle",
                 "w": s(560), "h": s(560), "x": "center", "y": "center",
                 "anchor": "center", "fill": "none", "stroke": col,
                 "stroke_width": s(5), "blend": "add", "shading": 0.35})
    tl += [{"action": "animate", "target": "ring%d" % i, "property": "z", "keys": kz},
           {"action": "animate", "target": "ring%d" % i, "property": "opacity", "keys": ka}]

objs.append({"id": "mote", "type": "circle", "radius": s(4), "color": "#FFFFFF",
             "blend": "add",
             "emitter": {"count": 300, "seed": 3, "cx": "center", "cy": "center",
                         "spread": s(300), "speed": [s(4), s(26)], "angle": [0, 360],
                         "life": [3, 6], "emit_ms": 6500, "fade": .7,
                         "size_jitter": [.4, 1.6],
                         "color_cycle": ["#FFFFFF", GREEN, BLUE, PURPLE]}})
objs.append(caption("c2", "tunnel · depth sorted", .2, D - .1))
scenes.append({"id": "tunnel", "duration_ms": int(D * 1000),
               "camera": {"perspective": FOCAL,
                          "rotation": [{"t": 0, "v": -6}, {"t": D, "v": 6, "ease": "easeinout"}]},
               "objects": objs, "timeline": tl})

doc = {"_comment": "Perspective-projected layers: a carousel and a tunnel. "
                   "Generated by anim/build_3d.py.",
       "project": {"width": W, "height": H, "fps": 60, "bg_color": "#05060A"},
       "output": {"encoder": "h264_nvenc", "preset": "p6", "cq": 20},
       "transition": {"use": "crossfade", "duration": .8},
       "scenes": scenes}
json.dump(doc, open("anim/space3d.json", "w"), indent=1)
print("anim/space3d.json  %dx%d  scenes=%d" % (W, H, len(scenes)))
