#!/usr/bin/env python3
# This file is part of OpenBGI (https://openbgi.net).
# SPDX-License-Identifier: GPL-2.0-only
"""
mkicon.py - the program's icon (icons/; docs/dev/building.md)

The icon is drawn in icons/openbgi.svg.  `make icon` renders it and runs
the first two of these; the results are kept in the tree.

    python3 tools/mkicon.py --ico OUT.ico PICTURE.png ...
        the PNG pictures (square, 8 bits a channel) as one .ico file, the
        largest first: those of 128 pixels and more as they are, the
        smaller ones as bitmaps of 32 bits, which every Windows reads.
        icons/openbgi.ico is made this way.

    python3 tools/mkicon.py --own IN.ico OUT.inc
        the icon the engine carries for a host that brings none
        (src/base/ownicon.inc, part of src/base/icon.c): the .ico file's
        pictures of 256, 48, 32 and 16 pixels, each as a PNG file, as the
        bytes of a smaller .ico file in C.

    python3 tools/mkicon.py --table IN.ico OUT.c SYMBOL
        the .ico file as a C table (SYMBOL[] and SYMBOLSize), which the
        build links into the desktop programs: the window's icon.

    python3 tools/mkicon.py --png IN.ico DIR
        each picture of the .ico file as DIR/openbgi-<size>.png, for a
        desktop entry's icons (icons/openbgi.desktop).  Pictures that are
        PNG files or bitmaps of 32 bits are written, the others left out.

Only the standard library is used.
"""
import os
import struct
import sys
import zlib

OWN_SIZES = (256, 48, 32, 16)  # the sizes the engine asks its own icon for
PNG_FROM = 128                 # pictures of this size and more stay PNG files in an .ico


def png(rows):
    """rows of (r, g, b, a) as a PNG file"""
    h, w = len(rows), len(rows[0])
    raw = b"".join(b"\0" + b"".join(bytes(p) for p in row) for row in rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def bitmap(rows):
    """rows of (r, g, b, a) as an icon's bitmap of 32 bits: the header, the rows from the bottom up, the mask"""
    h, w = len(rows), len(rows[0])
    head = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = b"".join(bytes((p[2], p[1], p[0], p[3])) for row in reversed(rows) for p in row)
    stride = ((w + 31) // 32) * 4
    mask = b""
    for row in reversed(rows):
        bits = bytearray(stride)
        for x, p in enumerate(row):
            if p[3] < 0x80:
                bits[x >> 3] |= 0x80 >> (x & 7)
        mask += bytes(bits)
    return head + pixels + mask


def unpng(data):
    """a PNG file of 8 bits a channel, with or without alpha and not interlaced, as rows of (r, g, b, a)"""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit("not a PNG file")
    at, raw, head = 8, b"", None
    while at + 8 <= len(data):
        n, kind = struct.unpack_from(">I4s", data, at)
        body = data[at + 8:at + 8 + n]
        if kind == b"IHDR":
            head = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            raw += body
        at += 12 + n
    if not head or head[2] != 8 or head[3] not in (2, 6) or head[6] != 0:
        sys.exit("a PNG file of 8 bits a channel (RGB or RGBA), not interlaced, is needed")
    w, h, bpp = head[0], head[1], 4 if head[3] == 6 else 3
    raw = zlib.decompress(raw)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            left = line[i - bpp] if i >= bpp else 0
            up = prev[i]
            corner = prev[i - bpp] if i >= bpp else 0
            if kind == 1:
                line[i] = (line[i] + left) & 255
            elif kind == 2:
                line[i] = (line[i] + up) & 255
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                line[i] = (line[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 255
        rows.append([tuple(line[x * bpp:x * bpp + 3]) + ((line[x * bpp + 3],) if bpp == 4 else (255,)) for x in range(w)])
        prev = line
    return rows


def pack(pictures):
    """(size, bytes) pictures as the bytes of an .ico file, the largest first"""
    pictures = sorted(pictures, key=lambda p: -p[0])
    data = struct.pack("<HHH", 0, 1, len(pictures))
    offset = 6 + 16 * len(pictures)
    for size, pic in pictures:
        data += struct.pack("<BBBBHHII", size & 255, size & 255, 0, 0, 1, 32, len(pic), offset)
        offset += len(pic)
    for _, pic in pictures:
        data += pic
    return data


def ico(out, files):
    pictures = []
    for name in files:
        with open(name, "rb") as f:
            data = f.read()
        rows = unpng(data)
        size = len(rows)
        if size != len(rows[0]) or size > 256:
            sys.exit("%s: a square picture of 256 pixels at most is needed" % name)
        pictures.append((size, data if size >= PNG_FROM else bitmap(rows)))
    data = pack(pictures)
    with open(out, "wb") as f:
        f.write(data)
    print("%s: %d bytes, %s pixels" % (out, len(data), ", ".join(str(s) for s, _ in sorted(pictures, key=lambda p: -p[0]))))


def entries(data):
    """the pictures of an .ico file: (width, height, bytes)"""
    if len(data) < 6 or struct.unpack_from("<HH", data, 0) != (0, 1):
        sys.exit("not an .ico file")
    out = []
    for i in range(struct.unpack_from("<H", data, 4)[0]):
        w, h, _, _, _, _, size, off = struct.unpack_from("<BBBBHHII", data, 6 + i * 16)
        if off + size <= len(data):
            out.append((w or 256, h or 256, data[off:off + size]))
    return out


def table(src, out, symbol):
    with open(src, "rb") as f:
        data = f.read()
    entries(data)  # (it is one)
    with open(out, "w") as f:
        f.write("/* generated by tools/mkicon.py from %s: do not edit */\n" % src.replace("\\", "/"))
        f.write("const unsigned char %s[] = {\n" % symbol)
        for i in range(0, len(data), 20):
            f.write("\t" + ",".join(str(b) for b in data[i:i + 20]) + ",\n")
        f.write("};\nconst unsigned int %sSize = %d;\n" % (symbol, len(data)))


def picture(w, h, pic):
    """a picture of an .ico file as a PNG file; None for a bitmap that is not of 32 bits"""
    if pic[:4] == b"\x89PNG":
        return pic
    if len(pic) < 40 or struct.unpack_from("<H", pic, 14)[0] != 32:
        return None
    head = struct.unpack_from("<I", pic, 0)[0]
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            b, g, r, a = pic[head + ((h - 1 - y) * w + x) * 4:head + ((h - 1 - y) * w + x) * 4 + 4]
            row.append((r, g, b, a))
        rows.append(row)
    return png(rows)


def pngs(src, directory):
    with open(src, "rb") as f:
        data = f.read()
    os.makedirs(directory, exist_ok=True)
    for w, h, pic in entries(data):
        out = picture(w, h, pic)
        if not out:
            print("%d x %d: not a bitmap of 32 bits, left out" % (w, h))
            continue
        name = os.path.join(directory, "openbgi-%d.png" % w)
        with open(name, "wb") as f:
            f.write(out)
        print(name)


def own(src, out):
    with open(src, "rb") as f:
        data = f.read()
    pictures = []
    for w, h, pic in entries(data):
        if w == h and w in OWN_SIZES:
            p = picture(w, h, pic)
            if p:
                pictures.append((w, p))
    if sorted(s for s, _ in pictures) != sorted(OWN_SIZES):
        sys.exit("%s: pictures of %s pixels are needed" % (src, ", ".join(str(s) for s in OWN_SIZES)))
    data = pack(pictures)
    with open(out, "w") as f:
        f.write("// This file is part of OpenBGI (https://openbgi.net).\n// SPDX-License-Identifier: GPL-2.0-only\n//\n")
        f.write("// generated by tools/mkicon.py --own from %s (make icon): do not edit\n" % src.replace("\\", "/"))
        for i in range(0, len(data), 24):
            f.write(",".join(str(b) for b in data[i:i + 24]) + ",\n")
    print("%s: %d bytes" % (out, len(data)))


def main(argv):
    if len(argv) >= 4 and argv[1] == "--ico":
        ico(argv[2], argv[3:])
    elif len(argv) == 4 and argv[1] == "--own":
        own(argv[2], argv[3])
    elif len(argv) == 5 and argv[1] == "--table":
        table(argv[2], argv[3], argv[4])
    elif len(argv) == 4 and argv[1] == "--png":
        pngs(argv[2], argv[3])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
