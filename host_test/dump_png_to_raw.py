#!/usr/bin/env python3
"""Dump an image to a raw RGB888 binary so the host_test C harness can
consume the same bytes Python sees.

Usage:
    python3 dump_png_to_raw.py input.png output.raw [W H]

If W/H are omitted the image's native size is preserved.  The C harness
expects 400x600.
"""
import sys
from PIL import Image

if len(sys.argv) < 3:
    sys.exit("usage: dump_png_to_raw.py input.png output.raw [W H]")

src = sys.argv[1]
dst = sys.argv[2]
img = Image.open(src).convert("RGB")

if len(sys.argv) >= 5:
    w, h = int(sys.argv[3]), int(sys.argv[4])
    if img.size != (w, h):
        img = img.resize((w, h), Image.LANCZOS)

with open(dst, "wb") as fp:
    fp.write(img.tobytes())

print(f"wrote {dst}  ({img.size[0]}x{img.size[1]}, {img.size[0]*img.size[1]*3} bytes)")
