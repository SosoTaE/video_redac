#!/usr/bin/env python3
"""
build_binsearch.py — ბინარული ძებნის ანიმირებული ვიზუალიზაცია.

ეს არის ყველაზე რთული სცენა, რაც ამ ძრავაზე აგვიწყია: ~90 ობიექტი, რომელთა
დიდ ნაწილს რამდენიმე keyframe-ტრეკი აქვს, და სამი პარალელური "თხრობა",
რომლებიც ერთმანეთს ზუსტად უნდა დაემთხვეს:

  1. მასივის უჯრედები — გამორთული ნახევრები ქრება, mid-ს ყვითელი რგოლი უჩნდება;
  2. lo / hi / mid მაჩვენებლები — მოძრაობენ backout-ით, ანუ ოდნავ გადააჭარბებენ
     და დაჯდებიან (ეს კითხვადობასაც ეხმარება: თვალი მოძრაობას მიჰყვება);
  3. კოდის ბლოკი — მოსრიალე ხაზის მარკერი, რომელიც ზუსტად იმ სტრიქონზე დგას,
     რასაც ალგორითმი ამ წამს ასრულებს.

ანიმაცია ხელით არ არის გაწერილი: ქვემოთ ნამდვილი ბინარული ძებნა სრულდება,
მისი კვალი (lo/hi/mid ყოველ ბიჯზე) იწერება და keyframe-ები ამ კვალიდან იბადება.
ანუ თუ მასივს ან სამიზნეს შეცვლი, ანიმაცია თავისით გადაეწყობა.

გაშვება:
    python3 anim/build_binsearch.py && ./video_redac anim/binsearch.json -o binsearch.mp4
"""

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))

W, H, FPS = 1080, 1920, 60

# ------------------------------------------------------------------- პალიტრა
BG        = "#11111B"
CELL      = "#313244"
INK       = "#CDD6F4"
MUTED     = "#9399B2"
YELLOW    = "#F9E2AF"      # mid
BLUE      = "#89B4FA"      # lo
PINK      = "#F38BA8"      # hi
GREEN     = "#A6E3A1"      # found
DIM       = "#11111BC7"    # გამორთული უჯრედის ფარდა

FONT_B    = "DejaVu Sans-Bold"
FONT_R    = "DejaVu Sans"
FONT_M    = "JetBrains Mono"

# --------------------------------------------------------------- ალგორითმი --
A   = [3, 8, 12, 19, 23, 27, 31, 38, 42, 47, 51, 58, 63, 70, 79]
KEY = 51

trace, lo, hi, found = [], 0, len(A) - 1, None
while lo <= hi:
    mid = (lo + hi) // 2
    if A[mid] == KEY:
        trace.append({"lo": lo, "hi": hi, "mid": mid, "cmp": "eq"})
        found = mid
        break
    if A[mid] < KEY:
        trace.append({"lo": lo, "hi": hi, "mid": mid, "cmp": "lt"})
        lo = mid + 1
    else:
        trace.append({"lo": lo, "hi": hi, "mid": mid, "cmp": "gt"})
        hi = mid - 1

N     = len(A)
STEPS = len(trace)

# --------------------------------------------------------------- გეომეტრია --
#
# ჰორიზონტალურად ყველაფერი კადრის ცენტრზეა აწყობილი. ტექსტებს `x` განზრახ არ
# ეთითება — რენდერერი მათ თავად აცენტრებს; რაკი ბარათიც ცენტრშია, კადრზე
# ცენტრირებული წარწერა ავტომატურად ბარათზეც ცენტრირებულია.

CELL_W, CELL_H, GAP = 58, 96, 6
RING = 5
GRID_W = N * CELL_W + (N - 1) * GAP
X0     = (W - GRID_W) // 2


def cell_x(i):
    return X0 + i * (CELL_W + GAP)


# --- კოდის "ფანჯარა" --------------------------------------------------------
#
# ზომები probe-რენდერით არის გაზომილი და არა მიხედვით შერჩეული:
#   30px JetBrains Mono → სიმბოლო ზუსტად 18px, სტრიქონის ბიჯი ზუსტად 58px.
#   ყველაზე გრძელი სტრიქონი 28 სიმბოლოა → ტექსტის სიგანე 504px.
CODE_LINES = [
    "def search(a, key):",
    "    lo, hi = 0, len(a) - 1",
    "    while lo <= hi:",
    "        mid = (lo + hi) // 2",
    "        if a[mid] == key:",
    "            return mid",
    "        if a[mid] < key:",
    "            lo = mid + 1",
    "        else:",
    "            hi = mid - 1",
    "    return -1",
]
CODE_SIZE, CODE_PAD, LINE_H = 30, 36, 58
CH_W      = 18                       # მონოსფეისის სიმბოლოს სიგანე 30px-ზე
CODE_TXT_W = 504
CODE_H     = 693                     # ტექსტურის სიმაღლე padding-ის ჩათვლით

HEADER_H = 68                        # ფანჯრის სათაურის ზოლი
GUTTER   = 60                        # სტრიქონების ნომრების სვეტი

CARD_W = 34 + GUTTER + 24 + CODE_TXT_W + 34
CARD_X = (W - CARD_W) // 2

# ტექსტის ბლოკებს საკუთარი padding აქვთ, ამიტომ ვიჯეტის x/y ≠ ასოს პოზიცია:
#   კოდის ბლოკი  — padding 36
#   ტექსტი       — padding = size/4 + 4  (30px-ზე → 11)
CODE_X    = CARD_X + 34 + GUTTER + 24 - CODE_PAD
LINENO_X  = CARD_X + 34 - 11
LINENO_DY = CODE_PAD - 11            # ნომრები კოდის სტრიქონებს რომ დაემთხვეს

# --- ვერტიკალური რიტმი ------------------------------------------------------
#
# ჯერ შიგთავსს ვაწყობთ 0-დან, ბოლოს კი მთელ ბლოკს ერთი წანაცვლებით ვსვამთ
# კადრის ცენტრში. ასე ზომების შეცვლისას ცენტრირება თავისით რჩება სწორი.
_TITLE_Y   = 0
_SUB_Y     = 118
_MIDLBL_Y  = 240
_MIDCAR_Y  = 292
_ARRAY_Y   = 312
_CARET_Y   = _ARRAY_Y + CELL_H + 16
_LOLBL_Y   = _CARET_Y + 16
_HILBL_Y   = _LOLBL_Y + 46
_NARR_Y    = 560
_CARD_Y    = 650
CARD_H     = HEADER_H + CODE_H + 6
_CONTENT_H = _CARD_Y + CARD_H

OFFSET = (H - _CONTENT_H) // 2


def Y(v):
    """შიგთავსის კოორდინატი → კადრის კოორდინატი (ვერტიკალური ცენტრირება)."""
    return v + OFFSET


TITLE_Y  = Y(_TITLE_Y)
SUB_Y    = Y(_SUB_Y)
MIDLBL_Y = Y(_MIDLBL_Y)
MIDCAR_Y = Y(_MIDCAR_Y)
ARRAY_Y  = Y(_ARRAY_Y)
CARET_Y  = Y(_CARET_Y)
LOLBL_Y  = Y(_LOLBL_Y)
HILBL_Y  = Y(_HILBL_Y)
NARR_Y   = Y(_NARR_Y)
CARD_Y   = Y(_CARD_Y)
CODE_Y   = CARD_Y + HEADER_H


def line_y(i):
    """კოდის i-ური სტრიქონის მარკერის ზედა კიდე."""
    return CODE_Y + CODE_PAD + i * LINE_H - 7


# ------------------------------------------------------------------ დროები --
T_INTRO   = 3.0        # სათაური + მასივის აწყობა
STEP      = 4.2        # ერთი ბიჯი
T_OUTRO   = 4.2
T_STEPS0  = T_INTRO
TOTAL     = T_INTRO + STEPS * STEP + T_OUTRO

objects, timeline = [], []


def kf(points):
    """[(t, v), (t, v, ease), …] → keyframe-ების მასივი."""
    out = []
    for p in points:
        k = {"t": round(p[0], 3), "v": p[1]}
        if len(p) > 2:
            k["ease"] = p[2]
        out.append(k)
    return out


def rect(oid, x, y, w, h, color, radius=10, opacity=None):
    o = {"id": oid, "type": "rect", "x": x, "y": y, "w": w, "h": h,
         "color": color, "corner_radius": radius}
    if opacity is not None:
        o["opacity"] = opacity
    objects.append(o)
    return o


def text(oid, content, size, y, color=INK, bold=True, x=None, opacity=None,
         scale=None, font=None):
    o = {"id": oid, "type": "text", "content": content,
         "font": font or (FONT_B if bold else FONT_R),
         "size": size, "color": color, "y": y}
    if x is not None:
        o["x"] = x
    if opacity is not None:
        o["opacity"] = opacity
    if scale is not None:
        o["scale"] = scale
    objects.append(o)
    return o


def fade(t_in, dur=0.45, hold=None, t_out=None, out_dur=0.4):
    """მოსახერხებელი opacity-ტრეკი: გამოჩნდი → გაჩერდი → გაქრი."""
    pts = [(0.0, 0.0), (t_in, 0.0), (t_in + dur, 1.0, "easeout")]
    if t_out is not None:
        pts += [(t_out, 1.0), (t_out + out_dur, 0.0, "easein")]
    return kf(pts)


# =============================================================== 1. სათაური ==
text("title", "Binary Search", 62, TITLE_Y, color=INK,
     opacity=fade(0.2, 0.6))
text("sub", f"{N} sorted items   ·   target = {KEY}", 32, SUB_Y, color=MUTED,
     bold=False, opacity=fade(0.5, 0.5))

# ============================================================== 2. უჯრედები ==
#
# ბიჯების მიხედვით ვაგროვებთ, რომელ დროს რომელი უჯრედია აქტიური/mid/გამორთული.
for i in range(N):
    x = cell_x(i)

    # --- mid-ის ყვითელი რგოლი (უჯრედზე ოდნავ დიდი მართკუთხედი მის უკან) ---
    mid_pts = [(0.0, 0.0)]
    for s, st in enumerate(trace):
        t = T_STEPS0 + s * STEP
        if st["mid"] == i:
            mid_pts += [(t + 0.55, 0.0), (t + 0.85, 1.0, "backout"),
                        (t + 3.40, 1.0), (t + 3.80, 0.0, "easein")]
    rect(f"ring{i}", x - RING, ARRAY_Y - RING,
         CELL_W + 2 * RING, CELL_H + 2 * RING, YELLOW, radius=14,
         opacity=kf(mid_pts) if len(mid_pts) > 1 else 0.0)

    # --- ნაპოვნის მწვანე რგოლი: უჯრედის *უკან*, თორემ რიცხვს გადაფარავს ---
    if found == i:
        t_found = T_STEPS0 + (STEPS - 1) * STEP + 1.9
        rect(f"found{i}", x - RING, ARRAY_Y - RING,
             CELL_W + 2 * RING, CELL_H + 2 * RING, GREEN, radius=14,
             opacity=kf([(0.0, 0.0), (t_found, 0.0), (t_found + 0.35, 1.0, "easeout")]))
        objects[-1]["scale"] = kf([(t_found, 0.55), (t_found + 0.8, 1.0, "elasticout")])

    # --- უჯრედის ფონი + რიცხვი (შემოსვლა კასკადით) ---
    appear = 0.8 + i * 0.045
    rect(f"cell{i}", x, ARRAY_Y, CELL_W, CELL_H, CELL, radius=10,
         opacity=kf([(0.0, 0.0), (appear, 0.0), (appear + 0.35, 1.0, "easeout")]))

    val = str(A[i])
    # ციფრები მონოსფეისია → სიგანე პროგნოზირებადია და ხელით ცენტრირება ზუსტია.
    text(f"num{i}", val, 30, ARRAY_Y + 30,
         x=x + (CELL_W - len(val) * CH_W) // 2 - 11, color=INK, font=FONT_M,
         opacity=kf([(0.0, 0.0), (appear + 0.1, 0.0), (appear + 0.45, 1.0, "easeout")]))

    # --- გამორთვის ფარდა: ეშვება მაშინ, როცა უჯრედი დიაპაზონს გარეთ რჩება ---
    dim_pts, dimmed = [(0.0, 0.0)], False
    for s, st in enumerate(trace):
        t = T_STEPS0 + s * STEP
        inside = st["lo"] <= i <= st["hi"]
        if not inside and not dimmed:
            # ბიჯი, რომელზეც ის *ამოვარდა* — წინა ბიჯის ბოლოს ვაქრობთ
            dim_pts += [(t - STEP + 2.55, 0.0), (t - STEP + 3.25, 1.0, "easeout")]
            dimmed = True
    rect(f"dim{i}", x - RING, ARRAY_Y - RING,
         CELL_W + 2 * RING, CELL_H + 2 * RING, DIM, radius=14,
         opacity=kf(dim_pts) if len(dim_pts) > 1 else 0.0)

# ======================================================= 3. lo / hi / mid ===
#
# მაჩვენებლები x-ტრეკით მოძრაობენ. backout ნიშნავს, რომ ისინი ოდნავ გადააჭარბებენ
# სამიზნეს და უკან დაბრუნდებიან — მექანიკურის ნაცვლად "ცოცხალი" მოძრაობა.
def marker(oid, label, color, caret_y, label_y, key):
    xs = [(0.0, float(cell_x(trace[0][key])))]
    for s, st in enumerate(trace):
        t = T_STEPS0 + s * STEP
        xs.append((t + 0.65, float(cell_x(st[key])), "backout"))

    op = kf([(0.0, 0.0), (T_STEPS0 - 0.4, 0.0), (T_STEPS0, 1.0, "easeout"),
             (TOTAL - T_OUTRO + 0.2, 1.0), (TOTAL - T_OUTRO + 0.7, 0.0, "easein")])

    objects.append({"id": oid, "type": "rect", "y": caret_y,
                    "w": CELL_W, "h": 7, "color": color, "corner_radius": 3,
                    "x": kf(xs), "opacity": op})
    # წარწერა იმავე ტრაექტორიით მიჰყვება კარეტს, უჯრედის ცენტრზე გასწორებული.
    label_xs = []
    for item in xs:
        shifted = (item[0], item[1] + CELL_W / 2 - 18)
        label_xs.append(shifted + tuple(item[2:]))

    objects.append({"id": f"{oid}_lbl", "type": "text", "content": label,
                    "font": FONT_M, "size": 26, "color": color, "y": label_y,
                    "x": kf(label_xs), "opacity": op})


marker("mid_c", "mid", YELLOW, MIDCAR_Y, MIDLBL_Y, "mid")
marker("lo_c",  "lo",  BLUE,   CARET_Y,  LOLBL_Y,  "lo")
marker("hi_c",  "hi",  PINK,   CARET_Y,  HILBL_Y,  "hi")

# ============================================================ 4. თხრობა =====
CMP_SIGN = {"lt": "<", "gt": ">", "eq": "="}
CMP_WORD = {"lt": "go right", "gt": "go left", "eq": "found it"}
CMP_COL  = {"lt": BLUE, "gt": PINK, "eq": GREEN}

for s, st in enumerate(trace):
    t = T_STEPS0 + s * STEP
    m = st["mid"]
    line = f'a[{m}] = {A[m]}   {CMP_SIGN[st["cmp"]]}   {KEY}   →   {CMP_WORD[st["cmp"]]}'
    text(f"nar{s}", line, 36, NARR_Y, color=CMP_COL[st["cmp"]], bold=True,
         opacity=fade(t + 1.30, 0.35, t_out=t + 3.45, out_dur=0.35))

# ============================================================ 5. კოდი =======
#
# ფირფიტას ცალკე ვხატავთ (და კოდის bg-ს ვაქრობთ), რომ ხაზის მარკერი ფირფიტასა
# და ასოებს *შორის* მოექცეს — თორემ ის ან ტექსტს გადაფარავდა, ან უხილავი იქნებოდა.
T_CLEAR = TOTAL - T_OUTRO + 0.1     # როდის იწმინდება კოდი ფინალისთვის

T_CLEAR = TOTAL - T_OUTRO + 0.1     # როდის იწმინდება კოდი ფინალისთვის
CARD_FADE = fade(1.6, 0.5, t_out=T_CLEAR, out_dur=0.55)

# --- ფანჯრის კორპუსი ---
rect("card", CARD_X, CARD_Y, CARD_W, CARD_H, "#181825F7", radius=20,
     opacity=CARD_FADE)

# სათაურის ზოლი ოდნავ ღიაა; მისი მომრგვალებული ქვედა კუთხეები კორპუსის იმავე
# ფერს ედება, ამიტომ სტიკი ზედა ზოლის ეფექტი გამოდის ცალკე hack-ის გარეშე.
rect("card_bar", CARD_X, CARD_Y, CARD_W, HEADER_H + 20, "#1E1E2EF7", radius=20,
     opacity=CARD_FADE)
rect("card_rule", CARD_X + 1, CARD_Y + HEADER_H, CARD_W - 2, 2, "#313244F0",
     radius=0, opacity=CARD_FADE)

# --- სამი წერტილი (ფანჯრის ღილაკები) ---
for di, dcol in enumerate(("#F38BA8", "#F9E2AF", "#A6E3A1")):
    objects.append({"id": f"dot{di}", "type": "circle",
                    "cx": CARD_X + 34 + di * 26, "cy": CARD_Y + HEADER_H // 2,
                    "radius": 8, "color": dcol, "opacity": CARD_FADE})

# ფაილის სახელი — კადრზე ცენტრირებული, ანუ ბარათზეც ცენტრირებული
text("card_name", "binary_search.py", 24, CARD_Y + 18, color="#6C7086",
     bold=False, font=FONT_M, opacity=CARD_FADE)

# --- სტრიქონების ნომრები ---------------------------------------------------
#
# ერთი ტექსტ-ბლოკი იმავე ფონტით, ზომითა და line_spacing-ით, რაც კოდს აქვს —
# ამიტომ სტრიქონები თავისით ემთხვევა და ცალკე გასწორება არ სჭირდება.
# განსხვავებას მხოლოდ padding ქმნის (36 vs 11), რასაც LINENO_DY ასწორებს.
text("linenos", "\n".join(f"{i + 1:>2}" for i in range(len(CODE_LINES))),
     CODE_SIZE, CODE_Y + LINENO_DY, x=LINENO_X, color="#45475A",
     font=FONT_M, opacity=CARD_FADE)
objects[-1]["line_spacing"] = 1.45

# --- მოსრიალე ხაზის მარკერი ---
def active_line(s, st):
    """რომელ სტრიქონზეა ალგორითმი ბიჯის შიგნით, დროის მიხედვით."""
    t = T_STEPS0 + s * STEP
    seq = [(t + 0.00, 2),          # while lo <= hi
           (t + 0.55, 3),          # mid = (lo + hi) // 2
           (t + 1.15, 4)]          # if a[mid] == key
    if st["cmp"] == "eq":
        seq.append((t + 1.90, 5))  # return mid
    elif st["cmp"] == "lt":
        seq += [(t + 1.90, 6), (t + 2.60, 7)]   # if a[mid] < key ; lo = mid + 1
    else:
        seq += [(t + 1.90, 8), (t + 2.60, 9)]   # else ; hi = mid - 1
    return seq

ys = []
for s, st in enumerate(trace):
    for (tt, ln) in active_line(s, st):
        # პირველი გადასვლა თითოეულ სტრიქონზე მკვეთრია, შემდეგ სრიალი
        ys.append((tt, float(line_y(ln)), "cubicout"))

objects.append({
    "id": "code_hl", "type": "rect",
    "x": CARD_X + 16, "w": CARD_W - 32, "h": 46,
    "color": "#89B4FA33", "corner_radius": 8,
    "y": kf([(0.0, float(line_y(2)))] + ys),
    "opacity": kf([(0.0, 0.0), (T_STEPS0 - 0.2, 0.0), (T_STEPS0 + 0.2, 1.0, "easeout"),
                   (T_CLEAR - 0.2, 1.0), (T_CLEAR + 0.3, 0.0, "easein")]),
})

objects.append({
    "id": "code", "type": "code", "language": "python", "font": FONT_M,
    "size": CODE_SIZE, "padding": CODE_PAD, "line_spacing": 1.45,
    "bg_color": "#00000000",                 # კორპუსი ცალკე შრეებია (იხ. ზემოთ)
    "x": CODE_X, "y": CODE_Y,
    "code": "\n".join(CODE_LINES),
    "opacity": fade(1.75, 0.5, t_out=T_CLEAR, out_dur=0.55),
})

# ============================================================ 6. ფინალი =====
t_out = TOTAL - T_OUTRO + 0.85      # კოდის გაქრობის შემდეგ
text("res1", f"{STEPS} steps", 78, CARD_Y + 150, color=GREEN,
     opacity=fade(t_out, 0.5),
     scale=kf([(t_out, 0.7), (t_out + 0.8, 1.0, "backout")]))
text("res2", f"for {N} items", 38, CARD_Y + 262, color=MUTED, bold=False,
     opacity=fade(t_out + 0.25, 0.5))
text("res3", "O(log n)", 92, CARD_Y + 400, color=YELLOW, font=FONT_M,
     opacity=fade(t_out + 0.6, 0.5),
     scale=kf([(t_out + 0.6, 0.85), (t_out + 1.5, 1.0, "elasticout")]))

# ============================================================== პროექტი =====
project = {
    "_comment": [
        "binsearch.json — ავტომატურად აგებული build_binsearch.py-ით.",
        f"მასივი: {A}",
        f"სამიზნე: {KEY} → ნაპოვნია ინდექსზე {found}, {STEPS} ბიჯში.",
        "ანიმაცია ალგორითმის ნამდვილი კვალიდან იბადება — მასივის შეცვლა",
        "ავტომატურად გადააწყობს ყველა keyframe-ს.",
    ],
    "project": {"width": W, "height": H, "fps": FPS,
                "bg_color": BG, "duration_ms": int(round(TOTAL * 1000))},
    "objects": objects,
    "timeline": [],          # ყველაფერი keyframe-ტრეკებზეა, მოვლენები არ გვჭირდება
    "effects": [
        {"type": "color_grade", "contrast": 1.04, "saturation": 1.05},
        {"type": "vignette", "amount": 0.34, "radius": 0.70, "softness": 0.62,
         "color": "#000000"},
        {"type": "grain", "amount": 0.010},
    ],
}

out = os.path.join(HERE, "binsearch.json")
with open(out, "w", encoding="utf-8") as f:
    json.dump(project, f, ensure_ascii=False, indent=2)

print(out)
print(f"  ალგორითმი    : {STEPS} ბიჯი, ნაპოვნია index={found}")
print(f"  ხანგრძლივობა : {TOTAL:.1f}s ({int(TOTAL * FPS)} კადრი @ {FPS}fps)")
print(f"  ობიექტები    : {len(objects)}")
