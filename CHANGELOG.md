# Changelog

All notable changes to dcrecomp. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versions: [SemVer](https://semver.org/). No release has been tagged yet.

## [Unreleased]

### Changed
- Relicensed MIT -> GPL-2.0. The HAL's PVR2/Holly/AICA handling and the Maple
  DMA handler are derived from Flycast (GPL-2.0), so MIT was never accurate.
  `NOTICE` lists what is derived.
- Default branch renamed `master` -> `main`.
- Per-title entry points moved out of `static_recompile.py` into a game-repo
  `--entries` file; `sh4_disasm.py` now uses the recompiler's function
  discovery (#2, raised in #1).

### Fixed
- `static_recompile.py` crashed writing `dispatch_table.c` to an output
  directory that did not exist yet (#2).

### Removed
- `docs/flycast-extraction.md`: described a vendored `flycast/` tree that was
  removed in August. The plan is in `ROADMAP.md`.
