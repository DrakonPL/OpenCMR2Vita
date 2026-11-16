#!/usr/bin/env python3
"""Makes the LiveArea images of the .vpk from a background picture and a logo.

    tools/make_livearea.py PICTURE LOGO OUT_DIR [--focus X,Y]

PICTURE is a wide picture (1920x1080 or so). LOGO is a picture with
transparency. --focus is the point of the picture to keep in the middle of
the crops, as fractions of its width and height (default 0.5,0.5).

OUT_DIR gets sce_sys/icon0.png (128x128), sce_sys/livearea/contents/bg.png
(840x500) and startup.png (280x158), as 8-bit palette PNGs, and a copy of
res/vita/sce_sys/livearea/contents/template.xml. The build uses OUT_DIR in
place of res/vita when it is res/vita-local (see CMakeLists.txt). Needs Pillow.
"""

import argparse
import os
import shutil

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def crop_to(picture, width, height, focus, zoom=1.0):
    """The picture scaled to cover width x height (times zoom), cut around focus."""
    scale = max(width / picture.width, height / picture.height) * zoom
    scaled = picture.resize((round(picture.width * scale), round(picture.height * scale)), Image.LANCZOS)
    left = min(max(round(scaled.width * focus[0] - width / 2), 0), scaled.width - width)
    top = min(max(round(scaled.height * focus[1] - height / 2), 0), scaled.height - height)
    return scaled.crop((left, top, left + width, top + height))


def fit_square(picture, size, focus, width_fraction):
    """A size x size icon: width_fraction of the picture's width around focus,
    whole, on black above and below. The system rounds the corners."""
    side = round(picture.width * width_fraction)
    left = min(max(round(picture.width * focus[0] - side / 2), 0), picture.width - side)
    top = round(picture.height * focus[1] - side / 2)
    square = Image.new("RGB", (side, side), (0, 0, 0))
    square.paste(picture.crop((left, max(top, 0), left + side, min(top + side, picture.height))), (0, max(-top, 0)))
    return square.resize((size, size), Image.LANCZOS)


def shade_bottom(image, fraction, strength):
    """Darkens the bottom part of the image, so a white logo reads on it."""
    shade = Image.new("L", image.size, 0)
    draw = ImageDraw.Draw(shade)
    start = round(image.height * (1 - fraction))
    for y in range(start, image.height):
        draw.line([(0, y), (image.width, y)], fill=round(strength * (y - start) / (image.height - start)))
    black = Image.new("RGB", image.size, (0, 0, 0))
    return Image.composite(black, image, shade)


def put_logo(image, logo, width_fraction, bottom_margin):
    fitted = logo.copy()
    fitted.thumbnail((round(image.width * width_fraction), image.height), Image.LANCZOS)
    shadow = Image.new("RGBA", fitted.size, (0, 0, 0, 0))
    shadow.putalpha(fitted.getchannel("A").filter(ImageFilter.GaussianBlur(2)))
    x = (image.width - fitted.width) // 2
    y = image.height - fitted.height - bottom_margin
    image.paste(shadow, (x + 1, y + 1), shadow)
    image.paste(fitted, (x, y), fitted)
    return image


def save_palette(image, path):
    """8-bit palette PNG without transparency: the form the LiveArea takes."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    image.convert("RGB").quantize(256, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG).save(path, optimize=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("picture")
    parser.add_argument("logo")
    parser.add_argument("out")
    parser.add_argument("--focus", default="0.5,0.5")
    args = parser.parse_args()
    focus = tuple(float(v) for v in args.focus.split(","))
    picture = Image.open(args.picture).convert("RGB")
    logo = Image.open(args.logo).convert("RGBA")
    contents = os.path.join(args.out, "sce_sys", "livearea", "contents")

    bg = shade_bottom(crop_to(picture, 840, 500, focus), 0.35, 200)
    save_palette(put_logo(bg, logo, 0.62, 22), os.path.join(contents, "bg.png"))

    startup = shade_bottom(crop_to(picture, 280, 158, focus), 0.45, 210)
    save_palette(put_logo(startup, logo, 0.78, 8), os.path.join(contents, "startup.png"))

    save_palette(fit_square(picture, 128, focus, 0.88), os.path.join(args.out, "sce_sys", "icon0.png"))

    shutil.copy(os.path.join(ROOT, "res", "vita", "sce_sys", "livearea", "contents", "template.xml"), contents)
    print(f"livearea: images in {os.path.join(args.out, 'sce_sys')}")


if __name__ == "__main__":
    main()
