# SPDX-License-Identifier: GPL-2.0-or-later
"""Two recorder-only call redirects; verified private host ABI for GPU readback."""
import struct

DRAW='_ZN6Vulkan12FrameCapture4DrawEyyjj'
SAMPLED='_ZN6Vulkan12FrameCapture7SampledERKN9VideoCore9ImageInfoEb'
DOWNLOAD='_ZN6Vulkan7Runtime13DownloadImageEPN9VideoCore5ImageEPKNS1_6BufferENSt3__14spanIKN2vk15BufferImageCopyELy18446744073709551615EEE'
REQUEST='_ZN6Vulkan17StagingBufferPool7RequestEyN9VideoCore10MemoryTypeEyb'
FINDVIEW='_ZN9VideoCore5Image8FindViewERKNS_13ImageViewInfoEb'
REFRESH='_ZN9VideoCore12TextureCache12RefreshImageERNS_5ImageE'
IMAGE_DOWNLOAD='_ZN9VideoCore12TextureCache19DownloadImageMemoryEN6Common6SlotIdEb'


def apply_texture_probe(data,old_symbol,file_offset,draw_rva,sampled_rva):
    def expect(rva,witness):
        expected=bytes.fromhex(witness);at=file_offset(rva)
        if data[at:at+len(expected)]!=expected:
            raise ValueError(f'GPU texture probe ABI witness mismatch at {rva:#x}')
    # Sampled is passed Image.info at Image+0x68 in the single actual bind call.
    expect(0xa9c8b,'48 8d 4b 68 83 bd f8 05 00 00 01 0f 94 c2')
    # The runtime pointer belongs to the selected Image, not guessed from its address.
    expect(old_symbol(FINDVIEW)+0x40,'48 8b 8e e8 01 00 00')
    # Runtime scheduler +0x20, hook +8, hook context +0x10.
    expect(old_symbol(DOWNLOAD)+0x36,'49 8b 4d 20')
    expect(old_symbol(DOWNLOAD)+0x55,'49 8b 45 08')
    expect(old_symbol(IMAGE_DOWNLOAD)+0x1a0,'49 8d 50 30')
    expect(old_symbol(IMAGE_DOWNLOAD)+0x45,'8b 97 9c 00 00 00')
    # Image sample count and dimensions, also the ones RefreshImage uploads.
    expect(old_symbol(DOWNLOAD)+0x27,'44 8b 82 b0 00 00 00')
    expect(old_symbol(REFRESH)+0xbec,'44 8b bf a0 00 00 00')
    expect(old_symbol(REFRESH)+0xbf9,'44 8b a7 a4 00 00 00')
    # Request has a 40-byte sret result: RCX=out, RDX=pool, R8=size, R9=type.
    expect(old_symbol(REQUEST)+0xb,'48 89 ce')
    expect(old_symbol(REQUEST)+0x17,'49 81 f8 01 00 00 01')
    records=[]
    for name,site,replacement in ((DRAW,0x9650f,draw_rva),(SAMPLED,0xa9c99,sampled_rva)):
        before=b'\xe8'+struct.pack('<i',old_symbol(name)-site-5)
        expect(site,before.hex())
        after=b'\xe8'+struct.pack('<i',replacement-site-5)
        data[file_offset(site):file_offset(site)+5]=after
        records.append(dict(symbol=name,rva=hex(site),before=before.hex(),after=after.hex(),
                            purpose='Read-only GPU texture capture during an explicitly enabled serial frame recording'))
    return records
