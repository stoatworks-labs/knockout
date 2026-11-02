#!/usr/bin/env python3
"""
Write the harness's fonts: tools/fixtures/Knockout*.ttf.

Three faces of one family, "Knockout Test" -- Regular (400), Bold (700) and
Italic (400, fsSelection ITALIC) -- with a handful of glyphs whose geometry
the checks know exactly, because this file says what it is:

    H   a square, 100..700 x 0..600 (the Bold's is 50..750 x 0..600, so a
        face choice can be seen in the picture, not only in the log)
    O   the same square with a square hole, 250..550 x 150..450, wound the
        other way: inside the hole is outside the letter
    X   two bars wound the same way and crossing, 100..700 x 200..400 and
        300..500 x 0..600: a plus sign whose middle has winding 2, the shape
        a variable font's overlapping strokes make
    C   a disc of radius 300 about (400, 300), eight quadratics: the curves
    A   a triangle (100,0) (400,600) (700,0)
    V   a triangle (100,600) (700,600) (400,0), kerned -150 after A
    I   a bar 100..200 x 0..600, advance 300
    space, advance 250

Every advance but I's is 800; units per em 1000, ascent 800, descent -200.
The Italic's H is sheared by 0.25 (x += 0.25 y) so it, too, is visible.
A second family, "Knockout Twin" (one Regular face, its H 200..600 x
0..600), is there so the Font list has two entries and a check can tell
which one a name or an index picked.

No dependencies: the tables are packed by hand, which is a few hundred
lines of struct and much less than a font library. Run it again after
changing a glyph; the TTFs are committed.
"""

import math
import os
import struct
import sys

UPM = 1000
ASCENT = 800
DESCENT = -200


def rect(x0, y0, x1, y1, clockwise=True):
    """A rectangle as one contour of on-curve points. TrueType winds outer
    contours clockwise (y up); a hole goes the other way."""
    pts = [(x0, y0), (x0, y1), (x1, y1), (x1, y0)]
    if not clockwise:
        pts.reverse()
    return [(x, y, True) for x, y in pts]


def disc(cx, cy, r, segments=8):
    """A circle of quadratics, clockwise: on-curve points on the circle,
    each control point where the tangents at its two ends meet."""
    out = []
    step = 2.0 * math.pi / segments
    for k in range(segments):
        a = -k * step  # clockwise
        m = a - 0.5 * step
        out.append((round(cx + r * math.cos(a)), round(cy + r * math.sin(a)), True))
        rc = r / math.cos(0.5 * step)
        out.append((round(cx + rc * math.cos(m)), round(cy + rc * math.sin(m)), False))
    return out


def glyphs_for(variant):
    """name -> (advance, [contours]). Each contour is [(x, y, onCurve)]."""
    h = {"bold": rect(50, 0, 750, 600), "twin": rect(200, 0, 600, 600)}.get(variant, rect(100, 0, 700, 600))
    if variant == "italic":
        h = [(round(x + 0.25 * y), y, on) for x, y, on in h]
    return [
        (".notdef", 800, []),
        ("space", 250, []),
        ("H", 800, [h]),
        ("O", 800, [rect(100, 0, 700, 600), rect(250, 150, 550, 450, clockwise=False)]),
        ("X", 800, [rect(100, 200, 700, 400), rect(300, 0, 500, 600)]),
        ("C", 800, [disc(400, 300, 300)]),
        ("A", 800, [[(100, 0, True), (400, 600, True), (700, 0, True)]]),
        ("V", 800, [[(100, 600, True), (700, 600, True), (400, 0, True)]]),
        ("I", 300, [rect(100, 0, 200, 600)]),
    ]


CMAP = {0x20: "space", ord("H"): "H", ord("O"): "O", ord("X"): "X", ord("C"): "C",
        ord("A"): "A", ord("V"): "V", ord("I"): "I"}
KERN = {("A", "V"): -150}


def glyf_entry(contours):
    if not contours:
        return b""
    xs = [p[0] for c in contours for p in c]
    ys = [p[1] for c in contours for p in c]
    out = struct.pack(">hhhhh", len(contours), min(xs), min(ys), max(xs), max(ys))
    end = -1
    for c in contours:
        end += len(c)
        out += struct.pack(">H", end)
    out += struct.pack(">H", 0)  # no instructions
    points = [p for c in contours for p in c]
    # Flags without the short-vector bits: every delta is a signed 16-bit.
    out += bytes(0x01 if on else 0x00 for _, _, on in points)
    px = py = 0
    for x, _, _ in points:
        out += struct.pack(">h", x - px)
        px = x
    for _, y, _ in points:
        out += struct.pack(">h", y - py)
        py = y
    while len(out) % 4:
        out += b"\0"
    return out


def name_table(family, style):
    full = family if style == "Regular" else family + " " + style
    ps = (family + "-" + style).replace(" ", "")
    records = [(1, family), (2, style), (4, full), (6, ps)]
    storage = b""
    entries = b""
    for name_id, text in records:
        raw = text.encode("utf-16-be")
        entries += struct.pack(">HHHHHH", 3, 1, 0x409, name_id, len(raw), len(storage))
        storage += raw
    header = struct.pack(">HHH", 0, len(records), 6 + 12 * len(records))
    return header + entries + storage


def os2_table(weight, italic):
    # Version 4: 96 bytes. Only what a reader of this face looks at is
    # meaningful; the rest is zero.
    t = bytearray(96)
    struct.pack_into(">H", t, 0, 4)              # version
    struct.pack_into(">h", t, 2, 700)            # xAvgCharWidth
    struct.pack_into(">H", t, 4, weight)         # usWeightClass
    struct.pack_into(">H", t, 6, 5)              # usWidthClass
    selection = 0x0001 if italic else (0x0020 if weight >= 700 else 0x0040)
    struct.pack_into(">H", t, 62, selection)     # fsSelection
    struct.pack_into(">H", t, 64, 0x20)          # usFirstCharIndex
    struct.pack_into(">H", t, 66, 0x58)          # usLastCharIndex
    struct.pack_into(">h", t, 68, ASCENT)        # sTypoAscender
    struct.pack_into(">h", t, 70, DESCENT)       # sTypoDescender
    struct.pack_into(">h", t, 72, 0)             # sTypoLineGap
    struct.pack_into(">H", t, 74, ASCENT)        # usWinAscent
    struct.pack_into(">H", t, 76, -DESCENT)      # usWinDescent
    return bytes(t)


def build(family, style, weight, italic, variant):
    glyphs = glyphs_for(variant)
    index = {name: i for i, (name, _, _) in enumerate(glyphs)}

    glyf = b""
    loca = []
    for _, _, contours in glyphs:
        loca.append(len(glyf))
        glyf += glyf_entry(contours)
    loca.append(len(glyf))
    loca_table = b"".join(struct.pack(">I", o) for o in loca)

    hmtx = b""
    for _, advance, contours in glyphs:
        lsb = min((p[0] for c in contours for p in c), default=0)
        hmtx += struct.pack(">Hh", advance, lsb)

    all_x = [p[0] for _, _, cs in glyphs for c in cs for p in c]
    all_y = [p[1] for _, _, cs in glyphs for c in cs for p in c]
    mac_style = (1 if weight >= 700 else 0) | (2 if italic else 0)
    head = struct.pack(">HHiIIHHqqhhhhHHhhh",
                       1, 0,              # version 1.0
                       0x00010000,        # fontRevision 1.0
                       0,                 # checkSumAdjustment, filled below
                       0x5F0F3CF5,        # magicNumber
                       0x000B,            # flags
                       UPM,
                       0, 0,              # created, modified
                       min(all_x), min(all_y), max(all_x), max(all_y),
                       mac_style,
                       8,                 # lowestRecPPEM
                       2,                 # fontDirectionHint
                       1,                 # indexToLocFormat: long
                       0)                 # glyphDataFormat

    hhea = struct.pack(">HHhhhHhhhhhhhhhhhH",
                       1, 0, ASCENT, DESCENT, 0,
                       max(a for _, a, _ in glyphs),
                       0, 0, max(all_x), 1, 0, 0, 0, 0, 0, 0, 0,
                       len(glyphs))

    maxp = struct.pack(">IHHHHHHHHHHHHHH", 0x00010000, len(glyphs),
                       64, 4, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0)

    # cmap: one format 12 subtable, Windows Unicode full repertoire.
    groups = sorted((cp, index[name]) for cp, name in CMAP.items())
    sub = struct.pack(">HHIII", 12, 0, 16 + 12 * len(groups), 0, len(groups))
    for cp, gid in groups:
        sub += struct.pack(">III", cp, cp, gid)
    cmap = struct.pack(">HHHHI", 0, 1, 3, 10, 12) + sub

    # kern: version 0, one horizontal format-0 subtable.
    pairs = sorted((index[l], index[r], v) for (l, r), v in KERN.items())
    n = len(pairs)
    search = 1
    while search * 2 <= n:
        search *= 2
    selector = int(math.log2(search))
    body = struct.pack(">HHHH", n, search * 6, selector, n * 6 - search * 6)
    for l, r, v in pairs:
        body += struct.pack(">HHh", l, r, v)
    kern = struct.pack(">HH", 0, 1) + struct.pack(">HHH", 0, 6 + len(body), 0x0001) + body

    post = struct.pack(">IiHhIIIII", 0x00030000, 0, 0, 0, 0, 0, 0, 0, 0)

    tables = {
        "OS/2": os2_table(weight, italic),
        "cmap": cmap,
        "glyf": glyf,
        "head": head,
        "hhea": hhea,
        "hmtx": hmtx,
        "kern": kern,
        "loca": loca_table,
        "maxp": maxp,
        "name": name_table(family, style),
        "post": post,
    }
    return assemble(tables)


def checksum(data):
    data = data + b"\0" * ((4 - len(data) % 4) % 4)
    return sum(struct.unpack(">%dI" % (len(data) // 4), data)) & 0xFFFFFFFF


def assemble(tables):
    tags = sorted(tables)
    n = len(tags)
    search = 1
    while search * 2 <= n:
        search *= 2
    header = struct.pack(">IHHHH", 0x00010000, n, search * 16, int(math.log2(search)), n * 16 - search * 16)
    offset = 12 + 16 * n
    directory = b""
    body = b""
    head_offset = None
    for tag in tags:
        data = tables[tag]
        if tag == "head":
            head_offset = offset
        directory += struct.pack(">4sIII", tag.encode("ascii"), checksum(data), offset, len(data))
        padded = data + b"\0" * ((4 - len(data) % 4) % 4)
        body += padded
        offset += len(padded)
    font = bytearray(header + directory + body)
    adjustment = (0xB1B0AFBA - checksum(bytes(font))) & 0xFFFFFFFF
    struct.pack_into(">I", font, head_offset + 8, adjustment)
    return bytes(font)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "fixtures")
    os.makedirs(out_dir, exist_ok=True)
    faces = [
        ("Knockout Test", "Regular", 400, False, "regular"),
        ("Knockout Test", "Bold", 700, False, "bold"),
        ("Knockout Test", "Italic", 400, True, "italic"),
        ("Knockout Twin", "Regular", 400, False, "twin"),
    ]
    for family, style, weight, italic, variant in faces:
        path = os.path.join(out_dir, "%s-%s.ttf" % (family.replace(" ", ""), style))
        with open(path, "wb") as f:
            f.write(build(family, style, weight, italic, variant))
        print(path)


if __name__ == "__main__":
    main()
