#!/usr/bin/env python3
"""
build_listing.py — ქმნის listing.json-ს MLS-ის მონაცემებიდან.

რატომ გენერატორი და არა ხელით დაწერილი JSON:
ვიდეოში ~45 ობიექტია, თითოეულს რამდენიმე დროითი ნიშნული აქვს. ხელით რომ
დაგვეწერა, ერთი სცენის გადაწევა ათეულობით რიცხვის ხელახლა გამოთვლას ნიშნავდა.
აქ სცენები დეკლარაციულადაა აღწერილი და დროები თავად ითვლება.

გაშვება:
    python3 listing/build_listing.py && ./video_redac listing/listing.json -o listing.mp4
"""

import argparse
import json
import os
import sys

import numpy as np
from PIL import Image, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))

_ap = argparse.ArgumentParser(description="MLS-ის მონაცემებიდან ვიდეოს JSON-ის აგება")
_ap.add_argument("--seconds", type=float, default=36.0,
                 help="სამიზნე ხანგრძლივობა წამებში (default: 36)")
_ap.add_argument("-o", "--out", default=None, help="გამომავალი JSON")
ARGS = _ap.parse_args()

W, H, FPS = 1080, 1920, 30

# ------------------------------------------------------------------ ბრენდი --
INK        = "#FFFFFF"
MUTED      = "#D7D3CC"
ACCENT     = "#E8C39E"      # თბილი ქვიშისფერი აქცენტი
BG         = "#0B0B0F"

FONT_BOLD  = "DejaVu Sans-Bold"
FONT_REG   = "DejaVu Sans"

# PIL-ს ვიყენებთ ტექსტის სიგანის გასაზომად, რომ სვეტებში ხელით ვაცენტროთ
# (რენდერერს ავტო-ცენტრი მხოლოდ მთელ კადრზე აქვს).
_PIL_BOLD = "/home/sosotae/.local/share/fonts/DejaVuSans-Bold.ttf"
_PIL_REG  = "/home/sosotae/.local/share/fonts/DejaVuSans.ttf"
_cache = {}


def text_width(s, size, bold=True):
    key = (size, bold)
    if key not in _cache:
        _cache[key] = ImageFont.truetype(_PIL_BOLD if bold else _PIL_REG, size)
    return _cache[key].getlength(s)


def centered_x(s, size, bold=True, box_x=0, box_w=W):
    return round(box_x + (box_w - text_width(s, size, bold)) / 2)


def fit_size(s, size, max_w=W - 150, bold=True, min_size=22):
    """
    ამცირებს ფონტს, სანამ ტექსტი მინდვრებში არ ჩაჯდება.

    ეს სკრიპტი ერთ კონკრეტულ განცხადებაზე არ უნდა იყოს მორგებული — სხვა
    მისამართი შეიძლება ორჯერ გრძელი აღმოჩნდეს. ხელით შერჩეული ზომა იქ
    კიდეებიდან გადმოვიდოდა, ეს კი თავად ალაგებს.
    """
    while size > min_size and text_width(s, size, bold) > max_w:
        size -= 2
    return size


def has_map_pin(path):
    """
    ამოწმებს, ხომ არ აქვს აეროფოტოს ჩაშენებული წითელი "map pin".

    განცხადებების პორტალები ზოგჯერ აეროკადრზე პინს აწებებენ. მარკეტინგულ
    ვიდეოში ის უცხო სხეულია, thumbnail-ზე კი თითქმის არ ჩანს — ამიტომ
    ავტომატურად ვამოწმებთ, ვიდრე ხელით ვეძებდეთ.
    """
    try:
        im = np.asarray(Image.open(path).convert("RGB")).astype(int)
    except Exception:
        return False
    r, g, b = im[..., 0], im[..., 1], im[..., 2]
    mask = (r > 150) & (g < 90) & (b < 90) & ((r - g) > 90) & ((r - b) > 90)

    total = int(mask.sum())
    if total < 300 or total > 8000:
        # ძალიან ცოტა → ხმაური. ძალიან ბევრი → წითელი ავეჯი (ამ ობიექტზე
        # წითელი დივანი 100k პიქსელს იკავებს, პინი კი ~2.3k-ს).
        return False

    # პინი *ლოკალურად მკვრივია*: თითქმის მთელი მისი წითელი ერთ პატარა
    # კვადრატში ჯდება. მიმოფანტული წითელი (აგური, ყვავილები) ამ ზღვარს ვერ იღებს.
    B = 48
    h, w = mask.shape
    hh, ww = h // B, w // B
    if hh == 0 or ww == 0:
        return False
    blocks = mask[:hh * B, :ww * B].reshape(hh, B, ww, B).sum(axis=(1, 3))

    return int(blocks.max()) >= 800


# ------------------------------------------------------------- მონაცემები --
DATA = {
    "street": "300 Upper Station Camp Crk Rd",
    "city":   "Gallatin, TN 37066",
    "price":  "$525,000",
    "beds":   "4",
    "baths":  "3",
    "sqft":   "2,192",
    "acres":  "1.9",
    "mls":    "2898711",
    "agent":  "Cynthia Pearson",
    "title":  "Realtor®",
    "broker": "Compass",
    "phone":  "919.621.9777",
    "email":  "cindy.pearson@compass.com",
}

PHOTOS = "photos"

# სახლის სრული ტური, ბუნებრივი თანმიმდევრობით.
# `prio` — რაც უფრო დაბალია, მით უფრო აუცილებელია კადრი: მოკლე ვერსია საუკეთესოებს
# ტოვებს, გრძელი კი მთელ სიას იყენებს. ორივე შემთხვევაში რიგი უცვლელია, ანუ ტური
# ლოგიკური რჩება.
MASTER = [
    # (ფოტო, prio, სათაური, ქვესათაური, zoom)
    ("05.jpg", 5, "Covered Front Porch",   "Morning coffee, evening quiet",  "in"),
    ("07.jpg", 1, "Vaulted Ceilings",      "Stone wood-burning fireplace",   "in"),
    ("06.jpg", 7, "Open to Above",         "Light from every angle",         "out"),
    ("13.jpg", 2, "Renovated Kitchen",     "New stainless appliances",       "out"),
    ("14.jpg", 8, "Open-Concept Living",   "Kitchen flows to the living room","in"),
    ("16.jpg", 3, "Sun-Filled Sunroom",    "Wall of windows, wood ceiling",  "out"),
    ("22.jpg", 6, "Primary Suite",         "Sliding barn door",              "in"),
    ("21.jpg", 4, "Spa-Worthy Bath",       "Full tiled walk-in shower",      "in"),
    ("19.jpg", 10, "Four Bedrooms",        "Room for everyone",              "out"),
    ("25.jpg", 5, "Bonus Room Upstairs",   "Movie nights or game room",      "in"),
    ("04.jpg", 9, "Massive Deck",          "Built for gathering",            "out"),
    ("28.jpg", 6, "Detached 4-Car Garage", "Plus workshop space",            "out"),
    ("32.jpg", 2, "1.9 Acres",             "Mini-farm ready — bring goats",  "in"),
]

HERO_SEC   = 4.6
STATS_SEC  = 3.6
CTA_SEC    = 6.0
TARGET     = ARGS.seconds

# რამდენ სცენას ვიტევთ: ბიუჯეტს ვყოფთ კომფორტულ ~3.4 წამზე კადრზე და
# პრიორიტეტით ვარჩევთ საუკეთესოებს, ტურის რიგის შენარჩუნებით.
_budget = TARGET - HERO_SEC - STATS_SEC - CTA_SEC
_n      = max(4, min(len(MASTER), round(_budget / 3.4)))
_chosen = sorted(sorted(range(len(MASTER)), key=lambda i: MASTER[i][1])[:_n])
_per    = _budget / _n

SCENES = [(MASTER[i][0], _per, MASTER[i][2], MASTER[i][3], MASTER[i][4])
          for i in _chosen]
XFADE      = 0.7          # კროსფეიდის ხანგრძლივობა
CULL       = 0.12         # რამდენ ხანში ქრება დაფარული ფოტო (კულინგისთვის)

objects, timeline = [], []


def add_photo(oid, path, t0, dur, zoom="in", z_hint=None):
    """
    სრულეკრანიანი ფოტო Ken Burns-ის ეფექტით.

    height=H და x-ის გამოტოვება იძლევა "cover"-ის ქცევას: სიგანე პროპორციულად
    გამოითვლება (ლანდშაფტურ კადრზე ~2880px) და ავტო-ცენტრი მას სიმეტრიულად
    ჭრის კადრის კიდეებზე — ზუსტად ისე, როგორც CSS-ის object-fit: cover.
    """
    s0, s1 = (1.0, 1.10) if zoom == "in" else (1.10, 1.0)
    objects.append({
        "id": oid,
        "type": "image",
        "path": f"{PHOTOS}/{path}",
        "height": H,
        "y": 0,
        # Ken Burns — ნელი, უწყვეტი მოძრაობა მთელი კადრის განმავლობაში
        "scale": [{"t": t0, "v": s0},
                  {"t": t0 + dur + XFADE, "v": s1, "ease": "linear"}],
        "opacity": [{"t": t0, "v": 0.0},
                    {"t": t0 + XFADE, "v": 1.0, "ease": "easeout"}],
    })


def fade_out_photo(oid, t):
    """დაფარული ფოტოს ჩაქრობა — ვიზუალურად უხილავია, GPU-ს კი ათავისუფლებს."""
    timeline.append({"time_ms": int(t * 1000), "action": "fade_out",
                     "target": oid, "duration_ms": int(CULL * 1000)})


def add_text(oid, content, size, y, color=INK, bold=True, x=None,
             t_in=None, t_out=None, rise=28, ease="easeout", spacing=1.25):
    o = {
        "id": oid, "type": "text", "content": content,
        "font": FONT_BOLD if bold else FONT_REG,
        "size": size, "color": color, "y": y, "line_spacing": spacing,
    }
    if x is not None:
        o["x"] = x
    objects.append(o)

    if t_in is not None:
        timeline.append({"time_ms": int(t_in * 1000), "action": "fade_in",
                         "target": oid, "duration_ms": 500, "ease": ease})
        if rise:
            timeline.append({"time_ms": int(t_in * 1000), "action": "move_y",
                             "target": oid, "value": -rise,
                             "duration_ms": 750, "ease": "backout"})
    if t_out is not None:
        timeline.append({"time_ms": int(t_out * 1000), "action": "fade_out",
                         "target": oid, "duration_ms": 400})


def add_image(oid, path, x, y, width=None, height=None, t_in=None, t_out=None,
              opacity=None):
    o = {"id": oid, "type": "image", "path": path, "x": x, "y": y}
    if width:
        o["width"] = width
    if height:
        o["height"] = height
    if opacity is not None:
        o["opacity"] = opacity
    objects.append(o)
    if t_in is not None:
        timeline.append({"time_ms": int(t_in * 1000), "action": "fade_in",
                         "target": oid, "duration_ms": 500})
    if t_out is not None:
        timeline.append({"time_ms": int(t_out * 1000), "action": "fade_out",
                         "target": oid, "duration_ms": 400})


# =========================================================== 1. HERO ========
t = 0.0
add_photo("hero", "00.jpg", 0.0, HERE_DUR := HERO_SEC, "in")

add_image("scrim_b", "assets/scrim_bottom.png", 0, H - 980, t_in=0.0)
add_image("scrim_t", "assets/scrim_top.png",    0, 0,       t_in=0.0)

add_text("brand", DATA["broker"].upper(), 30, 96, color=MUTED, bold=True,
         t_in=0.3, rise=0, spacing=1.0)
add_text("status", "JUST LISTED", 30, 150, color=ACCENT, bold=True,
         t_in=0.5, rise=0)

add_text("addr", DATA["street"], fit_size(DATA["street"], 58), 1360,
         color=INK, t_in=0.9)
add_text("city", DATA["city"],   38, 1450, color=MUTED, bold=False, t_in=1.1)
add_image("rule1", "assets/rule.png", (W - 220) // 2, 1530, t_in=1.35)
add_text("price", DATA["price"], fit_size(DATA["price"], 92), 1580,
         color=INK, t_in=1.5)

hero_end = HERO_SEC
for oid in ("brand", "status", "addr", "city", "price"):
    timeline.append({"time_ms": int((hero_end - 0.45) * 1000), "action": "fade_out",
                     "target": oid, "duration_ms": 450})
timeline.append({"time_ms": int((hero_end - 0.45) * 1000), "action": "fade_out",
                 "target": "rule1", "duration_ms": 450})

# ========================================================== 2. STATS ========
t = hero_end
add_photo("stats_bg", "33.jpg", t, STATS_SEC, "out")
fade_out_photo("hero", t + XFADE)

# ბნელი ფარდა, რომ ციფრები აერიალურ კადრზე იკითხებოდეს
objects.append({"id": "stats_veil", "type": "rect", "x": 0, "y": 0,
                "w": W, "h": H, "color": "#0B0B0FB8"})
timeline += [
    {"time_ms": int(t * 1000), "action": "fade_in", "target": "stats_veil",
     "duration_ms": 600},
    # ფარდა ტექსტზე გვიან ქრება — თორემ ციფრები ერთი წამის მეათედით ნათელ
    # აეროკადრზე დარჩებოდა და აღარ წაიკითხებოდა.
    {"time_ms": int((t + STATS_SEC - 0.40) * 1000), "action": "fade_out",
     "target": "stats_veil", "duration_ms": 400},
]

STATS = [("4", "BEDS"), ("3", "BATHS"), ("2,192", "SQ FT"), ("1.9", "ACRES")]
col_w = W / 2
for i, (val, label) in enumerate(STATS):
    cx = (i % 2) * col_w
    cy = 700 + (i // 2) * 300
    add_text(f"sv{i}", val, 92, cy,
             x=centered_x(val, 92, True, cx, col_w), color=INK,
             t_in=t + 0.45 + i * 0.16, t_out=t + STATS_SEC - 0.62, rise=22)
    add_text(f"sl{i}", label, 30, cy + 118, color=ACCENT, bold=True,
             x=centered_x(label, 30, True, cx, col_w),
             t_in=t + 0.55 + i * 0.16, t_out=t + STATS_SEC - 0.62, rise=0)

# ==================================================== 3. FEATURE SCENES =====
t += STATS_SEC
prev = "stats_bg"
for i, (photo, dur, head, sub, zoom) in enumerate(SCENES):
    oid = f"ph{i}"
    add_photo(oid, photo, t, dur, zoom)
    fade_out_photo(prev, t + XFADE)
    prev = oid

    add_image(f"sc{i}", "assets/scrim_bottom.png", 0, H - 980,
              t_in=t + 0.15, t_out=t + dur - 0.35)
    add_text(f"h{i}", head, fit_size(head, 62), 1430, color=INK,
             t_in=t + 0.35, t_out=t + dur - 0.35)
    add_text(f"s{i}", sub, fit_size(sub, 34, bold=False), 1540, color=MUTED,
             bold=False, t_in=t + 0.5, t_out=t + dur - 0.35)
    t += dur

# ============================================================ 4. CTA ========
add_photo("cta_bg", "05.jpg", t, CTA_SEC, "in")
fade_out_photo(prev, t + XFADE)

objects.append({"id": "cta_veil", "type": "rect", "x": 0, "y": 0,
                "w": W, "h": H, "color": "#0B0B0FCC"})
timeline.append({"time_ms": int(t * 1000), "action": "fade_in",
                 "target": "cta_veil", "duration_ms": 700})

add_text("cta_head", "Schedule a Private Tour",
         fit_size("Schedule a Private Tour", 54), 640, color=INK, t_in=t + 0.5)
add_image("rule2", "assets/rule.png", (W - 220) // 2, 760, t_in=t + 0.75)

add_text("agent", DATA["agent"], fit_size(DATA["agent"], 66), 850,
         color=INK, t_in=t + 0.9)
add_text("role", f'{DATA["title"]}  ·  {DATA["broker"]}', 34, 950,
         color=ACCENT, bold=False, t_in=t + 1.05)

add_text("phone", DATA["phone"], fit_size(DATA["phone"], 58), 1120,
         color=INK, t_in=t + 1.3)
add_text("email", DATA["email"], fit_size(DATA["email"], 32, bold=False), 1215,
         color=MUTED, bold=False, t_in=t + 1.45)

add_text("cta_addr", f'{DATA["street"]}\n{DATA["city"]}', 32, 1420,
         color=MUTED, bold=False, t_in=t + 1.6, spacing=1.35)
add_text("mls", f'MLS #{DATA["mls"]}  ·  {DATA["price"]}', 30, 1580,
         color=ACCENT, bold=True, t_in=t + 1.75, rise=0)

total = t + CTA_SEC

# ============================================================ პროექტი ======
project = {
    "_comment": [
        "listing.json — ავტომატურად აგებული build_listing.py-ით. ხელით ნუ დაარედაქტირებ.",
        "წყარო: MLS #%s, %s, %s" % (DATA["mls"], DATA["street"], DATA["city"]),
        "",
        "ვერტიკალური 1080x1920 @ 30fps — Reels / TikTok / YouTube Shorts.",
        "სურათები 'cover' რეჟიმშია (height=1920, x ავტო-ცენტრი) + Ken Burns.",
    ],
    "_audio_example": [
        "ხმა განზრახ არ არის ჩამატებული — სალიცენზიო მუსიკა ჩვენ არ გვაქვს.",
        "დასამატებლად ჩასვი ზედა დონეზე:",
        '"audio": [ { "path": "assets/track.mp3", "volume": 0.5, "loop": true,',
        '             "fade_in": 1.0, "fade_out": 2.5 } ]',
    ],
    "project": {
        "width": W, "height": H, "fps": FPS,
        "bg_color": BG,
        "duration_ms": int(round(total * 1000)),
    },
    "objects": objects,
    "timeline": timeline,
    # ერთიანი "ფოტოგრაფიული" ტონი მთელ ფილმზე
    "effects": [
        {"type": "color_grade", "contrast": 1.06, "saturation": 1.07,
         "temperature": 0.035, "exposure": 0.02},
        {"type": "vibrance", "amount": 0.18},
        {"type": "vignette", "amount": 0.30, "radius": 0.68, "softness": 0.62,
         "color": "#000000"},
        {"type": "grain", "amount": 0.012},
    ],
}

# ---- შემოწმება: არც ერთ გამოყენებულ კადრს არ უნდა ჰქონდეს map pin ----
used = sorted({o["path"] for o in objects
               if o.get("type") == "image" and o["path"].startswith(PHOTOS)})
flagged = [p for p in used if has_map_pin(os.path.join(HERE, p))]
if flagged:
    print("გაფრთხილება: ამ კადრებზე წითელი map pin-ია — შეცვალე:", file=sys.stderr)
    for p in flagged:
        print(f"  {p}", file=sys.stderr)

out = ARGS.out or os.path.join(HERE, "listing.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(project, f, ensure_ascii=False, indent=2)

print(f"{out}")
print(f"  ხანგრძლივობა : {total:.1f}s ({int(total * FPS)} კადრი), სამიზნე {TARGET:.0f}s")
print(f"  სცენები      : {len(SCENES)} × {_per:.2f}s")
print(f"  ობიექტები    : {len(objects)}")
print(f"  მოვლენები    : {len(timeline)}")
