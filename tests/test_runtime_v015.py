# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify the v0.1.5 renderer is byte-identical and adapters are narrowly scoped."""
from pathlib import Path
import contextlib
import hashlib
import io
import json
import struct
import sys
import tempfile
sys.dont_write_bytecode=True
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from link_runtime_extension import coff,link

donor=Path(sys.argv[1]);build=Path(sys.argv[2]);obj=build/'bin/ufc4-services.obj'
sha='3d2bcd9b6b59d349e73c03b940cec01e4589c972ed4f29b0e524335a3377651a'
original=donor.read_bytes();assert hashlib.sha256(original).hexdigest()==sha
pe=struct.unpack_from('<I',original,60)[0]
sections,symbols,optsize=coff(original,pe+4,True);opt=pe+24
def pos(rva):
    s=next(s for s in sections if s['rva']<=rva<s['rva']+s['size'])
    return s['raw']+rva-s['rva']
with tempfile.TemporaryDirectory(dir=build,prefix='v015-link-') as directory:
    out=Path(directory)/'test.exe'
    with contextlib.redirect_stdout(io.StringIO()):link(donor,obj,out,sha)
    result=out.read_bytes();report=json.loads(out.with_suffix('.link.json').read_text())
    new,_,_=coff(result,pe+4,True)
    assert len(new)==len(sections)+2
    allowed=set(range(pe+6,pe+8))|set(range(opt+56,opt+60))|set(range(opt+64,opt+68))
    header=pe+24+optsize+len(sections)*40;allowed.update(range(header,header+80))
    calls=[report['fallback_call_rva'],report['guest_thread_create_call_rva'],
           report['services_resolver_call_rva'],report['savedata_resolver_call_rva'],
           report['kernel_resolver_call_rva'],report['offline_resolver_call_rva'],
           *report['thread_attach_call_rvas']]
    assert len(calls)==8 and len(set(calls))==8
    for call in calls:
        rva=int(call,16);at=pos(rva)
        assert original[at] in (0xe8,0xe9) and result[at]==original[at]
        target=rva+5+struct.unpack_from('<i',result,at+1)[0]
        assert new[-2]['rva']<=target<new[-2]['rva']+new[-2]['vsize']
        allowed.update(range(at+1,at+5))
    differences={i for i,(a,b) in enumerate(zip(original,result)) if a!=b}
    assert differences<=allowed,sorted(differences-allowed)[:10]
    assert report['texture_sync_patches']==report['material_probe_patches']==report['gpu_label_patches']==[]
    assert not report['absolute_pointer_relocations']
    assert new[-2]['traits']&0x20000000 and not new[-1]['traits']&0x20000000
    renderer=next(s for s in symbols.values() if s['name']=='bbgpu_resolve')
    renderer_rva=sections[renderer['section']-1]['rva']+renderer['value']
    for call in calls:assert int(call,16)<renderer_rva
    with contextlib.redirect_stdout(io.StringIO()):link(donor,obj,out,sha)
    assert out.read_bytes()==result
    for target,bad_sha in ((out,'0'*64),(donor,sha),(obj,sha)):
        try:
            with contextlib.redirect_stdout(io.StringIO()):link(donor,obj,target,bad_sha)
        except ValueError:pass
        else:raise AssertionError('Unsafe or unidentified donor accepted')
assert hashlib.sha256(donor.read_bytes()).hexdigest()==sha
print('PASS: eight measured service/thread/offline redirects; entire donor renderer unchanged; deterministic PE, ASLR, RX/RW separation and donor/overwrite guards')
