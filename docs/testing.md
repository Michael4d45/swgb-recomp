# Testing

Every check is headless unless it says otherwise: `--headless` runs on
pcrecomp's private desktop (`native32_headless`), which is never shown, and
mutes the sound. `tools/test.py` runs them all, in levels; the sections after
it describe each tool on its own.

## Test levels: `tools/test.py`

```
py -3 tools\test.py --level unit|smoke|functional|soak|real [--build DIR] [--cc|--no-cc] [--jobs N]
                    [--only PATTERN,...] [--just] [--list] [--update-baselines] [--results DIR]
```

| Level | Name | Time | What it runs | Needs |
|---|---|---|---|---|
| L0 | `unit` | 25 s | The host's C units, built with `cl` and run (below), and `tests/unit/test_tools.py` (`tools/swgb`'s screens, paths and game folders, soak.py's scripts and log reading, `lift_check.py`, `res_check.py`'s places and shot readers, this runner's image and `--fps-log` readers) | Visual Studio, a pcrecomp checkout (its headers only). No game, no lift. |
| L1 | `smoke` | 2-2.5 min | L0, then for each game (base and Clone Campaigns): `conformance.py`; menu -> Standard Game -> a skirmish with four shots (menu, setup, map, command center selected), no unresolved call, fault or stub, 800x600 first; the lift check (below) | the game data, the host(s), the lift for the lift check |
| L2 | `functional` | 18-20 min at 5 games at once (12 for the base game alone) | L1, then `present_check.py` groups A-E (one game each, in parallel), `res_check.py` 800/1024/1280, its `ingame` switches (both games), `popups` (1024x768, 2560x1440; Clone Campaigns at 1920x1080) and `load` (both games at 2560x1440: below), `--res` from Options (1920x1080 for both games, with `--hud-sides black` too, 3440x1440, Clone Campaigns at 2560x1440: below), `--films` (logo and intro through FFmpeg, frames shown and dropped), `--record` with its sound (a film, a skirmish's clicks, the victory music; the sound stream as long as the video, loud where they play: [recording.md](recording.md)), a short skirmish per set of civilizations (all six, all eight of Clone Campaigns) with a save, then that save loaded from Single Player -> Saved Game, Enter in both text fields (`enter/*`: New Player accepted with Enter in a folder with no profile, then a skirmish won by the chat's Enter and `skywalker`), the CD music (a skirmish on a disc of 2-second tracks: tracks 2-49 on time, the notification, the game's repeat; [music.md](music.md)), the frame-rate check (below), and two multiplayer games through `mp_pair.py`: three players with chat and one dropped (base game), a save restored (Clone Campaigns) | as L1, ffmpeg |
| L3 | `soak` | about 4 h at 5 games at once (20 game-hours) | L2, then `soak.py` for both games: base skirmish, monkey, campaign, defeat, allied, victory then load, chain (OOM-9 1-3), fight (a victory by combat), every campaign's missions past the first (`missions`: OOM-9 6-7, Boss Nass, Chewbacca, Darth Vader, Princess Leia, Basic Training), the in-game Load, the monkey at 1024x768 and 1280x1024; Clone Campaigns skirmish (two civilization sets), monkey, both Episode II campaigns from mission 1 and past it, the in-game Load. Multiplayer (`mp_pair.py`): four players for 20 minutes with chat, a quit, the host dropped (host migration), a save restored, in both games. `--soak-scale 0.2` for a shorter trial. | as L2 |
| L4 | `real` | ~5 min | `present_check_real.ps1`: the real window, mouse and screen. **Only with `--real-screen`**, which means the user said so: it takes over the mouse and keyboard. | the user at the screen |

A level runs the levels below it (`--just`: only its own). Clone Campaigns is
tested when its host is in `--build` (`swgbrecomp_cc.exe`); `--cc` makes a
missing one a failure, `--no-cc` leaves it out. `--list` prints the checks.

**Before a merge** (the gate): `py -3 tools\test.py --level functional` on the
merged build, exit 0. **Nightly**: `--level soak` (`tools\test_nightly.ps1`;
`-Register` makes it a daily scheduled task, which only the user decides).
**After a re-lift**: `--level smoke` at least: the lift check says whether the
C changed.

**Where things are.** Every default path of the tools comes from one function,
`paths()` in `tools/swgb/runner.py` (`py -3 tools/swgb/runner.py` prints
them):

| Set | The defaults |
|---|---|
| `SWGB_STAR`, or `"star"` in `tools/local.json` (git ignores it; a worktree reads its main checkout's) | a shared workspace, laid out as [parallel-work.md](parallel-work.md) describes: `STAR/swgb-recomp` (the lift, `work/*.exe`, `game/`), `STAR/builds/integration` (the hosts), `STAR/games`, `STAR/test-baselines`, `STAR/test-results` |
| neither | this checkout: `build/` (`build-linux/`, `build-macos/`), `game/`, `work/*.exe`, and `work/games`, `work/test-baselines`, `work/test-results` |

`tools/local.json` can also set each of them by name (`main`, `build`,
`games`, `baselines`, `results`, `cc_profiles`, `max_jobs`); an unknown key is
an error. Below, GAMES, BUILD and so on are these.

**Games at once.** Every game is `--headless --multi`, in a game folder of its
own, `GAMES/testing-<base|cc>-<k>` (hard links to the shared install, real
copies of profiles and saves, as `worktree_env.ps1` makes them; made on first
use and kept; a copy cut short is completed by the next run; creating a new
test folder resets its copied `player.nf*` profiles and private HKCU hive
`Screen Size` to `800x600` without touching the source folder or real HKCU,
and `reg_isolate` only seeds missing keys into a folder's private hive).
`--jobs` games
run at once: default 5, at most `max_jobs` whatever is asked
(`SWGB_MAX_JOBS` or local.json's; else the machine's cores), and as many of
the units and lift checks before them. The tool stops only the processes it
started, by PID (`taskkill /T /PID`), when one runs past its time or on Ctrl-C.

Clone Campaigns' folders take their player profile (`player.nf*`) from
`GAMES/clone-campaigns` when that folder exists (`--cc-profiles`; one with no
profile in it is an error), else from the install itself. A Steam
install's was saved by Steam's `battlegrounds_cc.exe` with its widescreen mod
at 2560x1440, and the game's Options dialog (Fast speed) switches to it. Run
that way, without `--res`, the host draws a broken frame (the map in a corner,
flat colour over the rest): the first L2 run found it.

`--build` is the folder with both hosts (default BUILD);
`--build-cc` takes Clone Campaigns' from another folder.

One run at a time owns the `testing-*` folders (`GAMES/testing.lock`, its
PID): a second run at once is refused unless it passes `--prefix NAME` (its
own `NAME-*` folders). Two games in one folder share its saves, profile and
registry hive; two runs that did made every edge case of `present_check` fail.

**Flaky checks.** A game check (not a soak) that fails is run once more
(`--retry N`, default 1, until a try passes). If it passes then, it is
**FLAKY** only when every failure before the pass was a *timing* one: a
process this tool started ran past its time limit and was ended (exit -1:
the host past its watchdog and a margin, or `conformance.py`, `res_check.py`,
`present_check.py`, `mp_pair.py` past their budget), or the host ended at
its watchdog (exit 4) where the check wanted it to exit by itself. Those are
what a busy machine does (five to eight games on one CPU), so a FLAKY check
does not fail the run; the report lists it first, with the first try's
failure. Anything else fails the run however the retry went: a wrong picture
(a shot over its share of differing pixels, a layout unlike its baseline), a
crash (exit 3, a `=== fault`), a not-lifted stub (exit 2), an unresolved call
(exit 5, `ICALL`), a missing shot, a film that did not play, a fault in the
same try as a timeout. The report heads such a check `FAIL: try 2 passed, but
try 1 failed for more than time`, and each try's line says `FAIL (timing)` or
`FAIL`; `summary.json`'s `tries` has `hard` for each. The retry still runs
there: it shows whether the failure happens every time. Why not `--retry 0`:
a gate that fails on noise teaches people to rerun it until it is green, which
is worse than a retry that excuses only time. Why not every retry: when this
rule was written, the one FLAKY check in the record (films/base, 2026-11-23)
was a real fault, `0xC0000005` opening `xlogo1.avi`, which a passing retry hid.
The price: a failure that load may explain, but that is about what the game
did, stays FAIL too. Two runs of other branches, still on the old rule, since
had one pass on retry: `mp/cc-restore` (2026-11-27, neither game had started
by the end of its script) and `smooth/base` (2026-11-28, no object moved);
neither cause was established. `present/A`'s pacing case has twice shown fewer
than its 510 frames a second (507 and 502, 2026-11-27), both with the game at
1280x1024 by mistake; the 25 recorded runs at 800x600 showed 523-596. If such a
check fails again, fix its wait or its threshold rather than widen "timing".

Each retry runs in a folder of its
own beside the first, `<check>-retry1` (then `-retry2`), so the first try's
`run.log`, keys and shots are kept as they were, and a retry cannot pass on a
shot the first try left. The check's report lines start each try with its
result and folder (`try 1: FAIL (timing), in skirmish_base/`), its shots are captioned
`try 1:`, `try 2:`, and `summary.json` lists the tries (`tries`: try, status,
dir).

**Results.** `RESULTS/<time>-<level>/`: `summary.json`
(every check: status, time, lines, figures, shots), `report.md` and
`report.html` (the shots inline), and a folder per check with its logs, keys
and shots. Exit code 0 when nothing failed or errored; a SKIP (no Clone
Campaigns host, no ffmpeg) is listed but passes.

**Baselines** live outside the repository, in BASELINES (`--baselines`):
they are made from the game's files. A check whose baseline
is missing is **NEW**, not a pass: nothing was compared. The report lists NEW
checks first, and `functional` and up (so the merge gate) fail on them. This
run's picture, layout, figures or hashes go only into the check's folder
(`<check>/new-baselines/`), never into the baselines: a run never makes its
own output the reference. Look at them, then record them with
`--update-baselines` (with `--only CHECK` for one), which records every
baseline its checks reach from that run; those checks are NEW too, and the run
does not fail on them. Do that once on a new machine, or after an intended
change. The screenshots, the layouts (`--res`), the frame-rate figures
(`perf/*`) and the lift's hashes all go through one helper (`baseline()` in
`test.py`). Screenshots are compared as grey levels: a
pixel differs when it is more than 24 levels off, and a check allows a share
of differing pixels: 2% for the main menu, 3% for the setup screen's buttons
and the Options dialog, 35% for the HUD panel under a skirmish (its minimap
and counters change every game; the map itself is never compared). A failing
comparison writes `*_diff.png` (red: what differs).

### On Linux (and macOS)

The same runner drives the native host (`src/linux`, [linux-macos.md](linux-macos.md)):

```
python3 -m venv --system-site-packages work/venv && work/venv/bin/pip install pillow numpy
SWGB_CD=$PWD/work/disc work/venv/bin/python tools/test.py --level smoke   # or unit, functional, soak
```

`./setup.sh` makes `work/venv` (add `pillow numpy` for the tests). `SWGB_CD`
is the play disc for the test folders (`play.sh` passes `--cd`): `work/disc`
for a Steam copy, as setup.sh makes it, else disc 2's mount point. Without it
(and no `~/.steam` Saga to fall back on) the base game stops on "insert a
game CD".

Its defaults are `paths()`'s, as on Windows: without `SWGB_STAR` or a
`tools/local.json`, this checkout's (the hosts in `build-linux/`, or
`build-macos/` on a Mac; the lift and `work/*.exe` here; `work/games/testing-*`,
`work/test-baselines`, `work/test-results`). Each game runs in a process group
of its own, which is what a timeout or Ctrl-C ends.

Clone Campaigns' folders take their profile (`player.nf1`) from
`work/games/clone-campaigns` (`--cc-profiles`), as on Windows; the base
game's come from `game/` (`player.nfx`, which a Steam copy lacks: its
`player.nf1` alone is no profile). Without one the game stops at New Player
and every check that starts a game fails. Make them once with the hosts
themselves (New Player, a name, Enter, Exit):

```
K="20:move:345:250,21:click:345:250,26:t,26.5:e,27:s,27.5:t,29:enter,36:esc,40:move:40:565,41:click:40:565"
python3 -c "import sys; sys.path.insert(0, 'tools'); import soak; soak.ensure_game('game', 'work/games/baseprof'); soak.ensure_game('game', 'work/games/ccprof')"
rm -f work/games/baseprof/player.nf* work/games/ccprof/player.nf*
build-linux/swgbrecomp --run --headless --multi --watchdog 60 --game work/games/baseprof --cd work/disc --keys "$K"
build-linux/swgbrecomp_cc --run --headless --multi --watchdog 45 --game work/games/ccprof --keys "$K"
cp work/games/baseprof/player.nfx game/
mkdir -p work/games/clone-campaigns && cp work/games/ccprof/player.nf1 work/games/clone-campaigns/
```

`conformance.py` runs in `game/` itself, so `--cc` there leaves a
`player.nf1` without a player in it: do not let the test folders copy that.

What runs there:

- **L0:** `keys`, `films`, `profile`, `inflate`, `coverage` and `cdaudio` build with clang (or `$CC`) `-m32`
  against win32hle's headers (`-DSWGB_WIN32HLE`, covering shared `keyscript.c`, `movie_core.c` and `profile_core.c`), and `tests/unit/test_tools.py`.
  `scaler`, `present`, `resolve`, `pacing` and `recaudio` test the Windows host's own
  presenter, cache, pacer and audio recorder and SKIP: the native host's counterparts are
  win32hle's (`screen.c`, its `RECOMP_RESOLVE_CACHE`) or `src/linux`'s
  `framerate.c` and `movie.c`.
- **L1-L3:** every check, through `conformance.py`, `res_check.py`,
  `soak.py` and `mp_pair.py`, which find the Linux host by themselves. The
  host prints the lines they read as the Windows host does (`[host] frame N
  presented`, `[host] CreateWindowExA "..."`, `[fps]`, `[movie]`); win32hle's
  `SetDisplayMode(WxHxB)` is read as well as softdd's `(W, H, B)`.
- **The presenter's checks** (`present/*`, L4 `real/present`) SKIP: they test
  the Windows presenter (GDI, Direct3D 11, the real screen).

Expect `--level functional --cc --retry 1` to pass with 11 checks skipped
(the six Windows-only units and the five `present/*` groups); it takes about
15 minutes. Known to fail there (ROADMAP.md, "Known bugs"):

- the soak level's `base-fight`: the Ewoks raze the enemy's base (its
  colour is gone from the minimap), but the game declares no victory, and
  the controller finds nothing more to aim at;
- `soak.py --mode victory` at a length of its own: after the save dialog,
  the first Enter only destroys the dialog's leftover field, and the chat
  opens a step late.

### The units (L0)

| Unit | Source | Checks |
|---|---|---|
| `scaler` | `tools/present_bench.c --quick` | the scaler bit-exact against the reference one-pass scaler, every frame and window size and filter, after a band and a whole frame change |
| `present` | `tools/present_test.c` | the letterbox layout and the mouse through it; present pacing at 60-600 Hz |
| `coverage` | `tools/coverage_test.c` | the unresolved-call stop: exit 5, the seed line, `SWGB_ICALL_CONTINUE`, eh32's hook first |
| `resolve` | `tests/unit/resolve_test.c` | the indirect-call cache: one lookup per address, "not lifted" kept, an answer that is not the dispatch table's never kept, `__local_unwind2` callers bypass it, `SWGB_NO_RESOLVE_CACHE` |
| `inflate` | `tests/unit/inflate_test.c` | the deflate decoder called as the lifted code calls it: any input and output chunking, two streams in turn, a zeroed state block restarts, corrupt data is -2 |
| `pacing`, `pacing_cc` | `tests/unit/pacing_test.c` | `framerate.c` itself with a fake clock: `--fps 60/30/144/1000` passes a second, the 16/17 ms gate, the refresh default held to 30-1000, no catch-up burst after a 250 ms hitch, the menus untouched, `--fps` arguments; at the base and Clone Campaigns addresses |
| `films` | `tests/unit/films_test.c` | `movie_core.c` (both hosts) and `movie.c` (Windows): AVI headers, `--films` names (`NAME.avi`, then `xNAME.avi`), the film palette, ordered dither (flat colours stay flat, the level holds on average), the A/V clock, drawing into an 8-bit DC (the film palette and a foreign one) |
| `recaudio` | `tests/unit/recaudio_test.c` | `recaudio.c` over a fake DirectSound on a simulated clock (`recaudio_test_clock_ms`): one-shot, volume -10000 silent, pan, rewritten looping buffer, stream tap (time, resampling, pause), 3%-fast drift kept within 60 ms, sound-slider effects gain (with and without `--record`), WAV cut to the video's exact length |
| `smooth`, `smooth_cc` | `tests/unit/smooth_test.c` | `smooth.c` (`--smooth`) itself with a fake clock and fake objects: a walk moves at most 2 px a frame and is never drawn ahead of the object, a stop ends where it stopped, teleports, new objects, a new game and a pause are drawn where they are, other callers are left alone, nothing is written; at the base and Clone Campaigns addresses |
| `keys` | `tests/unit/keys_test.c` | `keyscript.c` and `hostargs.h`: every `--keys` step kind, fractions of a second, `@FILE` with CRLF, drags and modifiers, window places, `on:NAME` waits, `--keys-live`, `host_is_test_run`, `host_int_arg` validation, a bad step skipped (`--keys-live`) or the end of the run (`--keys`) |
| `profile` | `tests/unit/profile_test.c` | `profile_core.c` (both hosts): return-address call detection, stack scanning, sample deduplication/counting, profile file output and `--profile*` arguments |
| `cdaudio` | `tests/unit/cdaudio_test.c` | `cdaudio.c`'s core over an injected output sink (`cdaudio_test_sink`): `tests/data/tone.ogg` decoded (length, rate, tones, level) and seeked; MCI_STATUS counts, lengths and positions in ms, MSF and TMSF; track 2->3 samples and notify; CD & wave-out mixer lines, cubic taper (1/8 at 50%, silence at 1%), sound slider leaving music untouched; stop, pause and a second play abort/supersede notify; errors and `--music off` ([music.md](music.md)) |
| `reg_isolate` | `tests/unit/reg_isolate_test.c` | `reg_isolate.c` private HKCU hive isolation (`--multi`): seeds only missing keys/values from a source hive, preserves existing private hive settings across launches, and never modifies the source hive |
| `tools` | `tests/unit/test_tools.py` | the Python side, as above, plus test-folder creation (`ensure_game` resetting `player.nf*` and hive `Screen Size` to `800x600`), `perf_ab.py`'s figures and round-by-round ratio, `mp_pair.py`'s per-player options and sync-record compare, and `machine_paths.py` |

They run on GitHub Actions (`.github/workflows/unit.yml`, `windows-latest`):
pcrecomp is fetched at the commit `tools/pcrecomp_pin.sh` names (the one the
Linux build uses), and nothing of the game is downloaded, built or uploaded.
A step before them, `tools/machine_paths.py`, fails on a Windows drive path
(`X:\...`) in `tools/`, `src/`, `cmake/` or a script: a default that works
on one machine only (the game's own drives, C: and D: inside the recompiled
game, are on its allow-list). Two more jobs there compile every object of both hosts, `swgbrecomp` and
`swgbrecomp_cc`, without linking (no lift needed): `windows-host` (MSVC x86)
and `linux-host` (i386 clang, the packages of [linux-macos.md](linux-macos.md)).
Both run `tools/compile_hosts.sh`, which works on a machine too, before a
merge is pushed: `sh tools/compile_hosts.sh out/hosts [-DPCRECOMP=DIR]` from an
x86 MSVC prompt, with `-DCMAKE_TOOLCHAIN_FILE=cmake/linux-i386.cmake` on Linux. Each unit was checked against a deliberate
break of the code it covers (the cache keeping every answer, the pacing
catching up after a hitch, `xNAME.avi` looked for first, the decoder's state
never marked used): each one fails.

### The lift check

```
py -3 tools\lift_check.py [--cc] [--gen DIR] [--baseline FILE] [--save]
```

Reads the lift (`src\recomp\gen`, `gen_cc`) in about two seconds: the
dispatch table's functions against the bodies in the `recomp_*.c` files,
`run_lift.py`'s errors and not-lifted stubs (`work\lift_stats*.json`, both
must be 0), and a SHA-1 of each function's C against a baseline: identical,
changed, new, gone. It fails on an error or stub, a function in the table
without a body, a lower function count, or any changed or vanished function
(new ones pass: seeds add them). After an intended re-lift, record the new
baseline with `--save` (`test.py --update-baselines`). The baseline holds
hashes only.

### Games made at WxH (L2): `res/load/base`, `res/load/cc`

`res_check.py load --res 2560x1440`: a skirmish at the third Screen Size,
saved; the game menu's Load (Yes, the save, OK); Options to 1024x768 and
back; Load again; Restart. Each new game is made while the mode is already
2560x1440, which the game never lays out again by itself (building.md,
"Any resolution"). Each must show the first game's interface band (the
bottom three quarters of the interface rows, grey levels within 24, at
least 80% of the pixels: the same civilization's art; the counters and
minimap differ), its game menu centred, and the log no `[res] game screen
... covers` line (a screen that left part of the frame). Before the fix, the
loaded game's band matched 11.6%.

### The widescreen checks (L2)

`widescreen/base`, `cc`, `base_black`, `base_3440`, `cc_2560`: Options ->
the third Screen Size (it must name the size), Standard Game, then shots of
the skirmish. Each checks:

- the interface at the bottom, centred (its mean grey);
- beside it, with `--hud-sides fill` (the default), at least 97% of the side
  pixels equal to the art's pixel that `widescreen.c` mirrors there
  (`hud_sides`: the art's outer 12 columns folded back and forth; a panel
  drawn later over the art's last columns accounts for the rest at the 800
  layout), and not black; with `--hud-sides black`, black. The civilization
  (so the art) is random each game, so the sides are checked against the art
  in the same shot rather than a stored picture;
- the sides the same in two shots two seconds apart;
- the minimap answers where it is drawn: a click left of its centre, then
  right, and its view frame (the white pixels one shot has and the other
  has not) must be within 30 pixels of each click;
- the layout: `SWGB_RES_DUMP`'s panel tree (every panel's vtable and rect,
  so every clickable place) against `test-baselines\<game>\layout_<WxH>.txt`.
  `base` and `base_black` share theirs: the sides never move a panel.

### Smooth motion (L2): `smooth/base`, `smooth/cc`

A fixed skirmish (as the frame-rate check's) at `--smooth --fps 120` with
`SWGB_SMOOTH_LOG`: three workers trained with a rally point, then whoever
stands there box-selected and sent back and forth. `tools/smooth_check.py`
reads the log; it passes when the longest walk is 10 steps or more, 95% of
its frames move 3 px or less, and fewer of them are still than in the
game's own draw ([framerate.md](framerate.md#--smooth-drawn-between-steps)).

### The frame-rate check (L2)

A fixed skirmish (one Easiest computer, a Tiny Savannah map) at `--fps 60
--fps-log`, the view moved now and then, for 2.5 minutes of game: the median
of `--fps-log`'s CPU (% of one core, the process's own time) and presents a
second from 20 s after the game starts, against `tests/perf_thresholds.json`.
With no figures in the baselines the check is NEW (`--update-baselines`
records them); a later run 25% worse than them is noted in the report but
does not fail (other games share the machine).

## The conformance harness

```
py -3 tools\conformance.py
```

Runs `build\swgbrecomp.exe --run --headless` with a scripted click on the
main menu's Exit button, reads the log and reports each milestone:

```
PASS: boot
PASS: genie
PASS: directdraw
PASS: menu
PASS: exit
5/5
```

| Milestone | Passes when |
|---|---|
| `boot` | the image maps, binds and enters the game (`entering 0x00632881`) |
| `genie` | `data\genie.dat` is opened and inflated by the host's decoder |
| `directdraw` | the 800x600x8 mode is set and 100 frames are presented |
| `menu` | the main menu's child windows exist |
| `exit` | the click on Exit ends the game with code 0 |

`--cc` checks Clone Campaigns the same way (`build\swgbrecomp_cc.exe`,
`work\battlegrounds_x1.exe`, `genie_x1.dat`, its entry `0x00636601`), and
`--game` points at another game folder (a worktree's). Exit code 1 on any
FAIL; the full log is `work\conformance.log` (`work\conformance_cc.log`). Without the
game data (`game\`, `work\Battlegrounds.exe`) every milestone is SKIP
and the exit code is 0, so it is safe on a machine with no copy.

It always runs `--multi`. In a worktree, point it at your build, your game
folder and the shared executable:

```
py -3 tools\conformance.py BUILD\swgbrecomp.exe --game GAMES\<track> --exe MAIN\work\Battlegrounds.exe
```

## Into a skirmish

```
build\swgbrecomp.exe --run --headless --watchdog 120 --keys "20:move:345:250,21:click:345:250,30:a,31:b,33:move:323:352,34:click:323:352,44:move:600:168,45:click:600:168,55:move:118:568,56:click:118:568,95:move:310:170,96:click:310:170,105:shot=skirmish.bmp"
```

Single Player, a name typed into the New Player dialog, OK, Standard Game,
Start Game; then the command center is clicked. It passes when `skirmish.bmp`
shows the map with the command center selected
([the screenshot](screenshots/skirmish.png) is this run). Each `--keys` entry
is `seconds:action`: `move:X:Y`, `click:X:Y`, `rclick:X:Y` (right button),
a key name, or `shot=FILE.bmp`.

`--watchdog` ends the run and says where the game was; `--files` logs every
file opened; `NATIVE32_TRACE_EH=1` logs every guest exception and where it
lands.

## Long games: `tools/soak.py`

```
py -3 tools\soak.py [--mode skirmish|monkey|campaign|mixed|defeat|allied|victory|chain|missions|
                            load|ingame-load|fight]
                     [--runs N] [--minutes M] [--jobs J] [--seed S] [--run EXE] [--game DIR]
                     [--speed fast|normal] [--no-save] [--out DIR] [--screen 800|1024|1280]
                     [--campaign NAME] [--first K] [--last K] [--unlocked]
                     [--after M] [--allies N] [--row R]
                     [--ewoks N] [--dwell S] [--wave-every S] [--extra "HOST OPTIONS"]
```

Headless and `--multi`, each run in a game folder of its own (job k > 0 gets
`DIR-soakK`, hard links like `worktree_env.ps1` makes). The defaults are
BUILD's host, MAIN's `work\*.exe` and `GAMES\soak-base` (`soak-cc`), made on
first use; pass `--run` and `--game` for others.

| Mode | What it does after the main menu |
|---|---|
| `skirmish` | Standard Game with a setup varied from the seed: 1-7 computer players with random civilizations, one of 16 locations, a map size from Tiny to Giant, a difficulty from Easiest to Hardest. Start Game, then an idle player: `H`, the command center trains a worker, a right-click somewhere, every 25-45 s. |
| `monkey` | The same start, then random input every 0.4-2 s: left and right clicks on the map, the command grid, the minimap and its buttons; the HUD's panels (objectives, chat, diplomacy, the tech tree), with a click or two inside and then Escape; the game menu's Options and About; letters, digits, arrows, Tab, Enter, Escape and F1-F11. It never clicks Quit, Restart or Load. F3 (pause) and F10 (the menu) are left out: a monkey run that paused sat idle for its last minutes. |
| `campaign` | Campaigns, one of the six campaigns, its first mission, the difficulty dialog's OK, the briefing, Escape for the Objectives, then the monkey: a first mission has no command center for the idle player (`--idle-campaign` runs it anyway). |
| `mixed` | The three in turn. |
| `defeat` | Alone against two Hardest computers on one team, a Tiny map, no defence: the computers win. |
| `allied` | Me and `--allies` Hardest computers (team 1) against one (team 2), the idle player: meant as a victory nobody types. |
| `victory` | A varied skirmish played for `--minutes` with the save, then won with the chat cheat `skywalker` (Enter, the word, Enter), the score screen's six tabs, Main Menu, Single Player -> Saved Game -> that save -> OK, and `--after` more minutes of the loaded game. |
| `missions` | `--campaign`'s missions `--first`..`--last` in a row (default: to the campaign's last; OOM-9 when no campaign is named, Confederacy with `--cc`). Each: its box on the campaign screen, the briefing, Escape for the Objectives, Fast, the monkey for `--minutes` with a save 40% of the way in, `skywalker`, the end briefing, the score screen's tabs, Scenario Menu. A box shows once the mission before it is won, in that game folder's player profile, so the missions before `--first` are passed through first (each started and won at once, about three minutes); `--unlocked` starts at `--first` in a profile that has them open already. Mission 1's first start asks for the difficulty: its OK (Easy). Any of the six base campaigns (`Attichitcuk` is Basic Training, whose seven are open from the start) and, with `--cc`, `Confederacy` and `Galactic Republic`. |
| `chain` | `missions` under its old name (OOM-9 by default). |
| `load` | Single Player -> Saved Game -> the `--row`th save (the list is alphabetical) -> OK, then the monkey. |
| `ingame-load` | A varied skirmish (or `--campaign`'s mission `--first`, which must be open) with the monkey, a save 30% of the way in, the monkey on, then at 55% the game menu's Load, Yes ("quit and load a new game?"), the save from the list, OK, and the monkey in the loaded game to the end. Base game and `--cc` (its saves are `.ga1`). |
| `fight` | A victory won by combat, declared by the game. Me and `--allies` computers (team 1) against one (team 2), a Large Savannah map, Easiest (one setting for every computer), Reveal Map: All Visible. Waves of Simon the Killer Ewok (`simonsays` typed 3 s apart; the game stops at nine alive), each box-selected around the command center and made a control group (`ctrl+N`). Then a live controller (`--keys-live`): every `--dwell` seconds a shot, the densest cluster of the enemy's colour on the minimap (player `allies + 2`: magenta with 4 allies, yellow with 2), each group sent there with a right-click on the minimap, a new wave every `--wave-every` seconds. At the victory (or defeat) music it shoots the score screen and its tabs, then ends the run. |

The setup screen's Team buttons step to the next team on each click. A click
right after a drop-down closes, or a second click with the cursor still on
the button, does not count, so the tool waits and moves the cursor off
between clicks. Its `shot_setup` shows what was chosen.

The runs log file opens (`--files`), and the summary's `endings` lists each
game over by the music the game opens for it: `sound\stream\won1.mp3` (or
`won2`) for a victory, `lost.mp3` for a defeat. On the score screen, a
defeated player's Teams column reads `-`.

Every run sets the game speed to Fast (the game menu, Options, Speed) and
saves once, 40% of the way in (the game menu, Save, a name, OK). Out of
`DIR\<run>\` come `run.log`, `keys.txt` (the `--keys @FILE` script, so a run
can be repeated exactly), PNG shots (at the start, every five minutes, at
the end) and `summary.json`. The summary prints each run's outcome and every
`ICALL`/`ITAIL`, fault, not-lifted stub and `[coverage]` line, with the eight
`[keys]` steps before it. The outcome comes from the host's exit code:

| Exit | Outcome |
|---|---|
| 4 | survived: the watchdog ended it at the planned length |
| 0 | the game exited by itself |
| 2 | a not-lifted stub |
| 3 | a fault |
| 5 | an unresolved indirect call (`coverage.c`; docs/lifting.md) |

The clicks are 800x600 game pixels unless `--screen` says otherwise: run it
in a game folder whose Screen Size is 800 (it warns if the game sets another
mode). `--screen 1024|1280` (base game) picks that Screen Size on the main
menu's Options panel first and moves every in-game click to that size: the
HUD is the game's own art per width (`tools/swgb/screens.py`'s `HUD`: the
map view, the command grid, the minimap and its buttons, the four icons above
the panel, the buttons on the right), the game menu and its Options
dialog are centred, the Save and Load screens are the 800x600 one scaled. The
menus, the setup screen and the score screen stay 800x600. A run that did not
get the size it asked for counts as a problem. The game stores the Screen
Size only when it exits normally, and a soak run is ended by its watchdog, so
the folder keeps its own size. `--keys` takes up to 16,384 steps; a 30-minute
monkey run is about 2,500.

The monkey leaves the last 8 seconds before a scripted save or load to keys:
its panels and the game menu end with Escapes 5-6 seconds later, and one
such Escape closed the Save screen of the first in-game Load run (no save was
written, and the Load read an older game).

**The mission boxes** (`screens.py`'s `BASE_MISSIONS`, `CC_MISSIONS`) were found by winning each mission in
turn with a live script (`--keys-live`) and diffing the campaign screen before
and after: the new box is the largest change. Every campaign has seven
missions except Princess Leia's eight. A won mission opens its end briefing,
`campaign\media\<campaign>s<N>_end.mm` (`xc1s5_end.mm`: OOM-9's fifth;
`1c1s3_end.mm`: Clone Campaigns' Confederacy's third); the summary's
`missions_won` lists them, and `test.py` counts them, since Basic Training
plays no victory music. Its score screen is skipped too: after the cheat it
goes straight back to its campaign screen.

An unresolved address from any run (or from a player's log) goes into a seed
with `py -3 tools\seeds_from_logs.py --merge` (docs/lifting.md).

### Why long games

A game reaches code a short one does not. Traced runs (`--firsthit`, a
`SWGB_TRACE` build) log the first entry to each lifted function: about 3,050
have run by the end of loading, and a 30-minute skirmish still finds a few
hundred new ones after minute 5, in bursts: the computers' economy and first
attacks, the save (110-150 functions at once, the world's writer), and
THIS_COD's span blitters (`0x00645xxx`-`0x0064Cxxx`), each variant first hit
only when a sprite's span meets the screen in a particular alignment, at
minutes 13 to 28. The two faults the soaks found (fixed by softdd's surface
slack, [lifting.md](lifting.md)) were such blitters, at 13 and 25 minutes. A
30-minute skirmish also makes about 3 billion indirect calls: past 2^31 the
fault report's indirect-call ring once printed nothing (an `(int)`
comparison, fixed in `host.c` and `coverage.c`).

### What the tool works around

- **The monkey and scripted saves.** The monkey's panels and game menu end
  with Escapes 5-6 seconds after they open; one landed on the Save screen of
  an in-game Load run and closed it, and the Load read an older game. The
  monkey now leaves the last 8 seconds before a scripted save or load to
  keys.
- **Hardest computers defeat the monkey** in 11-20 minutes, and after a game
  over it wanders the score screen and the menus. `ingame-load` plays 1-3
  Easiest computers so the Load comes first. A natural victory needs a player
  that defends, which is why `fight` exists.
- **One difficulty for every computer.** Hardest allies against an Easiest
  enemy is not possible, so `fight` is Easiest for all, and the Ewoks win
  alone. Waves sent blind around the ring of start positions never met the
  enemy (on Shoreline half the ring is sea, and with only `forceexplore` the
  fog hid the base from the minimap), hence the aiming at the enemy's colour
  and Reveal Map: All Visible. [The Ewoks in the enemy's
  ruins](screenshots/fight.png), [the score screen](screenshots/fight-score.png).
- **A game per mission.** In one long game, a lost mission threw the script
  out of step (it once started a mission itself and quit the game with an
  Enter), so `missions` plays each mission in a game of its own and ends that
  game at a defeat.
- **Clone Campaigns' Saved Game list** shows its recorded games (`.mg1`)
  among the saves, so the row of a save counts both (`saved_names`).
- **Galactic Republic** opens only once Confederacy is won in that profile,
  so `missions --cc` passes through Confederacy first.
- In a campaign the monkey can scroll the view into unexplored black and
  stay there: `H` has no command center to return to.

### Clone Campaigns

`--cc` runs the expansion: `swgbrecomp_cc.exe`, `work\battlegrounds_x1.exe`
and the `clone-campaigns` game folder by default, its Single Player menu
(Standard Game and Saved Game a row lower than the base game's, Episode II
Campaigns where Campaigns is), its nine civilizations and its two Episode
II campaigns. `--civs A,B` gives the computers those civilizations in turn,
`--my-civ` the player's, `--campaign` names the campaign. The endgame modes
(`defeat` .. `load`) script the base game's OOM-9 campaign and are refused
with `--cc`. The game folder needs a profile at 800x600: one saved by
Steam's `battlegrounds_cc.exe` (the widescreen mod) is at 2560x1440.

```
py -3 tools\soak.py --cc --mode skirmish --minutes 28 --civs Confederacy,Republic --my-civ Republic
py -3 tools\soak.py --cc --mode campaign --campaign "Galactic Republic" --minutes 25
py -3 tools\seeds_from_logs.py --cc work\soak\...\run.log
```

## Multiplayer: `tools/mp_pair.py`

```
py -3 tools\mp_pair.py [--players 2-4] [--minutes 20] [--activity idle|habits|busy] [--seed S] [--cc]
                       [--chat] [--drop WHO@MIN] [--quit WHO@MIN] [--resign WHO@MIN] [--save MIN [--restore]]
                       [--wsl WHO] [--run [WHO=]EXE ...] [--host-arg WHO=SWITCHES ...] [--game DIR] [--port N]
```

WHO is a player (a, b, c, d) or `all`. `--host-arg WHO=SWITCHES` gives
that player more host switches, split as a POSIX shell would (write paths
with `/`): `--host-arg a=--smooth` (the sync records stay the same,
framerate.md), `--host-arg all=--mp-cross` with a WSL player. `--run b=EXE`
runs B on another build (performance.md); `--run EXE` alone is everyone's
(default BUILD's). Both repeat; a player's own beats `all`.

Two to four headless `--multi` games on one machine: one hosts, the others
join over 127.0.0.1, all start a game and play `--minutes`. It passes when
each reaches its watchdog, nobody drops but the player the options drop, the
game shows no out-of-sync message, and the game's own sync checkpoints
(`[sync]` lines) agree between every pair of logs. Its options, what each
check reads and the Windows-Linux games are in
[multiplayer.md](multiplayer.md#tests).

The joiner selects "Local (LAN) TCP/IP Connection" first, so its games
appear by themselves: a folder that last used it has it selected already,
and a new one has "Internet", whose Show Games asks for an address. A's
folder defaults to `GAMES/multiplayer-base` (`-cc`), made on first use. On
Linux it runs as `python3 tools/mp_pair.py`, with `build-linux/` and
`work/games/multiplayer` by default (`--cc --game DIR` with a Clone
Campaigns profile in DIR).

## The game's three resolutions

```
py -3 tools\res_check.py 800|1024|1280 [--out DIR] [--game DIR] [--no-options]
```

Headless (`--headless --multi`). It picks Screen Size on the 800x600 Options
menu (radio buttons 800x600 at 453,383, 1024x768 at 453,413, 1280x1024 at
453,443), clicks Single Player, Standard Game and Start Game, and after the map
loads (~85 s) takes shots into DIR (PNG): `a_start`, `b_selected` (the
command center clicked, near the centre of the map view), `c_left`, `d_right`,
`e_top`, `f_bottom` (the cursor held on each map edge: the view scrolls),
`g_minimap` (a click on the minimap), `h_menu` (the HUD's Menu button), then
Quit Current Game, Yes (the score screen, `i_score`) and Main Menu (`j_menus`).
It prints the `SetDisplayMode` calls and passes when the size was set:

| Size | `SetDisplayMode` calls | Screenshot |
|---|---|---|
| 800x600 | 800x600 (once: the skirmish needs no change) | [res-800](screenshots/res-800.png) |
| 1024x768 | 800x600, 1024x768, 800x600 | [res-1024](screenshots/res-1024.png) |
| 1280x1024 | 800x600, 1280x1024, 800x600 | [res-1280](screenshots/res-1280.png) |

Each shot is the size of the mode (the shot log says `1280x1024x8`), the HUD
is the game's art for that width, and the last `SetDisplayMode` is 800x600:
the score screen and the menus are 800x600 at any size.

The game writes Screen Size to HKCU when it exits, so a run killed by the
watchdog (as this one is) does not keep it. With `--multi` the host gives the
run a private copy of the game's HKCU keys per `--game` folder
(`HKCU\Software\swgb-recomp\hives\...`), so your Screen Size never reaches the
user's game or another worktree's. To check that it persists: pick a size, end
the game from the main menu (Exit at 40,565) and run
`tools\res_check.py SIZE --no-options`: it reaches that size without touching
Options. A `--keys` click is in game pixels, posted at the window pixel that maps
to it, so it goes through the letterbox as the real mouse does; window pixels
themselves are `wmove`/`wclick` (next section).

### Switching in the game: `res_check.py ingame`

```
py -3 tools\res_check.py ingame [--res WxH] [--cc] [--out DIR] [--game DIR] [--run EXE] [--exe EXE]
```

A skirmish at 800x600 (picked on the main Options first), then the game menu's
Options (Menu, then Options at the screen's middle + 17; its Screen Size radio
buttons at 133, 318/353/388 and OK at 305,455 on 800x600, centred on a bigger
screen) switches to 1024x768, to the third size (1280x1024, or WxH with
`--res`), and back to 800x600. After each switch: a shot (`NN_WxH`), the HUD's
Menu button, and the game menu must open centred on the new screen
(`NN_WxH_menu` against `NN_WxH_before`); then Quit Current Game, the score
screen at 800x600 and Main Menu. It passes when the modes are exactly 800x600,
1024x768, the third, 800x600.

`test.py` runs it as `res/ingame/base` and `res/ingame/cc` with `--res
1920x1080`. It guards against an early lead that never reproduced: "after an
in-game switch to 1024x768 the Menu button no longer answers".

One thing a script must do that a player does not: move the cursor after OK.
The scripted cursor stays where it clicked, and after a switch to a smaller
screen (1280x1024 -> 800x600) the old OK is past the bottom edge: the view
edge-scrolls to the bottom of the map and the map looks black. A real
fullscreen mode change clamps the cursor the same way; a window maps it in
proportion. The check parks the cursor at 400,300 (inside every size).

### Windows over the map: `res_check.py popups`

```
py -3 tools\res_check.py popups [--size 800|1024|1280|wide] [--res WxH] [--cc] [--out DIR] ...
```

A skirmish at `--size` (`wide`: the third size, WxH with `--res`; the
default), then each window, shot just before (`*_before`) and after: the
HUD's buttons (Menu, Objectives, Chat, Diplomacy, Tech Tree; on the right, the
base layout's places moved with the HUD at WxH), Save Game (the name
`rescheck` typed, OK: the save must be in `SaveGame` and the game back at its
size), Load (Yes, the Saved Game screen, Cancel) and About. A dialog passes
when the box of what changed is centred on the screen (its middle within 4%
across, 6% down); the tech tree when it spans the screen; Save and Load when
they are the largest of the game's sizes that fits, centred, with black
around (at the game's own sizes: the whole screen).

Why the rules differ: the game centres a dialog on its parent, the WxH
screen, so dialogs need nothing. Save Game and Load are screens of their own,
laid out in proportion to the screen from an 800x600 design over the
1280x1024 background drawn at 0,0; left alone at 1920x1080 their buttons
spread across the screen with the picture in a corner, so `widescreen.c`
("full screens shown in a game") gives them the largest of the game's sizes
that fits. The tech tree keeps the whole screen: it lays itself out at any
size, and made smaller it drew past its panel (a fault at 1920x1080).
`test.py`: `popups/base-1024`, `popups/base-2560x1440`, `popups/cc-1920x1080`.

## The picture (scaling, fullscreen, pacing)

Three levels: the maths alone, the whole thing headless, and the real screen.

### The maths: `tools/present_test.c`

The layout, the mouse mapping through the letterbox and the pacing beat,
without the game:

```
cl /nologo tools\present_test.c src\runtime\present.c src\runtime\scaler.c src\runtime\present_d3d.c /Isrc\runtime user32.lib gdi32.lib d3d11.lib dxgi.lib dxguid.lib
present_test.exe                                  OK
```

### The game, headless: `tools/present_check.py`

```
py -3 tools\present_check.py [--only A,B,C,D] [--run EXE] [--game DIR] [--out DIR]
```

About 21 minutes, `--headless --multi`, nothing on your screens. It prints
PASS or FAIL per case and exits 1 on any FAIL. Five scripted skirmishes:

| Group | Setup | Cases |
|---|---|---|
| A | the game's own size, a wide window (1500x700), a tall one (700x900), a window resized four times, then Alt+Enter fullscreen; `--refresh 600` | edge scrolling in each shape; the cursor coming back into the window; pacing |
| B | Alt+Enter on the menus, Options -> 1024 x 768, a game, Quit to the menus | edge scrolling in fullscreen at 1024x768; fullscreen kept at each `SetDisplayMode` (800 -> 1024 -> 800), on the score screen and on the menus |
| C | `--fullscreen` at launch | the menus are fullscreen from the start; edge scrolling in the monitor's rectangle |
| D | `--res 1920x1080` in a 1600x900 window, then fullscreen | no bars in a 16:9 window; edge scrolling at that size |
| E | a window sized to 1100x620 on the menus, Options -> 1024 x 768, a game, Quit to the menus, a second game | the window keeps the player's size through every mode change (the game sizes its own window at each one); edge scrolling in that shape |

**Edge scrolling** is checked at every window shape on all four sides of the
window, in each letterbox bar (where the shape has them), and past the screen's
edge in fullscreen. Each probe starts from the same view (`H` centres the game
on the command center), compares the game's frame (`shot=`) before and after
holding the cursor at the place for 0.8 s, and wants the view to change by at
least 10% (it changes by about half) at an edge and by less than 3% with the
cursor 60 pixels inside the picture or in the middle. It also wants the game
to have seen the cursor at game pixel 0, W-1 or H-1: the window's (or the
monitor's) edge is the game's edge. This is what the windowed check found by
eye: the right and bottom never scrolled and the left and top zone was huge in
any window that was not exactly the game's size, and in fullscreen nothing did.

**The cursor** is a virtual one: `--keys` puts it at a window pixel (or a game
pixel, turned into the window pixel that maps to it), in screen pixels held
inside the monitor and, in fullscreen, inside the rectangle the cursor is
confined to; the game's `GetCursorPos` reads it back through the same path as
the real cursor (screen -> client -> the letterbox -> game pixels), and the
mouse messages carry window pixels, mapped as a real mouse's are. So a
headless run exercises what a real mouse does, short of the operating system.

**The cursor coming back** (`setcursor`): the pointer is made the arrow (as
the other window it came from left it) and Windows asks the game window for
its cursor (`WM_SETCURSOR`); the game thread's cursor must be the game's own
choice again, not the arrow. **Pacing**: with `--refresh 600 --fps off` (the default
frame rate is the refresh, so the game's own rate is asked for) and a game that
presents 700 to 1,000 times a second, the frames shown must be about 600 a
second (a rule of "an interval since the last frame" showed every other
present: about 450).

The `--keys` steps it uses (all in `docs/building.md`): `wmove:X:Y` and
`wclick:X:Y` in window pixels, where X and Y are a number or a place: `L R C`
(left, right, centre), `T B C`, `PL PR PT PB` (the picture's edges), `BL BR
BT BB` (the middle of a bar), with an offset: `wmove:R:C`, `wmove:PL+60:C`;
`size:W:H`; `alt+enter`; `setcursor`; `wshot=FILE` (the window as drawn, bars
and all); and `--keys @FILE` to read a long script from a file.

The cases catch what the windowed check found by eye: on the presenter
before those fixes, 22 of group A's 48 fail, among them every right and
bottom edge and the cursor coming back. Run it against a build of
your own with `--run` and `--game` (a `worktree_env.ps1` build and game
folder).

### On the real screen: `tools/present_check_real.ps1`

```
powershell -ExecutionPolicy Bypass -File tools\present_check_real.ps1 [-Quick] [-Yes]
```

The same cases with a real window and the real mouse: it starts the game,
clicks through to a skirmish with `SendInput`, puts the real cursor
(`SetCursorPos`) on each edge and bar of the window, in a wide and a tall
shape and in fullscreen (Alt+Enter), and compares screen captures of the game
before and after. It parks the cursor at the corner of the screen and brings
it back to look at the Windows cursor, and reads the frame rate from the
present log (only seconds in the game with presents at least 1.3 times the
refresh: just above the refresh, and unevenly spaced, some refreshes get no
new frame at all, so 185 presents a second at 180 Hz show about 160 under any
pacing). **It takes over
your mouse and keyboard for about five minutes**
(`-Quick`: one shape, about three); do not touch them until it prints the
summary. Captures and the log are in `%TEMP%\present_check_real`. The Options
size change while fullscreen is only in the headless tool.
