# SPDX-License-Identifier: GPL-2.0-or-later
"""Measured renderer changes, equivalent to the supplied texture_cache.cpp fixes.

Only the recorded donor is supported. Every changed instruction and every layout
used by the GPU invalidation replacement is checked before writing any output.
"""
import struct

GPU_SYMBOL='_ZN9VideoCore12TextureCache23InvalidateMemoryFromGPUEyy'
REFRESH_SYMBOL='_ZN9VideoCore12TextureCache12RefreshImageERNS_5ImageE'
EXPAND_SYMBOL='_ZN9VideoCore12TextureCache11ExpandImageERKNS_9ImageInfoEN6Common6SlotIdE'
OVERLAP_SYMBOL='_ZN9VideoCore12TextureCache14ResolveOverlapERKNS_9ImageInfoENS0_11BindingTypeEN6Common6SlotIdES6_'
COPY_SYMBOL='_ZN6Vulkan7Runtime9CopyImageEPN9VideoCore5ImageES3_'
COPY_MIP_SYMBOL='_ZN6Vulkan7Runtime7CopyMipEPN9VideoCore5ImageES3_jj'
SLICE_SYMBOL='_ZNK9VideoCore9ImageInfo7SliceOfERKS0_i'
CAPTURE_DRAW_SYMBOL='_ZN6Vulkan12FrameCapture4DrawEyyjj'


def apply_texture_sync(data, old_symbol, file_offset, replacement_rva,
                       expand_rva,merge_rva):
    gpu=old_symbol(GPU_SYMBOL)
    refresh=old_symbol(REFRESH_SYMBOL)
    def expect(rva, expected):
        expected=bytes.fromhex(expected)
        at=file_offset(rva)
        if data[at:at+len(expected)]!=expected:
            raise ValueError(f'Texture sync donor instruction mismatch at {rva:#x}')
    # ABI/layout witnesses from the actual original method (not guessed offsets).
    for offset, expected in (
        (0x25,'48 8d b1 e0 05 00 00'),  # mutex at cache+0x5e0
        (0x4c,'48 8b 87 a0 05 00 00'),  # page table first level at +0x5a0
        (0x53,'4c 89 f1 48 c1 e9 1e'), # address >> 30: 10+20 page bits
        (0x6d,'81 e1 ff 03 00 00'),    # second level: 1024 entries
        (0x73,'48 6b c9 58'),          # each page's small_vector is 0x58 bytes
        (0x77,'4c 8b 6c 08 08'),       # small_vector count at +8
        (0x89,'4c 8b 24 08'),          # small_vector data pointer at +0
        (0xb4,'48 8b b7 58 01 00 00'),# image slot data at cache+0x158
        (0xbb,'48 69 d0 d8 02 00 00'),# sizeof(Image) = 0x2d8
        (0xcc,'48 8b 96 c8 01 00 00'),# guest address at image+0x1c8
        (0xd8,'44 8b 86 d0 01 00 00'),# guest size (u32) at image+0x1d0
        (0x99,'80 0e 04'),             # GpuDirty bit = 4
    ):
        expect(gpu+offset,expected)
    records=[]
    def patch(symbol, offset, expected, replacement, purpose):
        rva=old_symbol(symbol)+offset
        expect(rva,expected)
        before=bytes.fromhex(expected)
        if len(before)!=len(replacement):
            raise ValueError('Texture sync instruction size changed')
        at=file_offset(rva)
        data[at:at+len(before)]=replacement
        records.append(dict(symbol=symbol,offset=offset,rva=hex(rva),
                            before=before.hex(),after=replacement.hex(),purpose=purpose))
    patch(GPU_SYMBOL,0,'55 41 57 41 56',
          b'\xe9'+struct.pack('<i',replacement_rva-gpu-5),
          'Invalidate every overlapping image across the full GPU write range')
    patch(REFRESH_SYMBOL,0x134,'83 e0 03',bytes.fromhex('83 e0 07'),
          'Do not skip GPU-dirty uploads when the CPU hash is unchanged')
    patch(REFRESH_SYMBOL,0x182,'8b 97 ac 00 00 00 c1 ea 03 41 0f af d2 41 0f af d1',
          bytes.fromhex('8b 97 d0 01 00 00')+b'\x90'*11,
          'Hash all guest_size bytes, matching MarkAsMaybeDirty')
    expand=old_symbol(EXPAND_SYMBOL)
    # rsi = cache, r14 = original image, r15 = new image. The old block
    # refreshed only r15 before copying r14 over it. Both calls and the runtime
    # member load are checked, and the enclosing nonvolatile registers survive.
    expect(expand+0xe1,'48 8b 4e 10')
    expect(expand+0xe5,'4c 89 f2 4d 89 f8')
    def call(site,target):
        return b'\xe8'+struct.pack('<i',target-site-5)
    previous=(bytes.fromhex('48 89 f1 4c 89 fa')+
              call(expand+0xdc,refresh)+
              bytes.fromhex('48 8b 4e 10 4c 89 f2 4d 89 f8')+
              call(expand+0xeb,old_symbol(COPY_SYMBOL)))
    replacement=(bytes.fromhex('48 89 f1 4c 89 f2 4d 89 f8')+
                 call(expand+0xdf,expand_rva)+b'\x90'*12)
    patch(EXPAND_SYMBOL,0xd6,previous.hex(),replacement,
          'Refresh the source and destination before expanding/copying an image')
    overlap=old_symbol(OVERLAP_SYMBOL)
    # CopyMip originally receives runtime at cache+0x10. Pass the cache itself
    # to the wrapper; retain source/destination, mip and the fifth slice argument.
    expect(overlap+0x201,'89 44 24 20 4c 89 fa 41 89 f1 48 89 fe')
    patch(OVERLAP_SYMBOL,0x1fd,'48 8b 4f 10',bytes.fromhex('48 89 f9 90'),
          'Pass the owning texture cache to the subresource merge wrapper')
    patch(OVERLAP_SYMBOL,0x20e,call(overlap+0x20e,old_symbol(COPY_MIP_SYMBOL)).hex(),
          call(overlap+0x20e,merge_rva),
          'Refresh dirty images before copying a mip or array slice')
    # SliceOf's r8d holds parent mip_size/parent_layers. ecx initially holds
    # the WHOLE requested view's size, used only for the size-alignment check.
    # Switch ecx to the true slice stride before address alignment/division.
    # Keep the previous size-alignment rejection, using its existing -1 exit.
    slice_rva=old_symbol(SLICE_SYMBOL)
    expect(slice_rva+0xf5,'41 8b 01 31 d2 f7 77 34 41 89 c0')
    expect(slice_rva+0x100,'8b 8e 68 01 00 00 89 c8 31 d2 41 f7 f0')
    expect(slice_rva+0x136,'31 d2 48 f7 f1')
    expect(slice_rva+0x140,'31 d2 f7 f1')
    expect(slice_rva+0x149,'b8 ff ff ff ff 75 02 89 c8')
    patch(SLICE_SYMBOL,0x10d,'85 d2 74 07 b8 ff ff ff ff eb 3a',
          bytes.fromhex('44 89 c1 85 d2 75 35 90 90 90 90'),
          'Select array slices using the parent slice stride, not the entire view size')
    # Diagnostic-only: existing recordings omitted the skin material constants
    # because those draws are later than the first four. No extra work outside
    # a frame recording, and the launcher's capture-count/size limits still apply.
    patch(CAPTURE_DRAW_SYMBOL,0x453,'41 83 7d 90 04',bytes.fromhex('41 83 7d 90 40'),
          'Capture material constants for the first 64 draws instead of four')
    return records
