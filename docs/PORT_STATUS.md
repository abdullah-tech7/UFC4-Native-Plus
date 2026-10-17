# Port status and release checklist

## What has been verified by the project owner

- Offline Fight Now completed through KO.
- Career character creation, first fight, save, reload, and training continuation worked in the tested build.
- Version 0.0.10 integrated the UFC 3 v0.1.5 renderer. The owner confirmed that opponent skin and hair rendering then looked correct.
- Version 0.0.11 retained that renderer and added save backups.
- Spanish menus/subtitles, 1920×1080 fullscreen, and a 100 FPS cap were configured; 100 FPS timing has not been exhaustively validated.

## Known gaps

- Full Career/campaign completion has not been validated.
- Every fighter, arena, DLC, and extended sessions have not been tested.
- The base source documentation and build flow still need UFC 4-specific cleanup before a community-ready release.
- The exact tested game edition/update and supported hardware matrix need to be published from reproducible local metadata.

## Before creating a tagged release

1. Rebuild from this repository using a clean checkout and document exact build dependencies/commands.
2. Verify no game assets, private save data, local logs, absolute user paths, binaries, or extracted dumps are tracked.
3. Confirm bundled third-party licenses and notices.
4. Run the smoke-test matrix and record results, including rollback/save-backup behavior.
5. Tag a source release only after the source state and release notes match the tested build. Do not attach a prebuilt game package.
