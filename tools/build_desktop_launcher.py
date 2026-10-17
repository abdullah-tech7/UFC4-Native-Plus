# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a native desktop entry with a game icon and fixed package location."""
import argparse
import json
import os
from pathlib import Path
import subprocess

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',type=Path,required=True)
    parser.add_argument('--package',type=Path,required=True)
    parser.add_argument('--icon',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    output=args.out.resolve();output.parent.mkdir(parents=True,exist_ok=True)
    template=Path(__file__).with_name('ufc4_desktop_entry.c.in').read_text(encoding='utf-8')
    generated=output.with_suffix('.c')
    generated.write_text(template.replace('@PACKAGE_ROOT@',json.dumps(str(args.package.resolve()),ensure_ascii=False)),encoding='utf-8')
    resource=output.with_suffix('.rc');obj=output.with_suffix('.res.o')
    resource.write_text('1 ICON '+json.dumps(args.icon.resolve().as_posix())+'\n',encoding='utf-8')
    windres=args.compiler.resolve().with_name('x86_64-w64-mingw32-windres.exe')
    build_env=os.environ.copy();build_env.update(TMP=str(output.parent),TEMP=str(output.parent))
    subprocess.run([str(windres),'-i',str(resource),'-o',str(obj)],check=True,env=build_env)
    subprocess.run([str(args.compiler.resolve()),'-O2','-Wall','-Wextra','-Werror','-municode','-mwindows',
                    '-Wl,--no-insert-timestamp',str(generated),str(obj),'-o',str(output)],check=True,env=build_env)
    print(output)

if __name__=='__main__':main()
