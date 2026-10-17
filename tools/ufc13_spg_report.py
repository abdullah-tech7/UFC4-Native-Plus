#!/usr/bin/env python3
"""Decode UFC 3 SPG captures without changing them; emit pixel statistics as CSV.

Usage: python tools/ufc13_spg_report.py user/temp0/createplayer > /tmp/spg.csv
Requires Pillow. The 12-byte header is followed by a complete alpha PNG and RGB JPEG.
Hashes of the decoded planes allow comparison even when SPG headers differ.
"""

import argparse
import csv
import hashlib
import io
from pathlib import Path
import struct
import sys

from PIL import Image


def decode(path):
    data = path.read_bytes()
    if data[12:20] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: missing alpha PNG after 12-byte header")
    offset = 20
    while True:
        length = struct.unpack_from(">I", data, offset)[0]
        chunk = data[offset + 4:offset + 8]
        offset += length + 12
        if offset > len(data):
            raise ValueError(f"{path}: truncated PNG")
        if chunk == b"IEND":
            break
    if data[offset:offset + 2] != b"\xff\xd8":
        raise ValueError(f"{path}: missing color JPEG after alpha PNG")
    with Image.open(io.BytesIO(data[12:offset])) as png:
        alpha = png.convert("L")
    with Image.open(io.BytesIO(data[offset:])) as jpeg:
        color = jpeg.convert("RGB")
    if alpha.size != color.size:
        raise ValueError(f"{path}: alpha/color dimensions differ")
    return data, alpha, color


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    paths = sorted(args.root.glob("cap_*/*.spg"))
    if not paths:
        parser.error("no cap_*/*.spg files found")
    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow(["capture", "file", "bytes", "width", "height", "alpha_min",
                     "alpha_max", "alpha_nonzero", "rgb_extrema", "alpha_sha256",
                     "rgb_sha256", "spg_sha256"])
    for path in paths:
        data, alpha, color = decode(path)
        writer.writerow([path.parent.name, path.name, len(data), *alpha.size,
                         *alpha.getextrema(), sum(alpha.histogram()[1:]), color.getextrema(),
                         hashlib.sha256(alpha.tobytes()).hexdigest(),
                         hashlib.sha256(color.tobytes()).hexdigest(),
                         hashlib.sha256(data).hexdigest()])


if __name__ == "__main__":
    main()
