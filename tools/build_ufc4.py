# SPDX-License-Identifier: GPL-2.0-or-later
"""Reproduce UFC4 using the measured UFC3 v0.1.5 runtime and LLVM-MinGW."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
sys.dont_write_bytecode=True
from link_runtime_extension import link

DONOR_SHA256='3d2bcd9b6b59d349e73c03b940cec01e4589c972ed4f29b0e524335a3377651a'
SOURCE=Path(__file__).resolve().parent.parent

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler',type=Path,required=True)
    p.add_argument('--donor',type=Path,required=True,help='unmodified bin/sacredfire-game.exe from ufc3-v0.1.5')
    p.add_argument('--out',type=Path,required=True)
    args=p.parse_args()
    args.out.mkdir(parents=True,exist_ok=True)
    binary=args.out/'bin';binary.mkdir(exist_ok=True)
    obj=binary/'ufc4-services.obj'
    common=[str(args.compiler),'-O2','-Wall','-Wextra','-Werror']
    build_env=os.environ.copy();build_env.update(TMP=str(args.out.resolve()),TEMP=str(args.out.resolve()))
    subprocess.run([*common,'-c','-g0','-DUFC4_DONOR_EXTENSION=1','-DUFC4_DONOR_V015=1',
        '-fno-asynchronous-unwind-tables','-fno-unwind-tables','-fno-stack-protector',
        '-fno-omit-frame-pointer',str(SOURCE/'src/runtime_ufc4.c'),'-o',str(obj)],check=True,env=build_env)
    link(args.donor,obj,binary/'ufc4-runtime.exe',DONOR_SHA256)
    subprocess.run([*common,'-municode','-mwindows','-Wl,--no-insert-timestamp',str(SOURCE/'tools/ufc4_entry.c'),
        '-o',str(args.out/'UFC4.exe')],check=True,env=build_env)

if __name__=='__main__':main()
