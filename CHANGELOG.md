# Changelog

All notable changes. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- `--fps N`: the in-game frame rate, the host waiting between frames
  (9% of a core at 60 against 98%); game speed unchanged from 10 up.
- `--fps-log`: frames, changed frames, world steps and the game clock a
  second ([docs/framerate.md](docs/framerate.md)); `rclick:X:Y` in `--keys`.
- `--smooth`: units drawn between simulation steps, so they glide at high
  frame rates instead of moving once a step; drawing only, multiplayer sync
  unchanged. `--record-fps N`; `SWGB_SMOOTH_LOG` and `tools/smooth_check.py`.

## [0.1.0] - 2026-11-20

- Builds against pcrecomp main; the `pcrecomp-ingame` branch is merged.

### Changed
- Brought in line with the other recomps: the README in their order (Getting
  Started, Status, Screenshots, Documentation, Contributors, License), the
  build steps and command line moved to `docs/building.md`, `NOTICE` renamed
  `NOTICE.md` and saying what the MIT license covers and what it doesn't.
- Setup and the launcher fetch pcrecomp on `pcrecomp-ingame` (was `pcrecomp-msvceh`, which lacks the headless desktop).

### Added
- `CONTRIBUTING.md`, `CONTRIBUTORS.md`, `docs/building.md`, `docs/testing.md`.
- `setup.sh`, which says Linux and macOS are not supported yet and stops.
- `.github/workflows/installer.yml`, the recomp-launcher installer for
  Windows, run by hand only (`workflow_dispatch`).

### Added
- Lift driver for `Battlegrounds.exe` v1.1 (`run_lift.py`): whole program,
  14,273 functions, 0 lift errors, 4.0M lines of C, with MSVC exception
  frames (needs pcrecomp branch `pcrecomp-msvceh`, now inside `pcrecomp-ingame`).
- Host on pcrecomp native32 (`src/runtime/host.c`): scripted mouse and
  keys (`--keys`), headless runs
  with `--record`, `--shot`, `--watchdog`, `--files`, muted test runs.
- `softdd`: X-Wing's software DirectDraw without Direct3D; 8-bit modes from
  800x600 to 1280x1024 enumerated, 8-bit recording through the palette.
- The game's deflate decoder done by the host with miniz
  (`src/runtime/inflate.c`; the original is a stack-copying coroutine).
- `tools/bgupdate.py`: unpacks LucasArts' `BGUpdate1_1.exe` without running it.
- `Setup.cmd`, launcher manifest, `tools/conformance.py` (5 milestones, 5/5),
  netlab recipe (`projects/swgb-recomp.env` in recomp-netlab).
- Reaches the main menu; hover and the Exit button work.
- In game: a random-map skirmish starts and plays headless (dialogs painted on
  the private desktop, scripted keys into the focused edit control, the
  `THIS_COD` blitter's jump tables
  seeded).

### Changed
- The CD check runs against a real CD-ROM drive: disc 2 in the drive or its
  image mounted (`CD_DRIVE` or `--cd E` names the drive; the host stores that
  letter in `CDPath`). Setup no longer copies disc 2 into `cd\`; it mounts an
  `.iso` or `.cue`/`.bin` of it and leaves it mounted.

### Fixed
- The lift read only `.text`, so functions in `THIS_COD` were empty bodies
  that never popped their return address (docs/lifting.md).
