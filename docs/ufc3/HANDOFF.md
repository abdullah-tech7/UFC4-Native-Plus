# UFC 3 native port — handoff (2026-10-14)

EA SPORTS UFC 3 (PS4, CUSA06534 EU, update 1.14) running as a native Linux executable on the
SacredFire/bbport runtime. This file is the context to resume work in a fresh session.

## Where things are

- Repo: `~/Projects/ufc3-native` (copy of T3CHNOLOG1C/SacredFire, branch `ufc3`, **35+ files
  uncommitted** — user has not answered whether to commit).
- Game: `orig/CUSA06534` = base + 1.14 update merged with hardlinks (from `~/Games/PS4`).
- References: `~/Projects/refs/{bbport,SacredFire}`; old shadPS4 debug fork `~/Projects/ufc3-port`.
- Run: `bash run.sh` (no gdb needed). Build: `bash build.sh` — **never while the game runs**.
- Logs of the session: `out/run*.log`; conversation log
  `~/.claude/projects/-home-myzk-Work/conversations/2026-10-13-ps4-port-pc.md`.

## Status

- **Playable natively**, menus 120 FPS, fights ~90 FPS (RTX 4070, NVIDIA 610.57), sound, pad,
  saves. Runs without gdb.
- Open: (1) **octagon floor noise + gray blocks** (main open bug, see below); (2) 120 FPS in fights
  (perf); (3) first-run tutorial videos (~5 min) cannot be skipped.

## run.sh profile `ufc3` (defaults, all overridable)

`BB_READBACKS=2 BB_READBACK_LINEAR=1` (fight scene black otherwise, like shadPS4 #4375 UFC 4),
`BB_GPU_WRITE_TWINS=0` (deadlock with precise readbacks), `BB_FLIP_RATE=0 BB_VBLANK_HZ=120`
(game asks 30 Hz via its present-interval setter at guest 0x1182a0; sim uses real frame time),
shader patches compiled from `patches/shaders/*.comp` into `user/shader/patch/`.
FPS counter top-left: `bbport.ini show_fps=1`.

## Fixes made (where)

- Runtime imports (src/runtime_*.c): pthread attr stack/sched/cancel, mutexattr_setpshared,
  AppContent temp/addcont, SaveData mountinfo/backup, offline stubs (NpWebApi/Http/Net/Voice/
  Audio3d/Coredump/dialogs), fcntl/chmod/utimes/msync/modules/sockets. AJM **MP3 via minimp3**
  (third_party/minimp3, runtime_ajm.c).
- gpu/shadps4 kernel equeue.cpp: kqueues map mutex (race); WaitForSmallTimer delivers regular
  events + falls back; WaitForEvents handles small timers added while waiting; poll returns
  expired small timers; AddSmallTimer notifies. gpu/shim/bbgpu.cpp: asio io_context with
  work_guard (lost wakeup → black screen without gdb).
- vk_instance.cpp: D16S8 fallback prefers D32S8 (as AMD). image_view.cpp: depth-stencil views
  with both aspects → depth aspect (GPU hang Xid 109 in cs aae9f6c1).
- src/probe.c: speculative-read faults skip GPU readback (DrawPrep deadlock in fights).
- vk_pipeline_cache.cpp: patched SPIR-V is what gets cached; IsPatchShaders = BB_PATCH_SHADERS.
- vk_rasterizer.cpp: depth image viewed as R16_UINT → converted copy (host shader
  depth_to_r16ui.comp). (Did not fix the floor.)

## Diagnostics added

`BB_SKIP_SHADERS=hash,..` / `BB_SKIP_FILE=path` (live list, re-read 0.5 s; draws recorded with 0
instances), `BB_CAPTURE_ON_FLIP=1` with `BB_CAPTURE_TRIGGER/DIR` (frame capture, one line per draw
with textures, 64 shaders/pass), `BB_ALLOW_PTRACE=1` (attach gdb later: `gdb -p`),
timestamp breadcrumbs for NVIDIA + "GPU wait stuck" report (2 s timeout in vk_semaphore.cpp),
`BB_FLIP_RATE`, wave64 warning tagged with hash, `tools/ufc3-shot.sh` (grim cropped to the game
window; never capture whole monitors), `tools/ufc3-iter.sh`, `out/gdbwrap.sh` (BB_PROBE).
Temporary (remove before shipping): arena-bind printf in buffer_cache.cpp, semaphore 2 s timeout.

## Floor bug — where it stands

- Not SSAO, not tiled light culling (cs aae9f6c1), not pass-4 decals, not the wave64 cs.
- Canvas print tiles = **ps 86b28487** (vs 478d9514, quads of 6 indices). Skipping it leaves black
  squares where the printed tiles are. GLSL decompile: `fs_img3` is a **utexture2DArray
  R32G32Uint 512x512 at 0x1078616000**, read with texelFetch on 4 neighbours and manual bilinear
  weights (dynamic map: shadow/occlusion/dirt); also a samplerShadow (shadow map D16S8 1024² at
  0x1079620000, never re-rendered in the captured frame).
- Nobody writes 0x1078616000 in the captured frame → written earlier or **by the CPU**.
  Hypothesis: refresh/detiling of that 64bpp tiled array is wrong (cf. SacredFire backport of
  shadPS4 #5196 "small mips of tiled textures") or CPU writes are missed.
- Next step: run with `BB_IMAGE_SYNC_LOG=1` (was just launched as out/run57.log), get the image's
  tiling/refresh pattern; then try forcing a re-upload of that image per bind, or check detiling
  of R32G32 (64bpp) tiled arrays.

## Working style notes

- User plays and reports; for visual A/B use BB_SKIP_FILE + tools/ufc3-shot.sh while the user
  stays in a fight (no restarts).
- Jarvis voice is disabled (`~/Work/jarvis-voice/DISABLED`), no 🔊 lines.
- User wants token-efficient sessions: keep replies short.
- 2026-10-14 04:05: crash 'EOP flip submission failed' (flip queue >16) → video_out.cpp espera hasta 500 ms en vez de ASSERT.
