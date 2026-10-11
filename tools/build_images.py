#!/usr/bin/env python3
"""Download SwissMicros calculator photos from the webshop and render them as
800x480 frames dithered to the reTerminal E1002's 6-colour Spectra E6 palette.

Output: images/NN_<slug>.png (pure palette colours; committed)
        gallery_images.h     (the PNGs packed as 4-bit palette indices for the
                              firmware; generated, not committed)

Usage:  python tools/build_images.py --font-dir DIR   download, render and pack
        python tools/build_images.py --pack-only      only pack existing PNGs (CI)
"""

import argparse
import io
import re
import urllib.request
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageEnhance, ImageFont

SHOP = "https://www.swissmicros.com/en/products/"
ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "images"
W, H = 800, 480

# (slug, model, subtitle) - display order
CALCULATORS = [
    ("dm11l", "DM11L", "Scientific RPN Calculator"),
    ("dm12l", "DM12L", "Financial RPN Calculator"),
    ("dm12c", "DM12C", "Financial RPN Calculator"),
    ("dm15l", "DM15L", "Scientific RPN Calculator"),
    ("dm15c", "DM15C", "Scientific RPN Calculator"),
    ("dm16l", "DM16L", "Programmer's Calculator"),
    ("dm16c", "DM16C", "Programmer's Calculator"),
    ("dm32", "DM32", "Scientific RPN Calculator"),
    ("dm41l", "DM41L", "System Calculator"),
    ("dm41c", "DM41C", "System Calculator"),
    ("dm41x", "DM41X", "System Calculator"),
    ("dm42n", "DM42n", "Scientific RPN Calculator"),
    ("r47", "R47", "Scientific RPN Calculator"),
]

# Pure colours sent to the panel, and how they actually look on the panel.
# Dithering against the perceived colours gives far better results.
PURE = [(0, 0, 0), (255, 255, 255), (255, 255, 0), (255, 0, 0), (0, 0, 255), (0, 255, 0)]
PERCEIVED = [(25, 30, 33), (232, 232, 232), (239, 222, 68), (178, 19, 24), (33, 87, 186), (18, 95, 32)]
BLACK, WHITE, RED = PURE[0], PURE[1], PURE[3]
SHADOW_GAMMA = 0.7
NEUTRAL_SATURATION = 90  # max(R,G,B)-min(R,G,B) below this is treated as grey


def fetch(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "reterminal-image-builder"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read()


def product_photo(slug: str) -> Image.Image:
    html = fetch(SHOP + slug).decode("utf-8", "replace")
    m = re.search(r'https://swissmicros-webshop-media[^"\\ ]*?\.(?:md|lg)\.webp', html)
    if not m:
        raise RuntimeError(f"no product image found for {slug}")
    url = re.sub(r"\.md\.webp$", ".lg.webp", m.group(0))
    im = Image.open(io.BytesIO(fetch(url))).convert("RGBA")
    return im.crop(im.getbbox())  # trim transparent padding


def dither(im: Image.Image) -> Image.Image:
    """Floyd-Steinberg against the perceived palette. Near-neutral source pixels
    (the calculator bodies and keys) may only use black/white, otherwise dark
    greys turn into red/green noise; coloured pixels get the full palette."""
    im = ImageEnhance.Contrast(im).enhance(1.15)
    # Lift the shadows so dark keys stay distinguishable from the black body.
    src = 255 * (np.asarray(im, dtype=np.float32) / 255) ** SHADOW_GAMMA
    sat = src.max(axis=2) - src.min(axis=2)
    neutral = (sat < NEUTRAL_SATURATION) | (src.max(axis=2) < 100)  # greys and near-blacks
    buf = src.copy()
    perceived = np.array(PERCEIVED, dtype=np.float32)
    out = np.zeros(src.shape[:2], dtype=np.uint8)
    h, w = out.shape
    for y in range(h):
        row, nxt = buf[y], buf[y + 1] if y + 1 < h else None
        for x in range(w):
            px = row[x]
            cand = perceived[:2] if neutral[y, x] else perceived
            idx = int(((cand - px) ** 2).sum(axis=1).argmin())
            out[y, x] = idx
            err = px - perceived[idx]
            if x + 1 < w:
                row[x + 1] += err * 7 / 16
            if nxt is not None:
                if x > 0:
                    nxt[x - 1] += err * 3 / 16
                nxt[x] += err * 5 / 16
                if x + 1 < w:
                    nxt[x + 1] += err * 1 / 16
    return Image.fromarray(np.array(PURE, dtype=np.uint8)[out], "RGB")


def render(photo: Image.Image, model: str, subtitle: str, fonts: Path) -> Image.Image:
    bold = ImageFont.truetype(str(fonts / "Theinhardt-Bold.otf"), 96)
    medium = ImageFont.truetype(str(fonts / "Theinhardt-Medium.otf"), 30)
    small = ImageFont.truetype(str(fonts / "Theinhardt-Medium.otf"), 22)

    canvas = Image.new("RGB", (W, H), WHITE)
    margin = 16
    portrait = photo.height > photo.width
    if portrait:
        # Calculator on the left at full height, text column on the right.
        box = (W // 2 - margin, H - 2 * margin)
        p = photo.copy()
        p.thumbnail(box, Image.Resampling.LANCZOS)
        pos = (margin + (box[0] - p.width) // 2, margin + (box[1] - p.height) // 2)
        text_x, text_y, text_w = W // 2 + 10, 150, W // 2 - 10 - margin
    else:
        # Calculator across the top, text below.
        box = (W - 2 * margin, H - 150)
        p = photo.copy()
        p.thumbnail(box, Image.Resampling.LANCZOS)
        pos = ((W - p.width) // 2, margin)
        text_x, text_y, text_w = margin + 8, pos[1] + p.height + 6, W - 2 * margin
    layer = Image.new("RGB", p.size, WHITE)
    layer.paste(p, mask=p.split()[3])
    canvas.paste(dither(layer), pos)

    # Text is drawn after dithering without anti-aliasing so edges stay crisp.
    d = ImageDraw.Draw(canvas)
    d.fontmode = "1"
    d.text((text_x, text_y), model, font=bold, fill=BLACK)
    if portrait:
        d.rectangle((text_x + 2, text_y + 112, text_x + 82, text_y + 118), fill=RED)
        d.text((text_x, text_y + 136), subtitle, font=medium, fill=BLACK)
        d.text((text_x, H - margin - 30), "swissmicros.com", font=small, fill=RED)
    else:
        mw = d.textlength(model, font=bold)
        d.rectangle((text_x + mw + 24, text_y + 46, text_x + mw + 30, text_y + 96), fill=RED)
        d.text((text_x + mw + 50, text_y + 36), subtitle, font=medium, fill=BLACK)
        d.text((text_x + mw + 50, text_y + 76), "swissmicros.com", font=small, fill=RED)
    return canvas


def pack(pngs: list[Path]) -> None:
    """Write gallery_images.h: two pixels per byte (high nibble first), each a
    PURE palette index. 192 KB per image instead of 768 KB as RGB565."""
    keys = np.array([(r << 16) | (g << 8) | b for r, g, b in PURE])
    out = [
        "#pragma once",
        "// Generated by tools/build_images.py - do not edit.",
        "#include <cstdint>",
        "",
        f"static constexpr int GALLERY_COUNT = {len(pngs)};",
        f"static constexpr int GALLERY_W = {W}, GALLERY_H = {H};",
        "static const uint8_t GALLERY_PALETTE[][3] = {"
        + ", ".join("{%d, %d, %d}" % rgb for rgb in PURE)
        + "};",
        "",
    ]
    for i, png in enumerate(pngs):
        im = Image.open(png).convert("RGB")
        if im.size != (W, H):
            raise ValueError(f"{png} is {im.size}, expected {(W, H)}")
        rgb = np.asarray(im, dtype=np.uint32).reshape(-1, 3)
        key = (rgb[:, 0] << 16) | (rgb[:, 1] << 8) | rgb[:, 2]
        if not np.isin(key, keys).all():
            raise ValueError(f"{png} contains colours outside the panel palette")
        idx = np.searchsorted(keys, key, sorter=np.argsort(keys))
        idx = np.argsort(keys)[idx].astype(np.uint8)
        packed = (idx[0::2] << 4) | idx[1::2]
        body = ",".join(map(str, packed.tolist()))
        out.append(f"// {png.name}")
        out.append(f"static const uint8_t gallery_image_{i}[] = {{{body}}};")
    out.append(
        "static const uint8_t *const GALLERY_IMAGES[] = {"
        + ", ".join(f"gallery_image_{i}" for i in range(len(pngs)))
        + "};"
    )
    (ROOT / "gallery_images.h").write_text("\n".join(out) + "\n")
    print(f"wrote gallery_images.h with {len(pngs)} images")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--font-dir", type=Path, default=ROOT / "fonts")
    ap.add_argument("--pack-only", action="store_true", help="skip download/render, pack images/*.png")
    args = ap.parse_args()

    if not args.pack_only:
        OUT.mkdir(exist_ok=True)
        for old in OUT.glob("*.png"):
            old.unlink()
        for i, (slug, model, subtitle) in enumerate(CALCULATORS):
            name = f"{i:02d}_{slug}"
            render(product_photo(slug), model, subtitle, args.font_dir).save(OUT / f"{name}.png")
            print(f"rendered {name}")
    pack(sorted(OUT.glob("*.png")))


if __name__ == "__main__":
    main()
