#!/usr/bin/env python3
"""
Erzeugt die eingebetteten Grafiken der App im Stil des Original-Dashboards.

  assets/overlay.png   640x480 RGBA: Panels, Ueberschriften, B-Knopf mit Faden, Leuchten (liegt ueber dem Tunnel)
  assets/cells.png     256x256 Graustufen: eigenes Zellmuster, Ersatz-Textur des Tunnels
  assets/tunnel.png    nur Referenzbild (Vorschau, Logo-Hintergrund); die App rendert den Tunnel selbst
  src/gen_assets.c     overlay.png und cells.png als zlib-gepackte Rohpixel (RGBA bzw. Graustufen), dazu die
                       TTF-Schriften. Rohpixel statt PNG: die App braucht dann nur zlib statt SDL_image, libpng
                       und libjpeg (die XBE wird dadurch rund 650 KB kleiner).
  src/gen_layout.h     Panel-Koordinaten fuer main.c

Der Tunnel wird beim Start in der App berechnet (src/tunnel.c). Als Zellmuster dient bevorzugt die Textur
des Original-Dashboards von C: (src/dashassets.c), sonst assets/cells.png. Dieses Skript enthaelt und
benutzt kein Microsoft-Material.  tunnel_reference() hier entspricht src/tunnel.c Zeile fuer Zeile.

Nur Python 3 + Pillow.  Aufruf:  make_assets.py [--tunnel]   (--tunnel rendert tunnel.png neu, dauert)
"""
import math
import os
import sys
import zlib

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FTP = os.path.dirname(HERE)
ASSETS = os.path.join(FTP, "assets")
SRC = os.path.join(FTP, "src")
FONT_HEAD = os.path.join(ASSETS, "fonts", "Michroma-Regular.ttf")
FONT_BODY = os.path.join(ASSETS, "fonts", "SairaSemiCondensed-Regular.ttf")

W, H = 640, 480
SS = 3  # Supersampling fuer Panels

# Farben (aus Screenshots des Original-Dashboards abgenommen)
LINE = (100, 136, 38)        # Rahmenlinie, wie im Original-Dashboard gemessen (frueher 178,214,40: brannte auf LCDs ein)
LINE_DIM = (60, 100, 18)
FILL = (4, 22, 3, 228)       # Panelfuellung, halbtransparent
HEAD_TXT = (226, 240, 206)

# Panels: (Schluessel, x0, y0, x1, y1, Ueberschrift)
PANELS = [
    ("NET", 48, 74, 314, 236, "NETZWERK"),
    ("MEM", 326, 74, 592, 236, "SPEICHER"),
    ("ACT", 48, 250, 592, 404, "AKTIVITÄT"),
]
TITLE_POS = (54, 30)
BTN_B = (150, 430)  # Mittelpunkt des B-Knopfs


# ---------------------------------------------------------------- Hintergrund
def procedural_cells(size=256, grid=3, seed=11):
    """Eigenes, kachelbares Zellmuster (Voronoi-Kanten). Kein fremdes Material."""
    import random
    rnd = random.Random(seed)
    cell = size / grid
    pts = [((gx + 0.15 + 0.7 * rnd.random()) * cell, (gy + 0.15 + 0.7 * rnd.random()) * cell)
           for gy in range(grid) for gx in range(grid)]
    # Wiederholungen fuer nahtlose Kacheln
    allp = [(x + ox * size, y + oy * size) for x, y in pts for ox in (-1, 0, 1) for oy in (-1, 0, 1)]
    width = 7.0
    px = []
    for y in range(size):
        row = []
        for x in range(size):
            d1 = d2 = 1e9
            for qx, qy in allp:
                if abs(qx - x) > cell * 1.6 or abs(qy - y) > cell * 1.6:
                    continue
                d = math.hypot(qx - x, qy - y)
                if d < d1: d1, d2 = d, d1
                elif d < d2: d2 = d
            row.append(max(0.0, min(1.0, 1.0 - (d2 - d1) / width)))
        px.append(row)
    return px, size, size


def sample(tex, tw, th, u, v):
    x = (u % 1.0) * tw - 0.5
    y = (v % 1.0) * th - 0.5
    x0 = int(math.floor(x)); y0 = int(math.floor(y))
    fx = x - x0; fy = y - y0
    x0 %= tw; y0 %= th
    x1 = (x0 + 1) % tw; y1 = (y0 + 1) % th
    a = tex[y0][x0] * (1 - fx) + tex[y0][x1] * fx
    b = tex[y1][x0] * (1 - fx) + tex[y1][x1] * fx
    return a * (1 - fy) + b * fy


def tunnel_reference():
    tex, tw, th = procedural_cells()
    img = Image.new("RGB", (W, H))
    pix = img.load()
    # Kamera im Inneren einer Roehre (Radius 1), Blick entlang +z, leicht nach oben/rechts gedreht
    fov = math.radians(70)
    f = (W / 2) / math.tan(fov / 2)
    yaw, pitch = math.radians(8), math.radians(-10)
    cy_, sy_ = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)
    cam = (0.35, -0.15, 0.0)
    REP_U, REP_V = 28.0, 2.8   # eigenes Muster: rep = 1.0 in tunnel.c (Dashboard-Muster dort 1.5)
    sub = [(0.25, 0.25), (0.75, 0.25), (0.25, 0.75), (0.75, 0.75)]
    for py in range(H):
        for px_ in range(W):
            acc = 0.0
            for ox, oy in sub:
                dx = (px_ + ox - W / 2) / f
                dy = -(py + oy - H / 2) / f
                dz = 1.0
                # pitch (um x), dann yaw (um y)
                dy, dz = dy * cp - dz * sp, dy * sp + dz * cp
                dx, dz = dx * cy_ + dz * sy_, -dx * sy_ + dz * cy_
                # Schnitt mit Zylinder x^2+y^2=1
                a = dx * dx + dy * dy
                b = 2 * (cam[0] * dx + cam[1] * dy)
                c = cam[0] ** 2 + cam[1] ** 2 - 1
                t = (-b + math.sqrt(b * b - 4 * a * c)) / (2 * a) if a > 1e-9 else 50.0
                hx, hy, hz = cam[0] + t * dx, cam[1] + t * dy, cam[2] + t * dz
                ang = math.atan2(hy, hx) / (2 * math.pi)
                line = sample(tex, tw, th, ang * REP_U, hz * REP_V)
                fog = math.exp(-0.11 * t)
                acc += (line ** 3) * fog
            acc /= len(sub)
            # radiales Leuchten zur Bildmitte (leicht oberhalb)
            rx = (px_ - W * 0.55) / W
            ry = (py - H * 0.42) / H
            glow = math.exp(-(rx * rx + ry * ry) * 5.0)
            base_r = 4 + 13 * glow
            base_g = 18 + 36 * glow
            base_b = 2 + 6 * glow
            lr, lg, lb = 26 * acc, 64 * acc, 10 * acc
            r_ = min(255, int(base_r + lr)); g_ = min(255, int(base_g + lg)); b_ = min(255, int(base_b + lb))
            # Vignette (nur leichte Abdunklung der Ecken), analytisch wie in tunnel.c
            vx = (px_ - W / 2) / (0.75 * W); vy = (py - H / 2) / (0.8 * H)
            m = max(0.0, min(1.0, (1.12 - math.sqrt(vx * vx + vy * vy)) / 0.4))
            pix[px_, py] = (int(r_ * m), int(8 + (g_ - 8) * m), int(b_ * m))
    return img.convert("RGBA")


# ---------------------------------------------------------------- Panels
def chamfer_poly(x0, y0, x1, y1, c):
    """Rechteck mit abgeschraegten Ecken oben links und unten rechts (Dashboard-Form)."""
    return [(x0 + c, y0), (x1, y0), (x1, y1 - c), (x1 - c, y1), (x0, y1), (x0, y0 + c)]


def s(v):
    return int(round(v * SS))


def draw_panel(layer, glow, x0, y0, x1, y1, title, fonth):
    d = ImageDraw.Draw(layer)
    g = ImageDraw.Draw(glow)
    c = 14
    poly = [(s(x), s(y)) for x, y in chamfer_poly(x0, y0, x1, y1, c)]
    d.polygon(poly, fill=FILL)
    # Kopfleiste
    hy = y0 + 24
    head = [(s(x), s(y)) for x, y in [(x0 + c, y0), (x1, y0), (x1, hy), (x0, hy), (x0, y0 + c)]]
    d.polygon(head, fill=(18, 52, 6, 225))
    d.line([(s(x0 + 6), s(hy)), (s(x1 - 6), s(hy))], fill=LINE_DIM + (255,), width=s(1))
    # Rahmen doppelt: aussen hell, innen dunkel
    d.line(poly + [poly[0]], fill=LINE + (255,), width=s(2), joint="curve")
    g.line(poly + [poly[0]], fill=LINE + (70,), width=s(5), joint="curve")
    inner = [(s(x), s(y)) for x, y in chamfer_poly(x0 + 4, y0 + 4, x1 - 4, y1 - 4, c - 2)]
    d.line(inner + [inner[0]], fill=(40, 80, 10, 200), width=s(1), joint="curve")
    # Zierelemente: kleine Balken an der linken Kante (wie Bibliothek/Musik)
    for i in range(3):
        yy = y0 + 34 + i * 7
        d.rectangle((s(x0 - 7), s(yy), s(x0 - 3), s(yy + 3)), fill=LINE + (230,))
    # Punktreihe unten mittig (wie im Speicher-Menue), nur beim breiten Panel
    cx = (x0 + x1) / 2
    for i in range(5 if x1 - x0 > 400 else 0):
        xx = cx - 16 + i * 8
        d.ellipse((s(xx - 1.5), s(y1 - 7.5), s(xx + 1.5), s(y1 - 4.5)), fill=(90, 130, 28, 220))
    # Ueberschrift
    d.text((s(x0 + 20), s(y0 + 5)), title, font=fonth, fill=HEAD_TXT + (255,))
    g.text((s(x0 + 20), s(y0 + 5)), title, font=fonth, fill=(110, 150, 40, 110))


def circle_through(p1, p2, p3):
    """Mittelpunkt und Radius des Kreises durch drei Punkte."""
    (ax, ay), (bx, by), (cx, cy) = p1, p2, p3
    d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
    ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) + (cx * cx + cy * cy) * (ay - by)) / d
    uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) + (cx * cx + cy * cy) * (bx - ax)) / d
    return ux, uy, math.hypot(ax - ux, ay - uy)


def draw_button(layer, cx, cy, label, flabel):
    """Knopf wie "B ZURUECK" im Original-Dashboard: ovale Schale (heller Rand, dunkle Mitte), eckiger
    Buchstabe, Beschriftung links unten und ein doppelter "Faden", der vom Knopf nach links oben ausholt und
    unten links aus dem Bild laeuft. Masse aus Screenshots abgenommen (Knopf 28x39 px bei 640x480)."""
    d = ImageDraw.Draw(layer)
    rx, ry = 14, 19.5
    # Faden: Kreisbogen durch Ansatz am Knopf, Scheitel und einen Punkt unterhalb des Bildrands
    pa, pk, pe = (cx - 13, cy - 6), (cx - 48, cy - 15), (cx - 140, cy + 53)
    ux, uy, rad = circle_through(pa, pk, pe)
    a0 = math.atan2(pa[1] - uy, pa[0] - ux)
    a1 = math.atan2(pe[1] - uy, pe[0] - ux)
    if a1 > a0:
        a1 -= 2 * math.pi
    a1 -= 0.6  # weiter bis unter den Bildrand
    n = 600
    for off in (-1.5, 1.5):
        rr = rad + off
        pts = []
        for i in range(n + 1):
            a = a0 + (a1 - a0) * i / n
            x, y = ux + rr * math.cos(a), uy + rr * math.sin(a)
            # die letzten Pixel vor dem Knopf gestrichelt
            near = math.hypot(x - pa[0], y - pa[1]) < 9
            if near and int(math.hypot(x - pa[0], y - pa[1]) / 2) % 2:
                if len(pts) > 1:
                    d.line(pts, fill=(104, 144, 62, 210), width=s(1))
                pts = []
                continue
            pts.append((s(x), s(y)))
        if len(pts) > 1:
            d.line(pts, fill=(104, 144, 62, 210), width=s(1))
    # Schale: schmaler heller Rand, nach etwa 8 px dunkel; leicht im Uhrzeigersinn gekippt wie im Original
    bw, bh = s(2 * rx + 8), s(2 * ry + 8)
    btn = Image.new("RGBA", (bw, bh), (0, 0, 0, 0))
    bd = ImageDraw.Draw(btn)
    bx, by = bw / 2, bh / 2
    steps = 32
    for i in range(steps):
        t = i / (steps - 1)
        f = 1 - 0.95 * t
        k = min(1.0, t / 0.55) ** 0.7
        col = (int(112 - 102 * k), int(184 - 128 * k), int(46 - 43 * k), 255)
        bd.ellipse((bx - s(rx * f), by - s(ry * f), bx + s(rx * f), by + s(ry * f)), fill=col)
    bd.ellipse((bx - s(rx), by - s(ry), bx + s(rx), by + s(ry)), outline=(124, 194, 52, 255), width=s(1.3))
    btn = btn.rotate(-12, resample=Image.BICUBIC)
    layer.alpha_composite(btn, (int(s(cx) - bx), int(s(cy) - by)))
    # Buchstabe B eckig aus Balken (9 x 12 px, Strich 2 px), wie die Knopfschrift des Dashboards
    lx, ly, st = cx - 4.5, cy - 6, 2
    bars = [(0, 0, st, 12), (0, 0, 8, st), (0, 5, 9, 5 + st), (0, 12 - st, 9, 12),
            (8 - st, 0, 8, 6), (9 - st, 5, 9, 12)]
    for x0, y0, x1, y1 in bars:  # dunkle Kontur
        d.rectangle((s(lx + x0 - 0.7), s(ly + y0 - 0.7), s(lx + x1 + 0.7), s(ly + y1 + 0.7)), fill=(8, 40, 3, 255))
    for x0, y0, x1, y1 in bars:
        d.rectangle((s(lx + x0), s(ly + y0), s(lx + x1), s(ly + y1)), fill=(146, 206, 70, 255))
    # Beschriftung: steigt wie im Original um 8 Grad nach rechts an (folgt dem Faden); das rechte Ende sitzt
    # 21 px links und 13 px unter der Knopfmitte (gemessen an "ZURUECK")
    bb = d.textbbox((0, 0), label, font=flabel)
    lw, lh = bb[2] - bb[0], bb[3] - bb[1]
    pad = s(4)
    lab = Image.new("RGBA", (lw + 2 * pad, lh + 2 * pad), (0, 0, 0, 0))
    ImageDraw.Draw(lab).text((pad - bb[0], pad - bb[1]), label, font=flabel, fill=(140, 201, 25, 255))
    ang = math.radians(8)
    lab = lab.rotate(8, resample=Image.BICUBIC, expand=True)
    ex, ey = s(cx - 21), s(cy + 13)                      # rechtes Ende, Mitte der Schrifthoehe
    mx, my = ex - (lw / 2) * math.cos(ang), ey + (lw / 2) * math.sin(ang)
    layer.alpha_composite(lab, (int(mx - lab.width / 2), int(my - lab.height / 2)))


def panels():
    layer = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    glow = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    fh = ImageFont.truetype(FONT_HEAD, 12 * SS)
    ft = ImageFont.truetype(FONT_HEAD, 25 * SS)
    fs = ImageFont.truetype(FONT_HEAD, 11 * SS)
    for key, x0, y0, x1, y1, title in PANELS:
        draw_panel(layer, glow, x0, y0, x1, y1, title, fh)
    # Titel
    d = ImageDraw.Draw(layer)
    g = ImageDraw.Draw(glow)
    tx, ty = TITLE_POS
    # Zweizeiliger Titel: Name gross, Beschreibung klein darunter
    d.text((s(tx), s(ty - 18)), "CONDUIT", font=ft, fill=HEAD_TXT + (255,))
    g.text((s(tx), s(ty - 18)), "CONDUIT", font=ft, fill=(100, 150, 30, 140))
    d.text((s(tx + 2), s(ty + 14)), "FTP-SERVER", font=fs, fill=(150, 200, 60, 255))
    d.line([(s(tx), s(ty + 34)), (s(tx + 230), s(ty + 34))], fill=LINE + (255,), width=s(1))
    d.line([(s(tx + 230), s(ty + 34)), (s(tx + 240), s(ty + 26))], fill=LINE + (255,), width=s(1))
    draw_button(layer, BTN_B[0], BTN_B[1], "BEENDEN", ImageFont.truetype(FONT_HEAD, 9 * SS))
    layer = layer.resize((W, H), Image.LANCZOS)
    glow = glow.resize((W, H), Image.LANCZOS).filter(ImageFilter.GaussianBlur(4))
    return layer, glow


def make_overlay():
    """Panels + Leuchten als ein RGBA-Bild (normale Alpha-Mischung statt additiv)."""
    layer, glow = panels()
    r, g, b, a = glow.split()
    ov = Image.merge("RGBA", (r, g, b, a.point(lambda v: int(v * 0.6))))
    ov.alpha_composite(layer)
    return ov


# ---------------------------------------------------------------- Ausgabe
def c_array(name, data):
    lines = [f"const unsigned char {name}[] = {{"]
    for i in range(0, len(data), 20):
        lines.append("  " + ",".join(str(b) for b in data[i:i + 20]) + ",")
    lines.append("};")
    lines.append(f"const unsigned int {name}_len = {len(data)};")
    return "\n".join(lines) + "\n"


def main():
    tp = os.path.join(ASSETS, "tunnel.png")
    if "--tunnel" in sys.argv or not os.path.exists(tp):
        print("Rendere Referenz-Tunnel (dauert etwas) ...")
        tunnel_reference().convert("RGB").save(tp)

    cells, cw, ch = procedural_cells()
    cim = Image.new("L", (cw, ch))
    cim.putdata([int(v * 255 + 0.5) for row in cells for v in row])
    cp_ = os.path.join(ASSETS, "cells.png")
    cim.save(cp_, optimize=True)
    op = os.path.join(ASSETS, "overlay.png")
    make_overlay().save(op, optimize=True)

    with open(os.path.join(SRC, "gen_assets.c"), "w") as fp:
        fp.write("/* automatisch erzeugt von tools/make_assets.py - nicht von Hand aendern */\n")
        fp.write(c_array("asset_overlay_z", zlib.compress(Image.open(op).convert("RGBA").tobytes(), 9)))
        fp.write(c_array("asset_cells_z", zlib.compress(Image.open(cp_).convert("L").tobytes(), 9)))
        fp.write(c_array("asset_font_body", open(FONT_BODY, "rb").read()))
        fp.write(c_array("asset_font_head", open(FONT_HEAD, "rb").read()))
    with open(os.path.join(SRC, "gen_layout.h"), "w") as fp:
        fp.write("/* automatisch erzeugt von tools/make_assets.py */\n#pragma once\n")
        for key, x0, y0, x1, y1, title in PANELS:
            fp.write(f"#define PANEL_{key}_X0 {x0}\n#define PANEL_{key}_Y0 {y0}\n"
                     f"#define PANEL_{key}_X1 {x1}\n#define PANEL_{key}_Y1 {y1}\n")
        fp.write(f"#define TITLE_X {TITLE_POS[0]}\n#define TITLE_Y {TITLE_POS[1]}\n")
        fp.write(f"#define BTN_B_X {BTN_B[0]}\n#define BTN_B_Y {BTN_B[1]}\n")
        fp.write(f"#define CELLS_W {cw}\n#define CELLS_H {ch}\n")
        # Mittelwert von (Linienstaerke)^3 des eigenen Musters: Referenzhelligkeit fuer src/tunnel.c
        m3 = sum((v / 255.0) ** 3 for v in cim.tobytes()) / (cw * ch)
        fp.write(f"#define CELLS_REF_MEAN3 {m3:.6f}\n")
    print("geschrieben: assets/overlay.png, assets/cells.png, src/gen_assets.c, src/gen_layout.h")


if __name__ == "__main__":
    main()
