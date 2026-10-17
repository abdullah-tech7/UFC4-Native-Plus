# SPDX-License-Identifier: GPL-2.0-or-later
"""Known BC1/BC3 blocks, alpha, mip truncation and path guards; no game/GPU."""
import sys
import tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'tools'))
from analyze_ufc4_gpu_textures import inspect_record

with tempfile.TemporaryDirectory(dir=Path(sys.argv[1]),prefix='gpu-decode-') as folder:
    root=Path(folder)
    for fmt,block in ((132,bytes.fromhex('00f800f800000000')),
                      (138,bytes.fromhex('ffff00000000000000f800f800000000'))):
        (root/'red.raw').write_bytes(block)
        record=dict(file='red.raw',ps='7388baf7',indices=20220,address='0x1000',format=fmt,
                    flags=72,gpu_write_tick=10,width=4,height=4,levels=1,written=len(block),close_error=0,
                    mips=[dict(level=0,offset=0,width=4,height=4)])
        result=inspect_record(root,record)
        assert result['mips'][0]['mean_rgba']==[255,0,0,255]
        assert not result['mips'][0]['fraction_rgb_near_black']
        for bad in ({**record,'file':'../red.raw'},{**record,'written':0},
                    {**record,'mips':[dict(level=0,offset=8,width=4,height=4)]}):
            try:inspect_record(root,bad)
            except ValueError:pass
            else:raise AssertionError('Unsafe/incomplete texture accepted')
        (root/'red.raw').write_bytes(block[:-1])
        try:inspect_record(root,record)
        except ValueError:pass
        else:raise AssertionError('Truncated GPU readback accepted')
print('PASS: BC1/BC3 known pixel/alpha decode; path, mip-layout and truncated-readback guards')
