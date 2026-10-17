# UFC 3 native port — Linux

Runs **EA SPORTS UFC 3** (PS4 `CUSA06534`, update **1.14**) natively on Linux. Built from the
source at the commit in `VERSION` (https://github.com/GNSB/UFC-3-PS4-PORT-LINUX-WINDOWS).
**No game files are included**: you need your own dump of the game and its 1.14 update.

## Requirements

- x86-64 CPU with AVX, a GPU with a Vulkan 1.3 driver (Mesa RADV/ANV or NVIDIA).
- glibc 2.38 or newer (Ubuntu 24.04+, Debian 13+, Fedora 39+, Arch, SteamOS 3.6+) and
  `python3`. SDL3, libstdc++ and the other libraries are bundled in `bin/lib`.

## Playing

```bash
tar xf UFC3Native-*-linux-x86_64.tar.gz && cd UFC3Native
./ufc3.sh /path/to/PS4/CUSA06534      # first time; then just ./ufc3.sh
```

The update folder next to the base game (`CUSA06534-patch`, `-PATCH`, `-update`…) is merged
into `CUSA06534-merged` with hard links. Data, saves and logs go to
`~/.local/share/UFC3Native` (`BB_DATA_DIR` changes it).
Set `BB_LOG_DIR` to place the log files elsewhere, independently of saves and settings, for
example: `BB_LOG_DIR="$HOME/UFC3-logs" ./ufc3.sh`.

## Settings

- `~/.local/share/UFC3Native/ufc3.conf`: fullscreen, FPS limit, present mode, 2K/4K window
  (`BB_WINDOW_SIZE`) with FSR 1.0 + RCAS, player name, monitor.
- Controls: `bbport.ini` in the same folder, lines `key.<input>=<key>` and
  `pad.<input>=<button>`. Inputs: cross circle square triangle l1 r1 l2 r2 l3 r3 options
  touchpad touchpad_right up down left right, and for the keyboard move_up/down/left/right
  (left stick) and look_up/down/left/right (right stick). Keys use SDL names (`Space`,
  `Left Shift`, `E`, `Up`…); buttons `a b x y leftshoulder rightshoulder lefttrigger
  righttrigger leftstick rightstick start back dpup dpdown dpleft dpright`. Example:
  `key.cross=Return`, `pad.r2=rightshoulder`.
- In game: **Insert** or **L3+R3** opens the overlay menu.

Problems: attach the newest log from `~/.local/share/UFC3Native/logs`.
GPL-2.0-or-later; not affiliated with Electronic Arts, Sony or the UFC.

## Graphical launcher and private online play

Open `UFC3.sh` (mark it executable if your file manager asks), or run `./ufc3.sh` without arguments. The GUI requires Python 3, PyGObject and GTK 4 (`python-gobject gtk4` on Arch; `python3-gi gir1.2-gtk-4.0` on Debian/Ubuntu). The command line remains available: `./ufc3.sh --play /path/to/CUSA06534`, or the existing positional game-folder command.

Choose the game folder once. The launcher remembers the folder, player name, language, video settings and online mode in `~/.local/share/UFC3Native/launcher.json` (or `BB_DATA_DIR`). Existing `ufc3.conf` and `game-folder` settings are imported on first use. Environment variables retain priority; remove `BB_ONLINE_SERVER` when using the GUI's Offline mode. The in-game menu still provides controller and advanced graphics settings.

- **Offline**: play locally.
- **Join server**: enter the address shared by the host.
- **Host server**: select this PC's LAN IP, or VPN IP for a friend outside the LAN, and click **Play**. Share that IP. The launcher starts and checks the server, then starts the game. The server stops with the session, Stop, or closing the launcher.

For Internet play both PCs need a VPN such as Tailscale, installed separately. Both games must use the host's VPN address, including the host's own game; do not use `127.0.0.1`. Use the same game build on both PCs, choose compatible Quick Fight settings and search at nearly the same time. VPN Internet play still needs validation outside a LAN. No public matchmaking service or automatic router configuration is included.

Server and game logs are under `~/.local/share/UFC3Native/logs/`. An occupied port is reported instead of taking over another server. Allow the game and Python server through the firewall on your LAN/VPN interface.
