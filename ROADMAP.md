# Roadmap

What is next. Where things stand is the [README](README.md#status)'s table;
what changed, and when, is the [CHANGELOG](CHANGELOG.md). Each item names
the page with its details.

## Next

- **Play by hand on the real screen.** Everything so far is headless and
  scripted. A skirmish against the AI with a real mouse, `--smooth` (then
  make it the default if it holds up), and the windowed and fullscreen
  checks a script cannot do ([framerate.md](docs/framerate.md),
  `tools/present_check_real.ps1`).
- **A LAN game between two real machines.** Every multiplayer test is on one
  machine over loopback. Needs a firewall Allow rule (TCP and UDP 2300-2303)
  for the hosting exe; shows what one machine can't: different CPUs (does
  the CRT take another path?), real latency and loss, a pulled cable (the
  30 s drop) ([multiplayer.md](docs/multiplayer.md#whats-left)).
- **One pcrecomp for both hosts.** Upstream the fork's `swgb-linux-palette`
  branch, then move `tools/pcrecomp_pin.sh` to upstream `main` (the
  maintainers' and the user's call; [building.md](docs/building.md)).
- **macOS.** `build-macos.sh` is written and untried: it needs a Mac, a
  64-bit based build, and guest-pointer conversion in `dplay.c`
  ([linux-macos.md](docs/linux-macos.md#9-macos)). About a week.

## Known bugs

- **Linux host, multiplayer:** chat lines and the Multiplayer Status dialog
  are not drawn, though the game takes them in; the unit panel shows
  `UNIT-WORKERA1` where Windows shows the unit's name, so a string lookup
  may be missing. Two Linux hosts can't start a game without the play disc
  (the setup screen shows version `1.1`, not `CD 1.1`); with a Windows
  player in the game it starts ([multiplayer.md](docs/multiplayer.md#windows-and-linux)).
- **Linux host, soak:** `fight` razes the enemy's base but the game declares
  no victory; `soak.py --mode victory` fails after the save dialog (the
  first Enter only destroys the dialog's leftover field)
  ([testing.md](docs/testing.md#on-linux-and-macos)).
- **A rare fault at the base game's film start** (once, at `0x75B773FE`
  while opening `xlogo1.avi`; not seen in five reruns).
- `softdd`'s `GetMonitorFrequency` always says 60.

## Left, by area

- **Presentation:** remember the window size, filter and fullscreen between
  runs. The CPU scaler costs 3 ms (1080p) to 11 ms (4K) a frame for
  `sharp`/`smooth`; `--present d3d11` scales on the GPU but costs the same
  CPU as change-only GDI here ([performance.md](docs/performance.md)).
- **Widescreen:** switching the viewed player (`sub_0042F880`, debug and
  scenario tests) reloads the art by the screen's real size; the full
  screens no check opens (a campaign's, multiplayer's) may need a look at
  WxH.
- **Frame rate:** sprite animation frames still change once a simulation
  step; a smaller minimum step (the `0x32` at `0x0061ACAA`, single player
  only) is untried ([framerate.md](docs/framerate.md)).
- **Films:** disc 2's own films (`logo1`, `intro`, the campaigns' `INTRO`
  and `celebration`) are untested without the disc.
- **Music:** Clone Campaigns with its music on (no test profile has its
  slider up).
- **Recording:** the mouse cursor (the game draws its own from `mcursors`)
  in recordings; `--record` on Linux has no sound
  ([recording.md](docs/recording.md#not-done)).
- **Coverage:** `soak.py --screen` for Clone Campaigns; a monkey that
  defends itself (Hardest computers defeat it in 11-20 minutes); the
  monkey in a multiplayer game. For pcrecomp's catalog: a vtable run in a
  data section should outrank a decoded body, and switch index tables
  should be data ([lifting.md](docs/lifting.md#pointers-the-catalog-dropped)).
- **Performance:** callbacks into the game's window procedure each go
  through an exception (native32's VEH, pcrecomp). `/GL` and PGO are
  untried. The native host's whole lift at `-O2` is unmeasured
  ([linux-macos.md](docs/linux-macos.md)).
- **Multiplayer:** a long Windows-Linux game, and one libm for both hosts if
  it ever goes out of sync; computer players in a multiplayer game; chat in
  the setup screen; a game played to its end; a host migration in the setup
  screen (the new host does not answer probes yet); a restore with a player
  missing; answering the startup version check and the ordinal imports
  without `dplayx.dll` on disk; playing the original exe
  ([multiplayer.md](docs/multiplayer.md#whats-left)).
- **Tests and tools** (deferred from the 2026-11-27 code review):
  split `tools/test.py` into modules and build the C units with CMake and
  `ctest`; scripted runs waiting on `--keys on:NAME` instead of fixed
  load times; a CI check for a helper defined twice; a test that diffs
  each host's flags against its usage text. netlab QA on a test VM (it
  reports SKIP without game data).

## Out of scope

- Shipping any game file or generated source.
- The Zone.com service, which no longer exists.
