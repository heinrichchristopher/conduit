#!/usr/bin/env python3
"""
Erzeugt das Logo (SVG + PNG) und das GitHub-Vorschaubild.

  assets/logo/logo.svg            Symbol, Vektor (ohne Schrift, ohne Filter)
  assets/logo/logo-512.png        Symbol als PNG (aus derselben Geometrie)
  assets/logo/social-preview.png  1280x640 fuer GitHub (Settings -> Social preview)

Eigenes Material: Rahmenform wie die Panels der Oberflaeche, gestaffelt wie ein Tunnel.
Schrift: Michroma (OFL).
"""
import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FTP = os.path.dirname(HERE)
OUT = os.path.join(FTP, "assets", "logo")
FONT = os.path.join(FTP, "assets", "fonts", "Michroma-Regular.ttf")
NAME = "CONDUIT"
TAGLINE = "FTP Server for the original Xbox"

LINE = "#b2d628"
BG = "#05170a"
ARROW = "#e2f0ce"


def chamfer(x0, y0, x1, y1, c):
    return [(x0 + c, y0), (x1, y0), (x1, y1 - c), (x1 - c, y1), (x0, y1), (x0, y0 + c)]


def pts(p):
    return " ".join(f"{x:.1f},{y:.1f}" for x, y in p)


def shapes():
    """Gemeinsame Geometrie: (Punkte, Fuellfarbe|None, Fuell-Alpha, Linienfarbe, Linien-Alpha, Breite)"""
    out = []
    for k in range(5):
        s = 0.78 ** k
        w = 188 * s
        cx, cy = 128 + 9 * k, 128 - 7 * k
        p = chamfer(cx - w / 2, cy - w / 2 * 0.86, cx + w / 2, cy + w / 2 * 0.86, 22 * s)
        op = 1.0 - 0.17 * k
        sw = 7 * s + 1.2
        out.append((p, BG, 0.55, LINE, op, sw))                    # Rahmen
    def arrow(cx, cy, up):
        d = -1 if up else 1
        return [(cx, cy - d * 26), (cx + 17, cy - d * 4), (cx + 6, cy - d * 4), (cx + 6, cy + d * 26),
                (cx - 6, cy + d * 26), (cx - 6, cy - d * 4), (cx - 17, cy - d * 4)]
    for cx, up in ((150, True), (112, False)):
        a = arrow(cx, 130, up)
        out.append((a, LINE, 0.25, LINE, 0.25, 9))                 # Leuchten
        out.append((a, ARROW, 1.0, ARROW, 1.0, 2))
    return out


def svg():
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">',
         '<title>Conduit</title>',
         f'<rect width="256" height="256" rx="44" fill="{BG}"/>']
    for p, fill, fa, line, la, w in shapes():
        f = f'fill="{fill}" fill-opacity="{fa:.2f}"' if fill else 'fill="none"'
        o.append(f'<polygon points="{pts(p)}" {f} stroke="{line}" stroke-opacity="{la:.2f}" '
                 f'stroke-width="{w:.1f}" stroke-linejoin="round"/>')
    o.append("</svg>")
    return "\n".join(o) + "\n"


def hexrgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


def render_png(path, size=512, ss=4):
    S = size * ss
    k = S / 256
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    m = Image.new("L", (S, S), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, S - 1, S - 1), radius=44 * k, fill=255)
    img.paste(hexrgb(BG) + (255,), (0, 0), m)
    for p, fill, fa, line, la, w in shapes():
        pp = [(x * k, y * k) for x, y in p]
        layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        d = ImageDraw.Draw(layer)
        if fill:
            d.polygon(pp, fill=hexrgb(fill) + (int(255 * fa),))
        lay2 = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        d2 = ImageDraw.Draw(lay2)
        d2.line(pp + [pp[0], pp[1]], fill=hexrgb(line) + (int(255 * la),), width=max(1, int(w * k)), joint="curve")
        for x, y in pp:  # runde Ecken
            r = w * k / 2
            d2.ellipse((x - r, y - r, x + r, y + r), fill=hexrgb(line) + (int(255 * la),))
        img.alpha_composite(layer)
        img.alpha_composite(lay2)
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(img, (0, 0), m)
    out.resize((size, size), Image.LANCZOS).save(path)


def main():
    os.makedirs(OUT, exist_ok=True)
    sp = os.path.join(OUT, "logo.svg")
    open(sp, "w").write(svg())
    png = os.path.join(OUT, "logo-512.png")
    render_png(png)

    # Vorschaubild 1280x640
    W, H = 1280, 640
    bg_src = os.path.join(FTP, "assets", "tunnel.png")
    im = Image.open(bg_src).convert("RGB").resize((W, int(W * 480 / 640))).crop((0, 80, W, 80 + H))
    im = Image.eval(im, lambda v: int(v * 0.55))
    dark = Image.new("RGB", (W, H), (2, 12, 4))
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).ellipse((-300, -200, W + 300, H + 200), fill=255)
    im = Image.composite(im, dark, mask.filter(ImageFilter.GaussianBlur(120)))
    ic = Image.open(png).convert("RGBA").resize((400, 400), Image.LANCZOS)
    im.paste(ic, (130, 120), ic)
    d = ImageDraw.Draw(im)
    f1 = ImageFont.truetype(FONT, 84)
    f2 = ImageFont.truetype(FONT, 25)
    d.text((570, 232), NAME, font=f1, fill=(226, 240, 206))
    d.line([(574, 350), (1150, 350)], fill=(178, 214, 40), width=3)
    d.text((574, 376), TAGLINE, font=f2, fill=(160, 205, 70))
    im.save(os.path.join(OUT, "social-preview.png"), optimize=True)
    print("geschrieben:", sp, png, "social-preview.png")


if __name__ == "__main__":
    main()
