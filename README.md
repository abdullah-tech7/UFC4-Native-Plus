# EA SPORTS UFC 4 on PC (experimental)

This project adapts the open-source PS4 compatibility runtime used by the UFC 3 PC port to run a user-owned, legally obtained dump of **EA SPORTS UFC 4** on Windows. It does not include the game, updates, DLC, PlayStation firmware, saves, or game assets.

## Status

The current local build (`0.0.11-savebackups`) was tested offline. A Fight Now match reached KO. Career allowed character creation, a first fight, saving, reloading, and continuing training. The project owner confirmed that the renderer integrated from UFC3Native v0.1.5 corrected black opponents and the hair rendering problem observed in Career. Version 0.0.11 adds automatic save backups. Full Career, all fighters and arenas, DLC, long sessions, and timing at the 100 FPS cap have not been fully validated. Treat this as experimental software.

## Build and run

This is an early source snapshot adapted from a UFC 3 runtime. Some upstream scripts and documents still use UFC 3 terminology, and the build/release flow needs a clean-room review before a community release. Use `scripts/build-windows.sh` as a starting point only after reviewing its requirements and paths. You must provide your own game dump locally. Never commit game files, extracted content, saves, local logs, screenshots containing private information, or compiled game packages.

See `docs/PORT_STATUS.md` for tested behavior, outstanding issues, and release checklist.

## License and attribution

The runtime and integrated renderer are distributed under GPL-2.0; see `LICENSE`, `THIRD_PARTY_NOTICES.md`, and file-level notices. Third-party components retain their own licenses and notices. The project incorporates the renderer from UFC3Native v0.1.5; attribution and notice obligations for that component need final review before the first public release. See `THIRD_PARTY_NOTICES.md`.

This is an independent community project. EA SPORTS, UFC, PlayStation, and related marks belong to their respective owners. This project is not affiliated with or endorsed by EA or Sony. The source license grants no rights to the game or its assets.

## Contributions

Please report the exact game edition/update, Windows build, CPU/GPU/driver, runtime version, steps to reproduce, and relevant logs with personal paths and account identifiers removed. Never attach a game dump or save file to an issue.
