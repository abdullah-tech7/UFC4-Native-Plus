# Issue #2, the clobbered dword: hand-off for the next session (2026-10-11)

GitHub issue: https://github.com/T3CHNOLOG1C/SacredFire/issues/2 (the full symptom list, run
table and candidate writers). This note adds what the code reading at the end of the previous
session found, and the experiment it points to. Read the issue first, then this.

## The decisive observation from the code

`Liverpool::SubmitGfx` (gpu/shadps4/video_core/amdgpu/liverpool.cpp, around line 2070) copies
the submitted DCB and CCB into a host `std::vector` (`SubmittedCopy`) **synchronously on the
game's submitting thread**, and the decoder then decodes that copy, not guest memory
(`BB_COPY_GPU_BUFFERS=1`, the default). `ReportBadPacket` prints the address of the dword *in
that copy*: the "Bad PM4 packet at 0x32a17f28 ... buffer at 0x329f8190 (memory type -1, prot 0)"
lines name **host heap addresses** ("memory type -1" means not guest memory), not the game's
buffer. The same buffer address every frame (0x429155c0 in run 7) is the allocator handing the
copy the same block each frame.

So only two things are possible, and one check tells them apart:

1. The game's buffer already held the 1 when `SubmitGfx` ran `memcpy` (the writer acted before
   submission: the game itself, or one of the HLE'd Gnm writers that run on the game thread,
   or a port thread writing into guest memory the game is building commands in).
2. The host copy was modified after the `memcpy` (a host-side stray write: a dangling pointer
   into a freed-and-reused vector block, a label write resolving to a host address, a worker
   writing into the copy it was given).

## The experiment

Extend `ReportBadPacket` (liverpool.cpp ~line 975) so that, when the decoder meets a bad
header in a copy, it also reads the **guest** dword at the same offset (`SubmittedCopy` keeps
`guest_dcb`, `dcb_dwords`, `guest_ccb`; the copy is passed into `ProcessGraphics` as a
`shared_ptr`) and prints both: `copy=0x00000001 guest=0xc0016900` or `guest=0x00000001`. The
guest buffer may have been reused by the game by decode time, so also add a cheap header walk
right after the `memcpy` in `SubmitGfx` (type 3: advance by count+2; type 2: 1 dword; anything
else: print the offset and the dword, once per submission) and a second walk over the copy at
the start of `ProcessGraphics`. Three readings per clobber: at submit (guest), at decode (copy),
at decode (guest).

- Copy clean at submit, bad at decode: case 2, a host bug. Then set a hardware watchpoint:
  the copy block address repeats each frame, so `BB_PROBE="gdb -batch -ex 'watch *(int*)ADDR'
  ..."` or an `mprotect` of the copy block after `memcpy` (PROT_READ; the decoder only reads it;
  the fault handler names the writer) catches it within a few frames. The vector would need to
  be page-aligned for the mprotect trick (allocate with `aligned_alloc` or over-allocate).
- Bad already at submit: case 1. Then the suspects are the game and the HLE Gnm writers
  (gpu/shadps4/core/libraries/gnmdriver/gnmdriver.cpp, the functions that write packets into
  the game's buffers: the `sceGnm*` draw and state setters, `sceGnmInsert*`) and port threads
  writing guest memory (BbWriteLog covers fences, WriteData, DMA, backing writes; extend it to
  the HLE writers). A watchpoint on the guest address works the same way; the game reuses the
  buffer each frame, so the address repeats too.

## Reproduction

Cold cache with background compilation on fired in 2 of 3 unattended 4-minute runs:

```
BB_TIMEOUT=0 BB_FRAME_STATS=1 BB_CLEAR_SHADER_CACHE=1 BB_ASYNC_PIPELINES=1 bash run.sh
```

The game takes about 2 minutes to reach the intro videos; the clobbers came at 3 to 4 minutes.
With `BB_WRITE_LOG=1` the one run did not reproduce (timing). Logs of the earlier runs are in
`logs/backport-smoke*.log` and `logs/hang-repro*.log`.

## Environment reminders

- Builds run in the `archbuild` container (sudo works there); a GPU-library rebuild after a
  header change takes 20-40 minutes, `bash build.sh --test` also runs the C tests. Never run
  `build.sh` while the game is running (it replaces the mapped library).
- `kill -ALRM $(pgrep -x sacredfire)` dumps every thread's stack on a hang and exits.
- The clobber bug is independent of the backports: it was first seen on 2026-10-09 on the
  pre-backport tree. v0.2 shipped with background compilation opt-in because of it.

## 2026-10-11, later session: the instrumentation (branch `issue-2`)

### A pattern in the run-7 log

In run 7 (logs/backport-smoke-7-default.log) the clobbered dword is `0x13890` bytes into the
buffer, and the packet right after it is an `EVENT_WRITE_EOS` that writes the value 1 to the label
`0x10f6113890`, followed by a `WAIT_REG_MEM` for 1 at the same label. The low 16 bits of the
offset and of the label match. If the game's buffer started at `0x10f6100000` (a 1 MiB-aligned
allocation is plausible), the label *is* the clobbered header: a CPU-side label write of 1, made
by the port on the game's behalf, landing inside a submitted command buffer, before the memcpy in
`SubmitGfx` copied it. That is the "fence or label write landing in reused memory" candidate from
the issue, and it predicts case 1 (bad already at submit). The instrumentation below tests it
directly, and still covers case 2.

All four clobbers seen so far replaced a type-3 header (`c0016900`, `c0047600`) with
`00000001`; the payload dwords after it were intact.

### What the build on `issue-2` adds (always on, cheap)

New module `gpu/shim/bbport_clobber.{h,cpp}`; hooks in liverpool.cpp, bbgpu.cpp,
vk_rasterizer.cpp, buffer_cache.cpp, texture_cache.cpp.

1. **Three readings per clobber.** `SubmitGfx` walks the copy's headers right after the memcpy
   (`SubmittedCopy::bad_at_submit`); `ProcessGraphics` walks them again when the GPU thread
   starts decoding the copy (`bad_at_decode`); `ReportBadPacket` prints both, the guest dword at
   the same offset as it is now, and the time since submit:
   - `Command buffer check: submission N's dcb already holds a non-packet dword when the game
     submits it: 00000001 at +0x... (guest 0x...)` — case 1, the guest buffer was bad at submit.
   - `Command buffer check: submission N's copy changed between submit and decode start` — case
     2, a host-side write into the copy while it waited in the queue.
   - `  issue #2: copy now 00000001; guest dword at ... now ...; first non-packet dword of the
     copy at submit: none|+0x..., at decode start: none|+0x...` under every `Bad PM4 packet`.
2. **Guest write check.** Every write the port makes into guest memory on the game's behalf
   (EOP/EOS/RELEASE_MEM labels, WRITE_DATA, DMA fill/copy, buffer and image downloads) goes into a
   ring of the last 16384 writes, and is reported immediately when it lands inside one of the last
   256 submitted command buffers (`Guest write check: EOS fence of 4 bytes, value 0x1, to 0x...
   lands inside submission N's command buffer at +0x... (<where>; decoding submission D; submitted
   T ms ago; thread ...)`). `<where>` is "the buffer being decoded" (a label the game placed inside
   its own buffer: legal on the hardware, which has parsed past it), "a buffer submitted after the
   one being decoded, not decoded yet" (the clobber scenario: the game already reused the memory),
   or "an older buffer, not resubmitted since". The two checks above also list the remembered
   writes into the clobbered dword (`  port write into it: ...`).
3. **`BB_COPY_PROTECT=1`** (opt-in): the copies are allocated in whole pages and made read-only
   after the walk at submit until they are freed. A host-side write into one faults; the fault
   handler prints `Clobber trap: a write to the read-only copy of submission N at +0x... from RIP
   ...` and lets the runtime print the writer's call chain (`Host fault (signal 11) ... #2 ...`) and
   exit. Use it when the readings say case 2.

The decoder, the draw-preparation workers (they take their own copy in `DrawPreparation::Build`)
and `CheckSubmittedCopy` only read the copy, so the read-only mode has no legitimate faulter.

### Runs with the instrumentation (2026-10-11, evening; logs/issue2-run*.log)

Reproduction command as above, `timeout -s INT 300`, unattended (the license is accepted in the
save). Build v0.2-4-g41a3e71 (PGO off, LTO on).

| run | extra | result |
|---|---|---|
| 1 | | clean 5 min; 30 FPS; 248 compiles, 0.5 s in total |
| 2 | | clean 5 min; 296 compiles, 0.5 s; 15 stalls, the longest 700 ms |
| 3 | `BB_COPY_PROTECT=1` | clean 5 min; 602 compiles; no fault, so nothing legitimate writes the copies |
| 4 | `BB_COPY_PROTECT=1`, empty `MESA_SHADER_CACHE_DIR` | clean 5 min; 693 compiles, 4.0 s (7 ms each, like the morning); 30 stalls, the longest 534 ms |
| 5 | same | clean 5 min; 248 compiles |
| 6 | `BB_COPY_PROTECT=1 BB_FREE_CHECK=1 BB_FENCE_DELAY_TEST=1` (every 200th EOP label held 80 ms) | clean 5 min; the free check saw the held fences (80 ms late) and nothing else |
| 7 | as 6 plus `BB_EOS_DELAY_TEST=1` (every 200th EOS label held 80 ms) | clean 5 min; 496 stalls from the holds, no clobber |
| 8 | `BB_COPY_PROTECT=1 BB_FENCE_DELAY_TEST=1500,300 BB_EOS_DELAY_TEST=1500,2000` (1.5 s holds) | clean 5 min; 60 stalls of 1.5 s; **no guest event wait timed out** (new report, commit 16c3e85) |
| 9 | `BB_COPY_PROTECT=1`, empty Mesa cache | clean 5 min (start screen only) |

### Where this leaves the bug (2026-10-11, end of the session)

- Nine runs with the instrumentation, zero clobbers (the morning: 3 events in 7 runs on the
  uncommitted tree that became 4a91d4e; the two play sessions after that commit were clean too).
  Nothing in gpu/ changed between that commit and the instrumentation except 381cb24 (texture
  format fallback) and 15b0cdd (overlay, saves, controllers), so the difference is timing, not a
  fix; treat the bug as latent.
- The game's fence labels live inside its two alternating command buffers, written by the port
  20 to 80 ms after submit. A clobber is such a write arriving after the game rebuilt the buffer.
- The port's gating against exactly that (GPU idle only after the recording thread and the
  deferred fences are done; the game's next submission waits for it) held under every forced
  delay: 80 ms and 1.5 s holds of EOP and EOS labels produced no clobber and no game-side event
  wait timed out. So the label that clobbers reaches the buffer by a path these holds do not
  cover, or the writer is not a label at all (the game itself, or a port write not in the ring).
- Every port write into guest memory that the ring knows (EOP, EOS, RELEASE_MEM, WRITE_DATA,
  DMA fill and copy, buffer and image downloads, DUMP_CONST_RAM) landed in "the buffer being
  decoded" in all nine runs; a write classified "a buffer submitted after the one being decoded,
  not decoded yet" would be the clobber in the making and has not been seen.

### What to do when it fires again

The game's log now says so itself: at the first sign of the bug (any of the lines below) the GPU
library prints, once, two lines starting with `SacredFire issue #2:` that name the issue and ask
for the whole log to be attached to it; the launcher's Log page watches for that text and shows a
notice with an *Open issue #2* button and a toast (`BB_ISSUE2_TEST=1` prints the lines at the
first submission, to try the notice). Users who hit the bug can thus report it with the log that
holds the diagnostics; the lines to read in such a log, in order:

The instrumentation is always on (branch `issue-2`), so any play session that hits the bug
leaves the answer in the log. Look for, in this order:

1. `Command buffer check: submission N's dcb already holds a non-packet dword when the game
   submits it` — case 1 (the guest buffer was bad at submit). Right under it, `port write into
   it: <source> ... while decoding submission S` names the port write that landed there and how
   long before; `no remembered port write into it` means the game itself (or a write the ring
   does not know) put it there.
2. `Command buffer check: submission N's copy changed between submit and decode start` — case 2,
   a host-side write into the copy. Reproduce with `BB_COPY_PROTECT=1`: the writer faults with
   `Clobber trap: ...` and its call chain.
3. Under every `Bad PM4 packet`: `issue #2: copy now ...; guest dword ... now ...; first
   non-packet dword of the copy at submit: ..., at decode start: ...` (the three readings).
4. `Guest write check: ... lands inside submission N's command buffer at +... (a buffer submitted
   after the one being decoded, not decoded yet ...)` — a late label caught in the act, before
   the decoder meets it.

Experiments available without a rebuild: `BB_FENCE_DELAY_TEST=<ms>[,<every>]` and
`BB_EOS_DELAY_TEST=<ms>[,<every>]` (hold labels back), `BB_COPY_PROTECT=1` (read-only copies),
`MESA_SHADER_CACHE_DIR=<empty dir>` (cold driver cache: 7 ms pipeline compiles, as in the morning
runs), `BB_FREE_CHECK=1` (late-fence report). Guest event waits that time out are always reported
(`Guest event wait timed out: queue ...`).

The user pressed through the start screen and loaded races in some of these runs (runs 2, 3, 4 and 6
loaded Crash Cove or September Spyro), so the race-load trigger of the morning's run 7 was tried
four times without a clobber. The forced-delay runs test the gating directly: if the game could
rebuild a command buffer before its labels are written, holding labels back makes it happen; 80 ms
did not. Run 7 holds EOS labels too (`BB_EOS_DELAY_TEST`, commit 99aceb7; both switches then
took `"<ms>[,<every>]"`), run 8 holds every 300th EOP and every 2000th EOS label for 1.5 s, longer
than a plausible game-side wait timeout (the morning's clobber followed a 1633 ms stall).

What the guest write check established without a clobber:

- **The game keeps its fence labels inside the command buffers it submits.** Every EOS/EOP label
  write of value 1 (thousands a minute, `hit 149500` after five minutes) lands inside "the buffer
  being decoded": the game allocates the label from the command buffer itself (a NOP packet's
  payload, Gnm's `allocateFromCommandBuffer`). The two command buffers alternate between
  `0x10f6100000` and `0x10f6900000` (8 MiB apart), one per frame, so the game rebuilds a buffer two
  frames after submitting it. This confirms the run-7 reading: label `0x10f6113890` was the
  clobbered dword at `+0x13890` of the buffer at `0x10f6100000`.
- The labels are written by the port 20 to 33 ms after the submit (the game's frame time at 30
  FPS), up to 80 ms during stalls. The write of 1 to such a label is a write into the submitted
  command buffer, legal on the hardware (the CP has fetched past it) and harmless here while the
  game has not reused the buffer. A clobbered header is therefore a label write that arrived after
  the game rebuilt the buffer: the label's slot became a packet header in the new frame.
- In the default mode (no honest labels) the GPU thread drains the recording thread and the
  deferred fences before it signals GPU idle (`Process`, `work_retired`), and the game's next
  submission waits for that (`WaitGpuIdle`), so a label of frame F should be written before the
  game builds frame F+2 into the same buffer. Which path lets one through is what a reproduction
  with the instrumentation will show (`port write into it: EOS fence ... while decoding submission
  S`).

Why the runs stay clean now: the runs that fired on the morning of 2026-10-11 did 666 to 908
pipeline compiles costing 3.4 to 8.3 s in total (5 to 9 ms each, 119 stalls, the longest 1633 ms,
right before the clobber in run 4); the evening runs did 250 to 300 compiles at 2 ms each. The
port's cache is cleared each time (`user/cache/CUSA13795` is written and removed as expected), so
the difference is RADV's own on-disk cache (`~/.cache/mesa_shader_cache`), cold in the morning,
warm now. Run 4 points `MESA_SHADER_CACHE_DIR` at an empty directory to bring the long stalls back.

## 2026-10-12: a reproduction on native Windows, and the late-label guard

The Windows build (branch windows-port, v0.2.1-7-g4455a29, AMD Custom GPU 0932 with the AMD
proprietary driver 2.0.302, Vulkan 1.3.280) hit the bug three times in three play sessions
(race 2, race 2, race 4), each with the diagnostics on. The race-4 log is the first reproduction
with the instrumentation (kept outside the repository: it holds the user's paths), and it reads
as case 1 of the list above:

- `Command buffer check: submission 34303's dcb already holds a non-packet dword when the game
  submits it: 00000001 at +0x9228 (guest 00000010F6909220; dcb at 00000010F68FFFF8, 0x1e198
  bytes; submitting thread 6976)`.
- Under it, the ring names the writer: `port write into it: EOS fence of 4 bytes, value 0x1, to
  0x10f6909220, 1525.60 ms ago, while decoding submission 34299 (thread 12580)`, then the same
  EOS fence to the same address 7.7 to 8.1 s earlier, once every four submissions (33963, 33967,
  ... 33987: every other frame at 30 FPS, the buffer at 0x10f6900000 alternating with the other
  one). The slot is a NOP payload in those frames (a label allocated from the command buffer)
  and a `SET_SH_REG` header in submission 34303 (`c0001000 6875000d [00000001] 00000050
  f4a99910 ...`).
- The decoder then met the header 29.66 ms after the submit, skipped two dwords, and died on
  `Unknown PM4 type 3 opcode 0x99 with count 13482` (exit 23).

So the writer is the port's own EOS label write for submission 34299, and it landed after the
game had rebuilt the buffer for submission 34303: the game wrote the `SET_SH_REG` header, the
port wrote 1 over it 1.5 s before the game submitted the buffer (a load stall: race 4 was
starting). Which ordering let the game rebuild the buffer before that label was written is still
not read off the log: in the default mode the next submission waits for the GPU-idle signal that
follows the drain of the draw recording thread, which runs the EOS tasks; the `--tls-loads`,
`BB_SUBMIT_AHEAD` and flip paths were checked and do not bypass it on paper. The log does not
record when the game called the submit that was then held for 1.5 s.

**The guard (gpu/shim/bbport_clobber.cpp `GuardLabel`, always on).** Every fence label store
(EOP, EOS, RELEASE_MEM; `SignalEop`, `WriteEosLabel`, the RELEASE_MEM cases in liverpool.cpp)
first checks whether its slot lies inside a submitted command buffer (the ring of 256 ranges)
and now holds a plausible PM4 type-3 header (type 3, opcode 0x10 to 0xaf, reserved bits clear).
A fence slot holds 0, 1, a counter or a clock, never that; the game resets a label it still waits
on before it submits the buffer, so a header in the slot means the game has rebuilt the buffer
and is done with the label. Such a write is dropped and reported once (the first 16, then every
100th) with the writer's call chain:

    Late label guarded: EOS fence of 4 bytes, value 0x1, to 0x10f6909220 from submission 34299
    lands at +0x9228 of submission 34303's command buffer, which now holds the packet header
    c0047600 there (the game rebuilt the buffer; submitted ... ms ago; decoding submission ...;
    thread ...; guarded 1): the write is dropped
        #0 sacredfire-game.exe+0x...
        ...

followed by the `SacredFire issue #2` notice, so the log still gets reported. The fence tasks now
carry the graphics submission's seq (`EopTask`, `EosTask`) for that line. The guard turns the
crash into a logged event; the call chain in the next report names the path (GPU thread, draw
recording thread, Vulkan recording thread; honest, async or recorder fences), which is what the
nine clean runs could not. The ring timestamps show it would have fired here: the slot held
`c0047600` when the EOS fence arrived.

### The second Windows log (2026-10-12, seven races): the writer's call chain, and the rule

The next session on Windows (v0.2.1-8-g5edc381, five tracks, about seven races) hit the guard
twice and played on; the game ended later in an unrelated colour-buffer format assertion (below).
Both guard reports read the same, here the first:

    Late label guarded: EOS fence of 4 bytes, value 0x1, to 0x10f690d6f8 from submission 28822
    lands at +0xd700 of submission 28822's command buffer, which now holds the packet header
    c0016900 there (the game rebuilt the buffer; submitted 3232.98 ms ago; decoding submission
    28824; thread ; guarded 1): the write is dropped
        #0 sacredfire-game.exe+0xab4d0   GuardLabel        (gpu/shim/bbport_clobber.cpp)
        #1 sacredfire-game.exe+0x1ce570  WriteEosLabel     (liverpool.cpp)
        #2 sacredfire-game.exe+0x1cbd47  RunEventWriteEos  (liverpool.cpp)
        #4 sacredfire-game.exe+0x69f8d   DrawPipe::Run     (vk_draw_pipe.h:242)
        #7 ucrtbase.dll+0x2cd30          (thread start)

(symbolized with `llvm-symbolizer --obj=out/windows/bin/sacredfire-game.exe --relative-address`
against the unstripped build of the same commit). So:

- The writer is the **draw recording thread** (stage B, `DrawPipe::Run`) executing the EOS task
  of submission 28822 in the default path (no honest labels): `RunEventWriteEos` ->
  `WriteEosLabel`. Nothing exotic: the ordinary in-order task, run late.
- It ran **3.2 s after the submission**, while the GPU command thread (stage A) was already
  decoding submission 28824, two submissions (one frame) later. The game had rebuilt the buffer
  of 28822 for the frame after that (the label's own buffer; the slot held a `SET_CONTEXT_REG`
  header, `c0016900`). The second event is the same picture: 59267 written while 59269 was being
  decoded, 2.6 s late, the slot holding a NOP header. Both happened at a race load ("push load
  octane/t154_oxide_station" follows the second).
- Therefore the game's submissions and its rebuild of a command buffer ran ahead of stage B by
  a whole frame during a load stall. The GPU-idle gating of sceGnmSubmitDone drains stage B
  before it signals idle, so either the game does not go through sceGnmSubmitDone between
  these frames (it imports sceGnmSubmitDone, sceGnmRequestFlipAndSubmitDone - a stub that
  returns at once - and sceVideoOutSubmitFlip; which one it uses per frame is the next thing to
  log), or it gates its own reuse of the buffer on something the port signals before stage B is
  done (a flip, a label written on stage A). Either way the memory write of the label must not
  trust that the buffer is still the one it was decoded from.

**The rule the guard applies now.** The fence tasks (`EosTask`, `EopTask`) carry the label
slot's dwords *as the buffer was submitted*, read from the submission's copy when the packet is
decoded (`label_at_submit` in `ProcessGraphics`; only for labels inside the buffer being
decoded, which is where CTR keeps them). Before the store, `GuardLabel` compares the guest slot
with that value: equal means untouched since submission, so the write proceeds; different means
someone wrote the slot after the submission, which is either the port itself (the last port
write per address is remembered, and a slot holding it is fine: a label written twice in one
stream) or the game rebuilding the buffer, in which case the write is dropped and reported with
the two values. The game resets a label it waits on before it submits the buffer and leaves it
alone until the port writes it, so this rule does not drop a fence the game is waiting for; the
header check stays as the fallback for labels without a submitted value (another buffer, the
compute queues). A dropped write that lands on a packet *payload* (a register value, a shader
address) is now caught as well, which the header check alone did not do.

The stall itself (seconds of stage B work at a race load: pipeline compiles on first use of a
track's shaders) is what makes the window; a warm pipeline cache shrinks it, which is why the
Linux runs with a warm RADV cache stayed clean.

### The format assertion (the same log's end)

After the seven races the game stopped with `SurfaceFormat: Assertion Failed! Unknown
data_format=6 and num_format=0` (exit 23): a colour or vertex buffer with 10_11_11 Unorm, the
pair that textures already bind as null descriptors since 381cb24. `SurfaceFormat` now takes
the format of the same layout with another number format (10_11_11 Float, `B10G11R11UfloatPack32`)
and prints the substitution once, instead of asserting; `TrySurfaceFormat` stays strict for the
null-descriptor and draw-preparation paths.

### What the guard prints now (for the ordering question)

`BbTimeline` (gpu/shim/bbport_timeline.h) keeps an always-on ring of the last 8192 events
beside the opt-in file timeline: the guest's graphics submissions (`guest submit gfx seq`, with
the seq the clobber diagnostics use, and the dcb address), `sceGnmSubmitDone` and whether it
set the GPU-idle lock, the stubbed `sceGnmRequestFlipAndSubmitDone`, waits for the lock and
their length, flips (`sceVideoOutSubmitFlip`, EOP flips registered and reached, frames
presented), and the GPU thread's decode begin/end, submission-end drains with their length,
and idle signals. The first four trips of the late-label guard print, after the call chain,
every event from 50 ms before the label's buffer was submitted up to the write, with times
relative to that point and thread ids. Read it for: which call ended the frame after that
submission (`sceGnmSubmitDone` with the lock, or the stub, or only a flip), whether the next
submissions waited for GPU idle and how long, when the drain of that frame began and ended on
the GPU thread, and where in that sequence the game must have rebuilt the buffer (before the
write, after the last event that told it the frame was done).
