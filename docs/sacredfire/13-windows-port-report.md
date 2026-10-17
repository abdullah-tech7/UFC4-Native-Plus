# Windows port: verification report

Companion of [13-windows-port.md](13-windows-port.md). Every gate names its state (PASS, FAIL,
BLOCKED, NOT RUN), what was run, where, and what comes next. A skipped test is not a pass.
Written on the Steam Deck (Arch container "archbuild", 8 threads, 14 GiB) on 2026-10-11/12;
no Windows machine was available to this session, so native runtime acceptance is BLOCKED and
the archive is a candidate, not a release.

## Revisions and inputs

| item | value |
|---|---|
| SacredFire source | branch windows-port, commit faa002a on top of main f1f7935 (v0.2.1-1); the Windows candidate archive and the Wine runs described below were made from this tree before the commit (same content) |
| donor | Supermedo/bloodborne_pc ec8340a9f9d809436cf07fa1f07b08affa9c13e3 (read, not merged) |
| toolchain | llvm-mingw 20261006 ucrt ubuntu-22.04 x86_64 (clang 23.1.3, mingw-w64 a3d9399, UCRT, libc++, winpthreads), sha256 5f9c6ed9...8ab4 |
| inputs | packaging/windows/deps.lock (14 entries, every one pinned by commit or sha256) |
| host tools | cmake 4.4.4, ninja, glslang 1.4.363, python 3.14.7, llvm-readobj/llvm-dlltool from the toolchain, Wine 11.0 (diagnostic only) |

## Gates

| gate | state | evidence |
|---|---|---|
| 1 Linux baseline before changes | PASS | `bash build.sh --test` at f1f7935: six C test programs pass (scratch log baseline-build-test.log); `python3 -m unittest discover -s tests` |
| 1 Linux regression after the platform layer | see "Linux regression" below | `bash build.sh --test` after every runtime change |
| 2 Cross toolchain produces a Windows PE | PASS | tests/windows/abi_probe.c -> PE32+ AMD64 console executable, imports UCRT api-sets and kernel32 only (`llvm-readobj --file-headers --coff-imports`) |
| 2 SysV ABI bridge (compiler output, not just the attribute) | PASS under Wine; NOT RUN on Windows | abi_probe: 7 int + 2 double args, 16-byte and memory-class aggregates, nested host<->guest callbacks, rdi/rsi/xmm6-15 preserved across a clobbering SysV call: `aggregates/callbacks/preservation PASS` |
| 2 Target dependencies cross-built | PASS | sysroot: SDL3.dll (PE32+ DLL), libfmt, libxxhash, libZydis+libZycore, libminiz, libvulkan-1.a (from the loader's .def), headers (Vulkan 1.4.363, VMA, magic_enum, xbyak, robin-map, Boost 1.92 subset) |
| 3 Platform runtime compiles for both hosts | PASS | every src/runtime_*.c, src/probe.c and the two platform files compile with gcc (Linux) and x86_64-w64-mingw32-clang -Werror |
| 3 TLS probe (slot < 64, per-thread TCBs, Zydis-checked patch, patched code executes) | see below | tests/windows/tls_probe.c |
| 3 Memory probe (reservation, aliasing, split/coalesce, protection runs, discard, refusals) | see below | tests/windows/memory_probe.c |
| 3 File probe (UTF-8 names with spaces, positioned I/O, listing, rename-replace, truncate, directory descriptors, contained removal) | see below | tests/windows/file_probe.c |
| 3 Runtime tests (test_runtime, test_sema, test_eventflag, test_content) as Windows programs | see below | `scripts/build-windows.sh --tests` |
| 4 GPU library and game executable cross-build | PASS | `scripts/build-windows.sh --runtime`: 202 objects of the GPU library, FSR, sirit, ImGui, the runtime and the three executables; `llvm-readobj --file-headers`: PE32+ AMD64, DYNAMIC_BASE and NX_COMPAT set, HIGH_ENTROPY_VA absent on sacredfire-game.exe only, stack reserve = commit = 16 MiB; `--coff-imports`: KERNEL32, USER32, SHELL32, ntdll, bcrypt, WS2_32, the api-ms-win-core/crt sets, vulkan-1.dll, SDL3.dll, libc++.dll, libunwind.dll, libwinpthread-1.dll (nothing from MSYS or Cygwin) |
| 4 Vulkan smoke test (`sacredfire-game.exe --vulkan-only`) on Windows | PASS under Wine; BLOCKED on Windows (no machine) | Wine 11 with the Deck's RADV through winevulkan: `Vulkan: AMD Custom GPU 0932 (RADV VANGOGH); command submission + 4096-byte readback PASS`; `bb-gpu-capabilities.exe --device` names the GPU. Wine is a diagnostic: the same two commands are step 2 of "Native Windows" below |
| 3 Runtime tests as Windows programs (runtime, sema, eventflag, content) | PASS under Wine; NOT RUN on Windows | `scripts/build-windows.sh --tests`, run with the toolchain DLLs beside them: all four print their PASS line (the runtime test exercises the section-backed direct memory, guard ABI, mutexes, callbacks) |
| 4 Window, native PRX initialization, title screen, input, audio | PASS on Windows (user-run, 2026-10-12); Wine diagnostic earlier | The user ran the archive from commit 4455a29 on Windows (AMD Custom GPU 0932, AMD proprietary driver 2.0.302, Vulkan 1.3.280, 1920x1080): launcher window, game check, preparation, 3747 TCB loads patched ("matches the preparation step"), window, title screen, input, audio and races all worked ("launchers and in game menu work"); the log is the launcher's per-run file. Earlier under Wine: | The packaged tree under Wine 11 (Bottles flatpak) with the user's game folder: SacredFire.exe ran the bundled Python through game_check, prepare, link_libc, link_modules (fs->gs patched=3747), gen_import_names (bundled aerolib.inl), content_profile, patches; sacredfire-game.exe found the display and the GPU (RADV, Vulkan 1.4), created the window and presenter, patched 3747 TCB loads ("matches the preparation step"), ran the three module initializers (all 0), entered the eboot, loaded the system modules, opened two audio ports; the GPU thread decoded thousands of submissions and the user heard the game's audio. Window: with Wine's X11 driver under Xwayland, KWin kept a 1920x1080 windowed surface at 1 pixel high off-screen (xdotool could not help); BB_FULLSCREEN=1 gives a viewable 1920x1080 window. Wine's own Wayland driver (flatpak Wine 11) cannot create the Vulkan SDL window ("File not found"). Fullscreen run on 2026-10-12: the user played it ("it worked fine"), the engine log shows a race being loaded (eight drivers), and closing the window ended the process with exit code 0. None of this is Windows acceptance evidence: it is the Windows build on Wine's Win32 implementation over the Deck's Linux drivers. |
| 5 Race, menus, save, restart, reload, repeated sessions | PARTIAL on Windows: races and the in-game menu PASS; a fifth session (f9c3223) ran seven races and quit cleanly (exit 0), lighting wrong on Valve's Windows driver but right under Wine/RADV (observations below); saving confirmed by the user on native Windows (2026-10-12 evening: "all the saving stuff seems to work"); three sessions ended in the issue #2 crash (race 2, race 2, race 4), the fourth (5edc381, with the guard) played five tracks and about seven races, the guard caught issue #2 twice without a crash, and the session ended in a colour-buffer format assertion (fixed: nearest format); the in-game restart path is the remaining unreported step | The race-4 log is the first instrumented reproduction of issue #2 (docs/sacredfire/12-clobbered-dword-handoff.md, section 2026-10-12): the port's EOS label write landed in a command buffer the game had rebuilt. The late-label guard (same section) drops such a write and logs the writer's call chain; the seven-race log shows it holding twice (the draw recording thread writing a label 3.2 s after its submission, one frame behind the decoder, at a race load) and names the next question (what lets the game run a frame ahead of the recording thread). The format assertion that ended that session is fixed by the nearest-format fallback |
| 4 Launcher window (SacredFire.exe): folder, controller list, patches to bbport.ini, switches, Play, log, Stop, settings kept | PASS under Wine; NOT RUN on Windows | 2026-10-12, Wine 11 (Proton) with the X11 driver: the window lays out at 96 and 144 DPI (system message font, scaled positions), lists the one SDL gamepad, a Play press ran the preparation and the game with the log streaming into the window, the controls were locked during the run and freed when the watchdog ended it (exit 142 with BB_TIMEOUT=90), closing the window wrote sacredfire.ini. The folder picker (IFileOpenDialog) and "Open logs folder" were not exercised under Wine |
| 6 Release archive built on Linux, closure inspected | see below | `scripts/package-windows.sh` (needs a clean, tagged checkout or BB_ALLOW_DIRTY=1 for a candidate) |
| 6 Clean-Windows test of the archive, AMD/NVIDIA coverage | BLOCKED | needs real Windows |

## Linux regression

PASS. After every runtime change and again after the GPU-side edits (`bash build.sh --test`,
2026-10-12): out/sacredfire rebuilt (GPU library with LTO, AvPlayer out), the six C test programs
pass (pad, runtime, file mods, semaphores, AppContent, event flags); `python3 -m unittest discover
-s tests`: 87 tests OK. Behaviour on Linux is unchanged by design: platform_posix.c holds the
exact calls the runtime files made before. The game itself was not relaunched on the Deck by this
session (a boot takes two minutes; the user runs the gameplay checks).

## Windows probe runs (Wine 11.0 on the Deck, diagnostic)

Wine is not Windows: the memory probe exercises Wine's placeholder implementation, the TLS probe
Wine's TEB layout. A pass shows the code paths work against a faithful Win32 implementation; the
same binaries must be run on Windows (section "Native Windows").

| probe | result | note |
|---|---|---|
| abi_probe.exe | PASS | `sum 170 ok`, `aggregates/callbacks/preservation PASS` |
| tls_probe.exe | PASS | TEB TLS slot 3 (displacement 0x1498), 400 threads each reading their own TCB through gs:[slot], two synthetic sites decoded and patched, the patched instruction executed |
| memory_probe.exe (BB_PROBE_PAGE=16384 and 65536) | PASS | reservation of 64 GiB-1 TiB, aliasing of two views and the backing view, split and coalesce around a partial unmap, protection runs restored after a neighbour's remap, discard zeroing, refusal of ranges outside the reservation and of an offset past the section (logged, returned -1) |
| file_probe.exe | PASS | names with spaces and `éü日本`, positioned reads/writes leaving the offset alone, listing with types, rename over an existing file, truncate, directory descriptors, removal that refuses the root and anything outside it |

Found on the way: Wine 11 only exposes `swprintf_s`/`_countof`-style MSVC names when the
mingw-w64 headers are included (FidelityFX sources rely on MSVC's transitive includes;
gpu/shim/bb_ffx_prelude.h); libc++ has no `std::atomic_ref<const T>` (texture_cache.h now uses
`__atomic_load_n`); a positioned read on a write-only descriptor is correctly refused (the first
version of the file probe opened the file write-only).

## Observations from the user's Wine session (2026-10-12)

- The in-game menu's check boxes unchecked themselves at once: the settings writer replaced
  bbport.ini with the C `rename`, which refuses an existing destination on Windows, so the menu
  re-read the unchanged file. Fixed (std::filesystem::rename, which replaces on every host);
  the menu's quit button and the controller port assignments worked, the game ran and races
  loaded.
- The in-game menu's check boxes work on native Windows after the std::filesystem::rename fix
  (user-confirmed 2026-10-12 with the GUI launcher archive); the earlier report of the bug
  persisting was with the pre-fix archive.
- Lighting on native Windows is wrong "from the start" (the user's words) with the Vulkan
  driver Valve ships for the Steam Deck APU on Windows (AMD proprietary 2.0.302, 24.10.02.03,
  Vulkan 1.3.280, 2024). The same archive under Wine on the Deck, where the driver is RADV,
  renders the lighting correctly (2026-10-12, user-confirmed in a race), so the difference is
  the driver, not the port. That driver reports A2B10G10R10 Snorm/Sint and R32G32B32 image
  formats as unsupported and lacks VK_KHR_maintenance8, VK_EXT_attachment_feedback_loop_dynamic_state,
  VK_EXT_image_2d_view_of_3d, VK_KHR_compute_shader_derivatives and VK_KHR_cooperative_matrix;
  the game created no image the driver rejected (no "is not supported" line). Narrowing it to
  a feature needs an A/B on that machine (BB_* switches or a newer driver); a dGPU with a
  current AMD, NVIDIA or Intel driver is the other data point to get.
- The launcher window first came up 65535x65535 pixels under Wine: the Proton-made prefix
  carries `CaptionHeight=6352142` in WindowMetrics (Proton never shows ordinary windows), so
  every frame metric was garbage. Setting the three heights to Wine's defaults fixed the
  diagnostic; the launcher does not use AdjustWindowRect any more but measures the frame of
  the created window, and refuses a frame larger than 200 pixels. A Windows machine has sane
  metrics and is not affected.
- "Rescan controllers" found no controller plugged in after the start. Under Wine, SDL3's
  Windows gamepad backend depends on Wine delivering device-change notifications and its HID
  bus; this is to be re-checked on real Windows before any code change (noted, not fixed).

## Native Windows: how to test (for the user or any tester)

1. Unpack `dist/SacredFire-windows-x86_64.zip` on Windows 10 1903+ with a Vulkan 1.3 driver.
2. `bin\sacredfire-game.exe --vulkan-only` prints the GPU name and `command submission +
   4096-byte readback PASS`.
3. The probes: `abi_probe.exe`, `tls_probe.exe`, `memory_probe.exe`, `file_probe.exe` (from
   `scripts/build-windows.sh --tests`, out/windows/tests) each print a `PASS` line.
4. `SacredFire.exe`: the window; choose the folder, Play. Or from cmd, `SacredFire.exe --play
   --game <folder>` with `BB_TIMEOUT=180`: the log must reach `Entering original
   x86-64 code`, `Guest TCB loads redirected ... (matches the preparation step)`, the window, the
   licence screens, the title screen. Then a race, return to the menus, a save
   (`%LOCALAPPDATA%\SacredFire\user\savedata`), exit, start again, continue from the save.
5. Send the run's log from `%LOCALAPPDATA%\SacredFire\logs` ("Open logs folder"), GPU and
   driver version.

## Limitations recorded

- Not ported (reported as unavailable in the log): BB_UFFD, BB_PC_MODEL/dma-buf chunks,
  BB_FREE_CHECK/BB_LABEL_TRAP/BB_HEAP_HOOKS/BB_HEAP_SITES (single-step traps), BB_SAMPLE_THREAD,
  BB_DUMP_GOT, MangoHud, PGO, mod folder overlays, the GTK launcher's Steam shortcut writer.
- Paths longer than 260 characters need Windows' long-path policy; the manifest declares the
  capability.
- The archive's byte-for-byte reproducibility was not tested (two independent builds compared);
  the zip uses fixed timestamps so only content differences change its hash.
