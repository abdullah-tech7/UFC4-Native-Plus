# SPDX-License-Identifier: GPL-2.0-or-later
"""Reproduce the observed rejected 1->2->3 fence using the donor's guard code.

Compiles its unchanged decision code (only the logging tail is omitted), then
links the delivered C correction against it. No renderer/game is launched.
"""
from pathlib import Path
import os
import subprocess
import sys

source = Path(__file__).resolve().parents[1]
compiler = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
original = (source / 'gpu/shim/bbport_clobber.cpp').read_text()
start = original.index('bool GuardLabel(')
end = original.index('    static std::atomic<std::uint64_t> guarded', start)
decision = original[start:end] + '    return true;\n}\n'
helper_start = original.index('bool LooksLikeType3Header(')
helper_end = original.index('} // namespace', helper_start)
helper = original[helper_start:helper_end]
header = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
struct U4LabelRange { uint64_t seq,base,bytes,ns; };
extern "C" {
U4LabelRange u4_label_test_ranges[256]{};
std::atomic<uint64_t> u4_label_test_head{0};
uint8_t ufc4_gpu_guard_label(uint64_t,uint64_t,uint32_t,uint32_t,uint64_t,const uint32_t*);
}
namespace BbClobber {
using Range=U4LabelRange;
enum Source : uint32_t { Eop,Eos,EosGds,ReleaseMem };
std::span<Range,256> ranges{u4_label_test_ranges};
auto& ranges_head=u4_label_test_head;
struct LastWrite { std::atomic<uint64_t> address{0},value{0},ns{0}; };
std::array<LastWrite,4096> last_writes{};
LastWrite& LastWriteSlot(uint64_t address) { return last_writes[(address>>2)%4096]; }
'''
tail = r'''
}
extern "C" uint8_t u4_label_original(uint64_t a,uint64_t v,uint32_t n,uint32_t s,
                                    uint64_t p,const uint32_t* submitted) {
 return BbClobber::GuardLabel(a,v,n,static_cast<BbClobber::Source>(s),p,submitted);
}
static uint64_t slot=1;
static const uint64_t address=reinterpret_cast<uint64_t>(&slot);
static void submit(uint64_t seq,uint64_t ns) {
 auto i=u4_label_test_head.load();
 u4_label_test_ranges[i%256]={seq,address,8,ns};u4_label_test_head.store(i+1);
}
static void note(uint64_t value,uint64_t ns) {
 auto& last=BbClobber::LastWriteSlot(address);
 last.address=address;last.value=value;last.ns=ns;
}
int main() {
 uint32_t initial=1;
 submit(0,1);slot=1;
 assert(!ufc4_gpu_guard_label(address,2,4,1,0,&initial));slot=2;
 assert(!ufc4_gpu_guard_label(address,3,4,1,0,&initial));slot=3;
 submit(46099,100);slot=1;
 assert(!u4_label_original(address,2,4,1,46099,&initial));slot=2;
 assert(u4_label_original(address,3,4,1,46099,&initial));
 note(3,101); // opt-in NoteWrite also remembers the UPCOMING value too early.
 assert(u4_label_original(address,3,4,1,46099,&initial));
 puts("REPRODUCED: donor rejects its own 2 -> 3 progression, with diagnostics both off and on");
 for(unsigned diagnostic=0;diagnostic<2;++diagnostic) {
  uint64_t seq=50000+diagnostic*1000;submit(seq,200+diagnostic);slot=1;
  for(uint64_t value=2;value<=64;++value) {
   if(diagnostic)note(value,300+value);
   assert(!ufc4_gpu_guard_label(address,value,4,1,seq,&initial));slot=value;
  }
  slot=0xc0016900; // game replaced label slot with a genuine SET_CONTEXT_REG header.
  assert(ufc4_gpu_guard_label(address,65,4,1,seq,&initial));assert(slot==0xc0016900);
  assert(ufc4_gpu_guard_label(address,65,4,1,seq,nullptr));
  // Same address in a NEW submission must not inherit an old approval.
  submit(seq+100,500);slot=64;note(999,501);
  assert(ufc4_gpu_guard_label(address,65,4,1,seq,&initial));
  assert(ufc4_gpu_guard_label(address,65,4,1,seq+100,&initial));
 }
 uint64_t initial64=0x100000001ull;submit(60000,1000);slot=initial64;
 for(uint64_t i=1;i<=32;++i) {
  uint64_t value=initial64+i;note(value,1000+i);
  assert(!ufc4_gpu_guard_label(address,value,8,0,60000,reinterpret_cast<uint32_t*>(&initial64)));
  slot=value;
 }
 // A changed high dword is not mistaken for our 64-bit write.
 slot^=0x100000000ull;note(999,2000);
 assert(ufc4_gpu_guard_label(address,initial64+33,8,0,60000,reinterpret_cast<uint32_t*>(&initial64)));
 // Missing/invalid metadata remains delegated; no pointer read on invalid input.
 assert(!ufc4_gpu_guard_label(0,2,4,1,60000,&initial));
 assert(!ufc4_gpu_guard_label(1,2,3,1,60000,&initial));
 puts("PASS: 32/64-bit counters, diagnostic independence, real header protection, new submission isolation, invalid arguments; no GPU/game initialized");
}
'''
test_cpp = out / 'gpu-label-regression.cpp'
test_cpp.write_text(header + helper + decision + tail)
env = os.environ.copy()
env.update(TMP=str(out), TEMP=str(out))
obj = out / 'gpu-label-test.obj'
subprocess.run([str(compiler), '-O2', '-Wall', '-Wextra', '-Werror', '-DU4_LABEL_TEST',
    '-c', str(source / 'src/ufc4_gpu_labels.c'), '-o', str(obj)], check=True, env=env)
exe = out / 'gpu-label-regression.exe'
cpp = compiler.with_name(compiler.name.replace('clang.exe', 'clang++.exe'))
subprocess.run([str(cpp), '-std=c++20', '-O2', str(test_cpp), str(obj), '-o', str(exe)], check=True, env=env)
subprocess.run([str(exe)], check=True, env=env)
