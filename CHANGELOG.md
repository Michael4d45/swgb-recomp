# Changelog

All notable changes. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- `--fps N`: the in-game frame rate, the host waiting between frames
  (9% of a core at 60 against 98%); game speed unchanged from 10 up.
- `--fps-log`: frames, changed frames, world steps and the game clock a
  second ([docs/framerate.md](docs/framerate.md)); `rclick:X:Y` in `--keys`.
- The game's three resolutions checked in a skirmish (`tools/res_check.py`);
  with `--multi`, a private copy of the game's HKCU options per `--game`
  folder; the window takes each mode's shape.
- `--res WxH` and `--res desktop`: any resolution, widescreen included (the
  map fills the screen, the interface centred), `src/runtime/widescreen.c`.
- A presenter (`present.c`): aspect-correct letterbox, `--filter
  sharp|smooth|nearest|integer` (Alt+F12), borderless `--fullscreen`
  (Alt+Enter), presents paced to the display; `--present d3d11`, opt-in;
  `tools/present_check.py` and `present_check_real.ps1`.
- The films: MCIWnd answered by the host, Indeo 5 decoded by FFmpeg, with
  sound, `--films DIR` ([docs/movies.md](docs/movies.md)).
- Clone Campaigns: a second lift (`run_lift.py --build cc`) and host,
  `swgbrecomp_cc.exe`, on the same runtime (`src/runtime/guest_addrs.h`).
- Performance ([docs/performance.md](docs/performance.md)): a sampling
  profiler (`--profile`, `tools/profile_report.py`), menus that wait for
  input, the display's refresh as the default frame rate, an indirect-call
  cache (`resolve.c`), a change-only presenter and a faster scaler
  (`scaler.c`), the four hot generated files at `/O2`.
- Coverage: `tools/code_ptr_seeds.py` (19 entries the catalog dropped), an
  unresolved indirect call stops the run and names the address (exit 5),
  `tools/seeds_from_logs.py`, `tools/soak.py` (long games, games to the
  end, campaigns, `--mode fight`); `--keys` drags, `ctrl+`/`shift+` and
  `--keys-live`.
- Multiplayer (LAN): a DirectPlay 4 of the host's own (`dplay.c`, TCP and
  UDP discovery on 2300; `--dplay native` keeps Windows'), `[sync]`
  checkpoints and `tools/mp_pair.py` ([docs/multiplayer.md](docs/multiplayer.md)).
- `tools/test.py`: one test runner in levels (`unit`, `smoke`,
  `functional`, `soak`, `real`), reports in Markdown and HTML, `--retry`
  (a failed game check run again: FLAKY), `--prefix`; `keyscript.c` (the
  `--keys` parser) with a unit test.
- Linux: a native host on pcrecomp's `win32hle` (`src/linux`,
  `cmake/native.cmake`, `./build-linux.sh`), no Wine; it plays, and the
  harness, `test.py`, `soak.py` and `mp_pair.py` run there
  ([docs/linux-macos.md](docs/linux-macos.md)).
- `docs/parallel-work.md` and `tools/worktree_env.ps1`: several worktrees on
  one machine, linking one prebuilt `swgb_gen.lib`; `--multi`.

- `--smooth`: units drawn between simulation steps, so they glide at high
  frame rates instead of moving once a step; drawing only, multiplayer sync
  unchanged. `--record-fps N`; `SWGB_SMOOTH_LOG` and `tools/smooth_check.py`.
### Changed
- The README's status brought up to date (the lift, campaigns, Linux,
  multiplayer, tests) and its documentation list completed.

### Fixed
- `test.py`: a missing baseline (a shot, a `--res` layout, the `perf/*`
  figures, the lift's hashes) recorded itself and passed, so a new machine's
  first gate passed whatever it drew. It is now **NEW**: not compared, listed
  first, and it fails `functional` and up unless `--update-baselines`; this
  run's goes only into the check's folder. One helper, `baseline()`, for all
  four.
- `test.py --retry`: a failed check that passed on its retry was FLAKY, which
  passes, whatever the failure was. Only a timing failure (killed past its
  time limit, the watchdog, no crash) is retried into FLAKY now; a wrong
  picture, a fault or an unresolved call stays FAIL. NEW and FLAKY checks
  head the report ([docs/testing.md](docs/testing.md), "Flaky checks").
- `test.py --retry`: a retry ran in the first try's folder and overwrote its
  `run.log` and shots (and could pass on a shot the first try left). Each
  retry now has a folder of its own, `<check>-retry1`, and the report lists
  every try with its folder, lines and shots.
- softdd: text in 8-bit surfaces' DCs in the palette in use; the game's
  EDIT controls composed into the frame with a caret, no ghost box, focus
  kept across a trip to another window; edge scrolling in a window.
- A THIS_COD span blitter that reads the byte before the back buffer
  faulted long games (13 and 25 minutes): softdd puts 4 KB of slack around
  every surface.
- `0x005648A0`, a vtable slot the catalog lost, ended skirmishes about two
  minutes in.

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
