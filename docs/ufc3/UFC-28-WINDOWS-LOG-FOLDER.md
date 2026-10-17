# UFC-28 · Windows launcher: log folder picker + diagnostic package without .cmd

Two changes in one file, `packaging/windows/sacredfire_launcher.c` (the native Win32
launcher, `SacredFire.exe` / `UFC3.exe`). No runtime, no GPU build, no new reverse
engineering.

## 1. Log folder (the shared HDD case)

The launcher window has a new **Log folder** row (edit box + Browse..., the same
IFileOpenDialog folder picker as the game folder):

- Empty (the default): the logs go to `<data>\logs` as before.
- A path: every log of the run — the run's `<time>.log`, `launcher.log`, and the frame
  statistics CSVs — goes there. Spaces, Unicode, mapped drives and UNC shares
  (`\\server\share\...`) are handled (UTF-16 at the Win32 calls, `GetFullPathNameW` to
  make it absolute, `ensure_directory` to create it).
- `BB_LOG_DIR` set before starting the launcher wins over the window's folder.
- The choice is kept in `sacredfire.ini` (`log_dir=`), saved on Play / Add to Steam /
  close like the rest of the settings.
- After picking a folder with Browse... the launcher reopens `launcher.log` there at
  once, so the change is visible without restarting.
- If the folder cannot be written (an unreachable share, a removed drive), the log
  falls back to `<data>\logs` with a clearly visible note in the log window; the game
  still starts. DATA/USER/CONFIG, saves and the shader cache are not touched: only the
  logs move.
- The "Open logs folder" button opens the effective folder.
- With **Frame statistics** on, `BB_FRAME_LOG` / `BB_READBACK_LOG` are set for the
  session to `<logs>\<time>.frames.csv` / `.readbacks.csv` (as run.sh does); a value
  set before the launcher wins.

Known limitation: a UNC path works when the share itself exists and is reachable;
`ensure_directory` creates subfolders below it but cannot create the share. An
unreachable share triggers the local fallback above.

## 2. Diagnostic package without the .cmd (`UFC3_DIAGNOSTIC_PACKAGE`)

Compiling the same file with `-DUFC3_DIAGNOSTIC_PACKAGE` makes the diagnostic variant
that replaces `UFC3-DIAGNOSTIC-UFC2.cmd`:

- Data (settings, bbport.ini, generated files, saves, shader cache, logs) defaults to
  `diagnostic-data` beside the exe instead of `%LOCALAPPDATA%\UFC3Native`
  (`BB_DATA_DIR` still overrides).
- Every run forces `BB_WRITE_STEP=1`, `BB_WRITE_WINDOW=1`, `BB_WRITE_WINDOW_DIAG=1`,
  `BB_WRITE_LOG=0` (the normal defaults keep readbacks 2 and the rest of the runtime
  base).
- The window title says "Windows diagnostic test" and the log says what is forced.

The normal build (without the macro) is unchanged.

## Validation

Built on Linux with the pinned llvm-mingw toolchain (the commands of
`scripts/build-windows.sh`, target `sacredfire` only — no GPU build), both variants,
`-Wall -Wextra -Werror` clean. Not run on real Windows here; the user runs it by hand.
