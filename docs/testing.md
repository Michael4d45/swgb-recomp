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
is `seconds:action`: `move:X:Y`, `click:X:Y`, a key name, or `shot=FILE.bmp`.

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
Options. In `--keys` the cursor is in game pixels (the host does not scale it),
so the scaling of a real mouse (`to_game`) is not exercised here.

