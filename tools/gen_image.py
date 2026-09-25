#!/usr/bin/env python3
"""Convert a photo to a raw RGB565 framebuffer image the firmware can embed.

    tools/gen_image.py IN.png OUT.bin [--w 240 --h 280] [--no-dither]
                       [--floor N] [--dim F] [--preview OUT.png]

The watch draws its dial over a full-colour photo, and the board has no image
decoder and no need of one: the photo is stored exactly as the canvas holds a
frame, so drawing it is a memcpy into the framebuffer. That layout is
uint16_t fb[y*w + x], row-major, top row first, each pixel RGB565 (red in the
top five bits, blue in the bottom five) as a little-endian uint16 -- the
native order on the ESP32-S3. Do not pre-swap the bytes for the panel:
display_blit() in display_st7789.c swaps the whole frame just before it goes
out over SPI and swaps it back after, so the canvas, and this file, stay in
native order. A pre-swapped file would come out with red shown as blue,
green as red and blue as green, the cycle described there.

The input is flattened onto black if it has alpha (the photos are padded
with black, and the rounded-corner zones must stay black), centre-cropped to
the target aspect and resized once with LANCZOS, but only if it is not
already W x H.

Dithering is on by default. Five bits of red and blue leave steps of about
8/255, which shows as contour lines in dark, smooth gradients -- a moon
limb, nebula shadow, the dim edge of the Earth. A 4x4 ordered (Bayer)
threshold of one output step turns those contours into a fine, fixed
texture that the panel's ~220 ppi hides. Ordered rather than error
diffusion because it has no running state: flat areas stay flat (pure black
stays exactly 0, full white stays 31/63/31), there are no crawling "worms"
across empty space, and the same pixel always gets the same threshold. The
three channels share one threshold per pixel so the noise is in brightness,
not coloured speckle. --no-dither rounds to nearest instead.

--floor N (default 4) sends any pixel whose three channels are all <= N to
pure black before that. Photographed space is not quite black: the Apollo
frame's sky is JPEG noise at 0-3, and dithering faithfully turns that into
a sparse field of one-step dots, which ends in a hard edge where the photo
meets the black padding added around it. Values that low are invisible on
their own, so clearing them costs nothing and keeps space black.

--dim F multiplies the photo by F before quantising. It is off (1.0) by
default because the firmware is meant to do the dimming. But halving
already-quantised 565 values throws away a bit, leaving shadows at 4/5/4
bits; dimming here, before the dither, keeps the full 5/6/5 for the
dimmed picture. Regenerate with --dim 0.5 if that is the route taken.

--preview decodes the written file back from disk (not from memory) to a
PNG, so what is eyeballed is what the board will get.
"""
import argparse
import struct
import sys

from PIL import Image, ImageOps

# 4x4 Bayer matrix, values 0..15. Threshold for a pixel is (v + 0.5) / 16.
BAYER4 = [[0, 8, 2, 10],
          [12, 4, 14, 6],
          [3, 11, 1, 9],
          [15, 7, 13, 5]]


def load(path, w, h):
    img = Image.open(path)
    if img.mode in ("RGBA", "LA") or (img.mode == "P" and "transparency" in img.info):
        rgba = img.convert("RGBA")
        black = Image.new("RGBA", rgba.size, (0, 0, 0, 255))
        img = Image.alpha_composite(black, rgba)
    img = img.convert("RGB")
    if img.size != (w, h):
        img = ImageOps.fit(img, (w, h), method=Image.Resampling.LANCZOS,
                           centering=(0.5, 0.5))
    return img


def quantise(v, bits, t, dim):
    """8-bit v to `bits` bits: floor(v * dim * max / 255 + t), clamped.

    t = 0.5 is plain rounding; a Bayer t averages to the exact level, so the
    mean colour of an area is preserved rather than shifted by the scale
    mismatch between 255 and 31 or 63.
    """
    top = (1 << bits) - 1
    q = int(v * dim * top / 255.0 + t)
    return top if q > top else q


def to_rgb565(img, dither, dim, floor):
    w, h = img.size
    px = img.load()
    out = []
    for y in range(h):
        row = BAYER4[y & 3]
        for x in range(w):
            t = (row[x & 3] + 0.5) / 16.0 if dither else 0.5
            r, g, b = px[x, y]
            if r <= floor and g <= floor and b <= floor:
                out.append(0)
                continue
            out.append((quantise(r, 5, t, dim) << 11)
                       | (quantise(g, 6, t, dim) << 5)
                       | quantise(b, 5, t, dim))
    return out


def decode(path, w, h):
    """Read OUT.bin back and expand 565 to 888 by bit replication."""
    with open(path, "rb") as f:
        data = f.read()
    rgb = bytearray()
    for v in struct.unpack("<%dH" % (w * h), data):
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        rgb += bytes((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))
    return Image.frombytes("RGB", (w, h), bytes(rgb))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("inp", metavar="IN")
    ap.add_argument("out", metavar="OUT.bin")
    ap.add_argument("--w", type=int, default=240)
    ap.add_argument("--h", type=int, default=280)
    ap.add_argument("--no-dither", dest="dither", action="store_false")
    ap.add_argument("--floor", type=int, default=4)
    ap.add_argument("--dim", type=float, default=1.0)
    ap.add_argument("--preview", metavar="OUT.png")
    a = ap.parse_args()

    if not 0.0 < a.dim <= 1.0:
        sys.exit("--dim must be in (0, 1]")
    img = load(a.inp, a.w, a.h)
    pixels = to_rgb565(img, a.dither, a.dim, a.floor)
    data = struct.pack("<%dH" % len(pixels), *pixels)
    assert len(data) == a.w * a.h * 2, len(data)
    with open(a.out, "wb") as f:
        f.write(data)

    back = decode(a.out, a.w, a.h)
    if a.preview:
        back.save(a.preview)
    src = img.point(lambda v: v * a.dim) if a.dim != 1.0 else img
    worst = [0, 0, 0]
    mean = [0.0, 0.0, 0.0]
    for i, (p, q) in enumerate(zip(src.tobytes(), back.tobytes())):
        d = q - p
        worst[i % 3] = max(worst[i % 3], abs(d))
        mean[i % 3] += d
    n = a.w * a.h
    print("%s: %dx%d, %d bytes, dither %s, floor %d, dim %.2f, "
          "max err R%d G%d B%d, mean shift %+.2f %+.2f %+.2f"
          % (a.out, a.w, a.h, len(data), "on" if a.dither else "off",
             a.floor, a.dim,
             worst[0], worst[1], worst[2],
             mean[0] / n, mean[1] / n, mean[2] / n))


if __name__ == "__main__":
    main()
