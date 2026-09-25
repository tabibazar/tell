#!/usr/bin/env python3
"""Generate an anti-aliased font header for main/aafont.c from a TrueType font.

    tools/gen_aafont.py TTF SIZE --name NAME [--charset SETS] [--extra CHARS]
                        [--tnum] [--case] [--minus] [--no-kern] > main/font_NAME.h

SIZE is the em in pixels (fractions allowed). Each glyph is rendered by
FreeType (freetype-py), hinted and grayscale, and kept as 4-bit coverage --
never thresholded, which is the whole point: the 12x24 table gen_font.py makes
is ink or no ink, and on envo's 247 ppi panel every curve of it is a staircase.
Coverage is linear (the share of the pixel the outline covers); main/aafont.c
does the blending, in linear light, so none of that belongs here.

The charset is named sets joined by "+" -- ascii (space to tilde), upper,
lower, digits, space -- plus any literal characters in --extra, which is where
the degree sign goes. An empty --charset takes --extra alone. Glyphs are
cropped to their ink, rows padded to whole bytes, two pixels a byte with the
LEFT pixel in the HIGH nibble.

Advances and kerning are kept in sixteenths of a pixel and the glyphs placed
at the nearest whole pixel, as FreeType lays text out when it is not
positioning by subpixels: the hinted advance is rounded to a whole pixel per
glyph, so a 17 px space came out 4 px in one weight and 5 in the next and
every word's spacing wandered by a pixel. The glyph images are hinted by
FreeType's v40 interpreter running Inter's own instructions, which snap only
vertically -- baselines, x-height and cap height land on pixel rows -- and
their advances are not.

Horizontally nothing snaps, so where a vertical stem falls is luck: at 17 px
the left stem of an I, H, E or N came out as coverage 9/15/4, three columns
of grey for what is a stem and a half of ink, and a word of them looked out
of focus. So each glyph is rendered four times, shifted right by 0, 1/4, 1/2
and 3/4 of a pixel after hinting, and the one with the fewest part-covered
pixels kept (the least sum of c * (15 - c) over its 4-bit coverage c): a stem
that can land on the grid does, as 13/15/0 rather than 9/15/4. The shift is
kept in the glyph's x, as the nearest of +1/4, -1/4 (the 3/4 render, a pixel
left) and +1/2, ties going to the smallest; the advance is untouched, so no
word gets longer, and a glyph stands at most half a pixel from where the
design puts it -- no further than a hinter that snapped stems would move it,
and without changing any stem's weight, as such a hinter does (FreeType's
auto-hinter at full strength drew Inter Medium's labels a weight heavier).

Four things want fontTools as well as freetype-py:
  --tnum  tabular figures: Inter's own zero.tf..nine.tf, found through its
          GSUB 'tnum' lookups and put in the cmap in place of the proportional
          ones before FreeType sees the font -- the digits only, not the
          punctuation 'tnum' also respaces (see remap). Without fontTools the
          proportional digits are used, each centred in the widest one's
          advance.
  --case  the case-sensitive forms ('case'): a hyphen or colon raised to sit
          on the capitals' middle, as "-24H" and "21:47" want, rather than on
          the lower case's. Without fontTools, ignored.
  --minus "-" drawn as the minus sign (U+2212), as wide as the plus and on
          its axis, for faces that set signed numbers: "-5.2" with a hyphen
          reads as a dash beside the number. Without fontTools, ignored.
          (A text face keeps its hyphen and takes U+2212 in --extra, to be
          asked for by name where a minus is meant.)
  kerning the GPOS 'kern' pairs between the charset's glyphs, in 1/16 px.
          Pairs under a quarter pixel are dropped: they move a glyph at most
          one pixel one time in four, and cost four bytes each. FreeType's
          own kerning only reads the old 'kern' table, which Inter does not
          have. Without fontTools, none; --no-kern turns it off anyway.
FreeType does no layout, so the features are applied to the font rather than
asked of a shaper.

The python.org python3 on this Mac has fontTools and freetype-py (which
brings its own FreeType; `python3 -m pip install freetype-py`); without
freetype-py this will not run at all, and without fontTools it refuses unless
--allow-no-fonttools says a lesser table is meant.

The faces envo sets (main/aafont.h says what each is for). Medium for the
two text sizes, SemiBold for the rest: on a bright-on-black IPS the light
spreads into the black around it, so type there looks a weight heavier than
it is, and SemiBold grey labels at 17 px close up their e's and s's. The
label and secondary sizes have proportional figures: they set words like
"eCO2" and dates, where a tabular figure opens a gap (a tabular 1 takes five
pixels more than its own figure, so "11 Sep" opened up by ten), and no column
of numbers.

    F=assets/fonts
    python3 tools/gen_aafont.py $F/Inter-Medium.ttf 17 --name inter_label \
        --charset ascii --extra "°−" --case > main/font_inter_label.h
    python3 tools/gen_aafont.py $F/Inter-Medium.ttf 23 --name inter_sub \
        --charset ascii --extra ° --case > main/font_inter_sub.h
    python3 tools/gen_aafont.py $F/Inter-SemiBold.ttf 34 --name inter_word \
        --charset upper+digits --extra " .,:+-%/°es" --tnum --case --minus \
        > main/font_inter_word.h
    python3 tools/gen_aafont.py $F/Inter-SemiBold.ttf 44 --name inter_state \
        --charset upper+digits --extra " .,:+-%/°es" --tnum --case --minus \
        > main/font_inter_state.h
    python3 tools/gen_aafont.py $F/Inter-SemiBold.ttf 64 --name inter_number \
        --charset digits --extra " .:+-%°" --tnum --case --minus \
        > main/font_inter_number.h
    python3 tools/gen_aafont.py $F/Inter-SemiBold.ttf 77 --name inter_reading \
        --charset digits --extra " .-°" --tnum --case --minus \
        > main/font_inter_reading.h

and the word POOR a weight up, for its knock-out -- black on the red block,
where the same weight as the white words beside it reads a weight lighter
(see aafont.h):

    python3 tools/gen_aafont.py $F/Inter-SemiBold.ttf 17 --name inter_label_knock \
        --charset "" --extra POR > main/font_inter_label_knock.h
    python3 tools/gen_aafont.py $F/Inter-SemiBold.ttf 23 --name inter_sub_knock \
        --charset "" --extra POR > main/font_inter_sub_knock.h
    python3 tools/gen_aafont.py $F/Inter-Bold.ttf 34 --name inter_word_knock \
        --charset "" --extra POR > main/font_inter_word_knock.h
    python3 tools/gen_aafont.py $F/Inter-Bold.ttf 44 --name inter_state_knock \
        --charset "" --extra POR > main/font_inter_state_knock.h

Each header also records the command that made it, at its top.
"""
import argparse
import io
import os
import shlex
import sys

try:
    import freetype
except ImportError:
    sys.exit("freetype-py is missing: python3 -m pip install freetype-py")

try:
    from fontTools.ttLib import TTFont
except ImportError:          # the fallbacks above apply
    TTFont = None

SETS = {
    "ascii": "".join(chr(c) for c in range(32, 127)),
    "upper": "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "lower": "abcdefghijklmnopqrstuvwxyz",
    "digits": "0123456789",
    "space": " ",
}

DIGITS = "0123456789"

# Advances and kerning are in 1/SUB px; aafont.h's AAFONT_SUB must agree.
SUB = 16


def charset(sets, extra):
    chars = set()
    for name in sets.split("+") if sets else []:
        if name not in SETS:
            sys.exit("unknown charset %r (have %s)" % (name, ", ".join(SETS)))
        chars.update(SETS[name])
    chars.update(extra or "")
    for ch in chars:
        if ord(ch) > 0xFFFF:
            sys.exit("U+%04X is outside the BMP; aafont_glyph_t keeps 16 bits" % ord(ch))
    return sorted(chars, key=ord)


# ---- fontTools: features the layout cannot ask for -------------------------

def gsub_single(tt, tag):
    """glyph -> glyph for the single substitutions of one GSUB feature."""
    table = tt["GSUB"].table if "GSUB" in tt else None
    if table is None or table.FeatureList is None:
        return {}
    sub = {}
    idx = set()
    for rec in table.FeatureList.FeatureRecord:
        if rec.FeatureTag == tag:
            idx.update(rec.Feature.LookupListIndex)
    for i in sorted(idx):
        lookup = table.LookupList.Lookup[i]
        for st in lookup.SubTable:
            if lookup.LookupType == 7:                # extension
                st = st.ExtSubTable
            for a, b in getattr(st, "mapping", {}).items():
                sub.setdefault(a, b)
    return sub


def remap(tt, chars, tnum, case, minus):
    """Points the cmap of each of `chars` at the form it is to be drawn in,
    and returns the glyph name each character ends up with.

    'tnum' is taken for the digits only. Inter's also puts the period, comma,
    colon, hyphen and space on the figures' width, which is right for a
    column of numbers and wrong for one number standing alone: "22.5" would
    get a full figure's gap either side of its point, and a clock's colons
    would each be as wide as a digit."""
    t_sub = gsub_single(tt, "tnum") if tnum else {}
    c_sub = gsub_single(tt, "case") if case else {}
    best = tt.getBestCmap()
    names = {}
    for ch in chars:
        g = best.get(ord(ch))
        if ch == "-" and minus and 0x2212 in best:
            g = best[0x2212]
        if g is None:
            continue
        if ch in DIGITS:
            g = t_sub.get(g, g)
        g = c_sub.get(g, g)
        names[ch] = g
    for t in tt["cmap"].tables:
        if t.isUnicode():
            for ch, g in names.items():
                if ord(ch) in t.cmap:
                    t.cmap[ord(ch)] = g
    return names


def pair_values(tt):
    """(left, right) -> x advance adjustment in font units, from every
    PairPos lookup under 'kern'. Within a lookup the first subtable that
    takes the left glyph decides (a class-based one takes it even when the
    value is 0); separate lookups add up, as a shaper would apply them."""
    if "GPOS" not in tt:
        return lambda left, right: 0
    table = tt["GPOS"].table
    idx = set()
    for rec in table.FeatureList.FeatureRecord:
        if rec.FeatureTag == "kern":
            idx.update(rec.Feature.LookupListIndex)
    lookups = []
    for i in sorted(idx):
        lookup = table.LookupList.Lookup[i]
        subs = []
        for st in lookup.SubTable:
            if lookup.LookupType == 9:
                if st.ExtensionLookupType != 2:
                    continue
                st = st.ExtSubTable
            elif lookup.LookupType != 2:
                continue
            cov = {g: k for k, g in enumerate(st.Coverage.glyphs)}
            subs.append((st, cov))
        lookups.append(subs)

    def xadv(rec):
        v = getattr(rec, "Value1", None)
        return (getattr(v, "XAdvance", 0) or 0) if v is not None else 0

    def value(left, right):
        total = 0
        for subs in lookups:
            for st, cov in subs:
                if left not in cov:
                    continue
                if st.Format == 1:
                    hit = None
                    for rec in st.PairSet[cov[left]].PairValueRecord:
                        if rec.SecondGlyph == right:
                            hit = rec
                            break
                    if hit is None:
                        continue                      # try the next subtable
                    total += xadv(hit)
                    break
                c1 = st.ClassDef1.classDefs.get(left, 0) if st.ClassDef1 else 0
                c2 = st.ClassDef2.classDefs.get(right, 0) if st.ClassDef2 else 0
                total += xadv(st.Class1Record[c1].Class2Record[c2])
                break
        return total
    return value


# ---- rendering ---------------------------------------------------------------

def open_face(data, size):
    face = freetype.Face(io.BytesIO(data))
    face.set_char_size(int(round(size * 64)), 0, 72, 72)
    return face


def render_at(face, ch, dx64):
    """The glyph shifted right by dx64/64 px after hinting, as 4-bit coverage
    cropped to its ink: (rows, x, y, w, h), x and y the crop's top-left from
    the pen on the baseline (y < 0 is up)."""
    face.set_transform(freetype.Matrix(0x10000, 0, 0, 0x10000), freetype.Vector(dx64, 0))
    face.load_char(ch, freetype.FT_LOAD_DEFAULT)
    face.glyph.render(freetype.FT_RENDER_MODE_NORMAL)
    bm = face.glyph.bitmap
    buf, pitch = bm.buffer, bm.pitch
    q = [[(buf[y * pitch + x] * 15 + 127) // 255 for x in range(bm.width)]
         for y in range(bm.rows)]
    ys = [y for y in range(bm.rows) if any(q[y])]
    if not ys:
        return [], 0, 0, 0, 0
    xs = [x for x in range(bm.width) if any(q[y][x] for y in ys)]
    x0, x1, y0, y1 = xs[0], xs[-1] + 1, ys[0], ys[-1] + 1
    rows = [row[x0:x1] for row in q[y0:y1]]
    return rows, face.glyph.bitmap_left + x0, -face.glyph.bitmap_top + y0, x1 - x0, y1 - y0


def blur(rows):
    """How much of a glyph is part-covered: 0 for pure ink and pure ground,
    most for the half-covered pixels that make a stem look out of focus."""
    return sum(c * (15 - c) for row in rows for c in row)


# The phases tried, in 1/64 px, each with the whole pixels to take back so the
# glyph stands as near its design position as that phase allows: 3/4 of a
# pixel right is a quarter left of the next pixel. In order of how far each
# moves the glyph, which is how ties go.
PHASES = ((0, 0), (16, 0), (48, 1), (32, 0))


def render(face, ch):
    """The glyph at whichever of the four phases leaves it sharpest (see the
    top of the file), in render_at's form."""
    best = None
    for dx64, back in PHASES:
        rows, x, y, w, h = render_at(face, ch, dx64)
        b = blur(rows)
        if best is None or b < best[0]:
            best = (b, (rows, x - back, y, w, h))
    return best[1]


def pack(rows, w):
    out = []
    for row in rows:
        r = row + [0] * (w % 2)
        for x in range(0, len(r), 2):
            out.append((r[x] << 4) | r[x + 1])
    return out


def check(what, v, lo, hi):
    if not lo <= v <= hi:
        sys.exit("%s = %d does not fit %d..%d" % (what, v, lo, hi))
    return v


def label(ch):
    if ch == " ":
        return "space"
    if ch == "\\":
        return "backslash"
    if ch in "*/":
        return "'%s'" % ch
    return ch if ord(ch) < 128 else "U+%04X" % ord(ch)


def repo_rel(path):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    p = os.path.abspath(path)
    return os.path.relpath(p, root) if p.startswith(root + os.sep) else path


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("ttf")
    ap.add_argument("size", type=float, help="em in pixels")
    ap.add_argument("--name", required=True, help="C name: font_NAME.h, aafont_NAME")
    ap.add_argument("--charset", default="ascii")
    ap.add_argument("--extra", default="")
    ap.add_argument("--tnum", action="store_true")
    ap.add_argument("--case", action="store_true")
    ap.add_argument("--minus", action="store_true")
    ap.add_argument("--no-kern", action="store_true")
    ap.add_argument("--allow-no-fonttools", action="store_true")
    a = ap.parse_args()

    if TTFont is None and not a.allow_no_fonttools:
        sys.exit("fontTools is missing: use the python3 that has it, or pass "
                 "--allow-no-fonttools for a table with spaced (not tabular) "
                 "figures, no case forms and no kerning")

    chars = charset(a.charset, a.extra)
    name = a.name
    upm = None
    kern_of = None
    names = {}
    hmtx = None
    if TTFont is not None:
        tt = TTFont(a.ttf)
        upm = tt["head"].unitsPerEm
        names = remap(tt, chars, a.tnum, a.case, a.minus)
        hmtx = tt["hmtx"]
        buf = io.BytesIO()
        tt.save(buf)
        data = buf.getvalue()
        if not a.no_kern:
            kern_of = pair_values(tt)
        family = tt["name"].getDebugName(4) or os.path.basename(a.ttf)
    else:
        with open(a.ttf, "rb") as f:
            data = f.read()
        family = os.path.basename(a.ttf)
    face = open_face(data, a.size)

    # The capitals' height is the rendered H's, since everything is placed by
    # its ink: aafont_draw's y is the top of this H. The H is the font's even
    # when the charset leaves it out, so a subset face stands on the same line.
    _, _, hy, _, _ = render_at(face, "H", 0)
    cap = -hy
    _, _, xy, _, _ = render_at(face, "x", 0)
    xheight = -xy
    # Rounded as Pillow's getmetrics rounds them, which the first tables used.
    ascent = (face.size.ascender + 32) >> 6
    descent = -((face.size.descender + 32) >> 6)

    glyphs, bits = [], []
    for ch in chars:
        if names and ch not in names and ord(ch) != 32:
            sys.exit("%s has no glyph for U+%04X" % (a.ttf, ord(ch)))
        rows, x, y, w, h = render(face, ch)
        if ch in names:
            adv = int(round(hmtx[names[ch]][0] * a.size / upm * SUB))
        else:
            render_at(face, ch, 0)
            adv = int(round(face.glyph.linearHoriAdvance / 65536 * SUB))
        glyphs.append({"ch": ch, "rows": rows, "x": x, "y": y, "w": w, "h": h, "adv": adv})

    # Tabular figures without fontTools: every digit the widest one's advance,
    # its ink centred in it.
    if a.tnum and TTFont is None:
        ds = [g for g in glyphs if g["ch"] in DIGITS]
        if ds:
            wide = max(g["adv"] for g in ds)
            for g in ds:
                g["x"] += int(round((wide - g["adv"]) / 2 / SUB))
                g["adv"] = wide

    for g in glyphs:
        g["off"] = len(bits)
        bits.extend(pack(g["rows"], g["w"]))
        check("U+%04X width" % ord(g["ch"]), g["w"], 0, 255)
        check("U+%04X height" % ord(g["ch"]), g["h"], 0, 255)
        check("U+%04X x" % ord(g["ch"]), g["x"], -128, 127)
        check("U+%04X y" % ord(g["ch"]), g["y"], -128, 127)
        check("U+%04X advance" % ord(g["ch"]), g["adv"], 0, 65535)

    kerns = []
    if kern_of is not None and len(glyphs) <= 256:
        for li, gl in enumerate(glyphs):
            for ri, gr in enumerate(glyphs):
                if gl["ch"] not in names or gr["ch"] not in names:
                    continue
                v = kern_of(names[gl["ch"]], names[gr["ch"]])
                dx = int(round(v * a.size / upm * SUB))
                if abs(dx) >= SUB // 4:
                    kerns.append((li, ri, check("kern", dx, -32768, 32767)))

    guard = "FONT_%s_H" % name.upper()
    cmd = "python3 tools/gen_aafont.py " + " ".join(
        shlex.quote(repo_rel(v)) if v == a.ttf else shlex.quote(v) for v in sys.argv[1:])
    table_bytes = len(bits) + 12 * len(glyphs) + 4 * len(kerns)

    o = sys.stdout
    o.write("/* Generated by tools/gen_aafont.py -- do not edit by hand.\n")
    o.write(" *\n")
    o.write(" * %s at %g px: capitals %d px, x-height %d, ascent %d, descent %d.\n"
            % (family, a.size, cap, xheight, ascent, descent))
    traits = [t for t, on in (("tabular figures", a.tnum),
                              ("case forms", a.case and TTFont is not None),
                              ("a true minus", a.minus and TTFont is not None)) if on]
    o.write(" * %d glyphs, %d kerning pairs, %d bytes in all.\n"
            % (len(glyphs), len(kerns), table_bytes))
    if traits:
        o.write(" * With %s.\n" % ", ".join(traits))
    o.write(" * Made with:\n *   %s\n" % cmd)
    o.write(" *\n")
    o.write(" * The glyphs are rendered from Inter, copyright (c) 2016 The Inter Project\n")
    o.write(" * Authors, and like the font are under the SIL Open Font License 1.1\n")
    o.write(" * (assets/fonts/Inter-OFL.txt), not this repository's licence.\n")
    o.write(" *\n")
    o.write(" * Include from main/aafont.c only: the arrays are static and the font\n")
    o.write(" * object is defined here, declared in aafont.h.\n")
    o.write(" */\n")
    o.write("#ifndef %s\n#define %s\n\n#include \"aafont.h\"\n\n" % (guard, guard))

    o.write("static const uint8_t %s_bits[%d] = {\n" % (name, max(len(bits), 1)))
    for g in glyphs:
        chunk = bits[g["off"]:g["off"] + (g["w"] + 1) // 2 * g["h"]]
        if not chunk:
            continue
        o.write("    /* %s */\n" % label(g["ch"]))
        for i in range(0, len(chunk), 16):
            o.write("    " + ", ".join("0x%02X" % v for v in chunk[i:i + 16]) + ",\n")
    if not bits:
        o.write("    0\n")
    o.write("};\n\n")

    o.write("static const aafont_glyph_t %s_glyphs[%d] = {\n" % (name, len(glyphs)))
    o.write("    /* offset, code point, advance (1/16 px), w, h, x, y */\n")
    for g in glyphs:
        o.write("    { %6d, 0x%04X, %5d, %3d, %3d, %4d, %4d }, /* %s */\n"
                % (g["off"], ord(g["ch"]), g["adv"], g["w"], g["h"], g["x"], g["y"],
                   label(g["ch"])))
    o.write("};\n\n")

    if kerns:
        o.write("static const aafont_kern_t %s_kerns[%d] = {\n" % (name, len(kerns)))
        o.write("    /* left, right (glyph indices), 1/16 px */\n")
        for i in range(0, len(kerns), 6):
            o.write("   " + "".join(" { %d, %d, %d }," % k for k in kerns[i:i + 6]) + "\n")
        o.write("};\n\n")

    o.write("const aafont_t aafont_%s = {\n" % name)
    o.write("    .glyphs = %s_glyphs,\n" % name)
    o.write("    .bits = %s_bits,\n" % name)
    o.write("    .kerns = %s,\n" % ("%s_kerns" % name if kerns else "NULL"))
    o.write("    .bits_len = %d,\n" % len(bits))
    o.write("    .count = %d,\n" % len(glyphs))
    o.write("    .kern_count = %d,\n" % len(kerns))
    o.write("    .cap = %d,\n" % cap)
    o.write("    .x_height = %d,\n" % xheight)
    o.write("    .ascent = %d,\n" % ascent)
    o.write("    .descent = %d,\n" % descent)
    o.write("    .line = %d,\n" % (ascent + descent))
    o.write("};\n\n#endif /* %s */\n" % guard)

    sys.stderr.write("%s: %s %g px, cap %d, %d glyphs, %d kerns, %d bytes\n"
                     % (name, family, a.size, cap, len(glyphs), len(kerns), table_bytes))


if __name__ == "__main__":
    main()
