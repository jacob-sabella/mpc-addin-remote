#!/usr/bin/env python3
"""Draw the catalog tile (docs/tile.svg, docs/tile.png): the device's screen and the same screen in a browser,
with its pixels streaming across. Generated from a seed, so a rerun gives the same picture.

  python3 tools/gen_tile.py [--seed N]       needs chromium (or CHROMIUM=...) for the PNG
"""
import argparse
import os
import random
import shutil
import subprocess
import tempfile

W, H = 1200, 600                  # the catalog card shows its picture at 2:1
BG, INK, DIM = "#1b1c1f", "#f4f4f4", "#3a3c42"
ACCENT, AMBER = "#d63a30", "#e0a100"
PALETTE = ["#d63a30", "#e0a100", "#3f8f8a", "#5a6bd0", "#9b4fb0", "#2f6f4f", "#c46a2a", "#4c5560"]


def screen(rng_seed, x, y, w, h):
    """A made-up MPC page: header, a 4x4 pad grid in two rows of tiles, a tab bar. Same seed, same page."""
    rng = random.Random(rng_seed)
    out = ['<rect x="%g" y="%g" width="%g" height="%g" fill="#0e0f11"/>' % (x, y, w, h)]
    hh = h * 0.11
    out.append('<rect x="%g" y="%g" width="%g" height="%g" fill="#26282d"/>' % (x, y, w, hh))
    out.append('<rect x="%g" y="%g" width="%g" height="%g" rx="2" fill="%s"/>' % (x + w * 0.04, y + hh * 0.3, w * 0.22, hh * 0.4, DIM))
    out.append('<circle cx="%g" cy="%g" r="%g" fill="%s"/>' % (x + w * 0.93, y + hh / 2, hh * 0.22, ACCENT))
    cols, rows = 4, 4
    gx, gy, gw, gh = x + w * 0.04, y + hh + h * 0.05, w * 0.58, h * 0.68
    pw, ph = gw / cols, gh / rows
    for r in range(rows):
        for c in range(cols):
            col = rng.choice(PALETTE)
            lit = rng.random() < 0.35
            out.append('<rect x="%g" y="%g" width="%g" height="%g" rx="3" fill="%s" opacity="%s"/>'
                       % (gx + c * pw + 3, gy + r * ph + 3, pw - 6, ph - 6, col, "1" if lit else ".35"))
    # a mixer column of faders on the right
    fx, fw = x + w * 0.67, w * 0.29
    for i in range(4):
        cx = fx + fw * (i + 0.5) / 4
        out.append('<rect x="%g" y="%g" width="2" height="%g" fill="%s"/>' % (cx - 1, gy + 4, gh - 8, DIM))
        ky = gy + 4 + (gh - 8) * rng.uniform(0.15, 0.85)
        out.append('<rect x="%g" y="%g" width="%g" height="%g" rx="2" fill="%s"/>' % (cx - fw * 0.07, ky - 5, fw * 0.14, 10, INK))
    ty = y + h - h * 0.09
    out.append('<rect x="%g" y="%g" width="%g" height="%g" fill="#26282d"/>' % (x, ty, w, h * 0.09))
    for i in range(5):
        out.append('<rect x="%g" y="%g" width="%g" height="%g" rx="2" fill="%s"/>'
                   % (x + w * (0.03 + i * 0.195), ty + h * 0.025, w * 0.15, h * 0.04, AMBER if i == 0 else DIM))
    return "\n".join(out)


def ripple(cx, cy, s):
    return "\n".join('<circle cx="%g" cy="%g" r="%g" fill="none" stroke="%s" stroke-width="%g" opacity="%g"/>'
                     % (cx, cy, s * k, INK, 2.2 - k * 0.5, 1 - k * 0.28) for k in (1, 2, 3))


def tile(seed):
    rng = random.Random(seed)
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">' % (W, H, W, H),
         '<rect width="%d" height="%d" fill="%s"/>' % (W, H, BG)]
    for gx in range(12, W, 24):          # faint dot grid
        for gy in range(12, H, 24):
            o.append('<circle cx="%d" cy="%d" r="1" fill="#2a2c31"/>' % (gx, gy))

    # the device: a 16:10 screen in a body
    dx, dy, dw, dh = 70, 175, 360, 225
    o.append('<rect x="%g" y="%g" width="%g" height="%g" rx="14" fill="#2b2d33" stroke="#44474e" stroke-width="2"/>'
             % (dx - 22, dy - 22, dw + 44, dh + 70))
    o.append(screen(seed, dx, dy, dw, dh))
    for i in range(6):                    # a row of buttons under the screen
        o.append('<rect x="%g" y="%g" width="34" height="14" rx="3" fill="%s"/>'
                 % (dx + 8 + i * 58, dy + dh + 19, ACCENT if i == 0 else "#44474e"))

    # the browser: the same screen, larger
    bx, by, bw = 640, 110, 500
    bh = bw * 10 / 16
    o.append('<rect x="%g" y="%g" width="%g" height="%g" rx="10" fill="#2b2d33" stroke="#44474e" stroke-width="2"/>'
             % (bx - 10, by - 46, bw + 20, bh + 56))
    for i, c in enumerate((ACCENT, AMBER, "#3f8f8a")):
        o.append('<circle cx="%g" cy="%g" r="6" fill="%s"/>' % (bx + 10 + i * 20, by - 23, c))
    o.append('<rect x="%g" y="%g" width="%g" height="24" rx="12" fill="#1b1c1f"/>' % (bx + 80, by - 35, bw - 90))
    o.append('<text x="%g" y="%g" font-family="DejaVu Sans Mono, monospace" font-size="14" fill="#9a9ca3">'
             'localhost:6720</text>' % (bx + 98, by - 18))
    o.append(screen(seed, bx, by, bw, bh))

    # a touch on the browser, the same touch landing on the device
    u, v = 0.27, 0.43
    o.append(ripple(bx + bw * u, by + bh * v, 11))
    o.append(ripple(dx + dw * u, dy + dh * v, 8))

    # pixels streaming device -> browser, thinning out and growing as they travel
    x0, x1 = dx + dw + 30, bx - 26
    for _ in range(320):
        t = rng.random()
        x = x0 + (x1 - x0) * t
        mid = dy + dh * 0.5 + (by + bh * 0.5 - dy - dh * 0.5) * t
        spread = 18 + 70 * t
        y = mid + rng.gauss(0, spread * 0.5)
        s = 2 + 9 * t * rng.random()
        c = rng.choice(PALETTE[:3] + [INK])
        o.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s" opacity="%.2f"/>'
                 % (x, y, s, s, c, 0.25 + 0.65 * rng.random()))
    # and touches going back, a dashed return line
    o.append('<path d="M %g %g C %g %g, %g %g, %g %g" fill="none" stroke="%s" stroke-width="2" stroke-dasharray="6 7" opacity=".7"/>'
             % (x1, dy + dh + 40, (x0 + x1) / 2, dy + dh + 110, (x0 + x1) / 2, dy + dh + 110, x0, dy + dh + 40, AMBER))
    o.append('<path d="M %g %g l 12 -7 l 0 14 z" fill="%s" opacity=".85"/>' % (x0 - 4, dy + dh + 40, AMBER))
    o.append("</svg>")
    return "\n".join(o)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=6720)
    a = ap.parse_args()
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs")
    os.makedirs(root, exist_ok=True)
    svg = os.path.join(root, "tile.svg")
    with open(svg, "w") as f:
        f.write(tile(a.seed))
    chrome = os.environ.get("CHROMIUM") or shutil.which("chromium") or shutil.which("chromium-browser")
    if not chrome:
        print("wrote %s (no chromium for the PNG)" % svg)
        return
    with tempfile.TemporaryDirectory() as tmp:
        page = os.path.join(tmp, "t.html")
        with open(page, "w") as f:
            f.write('<html><body style="margin:0">%s</body></html>' % open(svg).read())
        png = os.path.abspath(os.path.join(root, "tile.png"))
        subprocess.run([chrome, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--user-data-dir=" + tmp,
                        "--window-size=%d,%d" % (W, H), "--screenshot=" + png, "file://" + page],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("wrote %s and %s" % (svg, png))


if __name__ == "__main__":
    main()
