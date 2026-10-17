# SacredFire

SacredFire runs *Crash Team Racing: Nitro-Fueled* (PlayStation 4, CUSA13795, update 1.21) as a
native x86-64 Linux executable. It is built on [bbport](https://github.com/deadinside28/bloodborne_pc)
(GPL-2.0-or-later), the native Linux port of Bloodborne: the game's own code runs on the CPU, a
small runtime replaces the PS4 system libraries, and a renderer derived from
[shadPS4](https://github.com/shadps4-emu/shadPS4) translates the PS4 GPU work to Vulkan.
Developed and tested on a Steam Deck.

> **No game files are included.** You need your own dump of the game and its 1.21 update. Both
> builds of the 1.21 update are accepted: the one for firmware 5.05 and the one for 6.72+ (the
> game patches find their sites by signature, so they apply on either).
> This project is not affiliated with or endorsed by Sony Interactive Entertainment, Activision
> or Beenox. *Crash Team Racing*, *Nitro-Fueled* and *PlayStation* are trademarks of their
> owners, named here only to identify the game this software runs.

**Status: playable.** Menus, arcade and adventure races with sound, controller and saving; split
screen with a second controller; fur and the sky render correctly. 30 FPS as on the PS4, or the
60 FPS patch from the in-game menu (60 FPS in menus, 37–45 FPS in races on the Steam Deck). A
full play-through has not been verified, and the Steam Deck (RADV) is the only tested machine.

## How it works

- **Native execution.** The eboot is converted offline (`scripts/`) into a flat memory image
  with the game's own PS4 libc, libSceFios2 and libSceNpToolkit2 linked in as native code. No
  CPU emulation: the game code runs at full speed.
- **A runtime for this game** (`src/runtime_*.c`) implements the PS4 OS functions the game calls:
  memory, threads, synchronisation, event flags, files, audio (ATRAC9 included), pad, saves,
  user service, network stubs. Imports are resolved by NID and library name; the names come
  from shadPS4's aerolib list, fetched once at the first start.
- **GPU** (`gpu/`): the vendored shadPS4 video core and shader recompiler with bbport's
  multi-threaded command processing (decode and draw recording on separate threads) and
  SacredFire's fixes: geometry-shader ring addressing (fur), 1D textures lowered to 2D (the
  sky's colour LUT), texture overlap resolution at 1080p, PM4 stream resynchronisation, a
  reliable on-disk shader cache.
- **Game patches** from [illusionyy/PS-Game-Patch](https://github.com/illusionyy/PS-Game-Patch)
  (`patches/CrashTeamRacing.xml`, authors illusion and TroyWarez) are compiled at start:
  60 FPS unlock, motion blur off, 1080p in PS4 Pro mode (the published address of that one was
  off by one byte; our copy is corrected).

## Requirements

- Linux x86-64 with a Vulkan 1.3 GPU. Tested: Steam Deck (AMD Van Gogh, Mesa RADV). Geometry
  shaders are required (fur), so macOS/MoltenVK is out; see
  [docs/sacredfire/10-other-platforms.md](docs/sacredfire/10-other-platforms.md).
- Your decrypted dump of CUSA13795 with the 1.21 update merged in: copy the dumped update over
  the base game, replacing files (eboot.bin and sce_sys too). The launcher and `run.sh` check
  the executable image and say what is missing (`BB_SKIP_GAME_CHECK=1` skips the check).
- To build: GCC, CMake, Ninja, Python 3, glslang, SDL3, Vulkan headers and the libraries in
  `shell.nix` (with [Nix](https://nixos.org), `shell.nix` provides them). On Arch:
  `tools/deps.sh` fetches the header-only libraries the distribution lacks. FFmpeg is no
  longer needed: the AvPlayer video libraries are Bloodborne's and CTR imports none of them
  (`-DBB_AVPLAYER=ON` builds them again).

## Build and run

```bash
git clone --recursive https://github.com/T3CHNOLOG1C/SacredFire.git && cd SacredFire
bash build.sh                        # out/sacredfire and out/gpu/libbbgpu.so (~40 CPU-minutes)
BB_GAME_DIR=/path/to/CUSA13795 bash run.sh
```

`run.sh` prepares the image (`out/`), compiles the selected patches, builds if needed and starts
the game. The game folder defaults to `orig/CUSA13795`. Saves and the shader cache go to `user/`,
settings to `bbport.ini`; `BB_DATA_DIR` moves all of them. The first start compiles shaders as it
goes; later starts preload them from the cache (`BB_CLEAR_SHADER_CACHE=1` starts fresh).

Or the launcher (pick the game folder and the controller, then *Start*):

```bash
bash launcher/bb-launcher.sh         # launcher/install-desktop.sh adds it to the app menu
```

The launcher (GTK4/libadwaita) has pages for the game folder, the controller, the game patches,
PS4 Pro mode, display (fullscreen, FPS counter) and developer switches (frame statistics, log to
a file, clear the shader cache, Vulkan validation, extra environment), and shows the game's
output. `--play` starts the game with the saved settings and no window (Steam Deck game mode).
The menu entry and the launcher use the game's own icon (`sce_sys/icon0.png`), copied from your
game folder into your icon theme; the game window shows it too.

**In-game menu** (Insert, or L3+R3 on the pad): game patches, controller ports for split
screen, FPS counter, menu language. It is saved to `bbport.ini`; patches apply after a restart.

**Controllers.** SDL3 gamepads; the launcher's *Controller* page or `BB_GAMEPAD=<GUID or part
of the name>` picks the one for port 1, and the port switches to whichever pad was used last
when the chosen one is missing. Split screen: assign pads to ports 2–4 in the in-game menu (or
`port.N=<GUID>` in `bbport.ini`); each port logs a user in and out as the pad comes and goes.
The keyboard works next to a gamepad: WASD left stick, arrow keys right stick, Space Cross,
LShift Circle, E Square, Q Triangle, 1/3 L1/R1, R/F L2/R2, Z/C L3/R3, Enter Options, IJKL
d-pad; `key.<input>=` and `pad.<input>=` lines in `bbport.ini` rebind them.

**PS4 Pro mode** (`BB_NEO=1`, the launcher's switch): the game sees a PS4 Pro (7-CPU mode,
5568 MiB of direct memory) and takes its Pro rendering path; with the 1080p patch it renders
at 1920×1080. Off by default: the Steam Deck is closer to a base PS4.

**Mods:** loose-file mod folders (with the game's folders inside) are overlaid on the game
folder at start, with enable switches and load order in `mods.json`
(`scripts/mods.py`, [docs/MODS.md](docs/MODS.md), written for bbport). Further shadPS4-format
patch files go into `patches/` or `BB_PATCHES_DIR`, selected through `patches.json`.

## AppImage (Steam Deck)

`packaging/appimage.sh` builds `dist/SacredFire-x86_64.AppImage` (about 430 MB) with Nix: the
binaries are built in `nix-shell shell.nix` so the bundle can collect their libraries, then
`nix bundle` packs them with the GTK launcher, Python and Mesa's Vulkan drivers (the header of
the script has the exact steps). Running it opens the launcher; `--play` starts the game. Data
lives in `~/.local/share/sacredfire`, launcher settings in `~/.config/sacredfire-launcher`.
Without a game folder chosen, the launcher shows the Game row as *not chosen* with Play disabled
until a folder passes the game check.

**Adding the AppImage to Steam**: the launcher's *Add to Steam* button (header bar, also under *Settings → Steam*) writes the
shortcut (with `--play`, for the Steam Deck's game mode) while Steam is closed; by hand, *Add a
Non-Steam Game* needs no options. The compatibility tool does not matter. Steam's overlay is removed before the port's own programs start. Where
Steam runs games without FUSE, set the launch options to

```
TMPDIR=$HOME/.cache APPIMAGE_EXTRACT_AND_RUN=1 NO_CLEANUP=1 %command%
```

The AppImage then unpacks itself (~1.2 GB, `~/.cache/appimage_extracted_*`) on the first start
and reuses that copy; `TMPDIR` keeps that out of a RAM-backed `/tmp`. Add ` --play` after
`%command%` to skip the launcher. **NVIDIA:** startup discovers the host's 64-bit NVIDIA Vulkan
ICD and exposes its libraries next to the bundled AMD/Intel drivers (`BB_NVIDIA_LIB_DIR` when
they are not under `/usr/lib*`); `--vulkan-info` prints what the package sees. MangoHud is
bundled (launcher switch) and drawn once even when a system MangoHud or Steam's overlay is on.

## Useful variables

`BB_GAMEPAD` (controller for port 1), `BB_NEO=1` (PS4 Pro mode), `BB_FPS=30|60|uncap` (virtual
vblank; the 60 FPS patch sets 480 Hz and a 60 FPS limit by itself: the patch removes the game's
cap altogether, and above 60 FPS its physics misbehave, slopes bleed speed and karts stick on
ledges; the *Unlocked frame rate* switch of the launchers and the in-game menu, `BB_FPS=uncap` or
`BB_FPS_LIMIT=0` lifts the limit, `BB_FPS_LIMIT=<n>` sets another), `BB_FRAME_STATS=1` (frame statistics in the
log, including VRAM, GTT and RSS), `BB_CLEAR_SHADER_CACHE=1`, `BB_VK_VALIDATION=1` (validation
layer), `BB_FULLSCREEN=1`, `BB_PRESENT_MODE`, `BB_TIMEOUT=N` (stop after N seconds with thread
stacks; 0 = never), `BB_GAME_DIR`, `BB_USER_DIR`, `BB_DATA_DIR`, `BB_CONFIG`, `BB_PATCHES_DIR`,
`BB_SKIP_GAME_CHECK=1`, `BB_PAD_RECORD=file` / `BB_PAD_REPLAY=file` (record and replay input),
`BB_PRESENT_DUMP_TRIGGER=file` with `BB_PRESENT_DUMP_COUNT=N` (dump presented frames),
`BB_CAPTURE_TRIGGER=file` (frame analysis), `BB_BREADCRUMBS=0` (no GPU breadcrumbs),
`BB_GPU_PROFILE=1`, `BB_ANISO=N` (anisotropic filtering, 16 by default), `BB_PC_MODEL=1`
(bbport's experimental in-place memory model: frees CPU time but needs more GPU-visible memory
than the Steam Deck has). bbport's temporal upscalers (`BB_UPSCALER`, `BB_OBJECT_MOTION`,
`BB_UPSCALER_MENU=1` for their menu sections) were built for Bloodborne's scene and stay off.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | Loader (`probe.c`) and the runtime |
| `scripts/` | Offline preparation of the game image, module linking, game check, patch compiler, mods |
| `gpu/` | Renderer library: vendored shadPS4 video core (`gpu/VENDOR.txt`) with bbport's and SacredFire's changes, shims, ImGui menu |
| `patches/` | Community patches for CTR (`CrashTeamRacing.xml`) |
| `launcher/`, `packaging/` | GTK4 launcher and desktop entry; Nix package and AppImage; `packaging/windows/`: the Windows launcher, manifest, pinned inputs (`deps.lock`) and licence list |
| `src/platform/`, `cmake/` | Host services (Linux and Windows implementations of one header) and the llvm-mingw cross toolchain file |
| `tools/` | Developer tools: import-name generation, scripted runs, screenshots, A/B toggles, GPU checks |
| `tests/` | Loader, runtime, patch, game-check and renderer tests |
| `docs/sacredfire/` | SacredFire's notes: [status](docs/sacredfire/STATUS.md), setup, imports, Bloodborne gating and removal, fur, controllers, patches, other platforms |
| `docs/` | bbport's design notes and history ([upscaler](docs/upscaler.md), [parallel GPU](docs/parallel_gpu.md), [motion vectors](docs/motion_vectors.md), [roadmap](docs/ROADMAP.md)); its README: [docs/bbport/README.md](docs/bbport/README.md) ([Русский](docs/bbport/README.ru.md)) |

Tests: `bash build.sh --test`, `python3 -m unittest discover -s tests`, and
`ninja -C out/gpu motion-history-test ui-composition-test scene-resolution-test motion-shader-test settings-test`.

## Reporting problems and contributing

Use the bug report template; the log (launcher *Log* tab or `run.sh` output, with
`BB_FRAME_STATS=1` for rendering and performance issues) is what matters. If the log says
`SacredFire issue #2` (the launcher shows a notice with an *Open issue #2* button), the game hit
the known clobbered command buffer bug: please attach that whole log to
[issue #2](https://github.com/T3CHNOLOG1C/SacredFire/issues/2), it carries the diagnostics that
can find the cause. **No game files** in issues or pull requests. See
[CONTRIBUTING.md](CONTRIBUTING.md).

## Known limitations

- Races with the 60 FPS patch reach 37–45 FPS on the Steam Deck: the port's GPU command thread
  is the limit at ~1400 draws per frame ([docs/sacredfire/08-patches.md](docs/sacredfire/08-patches.md)).
- Online features are stubbed; the game runs offline.
- Windows: a native x86-64 build is cross-compiled from Linux (`scripts/build-windows.sh`,
  `scripts/package-windows.sh`, [docs/sacredfire/13-windows-port.md](docs/sacredfire/13-windows-port.md));
  its state on real Windows is recorded in
  [docs/sacredfire/13-windows-port-report.md](docs/sacredfire/13-windows-port-report.md). macOS is not
  supported ([docs/sacredfire/10-other-platforms.md](docs/sacredfire/10-other-platforms.md)).

## History

SacredFire started from bbport on 2026-10-09. The Bloodborne profile, its patches, launcher and
FSR 4 tooling were removed the same day
([docs/sacredfire/09-bloodborne-strip.md](docs/sacredfire/09-bloodborne-strip.md)). bbport's
temporal upscalers and motion vectors remain in `gpu/`, off; its libGnm hook code remains without
a function table (the table was generated from Bloodborne's executable and is not distributed).

## Credits and licenses

SacredFire is licensed under the **GNU GPL v2 or later** ([LICENSE](LICENSE)): it contains code
from bbport and shadPS4 (both GPL-2.0-or-later). Third-party components keep their licenses:
[bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28 (the loader, runtime and
renderer this builds on), [shadPS4](https://github.com/shadps4-emu/shadPS4) video core and shader
recompiler, [sirit](https://github.com/shadps4-emu/sirit), [half](https://half.sourceforge.net/),
[FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) by FireBurn (MIT), AMD FidelityFX SDK (MIT),
[LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) (MIT), [Dear ImGui](https://github.com/ocornut/imgui)
(MIT), DejaVu fonts, [miniz](https://github.com/richgel999/miniz) (MIT). Game patches by illusion
and TroyWarez ([illusionyy/PS-Game-Patch](https://github.com/illusionyy/PS-Game-Patch)). Import
names from shadPS4's aerolib list.
