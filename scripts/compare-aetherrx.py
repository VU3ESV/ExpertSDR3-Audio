#!/usr/bin/env python3
"""Compare a render of one of our pages with AetherSDR's AetherRX screenshot of it.

Python's standard library only (run it with python3 -I): it decodes and writes PNG itself.

    python3 -I scripts/compare-aetherrx.py --ref 1.png --ours gate.png \\
        --regions tests/look/regions.json --page gate --diff gate-diff.png

Render ours offscreen from a temporary copy of tests/look/<page>.ini, at the size of the
window in the screenshot (see the .ini; for 1.png, 720x750 inside a 726x751 image):

    QT_QPA_PLATFORM=offscreen ExpertSDR3Audio --settings <copy> --page gate \\
        --size 720x750 --screenshot gate.png

For each region in regions.json it reports the share of pixels whose every channel is
within the tolerance (14/255 unless the region says otherwise), with the mean and worst
difference. It writes a heat map of the differences (--diff) and the reference, ours and
the heat map side by side (--side-by-side, by default beside --diff).

Regions are rectangles [x, y, width, height], or lists of [x, y] points, in our window's
coordinates. Our window's (0, 0) is the reference's `offset`: the screenshots include a few
pixels of desktop round the window. A region's own `shift` is added to that, for parts of
our window that sit elsewhere (the rail, which has a Radio row above AetherNR). Masks remove
our parts that AetherRX does not have; those marked "text" only with --mask-text, since text
rasterises differently from one platform to the next.

The numbers only report: the screenshots come from a Mac and renders from anywhere.
"""

import argparse
import json
import struct
import sys
import zlib

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


# ── PNG ──────────────────────────────────────────────────────────────────────

class Image:
    """8-bit RGB pixels, one bytearray per row."""

    def __init__(self, width, height, rows):
        self.width = width
        self.height = height
        self.rows = rows

    @staticmethod
    def blank(width, height, rgb=(0, 0, 0)):
        row = bytes(rgb) * width
        return Image(width, height, [bytearray(row) for _ in range(height)])

    def pixel(self, x, y):
        row = self.rows[y]
        i = 3 * x
        return row[i], row[i + 1], row[i + 2]

    def put(self, x, y, rgb):
        row = self.rows[y]
        i = 3 * x
        row[i], row[i + 1], row[i + 2] = rgb

    def contains(self, x, y):
        return 0 <= x < self.width and 0 <= y < self.height


def _unfilter(raw, height, bpp, stride):
    """Undoes PNG's per-row filters. Returns the rows as bytearrays."""
    rows = []
    previous = bytearray(stride)
    offset = 0
    for _ in range(height):
        kind = raw[offset]
        line = bytearray(raw[offset + 1:offset + 1 + stride])
        offset += 1 + stride
        if kind == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif kind == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif kind == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif kind == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = previous[i]
                c = previous[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                predictor = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + predictor) & 0xFF
        elif kind != 0:
            raise ValueError("unknown PNG filter %d" % kind)
        rows.append(line)
        previous = line
    return rows


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError("%s is not a PNG" % path)
    pos = len(PNG_SIGNATURE)
    idat = []
    palette = None
    width = height = depth = colour = interlace = None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if interlace:
        raise ValueError("%s is interlaced, which this reader does not handle" % path)
    if depth not in (8, 16):
        raise ValueError("%s has %d-bit samples; only 8 and 16 are handled" % (path, depth))
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    sample = depth // 8
    bpp = channels * sample
    stride = width * bpp
    rows = _unfilter(zlib.decompress(b"".join(idat)), height, bpp, stride)

    out = []
    for line in rows:
        if sample == 2:
            line = line[0::2]  # the high byte of each sample
        rgb = bytearray(3 * width)
        if colour == 2:
            rgb[:] = line
        elif colour == 6:
            rgb[0::3] = line[0::4]
            rgb[1::3] = line[1::4]
            rgb[2::3] = line[2::4]
        elif colour in (0, 4):
            grey = line[0::channels]
            rgb[0::3] = grey
            rgb[1::3] = grey
            rgb[2::3] = grey
        else:  # palette
            for x, index in enumerate(line):
                rgb[3 * x:3 * x + 3] = palette[3 * index:3 * index + 3]
        out.append(rgb)
    return Image(width, height, out)


def write_png(path, image):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes(row) for row in image.rows)
    header = struct.pack(">IIBBBBB", image.width, image.height, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(PNG_SIGNATURE + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def downscale(image, factor):
    """Averages factor x factor blocks: a render at 2x against a 1x screenshot."""
    if factor == 1:
        return image
    width, height = image.width // factor, image.height // factor
    rows = []
    area = factor * factor
    for y in range(height):
        row = bytearray(3 * width)
        for x in range(width):
            totals = [0, 0, 0]
            for dy in range(factor):
                src = image.rows[y * factor + dy]
                for dx in range(factor):
                    i = 3 * (x * factor + dx)
                    totals[0] += src[i]
                    totals[1] += src[i + 1]
                    totals[2] += src[i + 2]
            row[3 * x:3 * x + 3] = bytes(t // area for t in totals)
        rows.append(row)
    return Image(width, height, rows)


# ── Comparing ────────────────────────────────────────────────────────────────

def masked_at(masks, x, y):
    for (mx, my, mw, mh) in masks:
        if mx <= x < mx + mw and my <= y < my + mh:
            return True
    return False


def difference(ref, ours, rx, ry, ox, oy):
    a = ref.pixel(rx, ry)
    b = ours.pixel(ox, oy)
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))


def region_pixels(region):
    """Our coordinates the region covers, in rows."""
    if "rect" in region:
        x, y, w, h = region["rect"]
        for oy in range(y, y + h):
            for ox in range(x, x + w):
                yield ox, oy
    else:
        for ox, oy in region["points"]:
            yield ox, oy


def compare_region(ref, ours, region, offset, masks, tolerance, step=1, extra=(0, 0)):
    shift = region.get("shift", [0, 0])
    dx = offset[0] + shift[0] + extra[0]
    dy = offset[1] + shift[1] + extra[1]
    compared = within = total = worst = 0
    for ox, oy in region_pixels(region):
        if step > 1 and (ox % step or oy % step):
            continue
        if masked_at(masks, ox, oy) or not ours.contains(ox, oy):
            continue
        rx, ry = ox + dx, oy + dy
        if not ref.contains(rx, ry):
            continue
        d = difference(ref, ours, rx, ry, ox, oy)
        compared += 1
        total += d
        worst = max(worst, d)
        if d <= tolerance:
            within += 1
    return compared, within, total, worst


def best_shift(ref, ours, region, offset, masks, tolerance, search):
    """The extra shift, within +-search pixels, at which the region matches best."""
    best = (-1.0, (0, 0))
    for sy in range(-search, search + 1):
        for sx in range(-search, search + 1):
            compared, within, _, _ = compare_region(ref, ours, region, offset, masks, tolerance, 2, (sx, sy))
            share = within / compared if compared else 0.0
            if share > best[0] or (share == best[0] and abs(sx) + abs(sy) < sum(map(abs, best[1]))):
                best = (share, (sx, sy))
    return best[1]


def heat_pixel(ref, ours, rx, ry, ox, oy, masks, tolerance):
    r, g, b = ref.pixel(rx, ry)
    if masked_at(masks, ox, oy):
        return r // 6, g // 6, 40 + b // 6
    d = difference(ref, ours, rx, ry, ox, oy)
    if d <= tolerance:
        grey = (r * 30 + g * 59 + b * 11) // 300
        return grey, grey, grey
    level = min(d, 128) / 128.0
    return 255, int(255 * (1.0 - level)), 0


def heat(ref, ours, page, masks, offset):
    """A heat map on the reference's canvas: faint where alike, yellow to red where not."""
    out = Image.blank(ref.width, ref.height, (0, 0, 0))
    tolerance = page.get("tolerance", 14)
    for ry in range(ref.height):
        for rx in range(ref.width):
            ox, oy = rx - offset[0], ry - offset[1]
            if ours.contains(ox, oy):
                out.put(rx, ry, heat_pixel(ref, ours, rx, ry, ox, oy, masks, tolerance))
    # A region compared somewhere else in ours is drawn as it was compared.
    for region in page["regions"]:
        shift = region.get("shift", [0, 0])
        if "rect" not in region or shift == [0, 0]:
            continue
        for ox, oy in region_pixels(region):
            rx, ry = ox + offset[0] + shift[0], oy + offset[1] + shift[1]
            if ours.contains(ox, oy) and ref.contains(rx, ry):
                out.put(rx, ry, heat_pixel(ref, ours, rx, ry, ox, oy, masks, tolerance))
    # Outline every rectangle region in its own place on the reference.
    for region in page["regions"]:
        shift = region.get("shift", [0, 0])
        if "rect" not in region:
            # A small cross round each point, leaving the point itself to show.
            for ox, oy in region["points"]:
                cx, cy = ox + offset[0] + shift[0], oy + offset[1] + shift[1]
                for d in (-3, -2, 2, 3):
                    for px, py in ((cx + d, cy), (cx, cy + d)):
                        if out.contains(px, py):
                            out.put(px, py, (0, 220, 255))
            continue
        x, y, w, h = region["rect"]
        x0, y0 = x + offset[0] + shift[0], y + offset[1] + shift[1]
        for px in range(x0, x0 + w):
            for py in (y0, y0 + h - 1):
                if out.contains(px, py):
                    out.put(px, py, (0, 220, 255))
        for py in range(y0, y0 + h):
            for px in (x0, x0 + w - 1):
                if out.contains(px, py):
                    out.put(px, py, (0, 220, 255))
    return out


def side_by_side(ref, ours, heat_map, offset):
    gap = 6
    out = Image.blank(3 * ref.width + 2 * gap, ref.height, (40, 40, 40))
    for y in range(ref.height):
        out.rows[y][0:3 * ref.width] = ref.rows[y]
        start = 3 * (ref.width + gap)
        # Ours, placed where it lines up with the reference.
        oy = y - offset[1]
        if 0 <= oy < ours.height:
            for x in range(ref.width):
                ox = x - offset[0]
                if 0 <= ox < ours.width:
                    i = 3 * ox
                    out.rows[y][start + 3 * x:start + 3 * x + 3] = ours.rows[oy][i:i + 3]
        start = 3 * (2 * ref.width + 2 * gap)
        out.rows[y][start:start + 3 * ref.width] = heat_map.rows[y]
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--ref", required=True, help="AetherRX's screenshot of the page")
    parser.add_argument("--ours", required=True, help="our render of the page")
    parser.add_argument("--regions", required=True, help="tests/look/regions.json")
    parser.add_argument("--page", required=True, help="the page's entry in regions.json, e.g. gate")
    parser.add_argument("--diff", help="where to write the heat map (PNG)")
    parser.add_argument("--side-by-side", help="where to write reference, ours and heat map (PNG); "
                                               "default: beside --diff")
    parser.add_argument("--mask-text", action="store_true", help="also leave out the masks marked text")
    parser.add_argument("--search", type=int, default=0,
                        help="also report each rectangle at its best extra shift within this many pixels")
    parser.add_argument("--scale", type=int, default=0,
                        help="our render's device pixel ratio; 0 (default) guesses from the sizes")
    parser.add_argument("--json", help="also write the numbers here")
    args = parser.parse_args()

    with open(args.regions) as f:
        page = json.load(f)[args.page]
    ref = read_png(args.ref)
    ours = read_png(args.ours)
    scale = args.scale or max(1, round(ours.width / ref.width))
    ours = downscale(ours, scale)
    offset = page.get("offset", [0, 0])
    masks = [m["rect"] for m in page.get("masks", []) if args.mask_text or not m.get("text")]

    print("reference %s: %dx%d" % (args.ref, ref.width, ref.height))
    print("ours      %s: %dx%d%s" % (args.ours, ours.width, ours.height, " (from %dx)" % scale if scale > 1 else ""))
    print("our (0, 0) is the reference's (%d, %d); %d mask(s)" % (offset[0], offset[1], len(masks)))
    print()
    header = "%-10s %9s %9s %10s %6s" % ("region", "pixels", "within", "mean diff", "worst")
    if args.search:
        header += "   best shift within +-%d" % args.search
    print(header)

    results = {}
    for region in page["regions"]:
        tolerance = region.get("tolerance", page.get("tolerance", 14))
        compared, within, total, worst = compare_region(ref, ours, region, offset, masks, tolerance)
        share = 100.0 * within / compared if compared else 0.0
        mean = total / compared if compared else 0.0
        line = "%-10s %9d %8.2f%% %10.2f %6d" % (region["name"], compared, share, mean, worst)
        entry = {"pixels": compared, "within": share, "tolerance": tolerance, "mean": mean, "worst": worst}
        if args.search and "rect" in region:
            sx, sy = best_shift(ref, ours, region, offset, masks, tolerance, args.search)
            c2, w2, _, _ = compare_region(ref, ours, region, offset, masks, tolerance, 1, (sx, sy))
            best = 100.0 * w2 / c2 if c2 else 0.0
            line += "   %6.2f%% at (%+d, %+d)" % (best, sx, sy)
            entry["best"] = {"within": best, "shift": [sx, sy]}
        if tolerance != page.get("tolerance", 14):
            line += "   (tolerance %d)" % tolerance
        print(line)
        results[region["name"]] = entry
        if "points" in region:
            shift = region.get("shift", [0, 0])
            for ox, oy in region["points"]:
                rx, ry = ox + offset[0] + shift[0], oy + offset[1] + shift[1]
                if ours.contains(ox, oy) and ref.contains(rx, ry):
                    a, b = ref.pixel(rx, ry), ours.pixel(ox, oy)
                    print("    at (%d, %d): reference #%02x%02x%02x, ours #%02x%02x%02x" % ((ox, oy) + a + b))

    if args.diff or args.side_by_side:
        heat_map = heat(ref, ours, {"regions": page["regions"], "tolerance": page.get("tolerance", 14)}, masks,
                        offset)
        if args.diff:
            write_png(args.diff, heat_map)
            print("\nheat map: %s" % args.diff)
        combined = args.side_by_side
        if not combined and args.diff:
            combined = args.diff[:-4] + "-side.png" if args.diff.endswith(".png") else args.diff + "-side.png"
        if combined:
            write_png(combined, side_by_side(ref, ours, heat_map, offset))
            print("side by side (reference, ours, heat map): %s" % combined)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
