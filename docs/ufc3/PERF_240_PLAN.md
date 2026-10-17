# UFC 3 native port — plan to reach 240 FPS (2026-10-14)

Read `GROK_HANDOFF.md` first (full history, §10–§13 perf work). This file is the strategy for the
next agent. **Incremental 5–10 % shaving cannot reach 240 FPS; the architecture must change.**

## 1. Where we are

- Fights ~90–115 FPS (scene dependent, ~650–880 draws/frame), menus capped at 120.
- Defaults (run.sh, profile ufc3): `BB_READBACKS=2 BB_READBACK_LINEAR=1 BB_GPU_WRITE_TWINS=0
  BB_FLIP_RATE=0 BB_VBLANK_HZ=480 BB_FPS_LIMIT=120 BB_Z16S8_AS_D16=1`. For 240: raise
  `BB_FPS_LIMIT` (0 = none) and use a ≥240 Hz path.
- Untested last change (run75): SacredFire issue-#2 diagnostics off by default
  (`BB_CLOBBER_CHECK=1` re-enables: FirstBadHeader walks, guest write ring/prints). Verify it does
  not hang before keeping it.
- GPU (RTX 4070) only ~34–36 % busy → **CPU/latency bound**, not GPU bound.

## 2. The hard ceiling (why 10 % steps fail)

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

## 3. Structural levers (in order of expected payoff)

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

## 4. Use REA (reverse-engineer-anything) on the GoW III Remastered ports (user request, priority)

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

## 4b. REA/source results → see GROK_HANDOFF.md §14.R (eltutz v3 source: lazy-readback authorities, non-blocking WAIT_REG_MEM −4.6 ms/frame in GoW3, generation-keyed draw fast paths, PGO)

## 5. Concrete next steps (ordered)

1. Verify run75 (diagnostics off) is stable; keep or revert `BB_CLOBBER_CHECK` default.
2. Try `BB_PC_MODEL=1 BB_PC_MODEL_ANY_GPU=1` (+ `BB_FPS_LIMIT=0`); record FPS, waits, breakage.
3. REA pass over GoW3R/eltutz binaries per §4; write findings into this file.
4. Pick the lever with the biggest projected gain (§3) and prototype it behind a `BB_*` switch.
5. Measure with `BB_FRAME_STATS=1`, `BB_WAIT_TRACE=1`, `perf record -t <GpuComm tid>`.

## 6. Measurement recipe

- Launch: `BB_FRAME_STATS=1 BB_ALLOW_PTRACE=1 bash run.sh` (no gdb; gdb slows write faults).
- In a fight: `perf record -F 2000 -g --call-graph=fp -t $(ps -L -o tid=,comm= -p $(pgrep -x
  sacredfire) | awk '/GpuComm/{print $1}') -- sleep 12`; whole process: `-p <pid>`.
- GPU busy: `nvidia-smi --query-gpu=utilization.gpu --format=csv`.
- Compare scenes with similar draws/frame (Frame stats line) — FPS varies 85–115 with scene.
