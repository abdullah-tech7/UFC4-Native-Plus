# SPDX-License-Identifier: GPL-2.0-or-later
"""Inspect bounded, linear BC1/BC2/BC3 images actually read from the host GPU.

Reads captures only. Optional previews are decoded from captured pixels, never
generated or substituted game textures. Requires Pillow in the analysis Python.
"""
import argparse
import hashlib
import json
from pathlib import Path
from PIL import Image,ImageChops,ImageStat


def inspect_record(directory,record,preview=None):
    name=record['file']
    if not isinstance(name,str) or Path(name).name!=name or '/' in name or '\\' in name:
        raise ValueError('Unsafe capture filename')
    fmt=record['format']
    if fmt not in range(131,139):raise ValueError('Unsupported capture format')
    width,height,levels=(record[k] for k in ('width','height','levels'))
    if not (0<width<=4096 and 0<height<=4096 and 0<levels<=13):
        raise ValueError('Invalid captured dimensions')
    block=8 if fmt<=134 else 16
    codec=(1,'DXT1') if fmt<=134 else (2,'DXT3') if fmt<=136 else (3,'DXT5')
    if len(record['mips'])!=levels:raise ValueError('Incomplete mip metadata')
    total=sum(((max(width>>i,1)+3)//4)*((max(height>>i,1)+3)//4)*block for i in range(levels))
    path=directory/name
    if total>32*1024*1024 or path.stat().st_size!=total or record['written']!=total or record['close_error']:
        raise ValueError('Truncated or failed GPU readback')
    raw=path.read_bytes();results=[];at=0
    for level,mip in enumerate(record['mips']):
        w,h=max(width>>level,1),max(height>>level,1)
        size=((w+3)//4)*((h+3)//4)*block
        if mip!={'level':level,'offset':at,'width':w,'height':h}:
            raise ValueError('Noncanonical mip layout')
        pixels=Image.frombytes('RGBA',(w,h),raw[at:at+size],'bcn',codec)
        r,g,b,a=pixels.split();maximum=ImageChops.lighter(ImageChops.lighter(r,g),b)
        results.append(dict(level=level,width=w,height=h,sha256=hashlib.sha256(raw[at:at+size]).hexdigest(),
                            mean_rgba=[round(v,3) for v in ImageStat.Stat(pixels).mean],
                            fraction_rgb_near_black=round(sum(maximum.histogram()[:3])/(w*h),6),
                            fraction_alpha_zero=round(a.histogram()[0]/(w*h),6)))
        if preview and level==0:
            preview.mkdir(parents=True,exist_ok=True)
            pixels.thumbnail((1024,1024));pixels.save(preview/(Path(name).stem+'.png'))
        at+=size
    return {**{k:record[k] for k in ('file','ps','indices','address','format','flags','gpu_write_tick')},
            'source':'actual GPU image readback; first array layer; tightly packed mip chain',
            'mips':results}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    parser.add_argument('--preview',type=Path)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    manifest=args.directory/'textures.jsonl'
    if manifest.stat().st_size>256*1024:raise ValueError('Capture metadata exceeds limit')
    records=[json.loads(line) for line in manifest.read_text().splitlines() if line.strip()]
    if len(records)>48:raise ValueError('Capture image count exceeds limit')
    results=[inspect_record(args.directory,r,args.preview) for r in records]
    args.out.write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
    print(f'Analyzed {len(results)} actual GPU colour maps; results: {args.out}')


if __name__=='__main__':main()
