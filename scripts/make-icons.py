#!/usr/bin/env python3
"""Generates the app icon in every format the packages need, from one drawing.

Noisy signal on the left becoming clean on the right, which is what the app does.
Needs rsvg-convert, and iconutil on macOS. The results are committed, so this only
needs running when the design changes.
"""
import math
import pathlib
import random
import shutil
import struct
import subprocess
import tempfile

OUT = pathlib.Path(__file__).resolve().parent.parent / "packaging" / "icons"
NAME = "ExpertSDR3Audio"


def svg() -> str:
    rng = random.Random(7)
    left, right, mid = 190.0, 834.0, 512.0
    points = []
    x = left
    while x <= right:
        clean = 150.0 * math.sin((x - left) / (right - left) * 4.0 * math.pi)
        # Noise fades out across the width.
        noise_amount = max(0.0, 1.0 - (x - left) / (right - left) * 1.55)
        points.append((x, mid + clean + rng.uniform(-135.0, 135.0) * noise_amount))
        x += 5.0
    path = "M" + " L".join(f"{px:.1f},{py:.1f}" for px, py in points)
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#22324a"/><stop offset="1" stop-color="#0e151d"/>
    </linearGradient>
    <linearGradient id="wave" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#d29922"/><stop offset="0.5" stop-color="#8fbf3a"/>
      <stop offset="1" stop-color="#3fb950"/>
    </linearGradient>
  </defs>
  <rect x="100" y="100" width="824" height="824" rx="185" fill="url(#bg)"/>
  <path d="{path}" fill="none" stroke="url(#wave)" stroke-width="30"
        stroke-linecap="round" stroke-linejoin="round"/>
</svg>
"""


def render(svg_path: pathlib.Path, size: int, out: pathlib.Path) -> None:
    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), "-o", str(out), str(svg_path)], check=True)


def make_ico(pngs: list[tuple[int, bytes]], out: pathlib.Path) -> None:
    # An ICO holding PNG payloads, which every Windows since Vista reads.
    header = struct.pack("<HHH", 0, 1, len(pngs))
    offset = len(header) + 16 * len(pngs)
    entries = b""
    for size, data in pngs:
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out.write_bytes(header + entries + b"".join(data for _, data in pngs))


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    svg_path = OUT / f"{NAME}.svg"
    svg_path.write_text(svg())
    render(svg_path, 256, OUT / f"{NAME}-256.png")

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        pngs = []
        for size in (16, 24, 32, 48, 64, 128, 256):
            png = tmp_path / f"{size}.png"
            render(svg_path, size, png)
            pngs.append((size, png.read_bytes()))
        make_ico(pngs, OUT / f"{NAME}.ico")

        if shutil.which("iconutil"):
            iconset = tmp_path / f"{NAME}.iconset"
            iconset.mkdir()
            for size in (16, 32, 128, 256, 512):
                render(svg_path, size, iconset / f"icon_{size}x{size}.png")
                render(svg_path, size * 2, iconset / f"icon_{size}x{size}@2x.png")
            subprocess.run(["iconutil", "-c", "icns", "-o", str(OUT / f"{NAME}.icns"), str(iconset)], check=True)


if __name__ == "__main__":
    main()
