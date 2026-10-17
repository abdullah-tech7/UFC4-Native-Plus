"""Execute only the identified host SliceOf routine with synthetic descriptors.

Never launches a runtime/game or initializes Vulkan. The original routine has
two external format-compatibility calls, redirected to a tiny equality stub.
All remaining instructions come from the actual donor or delivered PE.
"""
import ctypes as c
from pathlib import Path
import struct
import sys
sys.dont_write_bytecode=True
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'tools'))
from link_runtime_extension import coff
from texture_sync_patch import SLICE_SYMBOL

def routine(path):
    data=Path(path).read_bytes();pe=struct.unpack_from('<I',data,60)[0]
    sections,symbols,_=coff(data,pe+4,True)
    symbol=next(s for s in symbols.values() if s['name']==SLICE_SYMBOL)
    section=sections[symbol['section']-1];offset=section['raw']+symbol['value']
    code=bytearray(data[offset:offset+0x160])
    for site in (0x1e,0x2d):
        assert code[site]==0xe8
        struct.pack_into('<i',code,site+1,0x200-site-5)
    code.extend(bytes(0x200-len(code)))
    code.extend(bytes.fromhex('39 d1 0f 94 c0 c3')) # bool format == format
    kernel=c.WinDLL('kernel32',use_last_error=True)
    kernel.VirtualAlloc.argtypes=[c.c_void_p,c.c_size_t,c.c_uint32,c.c_uint32]
    kernel.VirtualAlloc.restype=c.c_void_p
    kernel.VirtualProtect.argtypes=[c.c_void_p,c.c_size_t,c.c_uint32,c.POINTER(c.c_uint32)]
    kernel.VirtualFree.argtypes=[c.c_void_p,c.c_size_t,c.c_uint32]
    address=kernel.VirtualAlloc(None,4096,0x3000,4) # RW
    if not address:raise c.WinError(c.get_last_error())
    c.memmove(address,bytes(code),len(code));previous=c.c_uint32()
    if not kernel.VirtualProtect(address,4096,0x20,c.byref(previous)):raise c.WinError(c.get_last_error())
    function=c.CFUNCTYPE(c.c_int32,c.c_void_p,c.c_void_p,c.c_int32)(address)
    return function,lambda:kernel.VirtualFree(address,0,0x8000)

def info(layers,address,size,*,base=0,mip=0,levels=1):
    data=bytearray(0x180)
    struct.pack_into('<I',data,0x24,37) # synthetic matching RGBA8 format
    struct.pack_into('<Q',data,0x28,9) # matching image types
    struct.pack_into('<II',data,0x30,levels,layers)
    struct.pack_into('<III',data,0x38,4<<base,4<<base,1)
    struct.pack_into('<III',data,0x44,32,1,4<<base)
    struct.pack_into('<IIII',data,0x58+16*mip,size,4,4,0)
    struct.pack_into('<QI',data,0x160,address,size)
    return data

def invoke(function,view,parent,mip=0):
    a=c.create_string_buffer(bytes(view));b=c.create_string_buffer(bytes(parent))
    return function(a,b,mip)

old,free_old=routine(sys.argv[1]);new,free_new=routine(sys.argv[2])
try:
    for address in (0x100000,0x1000000000): # includes 40-bit guest addresses
        parent=info(8,address,512)
        for layer in range(8):
            view=info(1,address+layer*64,64)
            assert invoke(old,view,parent)==layer
            assert invoke(new,view,parent)==layer
        view=info(2,address+4*64,128)
        assert invoke(old,view,parent)==2 # observed arithmetic defect
        assert invoke(new,view,parent)==4
        view=info(2,address+3*64,128)
        assert invoke(old,view,parent)==-1 # valid odd slice incorrectly rejected
        assert invoke(new,view,parent)==3
        for offset,size in ((3*64+1,128),(3*64,127)):
            assert invoke(new,info(2,address+offset,size),parent)==-1
        view=info(2,address+4*64,128);struct.pack_into('<I',view,0x24,99)
        assert invoke(new,view,parent)==-1
        parent_mip=info(8,address,512,base=2,mip=2,levels=3)
        assert invoke(new,info(2,address+4*64,128),parent_mip,2)==4
    print('PASS: actual original SliceOf returns layer 2 instead of 4 and rejects valid layer 3; patched routine fixes both, retaining single-layer/mip/alignment/format behavior')
finally:
    free_old();free_new()
