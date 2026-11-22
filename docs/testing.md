# Testing

Two checks, both headless: the conformance harness, and a scripted run into a
skirmish. Neither opens a window on your screens: `--headless` runs on
pcrecomp's private desktop (`native32_headless`), which is never shown, and
mutes the sound.

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

Exit code 1 on any FAIL; the full log is `work\conformance.log`. Without the
game data (`game\`, `work\Battlegrounds.exe`) every milestone is SKIP
and the exit code is 0, so it is safe on a machine with no copy.

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
cl /nologo tools\present_test.c src\runtime\present.c /Isrc\runtime user32.lib gdi32.lib
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
choice again, not the arrow. **Pacing**: with `--refresh 600` and a game that
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
