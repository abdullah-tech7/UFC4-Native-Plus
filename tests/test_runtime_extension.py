"""Verify PE layout, narrowly scoped donor changes and overwrite/hash guards."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile

sys.dont_write_bytecode=True
source=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(source / 'tools'))
from link_runtime_extension import coff, link
from texture_sync_patch import (GPU_SYMBOL, REFRESH_SYMBOL, EXPAND_SYMBOL,
                                OVERLAP_SYMBOL, SLICE_SYMBOL, CAPTURE_DRAW_SYMBOL, apply_texture_sync)
from texture_probe_patch import apply_texture_probe, DRAW, SAMPLED
from gpu_label_patch import apply_gpu_labels, SITES

run=Path(sys.argv[1]).resolve()
donor=run/'bin/sacredfire-game.exe'
obj=run/'ufc4-services.obj'
sha=json.loads((run/'runtime-provenance.json').read_text())['sha256']['sacredfire-game.exe']
original=donor.read_bytes()
with tempfile.TemporaryDirectory(prefix='link-test-',dir=run) as directory:
    output=Path(directory)/'test.exe'
    link(donor,obj,output,sha)
    result=output.read_bytes()
    report=json.loads(output.with_suffix('.link.json').read_text())
    pe=struct.unpack_from('<I',original,0x3c)[0]
    sections,symbols,optsize=coff(original,pe+4,True)
    new_sections,_,_=coff(result,pe+4,True)
    opt=pe+24
    allowed=set(range(pe+6,pe+8))|set(range(opt+56,opt+60))|set(range(opt+64,opt+68))
    headers=pe+24+optsize+len(sections)*40
    allowed.update(range(headers,headers+80))
    def position(rva):
        s=next(s for s in sections if s['rva']<=rva<s['rva']+s['size'])
        return s['raw']+rva-s['rva']
    def original_symbol(name):
        matches={sections[s['section']-1]['rva']+s['value'] for s in symbols.values()
                 if s['section']>0 and s['name']==name}
        assert len(matches)==1
        return matches.pop()
    # An incompatible class layout must fail before applying a renderer patch.
    damaged=bytearray(original)
    damaged[position(original_symbol(GPU_SYMBOL)+0x25)]^=1
    try:apply_texture_sync(damaged,original_symbol,position,*([new_sections[-2]['rva']]*3))
    except ValueError:pass
    else:raise AssertionError('Incompatible texture-cache layout accepted')
    damaged=bytearray(original)
    damaged[position(0xa9c8b)]^=1
    try:apply_texture_probe(damaged,original_symbol,position,*([new_sections[-2]['rva']]*2))
    except ValueError:pass
    else:raise AssertionError('Incompatible GPU-capture ABI accepted')
    damaged=bytearray(original)
    damaged[position(SITES[0]-0x1c)]^=1
    try:apply_gpu_labels(damaged,original_symbol,position,new_sections[-2]['rva'])
    except ValueError:pass
    else:raise AssertionError('Incompatible graphics-label ABI accepted')
    calls=[report['fallback_call_rva'],report['guest_thread_create_call_rva'],
           report['services_resolver_call_rva'],report['savedata_resolver_call_rva'],
           report['kernel_resolver_call_rva'],*report['thread_attach_call_rvas']]
    for call in calls:
        p=position(int(call,16));allowed.update(range(p+1,p+5))
        assert original[p] in (0xe8,0xe9) and original[p]==result[p]
        target=int(call,16)+5+struct.unpack_from('<i',result,p+1)[0]
        assert new_sections[-2]['rva']<=target<new_sections[-2]['rva']+new_sections[-2]['vsize']
    patches=report['texture_sync_patches']
    assert [(p['symbol'],p['offset'],len(bytes.fromhex(p['before']))) for p in patches]==[
        (GPU_SYMBOL,0,5),(REFRESH_SYMBOL,0x134,3),(REFRESH_SYMBOL,0x182,17),
        (EXPAND_SYMBOL,0xd6,26),(OVERLAP_SYMBOL,0x1fd,4),(OVERLAP_SYMBOL,0x20e,5),
        (SLICE_SYMBOL,0x10d,11),(CAPTURE_DRAW_SYMBOL,0x453,5)]
    for patch in patches:
        p=position(int(patch['rva'],16));before=bytes.fromhex(patch['before']);after=bytes.fromhex(patch['after'])
        assert original[p:p+len(before)]==before
        assert result[p:p+len(after)]==after
        allowed.update(range(p,p+len(before)))
    p=position(int(patches[0]['rva'],16))
    assert result[p]==0xe9
    target=int(patches[0]['rva'],16)+5+struct.unpack_from('<i',result,p+1)[0]
    assert new_sections[-2]['rva']<=target<new_sections[-2]['rva']+new_sections[-2]['vsize']
    assert patches[1]['after']=='83e007'
    assert patches[2]['after']=='8b97d0010000'+'90'*11
    # Calls stay in their original functions, with checked argument setup, and
    # target the RX extension. No guessed source pointer or hidden tuple ABI.
    assert patches[3]['after'].startswith('4889f14c89f24d89f8e8')
    assert patches[3]['after'].endswith('90'*12)
    assert patches[4]['after']=='4889f990'
    assert patches[6]['after']=='4489c185d2753590909090'
    assert patches[7]['after']=='41837d9040'
    for patch,call_offset in ((patches[3],9),(patches[5],0)):
        rva=int(patch['rva'],16)+call_offset;p=position(rva)
        assert result[p]==0xe8
        target=rva+5+struct.unpack_from('<i',result,p+1)[0]
        assert new_sections[-2]['rva']<=target<new_sections[-2]['rva']+new_sections[-2]['vsize']
    probes=report['material_probe_patches']
    assert [(p['symbol'],int(p['rva'],16)) for p in probes]==[(DRAW,0x9650f),(SAMPLED,0xa9c99)]
    for patch in probes:
        rva=int(patch['rva'],16);p=position(rva)
        assert original[p:p+5]==bytes.fromhex(patch['before'])
        assert result[p:p+5]==bytes.fromhex(patch['after'])
        assert result[p]==0xe8
        target=rva+5+struct.unpack_from('<i',result,p+1)[0]
        assert new_sections[-2]['rva']<=target<new_sections[-2]['rva']+new_sections[-2]['vsize']
        allowed.update(range(p,p+5))
    assert not report['absolute_pointer_relocations'] # Current extension uses RIP-relative references.
    labels=report['gpu_label_patches']
    assert [int(p['rva'],16) for p in labels]==list(SITES)
    for patch in labels:
        rva=int(patch['rva'],16);p=position(rva)
        assert original[p:p+5]==bytes.fromhex(patch['before'])
        assert result[p:p+5]==bytes.fromhex(patch['after'])
        target=rva+5+struct.unpack_from('<i',result,p+1)[0]
        assert new_sections[-2]['rva']<=target<new_sections[-2]['rva']+new_sections[-2]['vsize']
        allowed.update(range(p,p+5))
    differences={i for i,(a,b) in enumerate(zip(original,result)) if a!=b}
    assert differences<=allowed,sorted(differences-allowed)[:10]
    assert len(new_sections)==len(sections)+2
    assert new_sections[-2]['name']=='.u4text' and new_sections[-2]['traits']&0x20000000
    assert new_sections[-1]['name']=='.u4data' and not new_sections[-1]['traits']&0x20000000
    assert new_sections[-1]['rva']>=new_sections[-2]['rva']+new_sections[-2]['vsize']
    assert len(result)>=len(original)
    link(donor,obj,output,sha)
    assert output.read_bytes()==result,'Linking is not deterministic'
    for target,expected in ((output,'0'*64),(donor,sha),(obj,sha)):
        try:link(donor,obj,target,expected)
        except ValueError:pass
        else:raise AssertionError('Unsafe link accepted')
    assert hashlib.sha256(donor.read_bytes()).hexdigest()==sha
print('PASS: service/thread and texture redirects, bounded capture, two graphics-label guards; PE protections, deterministic output and ABI witnesses')
