# EA SPORTS UFC 4 on PC (experimental)

This project adapts the open-source PS4 compatibility runtime used by the UFC 3 PC port to run a user-owned, legally obtained dump of **EA SPORTS UFC 4** on Windows. It does not include the game, updates, DLC, PlayStation firmware, saves, or game assets.

## Status

The current local build (`0.0.11-savebackups`) was tested offline. A Fight Now match reached KO. Career allowed character creation, a first fight, saving, reloading, and continuing training. The project owner confirmed that the renderer integrated from UFC3Native v0.1.5 corrected black opponents and the hair rendering problem observed in Career. Version 0.0.11 adds automatic save backups. Full Career, all fighters and arenas, DLC, long sessions, and timing at the 100 FPS cap have not been fully validated. Treat this as experimental software.

## Build and run

Download the Windows ZIP from [Releases](https://github.com/schans-arg/UFC4-PC-Port/releases), extract the complete archive, and open `UFC4.exe`. On first launch, select `eboot.bin` from your own extracted UFC 4 copy. The measured profile is CUSA14209 v28.00 (base and update combined). Python and runtime dependencies are included. New caches, logs, saves and save backups are created locally.

See [BUILD-COMMUNITY.md](BUILD-COMMUNITY.md) for the measured donor runtime, extension build and portable launcher commands. This preview uses the UFC3Native v0.1.5 binary renderer plus the UFC4 extension; a complete independent renderer rebuild is still pending. Inherited UFC 3 build scripts are reference material. Never upload game files, extracted content, saves, private logs or compiled game images.

See `docs/PORT_STATUS.md` for tested behavior, outstanding issues, and release checklist.

## License and attribution

The runtime and integrated renderer are distributed under GPL-2.0; see `LICENSE`, `THIRD_PARTY_NOTICES.md`, and file-level notices. Third-party components retain their own licenses and notices. The project incorporates the renderer from UFC3Native v0.1.5; runtime packages preserve the upstream licenses and identify the donor and extension hashes.

This is an independent community project. EA SPORTS, UFC, PlayStation, and related marks belong to their respective owners. This project is not affiliated with or endorsed by EA or Sony. The source license grants no rights to the game or its assets.

## Contributions

Please report the exact game edition/update, Windows build, CPU/GPU/driver, runtime version, steps to reproduce, and relevant logs with personal paths and account identifiers removed. Never attach a game dump or save file to an issue.
