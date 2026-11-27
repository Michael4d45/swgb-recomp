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
  unchanged (`mp_pair.py` with it on one side only: 86 of 86 records equal).
  `--record-fps N`; `SWGB_SMOOTH_LOG` and `tools/smooth_check.py`.
- `--hud-sides fill` (the default) beside the interface at a `--res` size:
  each row continues the art's outermost columns mirrored, in the
  civilization's colours; `--hud-sides black` keeps the bars.
- Screen Size switched in a game, from the game menu's Options, at every
  pair of sizes and `--res` WxH (`res_check.py ingame`); Save Game and Load
  at WxH laid out at the largest of the game's sizes that fits, centred
  (`res_check.py popups`). The old lead "after an in-game switch to
  1024x768 the Menu button no longer answers" did not reproduce.
- The in-game music: MCI `cdaudio` over the Steam release's
  `Music\TrackNN.ogg` (stb_vorbis), the mixer's CD line as the music slider,
  on both hosts (`src/runtime/cdaudio.c`, `--music off|native|DIR`).
- `--record` has the sound: a proxy over DirectSound (`recaudio.c`) mixes the
  game's buffers again with the films' PCM on the video's clock, cut to the
  video's length and muxed in.
- Every campaign: `soak.py --mode missions` plays any campaign's missions in
  a row from a table of mission boxes, `ingame-load`, `--screen 1024|1280`.
  57 missions won headless at least once (the six base campaigns and both
  Episode II ones), 13.5 game-hours of soak with no unresolved call, fault
  or stub.
- Multiplayer: three and four players, chat, a joiner dropped, the host
  dropped (host migration, `DPSESSION_MIGRATEHOST`), quit and resign, a
  multiplayer save restored; `--mp-cross` lets a Windows and a Linux host
  play together (15 minutes of three players in sync). `mp_pair.py`
  `--players`, `--chat`, `--drop`, `--quit`, `--resign`, `--save
  --restore`, `--wsl`; `test.py` runs short games at the functional level
  and long ones at the soak level.
- Linux: `./setup.sh` does the whole setup natively (tools through apt when
  asked, a venv, the Steam copy or a disc install, catalog, lift, build,
  `play.sh`), tested from a fresh clone in WSL2 Ubuntu 24.04; pcrecomp
  pinned in `tools/pcrecomp_pin.sh`; Enter in text fields as on Windows
  (`test.py enter/*`).
- `docs/building.md`: one table of every environment variable the hosts and
  tools read, and which pcrecomp each host builds against;
  `tests/unit/test_tools.py` fails when a variable the sources read is
  missing from the table.

### Changed
- The README's status brought up to date (the lift, campaigns, Linux,
  multiplayer, tests) and its documentation list completed.
- The lifted C is built at `/O2` by default (was `/O1`; `-DGEN_OPT=/O1`
  keeps it): a late game with 7 computers is 8-11% cheaper a frame; the
  build takes 34 minutes against 30.6 and 6.8 GB of compilers at once
  against 5.5. Builds at `/O1` and `/O2` stay in sync with each other (25
  minutes, 312 records, none differ). The native host stays at `-O1`.
- `ROADMAP.md` lists only what is next; its dated journal moved here and
  into git history. `docs/testing.md` and `docs/multiplayer.md` keep the
  reference and drop the dated lab notes. `CONTRIBUTING.md` names the real
  merge gate (`test.py --level functional --cc --retry 1`) and the review's
  habits. The lift's size is 5.4M lines everywhere (it said 3.9M in places).

### Fixed
- Linux host: the minimap was black and the terrain plain (the game's temp
  folder did not exist: each run now has its own as `T:`); the game stopped
  drawing after a chat line (win32hle's `DestroyWindow` now moves the focus
  first); the game's cursor was not drawn (`GetClipper` now returns
  `DDERR_NOCLIPPERATTACHED`). The Linux host computes doubles in SSE2, as
  MSVC does: with the x87 they rounded differently.
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
