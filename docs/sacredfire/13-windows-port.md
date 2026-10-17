# Windows x86-64 offline port, cross-compiled from Linux (design and log)

Started 2026-10-11 from main at f1f7935 (v0.2.1 plus one docs commit), clean tree apart from the
FSR-Vulkan submodule patch that build.sh applies. Donor: Supermedo/bloodborne_pc at
ec8340a9f9d809436cf07fa1f07b08affa9c13e3 (a bbport 0.2-era Windows port of Bloodborne, MSYS2
CLANG64). Nothing Windows-specific existed in SacredFire before this work beyond `#ifndef _WIN32`
guards that compile the runtime files to empty stubs.

## Starting state (Linux baseline, 2026-10-11)

| check | result |
|---|---|
| `bash build.sh --test` at f1f7935 | PASS: out/sacredfire built (PGO off, LTO on), six C test programs pass (pad, runtime, file mods, semaphores, AppContent, event flags) |
| `python3 -m unittest discover -s tests` | PASS (see 13-windows-port-report.md for the count) |
| game | playable on the Steam Deck (STATUS.md); the only tested machine |

## Decisions (explicit, not inherited silently)

1. **Toolchain: llvm-mingw 20261006** (LLVM 23.1.3, mingw-w64 a3d9399, UCRT, libc++, SEH
   exceptions, winpthreads), the Linux-hosted `ubuntu-22.04-x86_64` tarball, pinned by SHA-256 in
   `packaging/windows/deps.lock`. Target triple `x86_64-w64-mingw32`, linker lld, resource compiler
   llvm-windres. It is the cross counterpart of the donor's MSYS2 CLANG64 environment (same CRT,
   same C++ library, same pthread implementation), so the donor's runtime assumptions carry over.
   No GCC/libstdc++ library is mixed in; every target library is built by this toolchain.
2. **Guest ABI: `__attribute__((sysv_abi))`**, already the `ABI` macro of src/runtime.h and
   shadPS4's `PS4_SYSV_ABI`. Verified with an executable probe (tests/windows/abi_probe.c): 7
   integer plus 2 double arguments, 16-byte and memory-class aggregates, host-to-guest-to-host
   nested callbacks, and preservation of the Microsoft-ABI callee-saved registers (rdi, rsi,
   xmm6-xmm15) across a SysV call that clobbers them. Runs under Wine as a diagnostic; real Windows
   is the acceptance environment.
3. **ASLR: `--disable-high-entropy-va` on the game executable only.** The PS4 user range (64 GiB
   to 1 TiB) must be free of host allocations; with high-entropy bottom-up randomization Windows
   places heaps and stacks anywhere in the first terabyte, so the range cannot be reserved
   reliably. The donor makes the same choice; shadPS4 goes further (`--disable-dynamicbase`, fixed
   image base). Image ASLR, DEP, CFG-compatible code and the other mitigations stay on. The
   launcher and tools keep the defaults. Raised with the user on 2026-10-11; no objection,
   proceeding with the donor's setting.
4. **Guest TLS: TEB TLS slot, patched displacement.** GS keeps pointing at the Windows TEB. The
   nine-byte `mov rax, gs:[0]` the preparation scripts produce (they rewrite `fs:[0]`) gets the
   displacement `0x1480 + 8*slot` of `TEB.TlsSlots[slot]`, slot from `TlsAlloc` before any DLL
   can take the low 64. Only that exact instruction form is patched; the count must equal the
   count the preparation step recorded (`fs_loads_patched` in out/link.json), and every site is
   decoded with Zydis before patching. Any other GS access form stops the loader with a message
   naming the offset. Instruction boundaries in stripped game code cannot be proven in general;
   the cross-check with the preparation scan plus the Linux runs (where the same sites are
   rewritten and the game plays for hours) is the evidence.
5. **Memory: pagefile-backed section + placeholders**, the donor's design: one `CreateFileMapping2`
   section for direct plus flexible memory, the user range reserved as a placeholder at startup,
   `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)` views, `UnmapViewOfFile2(MEM_PRESERVE_PLACEHOLDER)`,
   placeholder split and coalesce. Needs Windows 10 1803+ (the APIs) and the UTF-8 code page
   manifest needs 1903+: **Windows 10 1903 is the floor**. The dma-buf guest memory (BB_PC_MODEL)
   is Linux-only and stays off; the memfd hole punching becomes a memset of the released range.
6. **Faults: vectored exception handler** that handles GPU page-tracking faults first, then the
   speculative-read recovery (`bb_setjmp`/`bb_longjmp`, all Win64 callee-saved registers, xmm6-15,
   MXCSR and the x87 control word), then reports a fatal fault with registers and an rbp chain and
   ends the process; everything else continues the search. No C++ exception crosses a guest frame.
7. **Threads: winpthreads.** Guest stacks are Windows stacks, fully committed (guest code has no
   stack probes): winpthreads passes the size as the commit size, and the main guest thread runs on
   a 16 MiB committed process stack (linker `--stack 16777216,16777216`) exactly as on the donor.
   Condition-variable deadlines use the clock winpthreads supports (CLOCK_REALTIME, as
   runtime_mutex.c already does on Linux); monotonic deadlines are converted right before the wait.
8. **GPU library static on Windows**, linked into one `sacredfire-game.exe` (a DLL cannot import
   the runtime's symbols from the executable without an import library of the executable).
9. **FFmpeg excluded on Windows** (`BB_AVPLAYER=OFF`): CTR imports no libSceAvPlayer or
   libSceVideodec function (out/import_names.txt), so the AvPlayer sources are Bloodborne-only.
   Linux keeps building them (default ON) so nothing changes there.
10. **Player entrypoint: a native Win32 launcher plus the Python embeddable package.** The
    preparation steps are the existing Python scripts (SELF parsing, module linking, patches);
    rewriting them in C was judged out of scope. `python-3.13.16-embed-amd64.zip` (stdlib only,
    no Tk, no pip) runs them; `SacredFire.exe` (C, Win32 API only, built by the cross toolchain)
    is the counterpart of the GTK launcher: a window with the game folder, the controller for
    port 1, the game patches, PS4 Pro mode, fullscreen, the developer switches, Play/Stop and
    the log, and behind Play what run.sh does. `--play` starts the game without the window
    (Steam shortcuts). No PyInstaller: it is not a cross-compiler; no GTK: PyGObject cannot be
    produced for the embeddable Python from Linux.
11. **Not ported** (reported, not faked): userfaultfd write tracking (BB_UFFD), the dma-buf memory
    model (BB_PC_MODEL), the SIGTRAP single-step diagnostics (BB_LABEL_TRAP, BB_FREE_CHECK trap
    mode, guest heap hooks), PGO, MangoHud, mods overlays (symlinks), the in-game restart through
    run.sh (becomes a relaunch of the launcher command line), the GTK launcher's Steam shortcut
    writer and issue-report helpers.

## Architecture: platform services

Guest-facing logic (imports, event flags, saves, pad, AJM, audio) stays in `src/runtime_*.c`. The
host-specific pieces move behind `src/platform/` with one header and two implementations:

| service | header | Linux | Windows |
|---|---|---|---|
| virtual memory (reserve, map section view, unmap, protect, discard, low mappings) | platform/memory.h | memfd + mmap (unchanged behaviour) | section + placeholders |
| threads (TCB publication, stack attributes, names, ids, yield, cpu) | platform/thread.h | arch_prctl(ARCH_SET_GS), pthread | TlsSetValue + patched displacement, winpthreads |
| faults (handler install, recovery point, fatal report, watchdog dumps) | platform/fault.h | sigaction, sigsetjmp | AddVectoredExceptionHandler, bb_setjmp |
| files (positioned I/O, directory listing, stat with d_type, rename-replace, fsync, truncate, remove tree) | platform/file.h | POSIX | CRT + Win32 |
| time (monotonic ns, process time, high-resolution sleep, time zone offset, random bytes) | platform/time.h | clock_gettime, nanosleep, getrandom | QueryPerformanceCounter, waitable timers, BCryptGenRandom |
| process (restart, exit, module name of an address, safe memory read) | platform/process.h | execlp, dladdr, process_vm_readv | CreateProcess, GetModuleHandleEx, ReadProcessMemory |

The GPU library gets the same split in `gpu/shim/bbport_host.h` (thread id and name, safe memory
read, page protection, fault-context accessors): the shim diagnostics include it instead of
`<sys/mman.h>`, `<sys/uio.h>` and `<ucontext.h>`.

## Reuse matrix (donor -> SacredFire)

| SacredFire (current) | donor counterpart | adaptation | Linux impact | validation |
|---|---|---|---|---|
| src/probe.c `fault`, `watchdog`, `enter_on_stack` | probe.c `vectored_fault`, `fatal_exception`, `patch_tcb_loads` | behind platform/fault.h; Zydis check of patch sites; watchdog on Windows via SuspendThread/GetThreadContext (new) | none | tests/windows fault and TLS probes, game run |
| src/runtime_thread.c `set_gs`, `attach`, `create` | runtime_thread.c `runtime_thread_tcb_offset`, TlsSetValue, committed stacks | platform/thread.h; slot claimed in main before Vulkan | none | TLS probe: separate TCBs per thread, create/destroy loops, host-thread attach |
| src/runtime_memory.c `pool`, `map_phys`, `zero_phys`, `place`, `unmap_locked`, `protect_locked`, `runtime_low_map` | runtime_memory.c `host_map/host_unmap/host_protect/host_discard`, `carve_host`, `runtime_memory_reserve` | platform/memory.h keeps SacredFire's chunk/dma-buf code Linux-only; Windows backend from the donor with split/coalesce and protection restore | chunk allocator untouched | memory test: alias coherence, split/coalesce, protect runs, exhaustion, release zeroing |
| src/runtime_savedata.c `nftw` remove, `write_atomic` | compat_remove_tree | platform/file.h remove_tree scoped to the save root | none | save tests on a disposable root incl. replace failure |
| src/runtime_file.c dirent/d_type, fstatat, pread, rename, truncate, access, O_DIRECTORY | runtime_file.c Windows branches, compat_pread/pwrite, MoveFileEx | platform/file.h | none | file tests with spaces and non-ASCII names |
| src/runtime_kernel.c clocks, nanosleep, getrandom, getrusage, sysctl, sampler | compat_sleep_ns, compat_random, compat_cpu_times | platform/time.h; sampler and BB_DUMP_GOT Linux-only | none | time tests (units, monotonicity) |
| src/runtime.c `/dev/urandom` canary | rand_s | platform/time.h random | none | runtime test |
| src/runtime_sema.c / eventflag.c monotonic condvars | WAIT_CLOCK = CLOCK_REALTIME | platform wait clock | none | sema/eventflag tests pass on both |
| gpu/shim/window.cpp X11/Wayland | `WindowSystemType::Windows`, HWND | add the Windows branch | none | window smoke test |
| gpu/CMakeLists.txt shared lib, X11, FFmpeg | static lib on WIN32, bb-probe target in CMake | options BB_AVPLAYER, WIN32 branches, the executable targets | default values keep Linux as is | Linux build + Windows build |
| scripts/run.sh, launcher/ (GTK4) | run.py, bbport_launcher_win.py (Tkinter) | SacredFire.exe: Win32 launcher window in C (same pages as the GTK one) + embedded Python for the preparation; aerolib.inl bundled (no download at run time) | none | launcher test on Wine (window, Play, Stop, settings), then Windows |
| packaging/appimage.sh | packaging/windows/package.sh | scripts/package-windows.sh: closure check with llvm-readobj, zip, manifest, sha256 | none | archive inspection |
| excluded | launcher/bbport_launcher_win.py (Tkinter, PyInstaller), bbnet/shadNet, DLSS bridge, run.py co-op overrides | not taken | - | - |

## Dependency matrix

See packaging/windows/deps.lock (name, kind, source, exact commit or sha256, licence, role).
Host tools: cmake, ninja, python3, glslang (Arch 1.4.363; host shaders are compiled on Linux),
llvm-dlltool (vulkan-1 import library from the Vulkan-Loader `.def`). Target libraries built by
scripts/windows/build-deps.sh: SDL3 (shared DLL, shipped), fmt, xxHash, Zydis+Zycore, miniz
(static). Headers: Vulkan-Headers, VMA, magic_enum, xbyak, robin-map, Boost (header-only uses:
container, icl, intrusive, pool). Runtime: python embeddable (shipped), vulkan-1.dll (from the
player's graphics driver, never shipped), UCRT (part of Windows 10), libwinpthread-1.dll and
libc++.dll from the toolchain (shipped, with their licences).

## Build contract

```
scripts/build-windows.sh            # all stages, or one of:
scripts/windows/fetch-deps.sh       # 1 fetch + verify (the only stage that uses the network)
scripts/windows/build-deps.sh       # 2 target libraries -> out/windows/sysroot
scripts/build-windows.sh --runtime  # 3 cmake -S gpu -B out/windows/build (toolchain file) + ninja
scripts/package-windows.sh          # 4 stage closure, inspect, zip, manifest, sha256
```
Directories: out/windows/toolchain (host tools), out/windows/deps/src and out/windows/sysroot
(target), out/windows/build (objects), out/windows/stage and dist/ (release). All under out/,
git-ignored. Toolchain file: cmake/windows-x86_64-llvm-mingw.cmake (Windows/AMD64, find modes
restricted to the sysroot, programs from the host, pkg-config pointed at the sysroot only).

## Gates

Recorded in 13-windows-port-report.md as the work proceeds.
