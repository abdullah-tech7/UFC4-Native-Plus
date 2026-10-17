#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract UFC 3's Blaze TDF schema (class -> members with tag, name and type) from a dump of the
running game's image (BB_NET_TRAP_DUMP; the reflection tables are filled by static constructors).

    python3 tools/ufc3_tdf_schema.py out/guest-live.bin > out/ufc3-tdf-schema.txt
"""
import struct
import sys

BASE = 0x800000000


def main(path):
    d = open(path, 'rb').read()

    def ptr(o):
        q = struct.unpack_from('<Q', d, o)[0]
        return q - BASE if BASE <= q < BASE + len(d) else None

    def cstr(o):
        if o is None or not 0 <= o < len(d):
            return None
        end = d.find(b'\0', o, o + 200)
        if end < 0:
            return None
        try:
            s = d[o:end].decode('ascii')
        except UnicodeDecodeError:
            return None
        return s if s and s.isprintable() else None

    def tag(v):
        v >>= 8
        return ''.join(chr(((v >> s) & 0x3f) + 0x20) for s in (18, 12, 6, 0)).rstrip()

    def type_name(o):
        if o is None:
            return '?'
        return cstr(ptr(o + 8) if ptr(o + 8) is not None else None) or f'kind{d[o]}'

    out, unions = {}, set()
    for o in range(0, len(d) - 0x40, 8):
        kind = struct.unpack_from('<I', d, o)[0]
        if kind not in (0xA, 0xB):  # union, class
            continue
        full = cstr(ptr(o + 8))
        if not full or not full.startswith('Blaze::'):
            continue
        members = ptr(o + 0x30)
        count = struct.unpack_from('<H', d, o + 0x38)[0]
        if members is None or count > 300:
            continue
        rows = []
        for i in range(count):
            m = members + 0x30 * i
            name = cstr(ptr(m + 8))
            t = struct.unpack_from('<I', d, m + 0x20)[0]
            rows.append((tag(t & ~0xFF), name or '?', type_name(ptr(m))))  # low byte: flags
        if rows is not None:
            out[full] = rows
            if kind == 0xA:
                unions.add(full)
    for full in sorted(out):
        print(full + ('  (union)' if full in unions else ''))
        for t, name, ty in out[full]:
            print(f'    {t:<4} {name:<32} {ty}')


if __name__ == '__main__':
    main(sys.argv[1])
