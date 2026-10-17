# SPDX-License-Identifier: GPL-2.0-or-later
"""Redirect the donor's two graphics fence guards, retaining the original guard.

All compute-queue guards (unknown packet / no submitted snapshot) stay intact.
The extension uses the same range metadata as the donor; check its layout and
the six-argument Microsoft ABI before producing an executable.
"""
import struct

GUARD = '_ZN9BbClobber10GuardLabelEyyjNS_6SourceEyPKj'
RANGES = '_ZN9BbClobber12_GLOBAL__N_16rangesE.llvm.17993348240306159657'
HEAD = '_ZN9BbClobber12_GLOBAL__N_111ranges_headE.llvm.17993348240306159657'
SITES = (0x20983d, 0x20a0d8)


def apply_gpu_labels(data, symbol, position, wrapper):
    original = symbol(GUARD)

    def expect(rva, raw):
        expected = bytes.fromhex(raw)
        at = position(rva)
        if data[at:at + len(expected)] != expected:
            raise ValueError(f'GPU label donor/ABI mismatch at {rva:#x}')

    # GuardLabel: head load, 256 entries, Range size=32, bytes+16, base+8,
    # sequence+0, timestamp+24. Both globals must match the RIP targets.
    expect(original + 0x47, '48 8b 05 9a f8 30 03')
    expect(original + 0x4e, '48 3d 00 01 00 00')
    expect(original + 0x77, '48 8d 35 82 fe 30 03')
    expect(original + 0x8e, 'c1 e7 05 48 8b 5c 37 10')
    expect(original + 0x9e, '4c 8b 6f 08')
    expect(original + 0xb4, '48 8b 1f 49 3b 1c 24')
    for at, name in ((original + 0x47, HEAD), (original + 0x77, RANGES)):
        offset = position(at)
        if at + 7 + struct.unpack_from('<i', data, offset + 3)[0] != symbol(name):
            raise ValueError('GPU label range metadata symbol mismatch')
    # Fifth argument is submission seq; sixth is pointer to submitted slot.
    expect(SITES[0] - 0x1c, '48 89 44 24 28 4c 89 64 24 20 48 89 f1 4c 89 f2 41 b8 04 00 00 00 41 b9 01 00 00 00')
    expect(SITES[1] - 0x16, '4c 89 6c 24 28 48 89 44 24 20 48 89 f1 4c 89 e2 41 89 d8 45 31 c9')
    records = []
    for site in SITES:
        before = b'\xe8' + struct.pack('<i', original - site - 5)
        expect(site, before.hex())
        expect(site + 5, '84 c0')  # C++ bool lives in AL, not a 32-bit int.
        after = b'\xe8' + struct.pack('<i', wrapper - site - 5)
        at = position(site)
        data[at:at + 5] = after
        records.append(dict(rva=hex(site), before=before.hex(), after=after.hex(),
            purpose='Preserve own completed graphics-label counter writes; retain rebuild protection'))
    return records
