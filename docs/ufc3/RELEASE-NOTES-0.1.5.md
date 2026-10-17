# UFC 3 Native Port — Release Notes 0.1.5

## Performance

- Write windows are back on in Windows: career fights go from ~30 to ~47 FPS on an RTX 3060 laptop.
- Single-stepped writes and pre-upload are on in both Windows and Linux.

## Graphics

- Blood and sweat on hits are back (missing since 0.1.3).

## Crash and freeze fixes

- Fixed a GPU hang in the career fighter editor (UFC-48).
- Possible fix for AMD freezes/closes (late EOS label guard, UFC-47). AMD users: please report whether this helps.

## Launchers

- New Windows launcher: UFC 3 dark/gold theme, log folder setting, and host/join for online play.
- New Linux GTK launcher with offline / join / host modes.

## Online

- Rematch now works.
- You can play outside your LAN with a VPN such as Tailscale. See [HOW-TO-PLAY-ONLINE.md](HOW-TO-PLAY-ONLINE.md).

## Experimental

- 2K (2560x1440) and 4K (3840x2160) internal resolution in the launcher. Requires the in-game setting VIDEO → RESOLUTION → Favor Resolution. Validated on Linux only.

## Known issues

- Fights on Windows still run below Linux FPS.
- The "Clear shader cache" option recompiles everything on each start — leave it off.
