# UFC 3 native PC port — complete handoff for another agent (2026-10-14)

Read this whole file before touching anything. Companion files in this folder:
`HANDOFF.md` (short version) and `ufc3-changes.diff` (full `git diff` of every tracked change,
1900 lines; untracked new files are listed in §4).

---

## 1. Goal and current result

- Goal: run **EA SPORTS UFC 3** for PS4 (EU, title **CUSA06534**, update **1.14**) as a **native
  x86-64 Linux executable** (no CPU emulation), bbport/SacredFire style: the game's own code runs
  on the CPU, a small runtime replaces PS4 system libraries, a shadPS4-derived renderer
  translates PS4 GPU work to Vulkan.
- **Result so far: fully playable** — boot, menus, fights, sound (MP3 + ATRAC9), DualShock/SDL
  pad, saves. **Menus 120 FPS, fights ~90 FPS** on the user's PC. Works without gdb.
- Machine: Arch/Omarchy (Hyprland, Wayland), **NVIDIA RTX 4070**, driver 610.57, Vulkan 1.4.341,
  12 threads, 15 GB RAM. bbport/SacredFire were only tested on **AMD**; most bugs below are
  NVIDIA-only.
- Open problems: **(A) noisy / blocky octagon floor** (main, §6), (B) fights at ~90 not 120 FPS,
  (C) ~5 min first-run tutorial videos cannot be skipped, (D) nothing is committed yet.

## 2. Layout

| Path | What |
|---|---|
| `~/Projects/ufc3-native` | the port: copy of github.com/T3CHNOLOG1C/SacredFire (itself built on bbport, GPL-2.0+), git branch `ufc3`, remote renamed `sacredfire` |
| `~/Projects/ufc3-native/orig/CUSA06534` | game folder: base game hard-linked from `~/Games/PS4/CUSA06534` with `~/Games/PS4/CUSA06534-patch` (1.14) copied over (`cp -r --remove-destination`). Git-ignored. |
| `~/Projects/refs/bbport`, `~/Projects/refs/SacredFire` | pristine references (Yharnam-Hunters/bbport, T3CHNOLOG1C/SacredFire) |
| `~/Projects/ufc3-port` | earlier shadPS4 v0.19.0 fork (branch `ufc3`) used for first debugging; superseded |
| `out/` | build products, logs `out/run*.log`, captures `out/capture/`, `out/skip.txt` |
| `user/` | runtime user data: saves, shader cache `user/cache/CUSA06534`, shader dumps `user/shader/dumps`, shader patches `user/shader/patch` |
| `docs/ufc3/` | these notes |

Build: `bash build.sh` (C runtime `out/sacredfire` + `out/gpu/libbbgpu.so`, Ninja, ~5–10 min
first time, incremental ~1 min). **Never build while the game runs** (it replaces the mapped .so).
Run: `bash run.sh` (prepares the image, links modules, compiles shader patches, starts the game).
Dependencies installed by the user: `boost robin-map miniz zydis spirv-cross vulkan-headers`
(pacman) + `tools/deps.sh` (magic_enum, VMA, xbyak).

## 3. How the port works (pipeline)

1. `scripts/game_check.py` checks the eboot image hash → profile name `ufc3`.
   Supported image sha256 (1.14): `46c2477702ae5d3fbff3a1911f10841aa150d20941d515ffb3edec75153f055b`;
   base 1.00: `1dc99716a77f5411834d004d6becc3c829e382a244aa0dcbd63932a7eeee0bf1` (rejected as old).
2. `scripts/prepare.py` → flat image `out/boot.bin` (675 imports, 206k relocations, entry 0x1720).
3. `scripts/link_modules.py --modules libc.prx libSceFios2.prx` → `out/boot-linked.bin`
   (136 native bindings, 627 imports left to the runtime). (CTR also linked libSceNpToolkit2; UFC
   3 doesn't ship it.)
4. `tools/gen_import_names.py` (shadPS4 aerolib → `out/import_names.txt`),
   `scripts/content_profile.py`.
5. `out/sacredfire out/boot-linked.bin ...` maps the image at 0x800000000 (guest offsets in logs
   are relative to that; the old shadPS4 dump used base 0x400000, so guest offset X = shadPS4 addr
   X+0x400000) and jumps to the entry. Unresolved imports STOP with
   `STOP: first unsupported PS4 import: NID#lib` → implement in `src/runtime_*.c` and repeat.
   `out/sacredfire out/boot-linked.bin --list-imports --import-names out/import_names.txt` lists
   bindings without running.

## 4. Every change (by file)

Tracked changes are in `ufc3-changes.diff`. Summary:

### Game profile / scripts
- `scripts/game_check.py`: SUPPORTED/OLD_IMAGES/KNOWN tables replaced with the UFC 3 entries
  above (title CUSA06534, version 01.14, modules libc+Fios2, patches False).
- `run.sh`: default game `orig/CUSA06534`; profile default `ufc3`; modules without NpToolkit2;
  patches.py `--app-version 01.14 --title-id CUSA06534`; shader cache dir `CUSA06534`; a `ufc3`
  block right after `echo "Game profile"` exporting defaults:
  - `BB_READBACKS=2` (precise) + `BB_READBACK_LINEAR=1` — without them the fight scene is black
    (same bug as shadPS4 issue #4375 for UFC 4, same engine).
  - `BB_GPU_WRITE_TWINS=0` — SacredFire turns "GPU write twins" on later in run.sh; with precise
    readbacks it deadlocked (RenderThread ReadMemory ↔ GPU thread DownloadMemory on twins_mutex).
  - `BB_FLIP_RATE=0 BB_VBLANK_HZ=120` — 60–120 FPS (see §5.6).
  - compiles `patches/shaders/*.comp` with glslangValidator into `user/shader/patch/*.spv`.
- `patches/CrashTeamRacing.xml` renamed to `.CrashTeamRacing.xml.disabled` (CTR patches off).
- `bbport.ini`: `show_fps=1` (overlay FPS counter).

### C runtime (`src/`)
- `runtime_thread.c`: `scePthreadAttrGetstack/Getstackaddr/Setstack` (address ignored, size kept),
  `scePthreadGetschedparam/Setschedparam` (sce error codes), `scePthreadSetcancelstate`.
- `runtime_mutex.c`: `pthread_mutexattr_setpshared` (posix, accepted).
- `runtime_content.c`: `sceAppContentTemporaryDataMount2` (→ "/temp0"), `...Format`,
  `...GetAvailableSpaceKb` (1 GiB), `sceAppContentAddcontMount` (NOT_FOUND 0x80D90005),
  `AddcontUnmount`.
- `runtime_savedata.c`: `sceSaveDataGetMountInfo` (32768 blocks, 32000 free),
  `UmountWithBackup` (= umount), `LoadIcon/GetEventResult/TransferringMount/CheckBackupData/
  RestoreBackupData` → NOT_FOUND.
- `runtime_services.c`: offline stubs: NpWebApi (CreateContextA/CreateHandle/SendRequest2/
  ExtdPushEventFilter…), NpManager (CreateRequest, GetAccountCountryA, Register/Unregister
  StateCallbackA), NpAuthAbortRequest, NpTrophyGetGroupInfo, ProfileDialogOpenA,
  GameLiveStreamingLaunchLiveViewerA, Coredump (3), Net (GetMacAddress, GetSockInfo,
  Getpeername, ResolverGetError), NetCtlGetResult, SaveDataDialog GetResult/GetStatus,
  CommonDialogIsUsed, Msg/Commerce/Invitation dialog Close, Https/Http setters, Voice (5),
  Audio3d (returns 0x80EA0009 so the game keeps stereo), VideoRecordingSetInfo.
- `runtime_file.c`: `sceKernelFcntl` (F_GETFL → O_RDWR), `sceKernelChmod`, `sceKernelUtimes`
  (validate path only).
- `runtime_kernel.c`: `sceKernelLoadStartModule` (ENOENT; modules are linked offline),
  `StopUnloadModule`, `msync`, `sceKernelGetPageTableStats`, `sigreturn`, posix `socket/sendto/
  recvfrom/setsockopt` (−1).
- `runtime_ajm.c`: **MP3 decoding** (UFC 3 music/speech) with **minimp3** (third_party/minimp3,
  CC0, new): per-instance decoder allocated on demand, PARTIAL_INPUT/NOT_ENOUGH_ROOM semantics,
  gapless skip/total, stream/format/codec-info sidebands (Mp3 codec info 16 bytes), S16/S32/F32
  output, interleaved or not. ATRAC9 path unchanged.
- `probe.c`: (a) `BB_ALLOW_PTRACE=1` → `prctl(PR_SET_PTRACER, ANY)` so `gdb -p` can attach later
  (Yama ptrace_scope=1 on this box); (b) **`gpu_fault` returns 0 when `runtime_fault_recover` is
  set** — a speculative guest read (draw-prep worker walking guest tables under the pipeline
  cache shared lock) must go to its recovery point, not to a GPU readback (that deadlocked fights:
  DrawPrep holds `programs_mutex` shared → page fault → ReadMemory waits GPU thread → GPU thread
  wants `programs_mutex` exclusive).

### GPU library (`gpu/`)
- `shadps4/core/libraries/kernel/equeue.cpp`:
  - `kqueues` (unordered_map) guarded by `kqueues_mutex` via `QueueContains/Get/Put/Take`
    (EAThread creates timer queues from many threads; a rehash under a lookup crashed in
    `sceKernelAddHRTimerEvent`).
  - `WaitForSmallTimer` also delivers regular (user) events; if all small timers vanish it falls
    back to a cond wait (predicate also wakes when a small timer appears); `AddSmallTimer`
    `notify_all`s.
  - `WaitForEvents` rewritten: poll (`timo==0`) also returns expired small timers; the regular
    wait wakes when a small timer is added and loops into the spin; deadline honoured.
  (Root cause of an intermittent black screen without gdb: EA's RenderThread slept on
  "EAThread Timer Queue" while other threads added small HR timers.)
- `shim/bbgpu.cpp`: kernel service thread runs `io_context` with a `work_guard` + stop_callback
  (old run()/restart()/counter-reset loop lost wakeups → HR timers ≥1.2 ms never fired → black
  screen without gdb).
- `shadps4/core/libraries/videoout/video_out.cpp`: `sceVideoOutSetFlipRate` logs the rate and
  caller; `BB_FLIP_RATE` overrides it. EOP flip IRQ: when the flip queue is full (>16 pending)
  wait up to 500 ms instead of `ASSERT` (crash "EOP flip submission failed").
- `shadps4/video_core/renderer_vulkan/vk_instance.cpp`: D16_UNORM_S8 fallback prefers
  **D32_SFLOAT_S8** before D24_UNORM_S8 (as on AMD).
- `shadps4/video_core/texture_cache/image_view.cpp`: a depth-stencil image viewed through a
  non depth/stencil format keeps **only the depth aspect** (view with DEPTH|STENCIL + R16_UINT
  hung NVIDIA: Xid 109 CTX SWITCH TIMEOUT in compute shader `aae9f6c1`).
- `vk_rasterizer.cpp`:
  - `BindTextures`: a depth image bound with an **R16_UINT** view is converted each bind into an
    R16_UINT copy (`round(depth*65535)`) by host shader `host_shaders/depth_to_r16ui.comp`
    (new; registered in gpu/CMakeLists.txt), VMA image, push descriptors, `RecordCrumb`. Did not
    change the floor (only a 1024² image triggers it).
  - `SkipShader()` diagnostics (§7), `BB_FORCE_REFRESH` (§7).
- `vk_pipeline_cache.cpp`: when a shader patch is loaded, the **patched** SPIR-V is registered in
  the on-disk cache (before, cached runs silently used the original).
- `shim/core/emulator_settings.h`: `IsPatchShaders()` = env `BB_PATCH_SHADERS` (default true).
- `patches/shaders/cs_0x00000000aae9f6c1_0.comp.disabled`: an earlier per-shader patch (depth
  read as float → uint16 bits), disabled because the general R16 conversion replaced it.
- `vk_semaphore.cpp`: `WAIT_TIMEOUT` 2 s with "GPU wait stuck: tick, next tick, GPU completed"
  report + breadcrumbs report (**temporary diagnostic**, keep or revert).
- `vk_breadcrumbs.cpp`: **timestamp-query breadcrumbs** for GPUs without VK_AMD_buffer_marker
  (NVIDIA): ring of 65536 queries per stream, `LastMarker()` finds the first unwritten/older
  timestamp. Names the stuck draw/dispatch and dumps its guest shader code.
- `buffer_cache.cpp`: printf per sparse arena bind (**temporary**, remove), `BB_UFC3_BIND_IDLE`
  experiment.
- `vk_frame_capture.cpp`: `BB_CAPTURE_ON_FLIP=1` (frame boundary at flips; UFC 3 never renders
  into a registered display buffer), 64 shaders per pass, **one line per draw** with vs/ps and
  sampled textures.
- `bbport_write_log.cpp`: fault dump also r12/r13, writes holding a register value, clobber-ring
  writes into [r14+0x18].
- `bbport_overlay.cpp`: FPS counter top-left.
- `lower_wave64_pass.cpp`: warning includes the shader hash.

### New files (untracked)
`docs/ufc3/*`, `gpu/shadps4/video_core/host_shaders/depth_to_r16ui.comp`, `patches/shaders/`,
`third_party/minimp3/{minimp3.h,LICENSE}`, `tools/ufc3-iter.sh` (build+run+print STOP),
`tools/ufc3-shot.sh` (grim screenshot cropped to the game window found by PID via hyprctl —
never capture whole monitors, the user had a credentials incident), `out/gdbwrap.sh`
(`BB_PROBE=out/gdbwrap.sh` runs the game under gdb; SIGINT then dumps `thread apply all bt 60`).

## 5. Bugs found and fixed (chronological, with root causes)

1. Missing imports — §4 runtime.
2. MP3 not implemented in AJM → minimp3.
3. Crash in `sceKernelAddHRTimerEvent` (null) → kqueues race (mutex). First attempt had a
   recursive `QueueContains` (deadlock) — fixed.
4. GPU hang Xid 109 (NVIDIA), loading froze 20–42 %: found with timestamp breadcrumbs →
   `cs aae9f6c1` (32×32 groups; tiled light-culling: reads Z16 depth as R16_UINT, min/max per
   32×32 tile into shared mem). Validation layer: `VUID-VkDescriptorImageInfo-imageView-01976`
   (view with both aspects). Fix: single depth aspect (+ D32S8 fallback order).
5. Fight scene black → precise readbacks + linear image readbacks (env defaults).
6. Loading deadlock with precise readbacks → twins off.
7. Fight froze after ~1 s → DrawPrep speculative read fault → recovery instead of readback.
8. Black screen only without gdb (timing): two kernel-event races (asio lost wakeup; small HR
   timers added while a thread sleeps in the regular wait).
9. Crash "EOP flip submission failed" (flip queue full) → wait instead of assert.
6'. **Frame rate**: the game calls `sceVideoOutSetFlipRate(1)` (30 Hz) from guest `0x1182a0`
   (`SetPresentInterval(interval)`: clamps 0..3, calls SetFlipRate(interval−1); interval comes
   from a virtual call at guest 0x117690). The simulation follows real frame time, so
   `BB_FLIP_RATE=0` + `BB_VBLANK_HZ=120` gives 60–120 FPS at normal speed. No community UFC 3
   patch exists (GoldHEN_Patch_Repository / illusionyy PS-Game-Patch have none).

## 6. OPEN BUG A — octagon floor: speckle noise + gray blocks

Symptom: the white canvas shows dark salt-and-pepper grain everywhere and blocky gray/dark
rectangles (worse near fighters / along the octagon line). User screenshots confirm.
Present from the first time the fight rendered.

Eliminated (A/B by skipping shaders, user-confirmed or by our own screenshots):
- SSAO chain (cs f568b64f, aa6b8f47, 900eb8e9, 8d0fcf29): no change.
- tiled light culling cs `aae9f6c1`: noise still there (less visible: lighting changed).
- forward pass #4 blended decals (ps 20c9b034, d427f82a, f9901e57, cff456ed, ea26276b): no change.
- wave64 non-uniform ballot warning (cs 0x1552f378): never runs in fights.
- depth-as-R16_UINT conversion: no change.
- `BB_FORCE_REFRESH=0x1078616000` (re-upload the suspect texture from guest memory every bind):
  **no change** → not a missed CPU write; points at how the data is produced/detiled/read.

Located:
- Frame (capture `out/capture/frame_1791947138.txt`): pass #0 depth prepass (ps a0caf33d), pass
  #1 main forward pass into B10G11R11 1600×900 + D32S8 (game renders at 900p).
- **Canvas print tiles = pixel shader `86b28487`** (vertex `478d9514`, quads of 6 indices, ~7
  draws). Skipping it leaves black squares where the printed tiles are; skipping the group
  {23960f1c, 5e2a265e, 407844e1, 86b28487, 4ab93ad4, bbe62d37, 88e87624} removes logos and the
  noise; 23960f1c = cage pads; 5e2a265e and 407844e1 alone don't change the floor.
  (Not yet tested alone: 4ab93ad4, bbe62d37, 88e87624 — do it to be sure 86b28487 is the noise.)
- Its textures (per-draw capture line): Bc5Snorm 1024², Bc1Unorm 1024², Bc5Snorm 256²,
  **R32G32Uint 512×512 @ 0x1078616000**, D16UnormS8 1024² @ 0x1079620000 (shadow map, host
  D32S8), Bc3Srgb 1024×512, Bc1Srgb 2048², …
- Decompiled GLSL (dump `user/shader/dumps/fs_0x0000000086b28487_0.spv`, spirv-cross):
  `fs_img3` = **utexture2DArray** (binding 13) read with `texelFetch(fs_img3, ivec3(x, y,
  ssbo_5.data[k]), 0)` at 4 neighbours, `.x` converted to float and blended with hand-computed
  bilinear weights (manual filtering of integer data); also a `samplerShadow` (binding 20).
- Texture cache info for 0x1078616000: **512×512, 6 layers, 1 level, R32G32Uint,
  tile_mode 13 = Thin1DThin, pitch 512, guest_size 0xC00000, flags 0x47**. 6 layers suggests a
  cube/omni map (e.g. per-light shadow or dirt map). In the captured frame **no pass writes it**.

Hypotheses to test next (in order):
1. **Thin1DThin detiling of a 64-bpp (R32G32) 6-layer array is wrong** → blocky scrambling.
   Compare with shadPS4 upstream tiling (`video_core/texture_cache/tile*.cpp`,
   `host_shaders/tiling.comp`) and SacredFire's backport of shadPS4 #5196 (small mips / micro
   tile / 128-bit pixel order). Test: force this image's tiling to linear, or detile on the CPU
   and compare with a GPU-detiled readback; check the `ssbo_5.data[k]` layer index range (0..5).
2. Who writes it: maybe a pass in an earlier frame (fight start) or a compute/DMA path not
   captured; run the capture at fight start, or `BB_IMAGE_SYNC_LOG=1` with its print caps (200 /
   100, texture_cache.cpp lines ~249 and ~913) raised or filtered to this address.
3. `texelFetch` on a utexture2DArray whose host image is a different format/aliased image (e.g.
   written as R32G32Float or as depth) — check overlap resolution for 0x1078616000.
4. The samplerShadow on the D16S8→D32S8 shadow map (compare reference bias/precision) — less
   likely (would not give blocks).

## 7. Diagnostics cheat sheet

| Env / tool | Effect |
|---|---|
| `BB_SKIP_SHADERS=h1,h2` | draws of those vs/ps recorded with 0 instances, dispatches with 0 groups |
| `BB_SKIP_FILE=out/skip.txt` | same list re-read every 0.5 s — change while in a fight |
| `tools/ufc3-shot.sh out.png` | screenshot of the game window only |
| `BB_CAPTURE_ON_FLIP=1 BB_CAPTURE_TRIGGER=$PWD/out/capture.trigger BB_CAPTURE_DIR=$PWD/out/capture` | `touch out/capture.trigger` → next frame written as text (passes, targets, per-draw shaders/textures) |
| `BB_FORCE_REFRESH=<hex addr>` | describe that image once + re-upload it every bind |
| `BB_DUMP_SHADERS=1 BB_CLEAR_SHADER_CACHE=1` | shader dumps (SPIR-V, IR, ASL) into `user/shader/dumps` |
| `BB_GPU_LOG=info` | GPU library info logs (pipeline compiles, "Loaded patch") |
| `BB_VK_VALIDATION=1` | Vulkan validation layer (installed) |
| `BB_ALLOW_PTRACE=1` then `gdb -p $(pgrep -x sacredfire) -batch -ex 'thread apply all bt 25'` | stacks of a hang that does not happen under gdb |
| `kill -ALRM <pid>` | runtime watchdog stack dump (unsymbolised) |
| `BB_FRAME_STATS=1` | per-5s frame stats (FPS, waits, draw pipe) |
| `BB_IMAGE_SYNC_LOG=1` | texture refresh log (capped) |
| `BB_READBACKS=0/1/2`, `BB_FLIP_RATE`, `BB_VBLANK_HZ`, `BB_GPU_WRITE_TWINS` | see §4 |
| `journalctl -k | grep NVRM` | Xid errors (109 = GPU hang) |

Shader hashes in frame captures/logs are guest program hashes (`pgm_hash`); dumps are named
`fs_0x<16 hex>_<perm>.spv` / `cs_…`.

## 8. Other open items

- **Performance (B)**: fights 84–93 FPS with vblank 120. Under gdb we measured GPU thread idle
  ~33 %, ReadMemory waits ~9 % (precise readbacks), ~2500 write faults/s. Next: profile without
  gdb (`BB_FRAME_STATS=1`), try `BB_READBACKS=1` with a better targeted fix for the black fight
  scene, PGO (`BB_PGO`).
- **Tutorial videos (C)**: EA MoviePlayer2 (`tutorialMovieEvents.lreq`), inside `.big`
  archives (EA "EB" v3 format, hashed names). Back/Tab/Options don't skip. Options: let the save
  record them as watched (finish a fight), or patch the condition in the eboot.
- **Commit (D)**: ask the user before committing branch `ufc3`.
- Temporary code to clean up later: arena-bind printf (buffer_cache.cpp), semaphore 2 s
  timeout/report (keep as opt-in?), `BB_UFC3_BIND_IDLE`, wave64 hash in warning (harmless).

## 9. User preferences / context

- Spanish (Chile); plays the game and reports what they see; wants a real native port, using the
  other ports (bbport/SacredFire) as the base.
- Wants token-efficient sessions and short replies.
- Jarvis voice disabled (`~/Work/jarvis-voice/DISABLED`).
- Never screenshot whole monitors; crop to the game window.

## 10. Performance research (2026-10-14, later)

- GoW III Remastered port found: github.com/FableCraze/GoW3R_Windows_Port (Windows, derived from
  bloodborne_pc_windows_port + shadPS4 b4e7ae7; cloned to ~/Projects/refs/GoW3R_Windows_Port). It
  targets 60 Hz and uses `readbacks=2 readback_linear=1` like us. Its gpu/VENDOR.txt documents
  tiling fixes from **cuesta4/shadPS4 commit 83352ea** ("complete tiling-region mapping: each mip
  uses its real aligned guest offset, tile-shaped dispatch, coordinated macro-to-micro tail
  addressing") and **staging host-visible texture payloads to device-local memory before the
  detiling compute pass** (timing-dependent corruption otherwise) — strong lead for the floor bug.
- The high-FPS build is **shadPS4-eltutz (github.com/cuesta4/shadPS4)**: GoW3 ~100 → locked 120
  FPS (with 120 FPS patch). Techniques: much less per-draw command-processing work, helper
  threads for copies/recording, non-blocking GPU waits (fences, readbacks, aliased images),
  sequential-write detection in memory tracking (Bloodborne 67 → 80 FPS), precise barriers,
  tile-only detiling, GPU timing profiler (SHADPS4_GPU_TIMING=1).
- Our fight stats (no gdb, run45): 84–90 FPS, 650–750 draws/frame, GPU thread 9.3–9.4 µs/draw,
  idle 12–17 %, blocked on **GPU ticks 19.4 %** (precise readbacks), host copies ~9 %, stage A
  drain waits 26 % (ReadMemory 7 %, ProcessCommands 12 %, OnSubmit 7 %), 2400–3300 write
  faults/s. Experiment running: `BB_READBACKS=1` (relaxed) + `BB_READBACK_LINEAR=1`.
- BB_WAIT_TRACE=1 (run61): GPU tick waits 17.5 %, of which **14.5 % = 446 waits/5 s in
  GpuComm → ReadMemory → DownloadMemory → Scheduler::Wait** (a full Finish per CPU access to a
  GPU-written page, ~1/frame, ~1.6 ms each). Relaxed readbacks were worse (75 FPS, ticks 35 %).
  GPU utilization only 36 % → latency-bound, not GPU-bound.
- Twins deadlock root cause: twins code (ReadMemory snapshot, ForEachTwinRun compare in
  SynchronizeMemory) read guest bytes directly while holding twins_mutex; with precise
  readbacks those pages are read-protected → fault → ReadMemory → DownloadMemory → write_back
  locks twins_mutex again on the same thread. Fix: both reads use runtime_memory_read_backing
  (buffer_cache.cpp). Test: BB_GPU_WRITE_TWINS=1 (run62).
- Twins + precise readbacks are incompatible by design: run62 crashed at start with
  `page_manager.cpp:634 Protect: Attempted to protect region as write-only` (twins wants a
  read-protected page writable; x86 has no write-only). Twins stays off (BB_GPU_WRITE_TWINS=0);
  the backing-read change is harmless and kept.
- **BB_READBACK_LOG** (run63, `out/readbacks-run63.csv`, 46 s of fight): **all 4742 readbacks are
  RenderThread *writes* into the 512 KB window 0x1001380000** (6.75 s of waiting in 46 s), each
  waiting for **tiny GPU writes (4–32 bytes) at 0x10013fdxxx** that share pages with memory the
  CPU rewrites every frame (near shader code 0x10013f1c00 → a per-frame heap/ring). This is the
  ~15 % GPU-tick wait and the main FPS limiter (GPU only 36 % busy).
  Ideas: (1) find the writer of 0x10013fdxxx (compute/EOP/RELEASE_MEM? `BB_TRACE_SHADER`,
  frame capture "W:" targets, BbClobber ring) and route that write through the host (CPU) instead
  of marking the page GPU-modified; (2) a per-range policy: pages whose GPU-written bytes are
  tiny and that the CPU only writes → don't read-protect them (relaxed for that range) and merge
  the GPU bytes on upload (what twins does, but without write-only protection); (3) port
  eltutz's non-blocking readbacks (cuesta4/shadPS4) — download asynchronously and only block on
  CPU *reads*.
- Also fixed after these runs: "Out of order flip IRQ" ASSERT → warning (video_out.cpp).
- **Fix (as eltutz / shadPS4 upstream): write-fault downloads only the touched pages** instead of
  bbport's 512 KB window (buffer_cache.cpp ReadMemory; `BB_READBACK_WRITE_WINDOW=1` restores it).
  Fights 85–90 → **~105 FPS** (run64: GPU thread 7.0 µs/draw, 882 draws/frame, idle 3.6 %, GPU
  ticks still 21.6 % = ~105 ReadMemory waits/s, ~1/frame). eltutz's ReadMemory is also blocking
  (async downloads only on buffer deletion), so the remaining wait needs its own cause: next run
  `BB_READBACK_LOG=out/readbacks.csv` (run65) to see address/is_write of what is left.
- Floor bug unchanged by these perf changes.
- run65 readback log (`out/readbacks.csv`, 95 s of fight, after the page-limited write fix):
  | wait | count | requester | address | kind |
  |---|---|---|---|---|
  | 5.5 s | 3256 | **shadPS4:GpuComm** | window 0x1000500000 (512 KB) | **read**, waits for GPU writes 0x1000569800+256, 0x100056b900+256, 0x1000548800+1536 |
  | 1.7 s | 3070 | RenderThread | page 0x10013fd000 | write, GPU wrote 4 B at 0x10013fd160 in the same page (true false sharing) |
  | ~0.9 s | many | RenderThread / General Job 0-2 | 0x1001338000, 0x101e791000, 0x106bfaa000, 0x10007b2000-3000 | writes |
  Main remaining cost: the **GPU thread itself reads guest memory that a GPU pass just wrote**
  (looks like GPU-written resource tables / indirect args read by the command processor or the
  SRT walker on the CPU). Next: find which shader writes 0x1000548800/0x1000569800 (frame capture
  "W:" lines, or `BB_TRACE_SHADER=<hash>` buffer report) and which CPU code reads them (gdb break
  in ReadMemory with requester GpuComm), then read that data from the GPU-side arena copy or keep it
  host-written. The same-page 4-byte case (0x10013fd160) could use a byte-granular GPU-modified
  check before waiting (wait only if the CPU write overlaps GPU-written bytes, else write through
  the backing and mark CPU-modified).

## 11. Floor bug — more stable, NOT fixed (2026-10-14)

- Cause: Z16+S8 depth images (e.g. the 1024² shadow map 0x1079620000 sampled by the canvas shader
  with a samplerShadow) were host D32_SFLOAT_S8; `TextureCache::RefreshImage` uploads guest bytes
  straight into the depth aspect (BufferImageCopy), so 16-bit Z16 texels were read as 32-bit
  floats → garbage shadow depths → speckle noise and blocks on the floor.
- Fix (default in run.sh for ufc3): `BB_Z16S8_AS_D16=1` → vk_instance.cpp GetSupportedFormat maps
  D16_UNORM_S8 to **D16_UNORM** (stencil dropped); image.cpp drops the stencil aspect when the host
  format has none. User: floor more complete and stable, but black tiling blocks still appear (bigger). Not fixed.
- Proper long-term fix: keep stencil and convert Z16 → float32 on upload (compute or CPU), or
  only drop stencil for images that are never bound as stencil targets.
- **Remaining floor artifact**: small gray squares on a regular grid that grow during the fight =
  the dynamic canvas dirt/sweat map: `fs_img3` utexture2DArray R32G32Uint 512×512×6
  (0x1078616000, tile_mode 13 Thin1DThin) read by ps 86b28487 with manual 4-tap bilinear. It
  renders blocky instead of smooth smudges → check its writer (dirt accumulates on contact) and
  the manual filtering inputs (layer index `ssbo_5.data[k]`, texel coordinates), and Thin1DThin
  64-bpp detiling of the array.
- gdb sampling of BufferCache::ReadMemory during a fight (out/rm-bt.txt): ~2266 from guest CPU
  writes to GPU-modified pages (InvalidateMemory path), **298 from ParseFetchShader on the GPU
  thread** (pipeline cache GetProgram, every draw: the fetch-shader cache revalidated with a
  direct memcmp on guest code that UFC 3 rewrites through the GPU in 256-byte copies → fault →
  full readback wait), 68 from a 7-byte memcmp. Fix: fetch_shader.cpp revalidates the cached
  parse against `runtime_memory_read_backing` (mismatch → normal decode). Test run68.
- run68 (fetch-shader fix): fights 86–114 FPS depending on the scene; the GPU thread's own fault
  waits are gone. Remaining GPU-tick wait ≈ 20 %: **one readback per frame queued by guest threads
  (RenderThread, General Job 0–2) writing pages that hold a few GPU-written bytes** (e.g. 4 B at
  0x10013fd160, likely a label/fence the game polls). Options: (1) byte-granular GPU-modified
  check — skip the download when the CPU write does not overlap GPU-written bytes (risk: stale
  label reads); (2) cut the GPU thread's per-draw cost (7.6–9 µs/draw, idle ~19–25 %), which is
  how eltutz took GoW3 from ~100 to 120 FPS (rewritten command processing / draw setup / buffer &
  texture lookups).
- **Option 1 implemented (BB_WRITE_SKIP_NO_OVERLAP, default on)**: buffer_cache.cpp ReadMemory —
  a CPU write fault that does not overlap any GPU-written byte skips the download and its GPU
  finish; the page's GPU-written ranges go into `keep_gpu_bytes`, and SynchronizeMemory excludes
  kept-and-still-GPU-modified bytes from uploads (so the guest's stale copy never overwrites them
  in the arena). RangeSet::Empty() added. Risk: CPU reads of those GPU bytes on that page are no
  longer read-protected until the GPU writes the page again. `BB_WRITE_SKIP_NO_OVERLAP=0` disables.

## 12. Final state of this session (2026-10-14, end)

- **Option 1 result**: with the tracker unmark fix it ran 100–115 FPS but **broke rendering**
  (first: fighter as a black blob; with BB_WRITE_SKIP_MAX_BYTES=64: fighters' skin invisible, only
  shorts/gloves) and gave no stable FPS gain (90–100). → **BB_WRITE_SKIP_NO_OVERLAP is now default
  OFF** (code kept, experimental). Lesson: the GPU bytes on those pages matter to the game (skin
  data / fences); don't skip their downloads.
- Kept perf fixes (default on): page-limited write-fault downloads (vs bbport's 512 KB window) and
  fetch-shader cache revalidation through the backing view. Fights now **~90–114 FPS** depending on
  the scene (was 84–90), menus 120 (vblank cap).
- Remaining perf: ~20 % GPU-tick waits = ~1 readback/frame from guest writes into pages with real
  GPU data; GPU thread ~7.6–9 µs/draw, little idle. Next real lever = **per-draw CPU cost of the GPU
  thread** (eltutz rewrite: command processing, draw setup, buffer/texture lookups —
  ~/Projects/refs/eltutz) and the draw-pipe drains (stage A waits ~26 %).
- New observation (user): **menu/character-select images appear ~0.5 s late every time the same
  screen is revisited** (not only the first time) → our side: texture re-upload/detile or file
  reads each visit (check texture cache eviction/refresh on revisit with BB_IMAGE_SYNC_LOG, and
  runtime file stats). Shader compiles are 0 (cache hit), async pipelines off.
- Floor: more stable (BB_Z16S8_AS_D16) but **black tiling blocks remain** (dynamic dirt map
  R32G32Uint Thin1DThin, §11). Lower priority than FPS per the user.
- Flip robustness: queue-full waits ≤500 ms; out-of-order flip IRQ is a warning.
- Still **uncommitted** (branch `ufc3`).

## 13. Perf round 3 (perf profiling)

- perf of shadPS4:GpuComm (out/perf-gpucomm.data): top was pthread_rwlock_unlock 8.7 % from
  PostDraw → CopySparseMemory → runtime_memory_read_backing (table rwlock per constant copy with
  precise readbacks). Fix: src/runtime_memory.c per-thread BackingCache {generation,start,end,phys}
  validated by the existing seqlock `table_generation` → rwlock 8.7 % → 2.4 %.
- Whole-process perf (out/perf-all.data): game job threads (General Job 0-4, Replay Job 0-3,
  Sim Job) spin in EA's job-queue try-pop (guest 0x11dc3a0 / 0x11d66f0, + clock_gettime); renicing
  them (+10) gave no gain. No thread was saturated at 120 Hz vblank → **vblank quantization**:
  `BB_VBLANK_HZ=480 BB_FPS_LIMIT=120` (now ufc3 default, as SacredFire's CTR 60 FPS patch) →
  ~90–100 FPS in heavy scenes, speed correct.
- After that the GPU thread is busy again (idle 7–9 %, ~9 µs/draw, GPU ticks ~24 %). Second perf
  (out/perf-gpucomm2.data): Liverpool::Process 7.5 %, PostDraw 7.1 %, DrainDrawPipe 6.4 %,
  NotePendingRead 5.7 % + PendingWriteOverlaps 4.3 %, ProcessGraphics 5.7 %, CollectRingBindings
  ~6 %, FirstBadHeader 1.4 %. SacredFire issue-#2 diagnostics (FirstBadHeader packet walks +
  guest write ring/"Guest write check" prints) made opt-in: `BB_CLOBBER_CHECK=1` (run75 test).

---

## 14. Plan to reach 240 FPS (merged from PERF_240_PLAN.md) (2026-10-14)

Read `GROK_HANDOFF.md` first (full history, §10–§13 perf work). This file is the strategy for the
next agent. **Incremental 5–10 % shaving cannot reach 240 FPS; the architecture must change.**

### 14.1 Where we are

- Fights ~90–115 FPS (scene dependent, ~650–880 draws/frame), menus capped at 120.
- Defaults (run.sh, profile ufc3): `BB_READBACKS=2 BB_READBACK_LINEAR=1 BB_GPU_WRITE_TWINS=0
  BB_FLIP_RATE=0 BB_VBLANK_HZ=480 BB_FPS_LIMIT=120 BB_Z16S8_AS_D16=1`. For 240: raise
  `BB_FPS_LIMIT` (0 = none) and use a ≥240 Hz path.
- Untested last change (run75): SacredFire issue-#2 diagnostics off by default
  (`BB_CLOBBER_CHECK=1` re-enables: FirstBadHeader walks, guest write ring/prints). Verify it does
  not hang before keeping it.
- GPU (RTX 4070) only ~34–36 % busy → **CPU/latency bound**, not GPU bound.

### 14.2 The hard ceiling (why 10 % steps fail)

240 FPS = **4.17 ms per frame**. Today, per frame:

| cost | now | notes |
|---|---|---|
| GPU command thread (shadPS4:GpuComm, stage A) | ~700 draws × 7–9 µs ≈ **5–6.3 ms** | single thread decodes PM4 + prepares every draw |
| GPU-tick waits (readbacks) | ~20–25 % of that thread | ≈1 full GPU finish per frame (guest writes into pages holding GPU data) |
| stage A waiting for stage B (DrawPipe drains) | ~6 % | |
| game CPU (RenderThread/SimThread) | not saturated | job threads spin (EA job queue) but not the limiter |

Even with zero waits, ~5 ms of serial per-draw work caps at ~160–200 FPS. **240 needs per-frame
GPU-thread work ≤ ~3 ms with no blocking waits**: ≤ ~4 µs/draw at 700 draws, or the work split
across threads.

### 14.3 Structural levers (in order of expected payoff)

1. **Remove readback waits structurally — bbport's "PC memory model"** (`BB_PC_MODEL=1` /
   `BB_GUEST_IN_PLACE=1`): game memory lives in GPU-visible system RAM, the GPU reads/writes it in
   place, no VRAM copies, no write tracking, **no readbacks** (WRITE_DATA on GPU via
   `WriteDataOnGpu`, `BB_GPU_COMMAND_WRITES`). bbport README: "does not work properly on NVIDIA
   (dma-buf maps at offset 0 only; falls back)"; run.sh: `BB_PC_MODEL_ANY_GPU=1` forces it (NVIDIA
   gets one chunk per direct-memory allocation). **First experiment for the next agent**:
   `BB_PC_MODEL=1 BB_PC_MODEL_ANY_GPU=1` on the 4070, then fix what breaks. This alone removes the
   ~20–25 % tick waits and the 2–4k write faults/s.
2. **Parallelise command processing** beyond the current 2 stages (decode on GpuComm, record on
   DrawRec): split per submission/command buffer (DCB/CCB) or per draw range; per-draw state is
   already "prepared" by DrawPrep workers (bbport `vk_draw_prep.cpp`) — extend so stage A only
   decodes and hands draws to N workers.
3. **DXVK-style translation instead of PM4 emulation** — bbport's own roadmap ("in the next patch
   it is reworked into DXVK-style translation: GnmDriver API calls translated directly into Vulkan,
   without decoding the GPU command stream"; `docs/EMULATION_REMOVAL_PLAN.ru.md` in SacredFire/bbport).
   UFC 3 builds its command buffers through libSceGnmDriver HLE (113 imports, all resolved), so the
   draws and state could be captured at the Gnm call level on the game thread and translated, skipping
   PM4 encode→decode entirely.
4. **Per-draw cost cuts (eltutz approach)**: shadPS4-eltutz (github.com/cuesta4/shadPS4, cloned at
   `~/Projects/refs/eltutz`) took GoW3 from ~100 to a locked 120 by rewriting command processing,
   draw setup and buffer/texture lookups. Port the measurable pieces: hottest functions now
   (perf, `out/perf-gpucomm2.data`): Liverpool::Process 7.5 %, PostDraw 7.1 %, DrainDrawPipe 6.4 %,
   NotePendingRead + PendingWriteOverlaps ~10 %, ProcessGraphics 5.7 %, CollectRingBindings ~6 %,
   ApplyGraphicsRegisterPacket + HashRegisterPacket ~4.4 %, StageSpecialization ctor/== ~4 %.
5. **Game-side spin**: EA job threads spin in try-pop (guest 0x11dc3a0, 0x11d66f0) + clock_gettime;
   not the limiter now, but at 240 FPS they will compete for the 12 host threads. A runtime patch
   (yield/backoff after N empty polls) — like eltutz did for Bloodborne's network thread.

### 14.4 Use REA (reverse-engineer-anything) on the GoW III Remastered ports (user request, priority)

Goal: understand **how the fast ports are structured**, not just their docs.

Targets:
- **GoW3R_Windows_Port** (github.com/FableCraze/GoW3R_Windows_Port, source at
  `~/Projects/refs/GoW3R_Windows_Port`): derived from bloodborne_pc_windows_port + shadPS4
  b4e7ae7. Has source → read `gpu/VENDOR.txt`, `docs/ARCHITECTURE.md`, then diff its
  `gpu/` against ours. Build or download its release `.exe`/`.dll` and use REA
  (`mcp__rea__binary_overview`, `list_procedures`, `procedure_pseudo_code`, `get_call_graph`,
  `compare_functions`) to see what is inlined/specialised on the hot path (Liverpool processing,
  draw preparation, buffer cache lookups) and compare with our `out/gpu/libbbgpu.so`
  (`mcp__rea__compare_artifacts`, `compare_functions` on the same symbols).
- **shadPS4-eltutz** release binary (Windows, from cuesta4/shadPS4 releases) — the 120 FPS GoW3
  build: REA over `AmdGpu::Liverpool::ProcessGraphics`, `Vulkan::Rasterizer::Draw`,
  `BufferCache::ObtainBuffer`, `TextureCache::FindImage` to locate their caching tricks; plus
  their `SHADPS4_GPU_TIMING=1` profiler.
- Also REA the **UFC 3 eboot** hot guest loops (job try-pop 0x11dc3a0) to design the spin patch.

Questions REA must answer: (a) how many threads process PM4 and how work is split; (b) whether
readbacks/aliases are non-blocking (eltutz README: "handles those without stopping the emulator
whenever it can"); (c) per-draw descriptor/pipeline caching strategy; (d) memory tracking (eltutz:
sequential-write detection, "handles data written in sequence in bulk" — Bloodborne 67→80 FPS).

### 14.5 Concrete next steps (ordered)

1. Verify run75 (diagnostics off) is stable; keep or revert `BB_CLOBBER_CHECK` default.
2. Try `BB_PC_MODEL=1 BB_PC_MODEL_ANY_GPU=1` (+ `BB_FPS_LIMIT=0`); record FPS, waits, breakage.
3. REA pass over GoW3R/eltutz binaries per §4; write findings into this file.
4. Pick the lever with the biggest projected gain (§3) and prototype it behind a `BB_*` switch.
5. Measure with `BB_FRAME_STATS=1`, `BB_WAIT_TRACE=1`, `perf record -t <GpuComm tid>`.

### 14.6 Measurement recipe

- Launch: `BB_FRAME_STATS=1 BB_ALLOW_PTRACE=1 bash run.sh` (no gdb; gdb slows write faults).
- In a fight: `perf record -F 2000 -g --call-graph=fp -t $(ps -L -o tid=,comm= -p $(pgrep -x
  sacredfire) | awk '/GpuComm/{print $1}') -- sleep 12`; whole process: `-p <pid>`.
- GPU busy: `nvidia-smi --query-gpu=utilization.gpu --format=csv`.
- Compare scenes with similar draws/frame (Frame stats line) — FPS varies 85–115 with scene.

- run75 verified: diagnostics off (BB_CLOBBER_CHECK default 0) is stable, fights 90–100 FPS.

## 14.R  GoW III port analysis (2026-10-14) — results

**REA status:** `mcp__rea__open_binary` on `~/Projects/refs/eltutz-bin/shadps4-eltutz.exe` opened, but
`binary_overview` timed out: the Hopper bridge was waiting on a loader/license dialog (`provider_timeout`).
To retry, finish Hopper setup in its window first. This turned out not to be needed: **the full
eltutz source is in git under tag `shadps4-eltutz-v3`** (`main` only holds the README).
`~/Projects/refs/eltutz` is now checked out at that tag (400 commits). Read the source and the
author's own profiling docs instead of decompiling. The docs are in Portuguese, extracted from
commit c2970ce/738ca98 into the session scratchpad. Recover them with
`git show c2970ce:fast-pgo.md`, `c2970ce:llvm-result.md`, `c2970ce:findings.md`, `c2970ce:fast.md`,
`738ca98:plano-readback-lazy.md`, `c2970ce:build-ultra.ps1`.

### Answers to the 4 questions
(a) **Threads:** one command processor (Liverpool::Process). Since 98105d5 there is a dedicated
   Vulkan **recording thread**: CommandRecorder copies args into 64 KiB chunks, which are replayed,
   then handed to a submit thread. Copy workers handle guest→GPU uploads. This is the same
   shape as our stage A/B; they did NOT parallelise PM4 decode.
(b) **Readbacks — the big one.** They run **Readbacks Mode = Disabled + Readback Linear Images = On**,
   with a "GPU authority" model (c2970ce, `src/video_core/gpu_authority_tracker.{h,cpp}`):
   - GPU-written data **stays on the GPU**;
   - guest pages are read-protected ("read watchers");
   - the download happens only when guest CPU code actually reads them.

   Signals (EOP/EOS labels + IRQ) are published when the CP processes them, NOT deferred to GPU
   completion, unless in-flight data has a CPU consumer.

   5e338fd: a WAIT_REG_MEM whose value will be written by an already-queued signal records a
   GPU barrier and **continues instead of blocking**. That alone saved **~4.6 ms/frame in GoW3**.
   It also removed the per-draw-count GPU kick (~27 submits/frame, no gain).

   52a2446: aliased images (images at the same address):
   - skip upload/detile when an alias copy overwrites them whole;
   - are placed in shared memory blocks;
   - are recycled by shape and destroyed off the CP.

   27961d8: read-protected ranges are copied inline on the CP, not on copy workers. This avoids
   the deadlock where a fault waits for a tick that waits for the copy, the same class we hit.
(c) **Per-draw:**
   - graphics-pipeline **fast path keyed on register generations**: if the generation is
     unchanged, reuse the last pipeline (516f18b: the key must include whether a DS attachment
     is needed);
   - `GetProgram` current-slot fast path;
   - dynamic state skipped by per-category generations in `WriteGraphicsRegisters`;
   - descriptor cache;
   - FindImage owner-only token + batched LRU;
   - compact sampler hit path;
   - stream copies compare the address first (35ad9aa, ef1379b, 26dc6a7, 6086ffe, 04764db, 4132b80).
(d) **Memory tracking:** sequential-write detection, with bulk unprotect for streamed regions
   (Bloodborne 67→80).

   Their profile's #1 waste: the **WaitRegMem poll loop**. 21k waits caused 47 M
   Test→yield→Process→scan/lock/resume cycles. Their fix:
   - pre-decode the predicate once;
   - add a WaitToken per address;
   - keep ready/blocked u64 masks.

   Our Liverpool::Process at 7.5 % is very likely the same loop.
   PopPendingOperations empty fast path: **we already have it** (bbport, vk_scheduler.cpp:838).

### What this means for UFC 3 (re-ranked levers)
1. **Port the authority/lazy-readback model + non-blocking WAIT_REG_MEM** (c2970ce, 5e338fd,
   52a2446, 27961d8). This targets exactly our ~20–25 % GPU-tick waits plus the deferred-signal
   pacing. It is the same win as BB_PC_MODEL, but proven on NVIDIA/Windows. Try BB_PC_MODEL first
   because it is cheap; if it breaks on NVIDIA, port this.
2. **WaitRegMem: pre-decode + token/mask scheduler** instead of yield polling (llvm-result.md §1).
3. **Generation-keyed pipeline/dynamic-state fast paths** to cut the 7–9 µs/draw toward ~3–4 µs. This
   matches our PostDraw / ApplyGraphicsRegisterPacket / HashRegisterPacket / StageSpecialization
   hotspots.
4. **PGO/LTO build** (their build-ultra.ps1 + fast-pgo.md): clang PGO over a fight-scene
   run, a cheap broad gain on the CP thread.

### 14.G  Re-read of the two GoW3 trees (2026-10-14, after run80/81)

Two different projects:

- `~/Projects/refs/GoW3R_Windows_Port` is a native port in the same shape as ours (bbport +
  shadPS4 b4e7ae7). Its "120 FPS patch" only sets `GOW3R_VBLANK_HZ=120` and
  `GOW3R_FPS_LIMIT=120` (`patches/README.md`). The renderer backports in `gpu/VENDOR.txt` are
  correctness: alias sync limited to matching GNM layouts, the full tiling-region map from
  eltutz `83352ea`, device-local copy before detile, and the two-draw MIN/MAX blend. They do
  not remove the command-thread ceiling. UFC already runs with vblank 480 and a 120 cap, and
  fights sit at 90–100 under that cap, so raising the cap does not reach 120.
- `~/Projects/refs/eltutz` tag `shadps4-eltutz-v3` is the Linux/Windows renderer that actually
  keeps GoW3 at high frame rates. Commit `7648269` reverted the first lazy-readback experiment;
  the tag tip puts it back, gated by `IsGow3FastpathActive()` to CUSA01623 and the other GoW3
  serials (`gpu_authority_tracker.cpp`). For those games, `WaitRegMem` calls `MatchVirtualWait`
  and, on a hit, continues (`liverpool.cpp` around 1868) instead of polling. GPU-written bytes
  stay on the GPU until the guest reads them (`ResolveForRamRead`). UFC's `WaitRegMem` only
  skips a fence the recording thread has already accepted (`PendingFenceValue`); every other
  unmet wait still drains and yields.

Route, on the model the user confirmed (run81, lights and fighters, 90–100):

1. **120:** port the virtual wait and the lazy materialize, behind a switch, not the PC memory
   model. The default profile is CPU-bound (GPU ~35 %). Cutting the GPU-tick waits is the
   ~2 ms that separates 100 FPS from 120 (8.33 ms). `BB_PC_MODEL` already made the command
   thread fast enough and then lost the frame on the GPU, and `BB_GARLIC_READS_VRAM` unlit
   the fight. Leave both off.
2. **240:** after 120 holds, the generation-keyed pipeline fast path (eltutz `516f18b` and
   the per-category skips in `WriteGraphicsRegisters`). The draw loop is still ~7–9 µs here;
   240 needs it near 4 µs with the waits already gone. Do not parallelise PM4 decode; they
   did not.

### 14.P  PC memory model test (run76/77, 2026-10-14)
- Launch: `BB_PC_MODEL=1 BB_PC_MODEL_ANY_GPU=1 BB_FPS_LIMIT=0 BB_FRAME_STATS=1 bash run.sh`.
- run76 crashed at the first sceGnmSubmitDone. Cause: `BbHeapSites::InstallWrappers`
  (gpu/shim/bbport_heap_sites.cpp) reads Bloodborne's libc malloc table at image+0x579a190. That
  address is unmapped in UFC 3. It is reached via `WatchOverlapLeak` (gnmdriver.cpp:153, a
  Bloodborne leak watcher active with BB_SUBMIT_AHEAD=2).
- Fix: an msync page check. When the page is unmapped, it prints "Heap sites: no libc malloc
  table ... off" and stays off.
- run77 boots. The log says "direct memory ... in GPU-visible memory (dma-buf)" (one chunk per
  allocation on NVIDIA), but "0 MiB of the arena bound in place" for every arena bind. Check whether
  in-place is effective for the arenas, or whether everything still goes through VRAM copies.
- Results (FPS / waits / visual breakage): see below.
- run77: the menu reached **frame pacing median 4.16 ms (240 FPS)**, then the game froze at the
  online/login flow. Cause, a deadlock:
  - bb:DrawRec (stage B) runs an EOP packet → `TextureCache::ProcessDownloadImages` →
    `DownloadImageMemory(sync)`;
  - under in-place there is no write tracking, so that calls `BufferCache::InvalidateMemory` →
    `ReadMemory` → `SendCommand<true>` to GpuComm;
  - GpuComm meanwhile sits in `DrawPipe::Drain` waiting for stage B.

  Fix (buffer_cache.cpp ReadMemory): run the flush inline when
  `Vulkan::DrawPipe::OnStageB()`.
- run78: running with both fixes; menu at 4.16 ms median. Fight result pending.
- run78: froze at 74 % of the match loading screen. The GPU watchdog said "waited 10 s for
  submission N, not submitted yet... threads are waiting for each other".
  - bb:GpuSignals sat in the semaphore wait (vk_rasterizer.cpp:383) on a tick that was recorded
    but never submitted.
  - GpuComm was idle (liverpool.cpp idle condvar) and DrawRec was idle.
  - Cause: the idle-entry `RequestSignalFlush()` runs once. If the signal isn't visible yet
    (HasUnsubmittedSignals false), nothing ever submits.

  Fix (liverpool.cpp idle wait): wait in 1 ms slices and re-call
  `RequestSignalFlush()` while idle with PipeSignals. The cost is one atomic exchange plus one
  pipe task per ms of idle.
- run79: launched with all three fixes. **NEXT for Grok:**
  1. If it still hangs, dump `gdb -p $(pgrep -x sacredfire) -batch -ex "thread apply all bt 10"`
     and look at GpuComm, DrawRec and GpuSignals.
  2. Measure fight FPS with BB_FPS_LIMIT=0.
  3. Check visuals: in-place model plus "Image overlap resolve failed" warnings.
  4. If PC_MODEL stays fragile on NVIDIA, port eltutz authorities (§14.R).
  The fixes are uncommitted, in gpu/shim/bbport_heap_sites.cpp,
  video_core/buffer_cache/buffer_cache.cpp (OnStageB inline) and video_core/amdgpu/liverpool.cpp
  (idle flush retry). Refresh docs/ufc3/ufc3-changes.diff with `git diff > docs/ufc3/ufc3-changes.diff`.
- **run79 RESULT (playable, no hangs): fights ~79 FPS (user: 60–80), menu 80.** That is lower than
  the default model (90–115), BUT the profile flipped:
  - **GPU thread 4.2 µs/draw, idle 68 %**, 0 write faults, 0 host-copy blocks, 846 draws/frame.
    The CPU side that capped us is solved: CP work is ~3.5 ms/frame, which fits 240.
  - **GPU 99 % busy, PCIe RX ~5 GB/s** (`nvidia-smi dmon -s t`): the GPU reads game memory in
    system RAM over PCIe. Only ~74 MiB of "loaded blocks" got promoted to VRAM.
  - stage B (DrawRec) busy ~79 %: the next CPU limiter after the GPU one.

  **Next step toward 240**: keep the PC model's CPU win but get hot data into VRAM.
  - Tune or extend the promotion: `buffer_cache.cpp` around 2230, "loaded blocks moved from in
    place to VRAM", plus ProcessDemotions and BB_VRAM_IDLE_SECONDS.
  - Make sure textures/vertex data read every frame live in VRAM (BB_GARLIC_VRAM default on for
    dGPU; check which arenas stay in place via the "GPU-written over 64 KiB" lines and
    BB_BUFFER_STATS=1).
  - Then split stage B work (bbport `vk_draw_prep` / eltutz `98105d5` recording thread).
  - Default run.sh does NOT enable BB_PC_MODEL, so normal play stays at 90–100.
- **run79 bindings (why only 74 MiB moved):** in a fight, `Buffer bindings` is ~122 MB/frame in
  place (0.8 of that GPU-written) and ~586 MB/frame already on the VRAM path. The 74 MiB are the
  GPU-written blocks that became fully known data. The PCIe traffic is the other ~121 MB/frame:
  read-only buffers over 16 KiB that stay in place because, without write tracking, `VramEligible`
  ignores Garlic unless every byte of the block was announced.
- **Code, not yet measured (2026-10-14):** `BB_GARLIC_READS_VRAM` (default on). A Garlic (type 3)
  block with no GPU write anywhere in it is bound in VRAM on first use and uploaded once from the
  guest chunk (`ChunkUploads`). `gpu_write_seen` is recorded at the first `NoteGpuWrite`, before
  the 3-frame rule, so a buffer the GPU writes even once is not taken by this path. Recycled and
  unmapped ranges clear that mark. libc copies over the write-watch bits still call `OnCpuWrite`:
  one-off updates are uploaded, and a block the CPU writes in 3 frames of 60 goes back in place
  (`NoteCpuWriteRange`). Game stores that skip libc are still invisible. `BB_GARLIC_READS_VRAM=0`
  restores run79. Build is `out/sacredfire` (5649bca-dirty). **NEXT:** same launch as run79
  (`BB_PC_MODEL=1 BB_PC_MODEL_ANY_GPU=1 BB_FPS_LIMIT=0 BB_FRAME_STATS=1`) and compare fight FPS,
  `Buffer bindings` in-place MB/frame, PCIe RX and GPU %. If the in-place line falls and FPS does
  not, the hot reads are Onion or share a block with a GPU write. Then split stage B.
- **run84 (`BB_FRAMES_AHEAD=2`, user: 240 menu, 120 in the fight):** menus hold **240 FPS** (GPU ticks ~0–2 %). The fight's heavy stretch (700–800 draws) is still **107–113 FPS**, same band as run83. A lighter stretch (609 draws) holds **131–132 FPS**. GPU ~51 % at 2820 MHz, not 99 %. GPU-tick waits during the fight are still ~22–29 %: two frames of overlap did not remove that wait. `BB_FRAMES_AHEAD=1` is the run83 pacing.
- **run83 (default model, `BB_FPS_LIMIT=0`, user in the fight, lights and both fighters):** **106–116 FPS** (overlay 106 FPS / 9.5 ms; log windows 109–116). ~680–800 draws/frame, GPU thread ~7.5 µs/draw and only 5–9 % idle, GPU-tick waits ~24 %, host copies ~10 %. GPU util ~49 % (sacredfire ~41 % SM) at 2805 MHz / ~98 W. This scene is just under 120 (8.33 ms). The 99 % GPU figure was the PC-model run, not this one.
- **run81 (default model, user confirmed):** lights and fighters are back, fights **90–100 FPS**. `BB_GARLIC_READS_VRAM` stays off.
- **run80 RESULT (fight, user in the match):** the picture was **unlit and the fighters were missing** (screenshot, 57 FPS). Do not default this on. `BB_GARLIC_READS_VRAM` now defaults **off**. In-place bindings fell 122 → **16.4 MB/frame**
  (1.0 of that still GPU-written). PCIe RX fell ~5 GB/s → **~1.4 GB/s**. Promoted blocks passed
  116 MiB. The GPU did not get faster: **~65 FPS** (64.8 and 66.6 in the steady windows; one dip
  to 34), GPU still **99 %**, memory controller ~7 %, GPU-tick waits ~44 %, GPU thread ~4.9 µs/draw
  and ~70 % idle, 763–794 draws/frame, write faults ~0–40/s. run79 was ~79 FPS with the same GPU
  saturation. The extra ~2.5 ms/frame is on the GPU, not on the command thread. More of the same
  promotion will not reach 240. `BB_GARLIC_READS_VRAM=0` is the faster of the two PC-model runs.

## 15. Windows build + BB_WRITE_STEP (2026-10-14, Claude)

- Windows: SacredFire's llvm-mingw cross build now targets UFC 3 (`scripts/build-windows.sh`,
  `BB_ALLOW_DIRTY=1 scripts/package-windows.sh` -> `dist/UFC3Native-windows-x86_64.zip`). Launcher
  `packaging/windows/sacredfire_launcher.c` (UFC3.exe): GoW3R-style Video/Frame-rate panel (output
  720p/1080p, present mode, FSR 1.0, RCAS, fullscreen, FPS limit, FPS counter), the ufc3 env
  defaults, `scripts/merge_update.py` (CUSA06534 + CUSA06534-patch -> hard-linked -merged folder).
  Windows-only fixes taken from GoW3R/eltutz: SRWLOCK guest mutex/condvars, timeBeginPeriod(1),
  above-normal priority + no EcoQoS, high-resolution AccurateSleep (the 480 Hz present loop ran at
  the 15.6 ms system tick), present-mode fallback Mailbox<->Immediate before FIFO, small-VRAM (<8 GB)
  texture GC budget rule, semaphore log capped.
- Omen 16 (i5-11400H, RTX 3060 6 GB, BIOS discrete mode) measured: menu 240 with Immediate (Mailbox capped it at 120 = 2x the 60 Hz panel; launcher default now Immediate), fights 70-83 (GPU thread 9-11 us/draw, idle 0.5 %, GPU ticks ~33 %): CPU-bound.
- `BB_WRITE_STEP` (default on): a guest-code store to a page with <=256 GPU-written bytes that does
  not touch them is single-stepped with the page open (trap flag; Linux SIGTRAP, Windows
  EXCEPTION_SINGLE_STEP) instead of a GPU finish + download; the changed words go to VRAM
  (vkCmdUpdateBuffer in ProcessLateWrites). Removes the per-frame sync in menus (UFC 3 writes
  0x10013fd280 beside a GPU label at 0x10013fd160): menu GPU ticks 24 % -> 0 %. Fights unchanged
  (~108 on the 4070): their remaining ~500 readbacks/s are writes over GPU bytes; stepping every
  page or hot pages was tried and lost 5-10 % (run87, run91).
- Menu 240 FPS is the game's own cap (BB_VBLANK_HZ=960 still 240, GPU thread 90 % idle).
- Known: moving the window to another monitor during startup crashed in the NVIDIA driver inside
  Swapchain::Destroy (run90).

## 16. Fight CPU work (2026-10-14 afternoon, Claude)

- perf of GpuComm in the attract-mode fight (out/perf-gpucomm3.data): DrainDrawPipe 11.8 %,
  NotePendingRead + PendingWriteOverlaps 10.3 %, PostDraw 7.4 %, ProcessGraphics + Process 11 %,
  CollectRingBindings 5.9 %, Apply/HashRegisterPacket 4.5 %, StageSpecialization 3.9 %.
- The step decision (BB_WRITE_STEP) now runs on the faulting thread under `gpu_ranges_mutex`
  (shared_mutex around gpu_modified_ranges changes): a step no longer sends a command to GpuComm
  (each command drained the draw pipe). A granule index was tried first and cost 60 % FPS
  (large GPU-written bindings walked per 16 B): removed.
- Not-stepped reasons in fights (run96, per s): host memcpy 300, page > 256 GPU bytes 250, near
  GPU bytes 50. Stepping whole pages (BB_WRITE_STEP_MAX_BYTES=4096, run97): 1500-2200 steps/s,
  readbacks unchanged, ~100 FPS: pages written many times per frame refault; keep 256.
- BB_READBACK_INORDER=1 (experimental, off): readbacks as ordered tasks on stage B instead of a
  drain (Liverpool::SendInOrderCommand). run98: command drains gone, stage B 83 % busy, stage A
  waits 50 % on it, 95 FPS (vs 105-113). Off.
- Remaining fight cost is structural: ~350 readbacks/s of CPU writes over GPU-written bytes, each a
  drain + GPU finish (~50 % of GpuComm). Windows build now uses LTO by default (scripts/build-windows.sh).
- Next ideas: PGO for the Windows build (llvm-mingw clang), cheaper PendingReads lookups (summary
  tables), generation-keyed pipeline fast path for the ~7 % of draws not prepared by the workers.

## 17. Write windows: fights 105-113 -> 140-190 FPS (2026-10-14 13:40, Claude)

- New frame-stats line "Readback sites": write faults that ended in a readback, by guest site
  and reason (PageManager::NoteReadbackSite). In the attract fight (run100) three sites made all
  ~400/s: +0x3f3562 (page with 288 GPU bytes, just over the 256 step limit), +0x47d855 (page with
  2176 GPU bytes) and a host memcpy. +0x3f3562 / +0x4219b8 write the engine's dynamic buffers:
  0x64b50 -> 0xf4120 is the buffer lock (flag 4 = discard: fresh range from the frame ring via
  0xf4240, size [buffer + 0x84]); 0x64b60 -> 0xf4310 the unlock. 48-byte blocks = 3x4 bone matrices.
- BB_DISCARD_HOOK=1 (Linux, int3 at 0xf4204): reports discard-lock ranges; 3-4k/s but 0 dead GPU
  bytes inside them. The GPU bytes are *neighbours* on the same page: BB_WATCH_GPU_WRITE=<page>
  showed page 0x1001338000 inside a 408-byte written shader binding [0x1001338ee0, +0x198).
  Kept opt-in, not useful.
- BB_WRITE_WINDOW=1 (now default for ufc3 in run.sh and UFC3.exe): a CPU write fault on a
  GPU-written page that does not touch the GPU bytes (past the step limit, a step burst, or a host
  memcpy) snapshots the page and leaves it read-write; Liverpool::SubmitGfx/SubmitAsc close all
  windows (re-protect first, then diff, changed words -> NoteSteppedWrite -> vkCmdUpdateBuffer
  skipping GPU words). One fault per page per submission instead of a readback per write.
  run107 (120 cap): pinned at 120, readbacks 380 -> 121/s, GPU ticks 30 -> 15 %. run108
  (uncapped): fights 137-196 FPS, GPU ticks ~0-3 %; screenshots look right.
  Risk: a CPU read of the GPU bytes of an open page sees stale data until the next submit.
- Remaining readbacks: +0x4219b8 "near GPU" (~120/s, writes within 16 B of GPU bytes).

## 18. Launcher controls, 2K/4K window, windowed swapchain crash, online notes (2026-10-14 16:10)

- Controls: UFC3.exe "Controls..." window edits bbport.ini key.<input>= / pad.<input>= (read by
  runtime_pad.c load_bindings): press-a-key capture (VK -> SDL scancode names), gamepad button
  dropdown (SDL names a/b/x/y/leftshoulder/.../lefttrigger), Defaults = runtime_pad.c defaults.
- 2K/4K: UFC 3 renders 1080p even in PS4 Pro mode with a 4K video out (run109: no target above
  1920x1080). BB_WINDOW_SIZE=WxH (shim/window.cpp) makes the window/swapchain larger; the presenter
  scales the game's frame with FSR 1.0 + RCAS. Launcher "Display resolution" (Same / 1080p /
  1440p / 4K). Fullscreen = the monitor's size anyway.
- Windowed crash after the first match (user report, Omen): Swapchain::Destroy did
  vkDeviceWaitIdle while the recording threads submit (BB_ASYNC_SUBMIT) - not externally
  synchronized, NVIDIA crash (also run90). Now: graphics/present queue waitIdle under
  Scheduler::submit_mutex; Presenter recreates outside that lock; ErrorSurfaceLost recreates
  instead of asserting. Linux run110: windowed 2560x1440, 6 resizes, no crash. Not yet confirmed
  on the user's Windows crash (no log of it).
- Online: servers closed 2025-02-17. The game uses EA Blaze (Redirector, Authentication via PSN
  ticket, GameManager matchmaking CCS PEERONLY/HOSTEDFALLBACK, UltimateTeam, Stats...) and DirtySDK
  ProtoSSL/ProtoTunnel/CommUDP for the peer link. The runtime stubs the network today.
  Paths: (1) now: local 2-player + Steam Remote Play Together / Parsec; (2) long: a Blaze emulator
  in the PocketRelay style (redirector + minimal Blaze components + ProtoSSL cert patch + real
  sockets in the runtime + PSN ticket stub).

## 19. Octagon floor noise / gray blocks — FIXED (2026-10-14 17:30)

- BB_DEPTH_VIEW_LOG=1 (new, vk_rasterizer.cpp BindTextures) lists depth images read through a
  colour format and the writers of R32G32_UINT images. It showed (run112): the spot-light shadow
  map 0x1079620000 is a **6-layer** Z16 array; cs **34617c9f** reads each layer as R16_UINT and
  writes the same layer of the R32G32_UINT 512x512x6 min/max map 0x1078616000 that the floor
  shader 86b28487 reads (the "dirt map" of §6/§11 is this shadow min/max map). cs aae9f6c1 does
  the same reads.
- The DepthAsR16 conversion (depth -> R16_UINT copy) only ran for layer 0: lights 1-5 read
  garbage -> grainy floor and gray blocks. Now one copy per (VkImage, layer), any single layer.
  Same bug class as shadPS4 PR #5284 (UFC 4, "grainy or shimmering shadows on the octagon floor").
- run113: user played a fight and confirmed the floor renders correctly.

## 20. Online P2P (host + guest) — work in progress (2026-10-14 17:10)

Goal: one player runs tools/ufc3_server.py and the game with BB_ONLINE_SERVER=<host IP>; the friend
points BB_ONLINE_SERVER at the host. The fight itself is the game's own peer-to-peer
(DirtySDK, CCS_MODE_PEERONLY); EA's services are replaced by a minimal Blaze server.

- src/runtime_net.c (new; only active with BB_ONLINE_SERVER): real TCP/UDP sockets (POSIX and
  Winsock), PS4 sockaddr/errno/sockopt/epoll ABI (from shadPS4), resolver (only *.ea.com -> the
  server, everything else refused), NetCtl connected, PSN signed in + PS Plus + fake auth code
  ("UFC3NATIVE-<hash>", issuer 256), NpWebApi unavailable, sceHttp as plain HTTP/1.0 to the
  server; ports < 1024 on the server reached at +40000. BB_NET_LOG=1 default (2: send/recv too).
- Facts found: redirector winter15.gosredirector.ea.com:42230 POST /redirector/getServerInstance
  (XML: ufc-2018-ps4, UFC2018_PS4, BlazeSDK 15.1.1.6.5, DirtySDK 15.1.3.0.5, standardSecure_v4).
  Answering <secure>0</secure> makes the client speak Blaze in plain TCP (no cert patch needed).
  Blaze framing = Fire2 (16-byte header), TDF heat2 — tools/blaze_tdf.py encodes/decodes it.
- Flow seen (run116): Util::preAuth (9/7) -> our reply accepted -> Util::ping (9/2) ->
  Util::fetchClientConfig "UfcConfig" (9/1) -> empty reply -> ~38 s later a host fault: strlen(NULL)
  in a string setter at +0x17234d0 called from +0xbb27a9 (obj->vtbl[0x20]() returned NULL right
  after obj->vtbl[0]() said true — likely a persona/display name that only exists after login).
  No Authentication login was sent yet.
- Next: find the UfcConfig keys the game reads (and/or the login trigger), implement
  Authentication login (PS4 auth code) + UserSessions notifications, then GameManager
  create/join for two clients and the peer address exchange.
