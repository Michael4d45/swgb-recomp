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
| L0 | `unit` | 25 s | The host's C units, built with `cl` and run (below), and `tests/unit/test_tools.py` (soak.py's scripts and log reading, `lift_check.py`, this runner's image and `--fps-log` readers) | Visual Studio, a pcrecomp checkout (its headers only). No game, no lift. |
| L1 | `smoke` | 2-2.5 min | L0, then for each game (base and Clone Campaigns): `conformance.py`; menu -> Standard Game -> a skirmish with four shots (menu, setup, map, command center selected), no unresolved call, fault or stub, 800x600 first; the lift check (below) | the game data, the host(s), the lift for the lift check |
| L2 | `functional` | 18-20 min at 5 games at once (12 for the base game alone) | L1, then `present_check.py` groups A-E (one game each, in parallel), `res_check.py` 800/1024/1280, `--res 1920x1080` from Options, `--films` (logo and intro through FFmpeg, frames shown and dropped), a short skirmish per set of civilizations (all six, all eight of Clone Campaigns) with a save, then that save loaded from Single Player -> Saved Game, and the frame-rate check (below) | as L1, ffmpeg |
| L3 | `soak` | about 2 h at 5 games at once (8 game-hours) | L2, then `soak.py` for both games: base skirmish, monkey, campaign, defeat, allied, victory then load, chain (OOM-9 1-3), fight (a victory by combat); Clone Campaigns skirmish (two civilization sets), monkey, both Episode II campaigns. `--soak-scale 0.2` for a shorter trial. | as L2 |
| L4 | `real` | ~5 min | `present_check_real.ps1`: the real window, mouse and screen. **Only with `--real-screen`**, which means the user said so: it takes over the mouse and keyboard. | the user at the screen |

A level runs the levels below it (`--just`: only its own). Clone Campaigns is
tested when its host is in `--build` (`swgbrecomp_cc.exe`); `--cc` makes a
missing one a failure, `--no-cc` leaves it out. `--list` prints the checks.

**Before a merge** (the gate): `py -3 tools\test.py --level functional` on the
merged build, exit 0. **Nightly**: `--level soak` (`tools\test_nightly.ps1`;
`-Register` makes it a daily scheduled task, which only the user decides).
**After a re-lift**: `--level smoke` at least: the lift check says whether the
C changed.

**Games at once.** Every game is `--headless --multi`, in a game folder of its
own, `A:\Games\star\games\testing-<base|cc>-<k>` (hard links to the shared
install, real copies of profiles and saves, as `worktree_env.ps1` makes them;
made on first use and kept). `--jobs` games run at once (default 5, at most 8
whatever is asked: the user plays on this machine). The tool stops only the
processes it started, by PID (`taskkill /T /PID`), when one runs past its
time or on Ctrl-C.

Clone Campaigns' folders take their player profile (`player.nf*`) from
`A:\Games\star\games\clone-campaigns` (`--cc-profiles`): the shared
install's was saved by Steam's `battlegrounds_cc.exe` with its widescreen mod
at 2560x1440, and the game's Options dialog (Fast speed) switches to it. Run
that way, without `--res`, the host draws a broken frame (the map in a corner,
flat colour over the rest): the first L2 run found it.

`--build` is the folder with both hosts (default `builds\integration`);
`--build-cc` takes Clone Campaigns' from another folder.

One run at a time owns the `testing-*` folders (`games\testing.lock`, its
PID): a second run at once is refused unless it passes `--prefix NAME` (its
own `NAME-*` folders). Two games in one folder share its saves, profile and
registry hive; two runs that did made every edge case of `present_check` fail.

**Flaky checks.** A game check (not a soak) that fails is run once more
(`--retry N`, default 1). If it passes then, it is **FLAKY**: the run still
passes, and the report keeps both tries. On main of 2026-11-22, present group
A failed once in three runs on the same build: after Alt+Enter, eight
fullscreen edge cases saw the game read the cursor at 400,300 (what the host
reports while another window is active) although the view scrolled. The other
two runs passed all 48 cases.

**Results.** `A:\Games\star\test-results\<time>-<level>\`: `summary.json`
(every check: status, time, lines, figures, shots), `report.md` and
`report.html` (the shots inline), and a folder per check with its logs, keys
and shots. Exit code 0 when nothing failed or errored; a SKIP (no Clone
Campaigns host, no ffmpeg) is listed but passes.

**Baselines** live outside the repository, in `A:\Games\star\test-baselines`
(`--baselines`): they are made from the game's files. A missing one is
recorded by the first run that needs it; `--update-baselines` records them all
again after an intended change. Screenshots are compared as grey levels: a
pixel differs when it is more than 24 levels off, and a check allows a share
of differing pixels: 2% for the main menu, 3% for the setup screen's buttons
and the Options dialog, 35% for the HUD panel under a skirmish (its minimap
and counters change every game; the map itself is never compared). A failing
comparison writes `*_diff.png` (red: what differs).

### The units (L0)

| Unit | Source | Checks |
|---|---|---|
| `scaler` | `tools/present_bench.c --quick` | the scaler bit-exact against the reference one-pass scaler, every frame and window size and filter, after a band and a whole frame change |
| `present` | `tools/present_test.c` | the letterbox layout and the mouse through it; present pacing at 60-600 Hz |
| `coverage` | `tools/coverage_test.c` | the unresolved-call stop: exit 5, the seed line, `SWGB_ICALL_CONTINUE`, eh32's hook first |
| `resolve` | `tests/unit/resolve_test.c` | the indirect-call cache: one lookup per address, "not lifted" kept, an answer that is not the dispatch table's never kept, `__local_unwind2` callers bypass it, `SWGB_NO_RESOLVE_CACHE` |
| `inflate` | `tests/unit/inflate_test.c` | the deflate decoder called as the lifted code calls it: any input and output chunking, two streams in turn, a zeroed state block restarts, corrupt data is -2 |
| `pacing`, `pacing_cc` | `tests/unit/pacing_test.c` | `framerate.c` itself with a fake clock: `--fps 60/30/144/1000` passes a second, the 16/17 ms gate, the refresh default held to 30-1000, no catch-up burst after a 250 ms hitch, the menus untouched, `--fps` arguments; at the base and Clone Campaigns addresses |
| `films` | `tests/unit/films_test.c` | `movie.c` itself: AVI headers, `--films` names (`NAME.avi`, then `xNAME.avi`), the film palette, the ordered dither (flat colours stay flat, the level holds on average), drawing into an 8-bit DC (the film palette and a foreign one) |
| `keys` | `tests/unit/keys_test.c` | `keyscript.c`: every `--keys` step kind, fractions of a second, `@FILE` with CRLF, drags and modifiers, a bad step skipped (`--keys-live`) or the end of the run (`--keys`) |
| `tools` | `tests/unit/test_tools.py` | the Python side, as above |

They run on GitHub Actions (`.github/workflows/unit.yml`, `windows-latest`):
pcrecomp is fetched at a pinned commit for its headers, and nothing of the game
is downloaded, built or uploaded. Each unit was checked against a deliberate
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

### The frame-rate check (L2)

A fixed skirmish (one Easiest computer, a Tiny Savannah map) at `--fps 60
--fps-log`, the view moved now and then, for 2.5 minutes of game: the median
of `--fps-log`'s CPU (% of one core, the process's own time) and presents a
second from 20 s after the game starts, against `tests/perf_thresholds.json`.
The first run records its figures as the baseline; a later run 25% worse is
noted in the report but does not fail (other games share the machine).

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
py -3 tools\conformance.py A:\Games\star\builds\<track>\swgbrecomp.exe --game A:\Games\star\games\<track> --exe A:\Games\star\swgb-recomp\work\Battlegrounds.exe
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
py -3 tools\soak.py [--mode skirmish|monkey|campaign|mixed|defeat|allied|victory|chain|load|fight]
                     [--runs N] [--minutes M] [--jobs J] [--seed S] [--run EXE] [--game DIR]
                     [--speed fast|normal] [--no-save] [--out DIR]
                     [--after M] [--allies N] [--first K] [--last K] [--row R]
                     [--ewoks N] [--dwell S] [--wave-every S] [--extra "HOST OPTIONS"]
```

Headless and `--multi`, each run in a game folder of its own (job k > 0 gets
`DIR-soakK`, hard links like `worktree_env.ps1` makes). The defaults are this
track's build (`A:\Games\star\builds\coverage`) and game folder; pass
`--run` and `--game` for another.

| Mode | What it does after the main menu |
|---|---|
| `skirmish` | Standard Game with a setup varied from the seed: 1-7 computer players with random civilizations, one of 16 locations, a map size from Tiny to Giant, a difficulty from Easiest to Hardest. Start Game, then an idle player: `H`, the command center trains a worker, a right-click somewhere, every 25-45 s. |
| `monkey` | The same start, then random input every 0.4-2 s: left and right clicks on the map, the command grid, the minimap and its buttons; the HUD's panels (objectives, chat, diplomacy, the tech tree), with a click or two inside and then Escape; the game menu's Options and About; letters, digits, arrows, Tab, Enter, Escape and F1-F11. It never clicks Quit, Restart or Load. F3 (pause) and F10 (the menu) are left out: a monkey run that paused sat idle for its last minutes. |
| `campaign` | Campaigns, one of the six campaigns, its first mission, the difficulty dialog's OK, the briefing, Escape for the Objectives, then the monkey: a first mission has no command center for the idle player (`--idle-campaign` runs it anyway). |
| `mixed` | The three in turn. |
| `defeat` | Alone against two Hardest computers on one team, a Tiny map, no defence: the computers win. |
| `allied` | Me and `--allies` Hardest computers (team 1) against one (team 2), the idle player: meant as a victory nobody types. |
| `victory` | A varied skirmish played for `--minutes` with the save, then won with the chat cheat `skywalker` (Enter, the word, Enter), the score screen's six tabs, Main Menu, Single Player -> Saved Game -> that save -> OK, and `--after` more minutes of the loaded game. |
| `chain` | OOM-9's missions `--first`..`--last` in a row (1-5), each: its box on the campaign screen, the briefing, the monkey for `--minutes`, `skywalker`, the score screen, Scenario Menu. A mission's box appears once the one before is won, in that game folder's player profile. |
| `load` | Single Player -> Saved Game -> the `--row`th save (the list is alphabetical) -> OK, then the monkey. |
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

The clicks are 800x600 game pixels: run it in a game folder whose Screen Size
is 800 (it warns if the first `SetDisplayMode` is not 800x600). `--keys` takes
up to 16,384 steps; a 30-minute monkey run is about 2,500.

An unresolved address from any run (or from a player's log) goes into a seed
with `py -3 tools\seeds_from_logs.py --merge` (docs/lifting.md).

### What the soaks found (2026-11-22)

| Batch | Host | Runs | Game time | Result |
|---|---|---|---|---|
| b1 | before the slack fix | 2 skirmish, 2 monkey, 2 campaign, 25 min | 2.5 h | 4 ran the full time; 2 faulted in `sub_0064AC8D`, at 13 min (monkey, right after a save) and 25 min (skirmish, 7 players): `read of 0x1A00FFFF` / `0x19F7FFFF`, the byte before the back buffer (docs/lifting.md, DirectDraw). |
| b2 | with it | 2 skirmish (6 and 7 computers, Giant and Normal maps), 2 monkey, 2 campaign (OOM-9, Boss Nass), 30 min | 3.3 h | all 6 ran the full time; nothing reached the code-coverage hooks. |
| trace | with it, `SWGB_TRACE` and `--firsthit` | 1 skirmish, 1 monkey, 30 min | 1.1 h | both ran the full time. |

| endings | with it | below | 4.4 h | every run went its planned length or to its end; no events. |
| fight | merged main + `--keys` drags, modifiers, `--keys-live` | below | 4.0 h | 3 victories by combat; no events. |

**Games to the end** (2026-11-22, the fixed host):

| Run | What happened |
|---|---|
| `defeat`, 2 Hardest on a Tiny map | a real defeat about 11 minutes in: the base destroyed, `lost.mp3`, the score screen (the two computers on team 1); then 35 minutes idling on the score screen. |
| `allied`, 2 allies, Medium | also a defeat, at 12 minutes: the one Hardest enemy rushed my base before the allies won. My defeat does not wait for my team. |
| `allied`, 6 allies, Large (twice; the first with the team clicks lost) | defeats at 7-9 minutes, the same way. Diplomacy at the start was right (6 Ally, 1 Enemy, also checked at 4, 7 and 8 players): an idle player is just the nearest target. A natural victory needs a player that defends. |
| `victory`, 7 computers on a Large Islands map | 22 minutes of play with a save at 9, then `skywalker`: "You are victorious!", `won1.mp3`, the score screen and its tabs, Main Menu, the save in Saved Game's list (row 0), loaded, and 15 more minutes of the loaded game. |
| `chain`, OOM-9 missions 3, 4 and 5 | each briefing, the mission (Spinnaker's Spoils; Capturing The Crown, Tech Level 4 with the droid army; The Search For An Underwater Kingdom), 15-20 minutes with the monkey, the victory, its end briefing, the score screen, Scenario Menu and the next mission's box. Missions 1 and 2 were won the same way while the tool was written. |
| `load` x2 | the monkey's save from b1 (made seconds before that run's fault) and a Boss Nass mission-1 save, each loaded and played 15 minutes. |

No run met an unresolved indirect call, a not-lifted stub, an eh32 funclet
miss or a fault. Every run saved at 40% (the files are in each game folder's
`SaveGame`).

**What a long game reaches that a short one does not.** The traced runs
log the first entry to each lifted function. About 3,050 functions have run
by the end of loading and 220-280 more in the first game minute; the
skirmish reached 3,592 in all and the monkey 4,032 (19% of 20,820). After
minute 5 the skirmish still found 237 new functions and the monkey about
400, in bursts:

- minutes 5-10: the computers' economy and first attacks ("Your base is
  being attacked" by 23 minutes in the b1 skirmish), combat and deaths;
- the save, 110-150 functions at once (the world's writer);
- THIS_COD's span blitters (`0x00645xxx`-`0x0064Cxxx`), first hit at
  minutes 13, 14, 21, 23 and 28: each variant runs only when a sprite's
  span meets the screen in a particular alignment. Both faults were one of
  these, which is why they came at 13 and 25 minutes and not in the first
  five.

Two more things only a long game reaches. One is 2^31 indirect calls (about
ten minutes): past that, the fault report's indirect-call ring printed
nothing (an `(int)` comparison, fixed in `host.c` and `coverage.c`). The
other: a 30-minute skirmish makes about 3 billion indirect calls and 2 million
frames.

**A victory by combat** (2026-11-22, the merged build's lift, host `coverage-win*`):

| Run | What happened |
|---|---|
| `fight`, 4 allies, enemy magenta (twice) and 2 allies, enemy yellow | the first wave of nine Ewoks set off at 4:20 (time since launch), straight at the enemy's base on the minimap; its colour fell from 73-92 pixels as the buildings fell, and the game played `won1.mp3` at 5:09, 6:25 and 7:39: 51 s, 2:04 and 3:18 of fighting (Fast speed). The score screen crowns me (Military 205 in the first); the enemy, alone on team 2, ends with Military 0. No `skywalker`. |
| `fight` with a blind sweep (4 runs, 47-60 min) | waves sent around the ring of start positions never met the enemy: two were on Shoreline (half the ring in the sea), and with only `forceexplore` the fog hid the enemy's base from the minimap. Hence the aiming, and All Visible. Long, clean runs all the same. |

[The Ewoks in the enemy's ruins](screenshots/fight.png) and [the score screen](screenshots/fight-score.png) are the first run.

Hardest allies against an Easiest enemy is not possible: the game has one
difficulty for every computer, so it is Easiest for all. The
Ewoks win the game alone.

Not yet covered: missions 6
and later and the other five campaigns past mission 1, the in-game Load
(the main menu's Saved Game is covered), and multiplayer. In a campaign the
monkey can scroll the view into unexplored black and stay there (`H` has no
command center to return to).

`tools/coverage_test.c` checks the host's unresolved-call stop without the
game (build line in the file): exit code 5 and the line the seed tool reads;
`SWGB_ICALL_CONTINUE=1` reports once and carries on; eh32's hook still sees
each address first.

### Clone Campaigns (2026-11-22)

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

On the `/O1` build with the code-pointer seeds, four at once:

| Run | Setup | Game time | Result |
|---|---|---|---|
| skirmish | Republic against 6 computers (Confederacy, Republic in turn), Normal Fortress map, Moderate, Fast | 1846 s | ran the full time and the game to its end: the Achievements screen at 47:01 game time; saved at 40% |
| skirmish | Confederacy against 6 (Republic, Confederacy), Giant Nova Lake, Moderate, Fast | 1846 s | ran the full time; saved |
| campaign | Confederacy, mission 1 (Strategic Retreat), Easy, the monkey | 1671 s | ran the full time (41 minutes of game time); saved |
| campaign | Galactic Republic, mission 1 (Alaris Prime), Easy, the monkey | 1671 s | ran the full time; saved |

1.9 game-hours, every run ended by the watchdog (exit 4): no fault, no
unresolved indirect call, no not-lifted stub, no eh32 miss, and nothing new
for `seeds_from_logs.py --cc`.

## The game's three resolutions

```
py -3 toolses_check.py 800|1024|1280 [--out DIR] [--game DIR] [--no-options]
```

Headless (`--headless --multi`). It picks Screen Size on the 800x600 Options
menu (radio buttons 800x600 at 453,383, 1024x768 at 453,413, 1280x1024 at
453,443), clicks Single Player, Standard Game and Start Game, and after the map
loads (~85 s) takes shots into DIR (BMP and PNG): `a_start`, `b_selected` (the
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
`toolses_check.py SIZE --no-options`: it reaches that size without touching
Options. In `--keys` the cursor is in game pixels (the host does not scale it); the
letterboxed mouse is tested with `wmove`/`wclick` (next section).

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

The tests fail on the build the windowed check was run on (PR #6 before its
fixes): 22 of the 48 cases in group A, among them every right and bottom edge
and the cursor coming back; with the fixes all pass. Run it against a build of
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
pacing). First run, 2026-11-22, 2560x1440 at 180 Hz: 40/40. **It takes over
your mouse and keyboard for about five minutes**
(`-Quick`: one shape, about three); do not touch them until it prints the
summary. Captures and the log are in `%TEMP%\present_check_real`. The Options
size change while fullscreen is only in the headless tool.
