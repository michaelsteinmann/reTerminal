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
import html
import io
import re
import urllib.request
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

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
LEVEL_BLACK, LEVEL_WHITE = 30, 225  # stretched to 0..255 before dithering
NEUTRAL_SATURATION = 90  # max(R,G,B)-min(R,G,B) below this is treated as grey


def fetch(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "reterminal-image-builder"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read()


def product_page(slug: str) -> tuple[Image.Image, str]:
    """Main product photo and the first paragraph of the product description."""
    page = fetch(SHOP + slug).decode("utf-8", "replace")
    m = re.search(r'https://swissmicros-webshop-media[^"\\ ]*?\.(?:md|lg)\.webp', page)
    if not m:
        raise RuntimeError(f"no product image found for {slug}")
    url = re.sub(r"\.md\.webp$", ".lg.webp", m.group(0))
    im = Image.open(io.BytesIO(fetch(url))).convert("RGBA")
    d = re.search(r'<div class="prose[^"]*"><p>(.*?)</p>', page, re.S)
    if not d:
        raise RuntimeError(f"no product description found for {slug}")
    # Tags become spaces: some descriptions separate sentences with <br>.
    text = html.unescape(re.sub(r"<[^>]+>", " ", d.group(1)))
    text = re.sub(r"\s+", " ", text).strip()
    return im.crop(im.getbbox()), text  # trim transparent padding


def wrap(draw: ImageDraw.ImageDraw, text: str, font: ImageFont.FreeTypeFont, width: int) -> list[str]:
    lines, line = [], ""
    for word in text.split():
        candidate = f"{line} {word}".strip()
        if line and draw.textlength(candidate, font=font) > width:
            lines.append(line)
            line = word
        else:
            line = candidate
    return lines + [line] if line else lines


def fit_description(draw, text, font, width, max_lines) -> list[str]:
    """Wrap text into at most max_lines lines, cutting at the last whole sentence
    that fits; if not even the first sentence fits, cut it with an ellipsis."""
    sentences = re.split(r"(?<=[.!?])\s+", text)
    for n in range(len(sentences), 0, -1):
        lines = wrap(draw, " ".join(sentences[:n]), font, width)
        if len(lines) <= max_lines:
            return lines
    lines = wrap(draw, text, font, width)[:max_lines]
    while draw.textlength(lines[-1] + "…", font=font) > width:
        lines[-1] = lines[-1].rsplit(" ", 1)[0]
    lines[-1] += "…"
    return lines


# Atkinson error diffusion: spreads only 6/8 of the error, so near-black bodies and
# near-white LCDs stay solid and small key legends stay crisp (Floyd-Steinberg
# turned them into speckle). (dx, dy, weight) relative to the current pixel.
ATKINSON = [(1, 0, 1 / 8), (2, 0, 1 / 8), (-1, 1, 1 / 8), (0, 1, 1 / 8), (1, 1, 1 / 8), (0, 2, 1 / 8)]


def dither(im: Image.Image) -> Image.Image:
    """Sharpen, stretch levels, then Atkinson-dither (serpentine) against the
    perceived palette. Near-neutral pixels (calculator bodies, keys, LCDs) may
    only use black/white, otherwise greys turn into red/green noise."""
    im = im.filter(ImageFilter.UnsharpMask(radius=1.2, percent=120, threshold=2))
    src = np.asarray(im, dtype=np.float32)
    src = 255 * np.clip((src - LEVEL_BLACK) / (LEVEL_WHITE - LEVEL_BLACK), 0, 1)
    sat = src.max(axis=2) - src.min(axis=2)
    neutral = (sat < NEUTRAL_SATURATION) | (src.max(axis=2) < 100)  # greys and near-blacks
    buf = src.copy()
    perceived = np.array(PERCEIVED, dtype=np.float32)
    h, w = neutral.shape
    out = np.zeros((h, w), dtype=np.uint8)
    for y in range(h):
        rev = y % 2 == 1  # serpentine: alternate direction, avoids diagonal streaks
        for x in range(w - 1, -1, -1) if rev else range(w):
            px = buf[y, x]
            cand = perceived[:2] if neutral[y, x] else perceived
            idx = int(((cand - px) ** 2).sum(axis=1).argmin())
            out[y, x] = idx
            err = px - perceived[idx]
            for dx, dy, f in ATKINSON:
                xx, yy = x - dx if rev else x + dx, y + dy
                if 0 <= xx < w and yy < h:
                    buf[yy, xx] += err * f
    return Image.fromarray(np.array(PURE, dtype=np.uint8)[out], "RGB")


def render(photo: Image.Image, model: str, subtitle: str, description: str, fonts: Path) -> Image.Image:
    """Calculator on the left, text column (model, subtitle, description) on the right.
    The bottom-right corner (x >= 684, y >= 446) stays empty for the battery icon."""
    canvas = Image.new("RGB", (W, H), WHITE)
    margin = 16
    p = photo.copy()
    p.thumbnail((int(W * 0.6), H - 2 * margin), Image.Resampling.LANCZOS)
    pos = (margin, (H - p.height) // 2)
    layer = Image.new("RGB", p.size, WHITE)
    layer.paste(p, mask=p.split()[3])
    canvas.paste(dither(layer), pos)

    # Text is drawn after dithering without anti-aliasing so edges stay crisp.
    d = ImageDraw.Draw(canvas)
    d.fontmode = "1"
    x = pos[0] + p.width + 32
    col_w = W - margin - x
    size = 96
    while True:
        title = ImageFont.truetype(str(fonts / "Theinhardt-Bold.otf"), size)
        if d.textlength(model, font=title) <= col_w:
            break
        size -= 4
    medium = ImageFont.truetype(str(fonts / "Theinhardt-Medium.otf"), 24)
    body = ImageFont.truetype(str(fonts / "Theinhardt-Regular.otf"), 20)
    small = ImageFont.truetype(str(fonts / "Theinhardt-Medium.otf"), 20)

    y = 36
    d.text((x, y), model, font=title, fill=BLACK)
    y = d.textbbox((x, y), model, font=title)[3] + 14
    d.rectangle((x + 2, y, x + 72, y + 5), fill=RED)
    y += 22
    for line in wrap(d, subtitle, medium, col_w):
        d.text((x, y), line, font=medium, fill=BLACK)
        y += 30
    y += 14
    line_h = 26
    bottom = H - margin - 34  # keep clear of the "swissmicros.com" line
    for line in fit_description(d, description, body, col_w, (bottom - y) // line_h):
        d.text((x, y), line, font=body, fill=BLACK)
        y += line_h
    d.text((x, H - margin - 24), "swissmicros.com", font=small, fill=RED)
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
            photo, description = product_page(slug)
            render(photo, model, subtitle, description, args.font_dir).save(OUT / f"{name}.png")
            print(f"rendered {name}")
    pack(sorted(OUT.glob("*.png")))


if __name__ == "__main__":
    main()
