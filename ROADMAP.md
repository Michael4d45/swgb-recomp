# Roadmap

## Next
- Play a skirmish for real: train workers, build, a few minutes against the AI.
- Campaign 1 mission 1.
- The mouse cursor (the game draws its own from `mcursors`) in recordings.
- netlab QA on a test VM (it reports SKIP without game data).

## Feature tracks, most feasible first

Each track says what is known, the work, and the risk. The order is by
feasibility: what is already half there, then what is mechanical, then
what needs research or a new subsystem.

### 1. The game's own resolutions (800x600, 1024x768, 1280x1024)
- **Known:** softdd takes any mode the game asks for. Options -> Screen
  Size -> 1024 x 768, then a Standard Game: `SetDisplayMode(1024, 768, 8)`,
  a 1024x768 primary, and the map and HUD laid out for it (checked
  2026-11-21). The menus stay 800x600, as in the original.
- **Done (2026-11-21, headless):** all three sizes run a skirmish end to end
  with `tools/res_check.py` (see `docs/testing.md`, which has the shots):
  `SetDisplayMode` gets 800x600, 1024x768 or 1280x1024; each shot is the
  size of the mode; the HUD is the game's art for that width; the command
  center selects with a click at game pixels; the cursor held on each map
  edge scrolls (the view goes on into unexplored black, the minimap's box
  stops at the diamond's tips); a click on the minimap moves the view; the
  Menu button, Quit Current Game, the score screen and Main Menu work, and
  the score screen and the menus are 800x600 again (`SetDisplayMode(800,
  600, 8)` on the way back). The Screen Size is saved when the game exits
  normally (the registry's `Screen Size`: 800, 1024 or 1280) and the next
  run starts the skirmish at it, with no Options clicks.
- **Fixed:** (1) The options live in HKCU, shared by every run, so a run that
  chose 1280 changed the next agent's (or the user's) skirmish: with
  `--multi` each `--game` folder now has a private copy (`host.c`,
  `reg_isolate`). (2) Windowed, softdd placed the window once, at the
  menus' 4:3, so 1280x1024 (5:4) would be stretched sideways; the window
  now takes the new mode's shape at the same height unless the player has
  sized it (`softdd.c`, `fit_window`). Hit-testing was already
  proportional (`to_game`).
- **Not checked:** a real window (headless only: no screen was used), so the
  `fit_window` change and a real mouse through `to_game` at 1280x1024 are
  untested on screen; the shots are not compared with the original. The
  scripted cursor is in game pixels, so `to_game` is not exercised by
  `--keys`. A drag box-select (`--keys` cannot drag), and a map larger
  than the starting area, were not tried. The skirmish map and
  civilization are random per run.
- **Risk:** low.

### 2. Presentation: scaling, fullscreen, pacing
- **Done (headless-verified; the windowed check is the user's):** a new
  presenter (`src/runtime/present.c`) letterboxes the frame at its own
  aspect in any window shape; filters `sharp` (default), `smooth`, `nearest`
  and `integer` (`--filter`, Alt+F12); borderless fullscreen (`--fullscreen`,
  Alt+Enter) on the window's monitor, never a display-mode change; presents
  coalesced to the display's refresh, with the last frame always shown by a
  timer; `--shot`, `--record`, `--frames` and the text-field composition
  still get every frame they need. The mouse (`to_game`, and so
  `click_child`) goes through the letterbox rectangle. Flags and hotkeys:
  `docs/building.md`; tests: `docs/testing.md`, `tools/present_test.c`.
  Measured: loading is 60,000+ presents a second, an idle menu 4 or 5; the
  shown rate follows the refresh.
- **Found by the user's windowed check, fixed:** edge scrolling (the game
  compares `GetCursorPos` with its window's `GetWindowRect`; the window now
  reports the game's own rectangle), the cursor kept to the window in
  fullscreen, fullscreen kept through a mode change, the game's cursor
  restored on re-entry, a steady pacing beat (210 presents a second at 180 Hz
  showed 105), the 100 ms timer redrawing only for a text field.
- **Left:** remember the window size, filter and fullscreen between runs;
  `softdd`'s `GetMonitorFrequency` still says 60. The CPU scaler costs 3 ms (1080p) to
  11 ms (4K) a frame for `sharp`/`smooth`: a GPU path (Direct3D 11) would
  take it off the game thread.
- **Risk:** low.

### 3. Widescreen and any resolution
- **Done (2026-11-21):** `--res WxH` / `--res desktop` (`src/runtime/widescreen.c`,
  `docs/building.md`). The 1280 x 1024 Screen Size becomes WxH; in a game
  the interface is laid out at the largest of the game's sizes that fits,
  then sits at the bottom, centred, with black beside it, and the map view
  takes the rest. Checked at 1024x600, 1280x720, 1366x768, 1600x900,
  1920x1080, 2560x1440 and 3440x1440: selection, command buttons, minimap,
  edge scrolling, menu and dialogs (centred on the whole screen), the
  Options label, quitting and playing again.
- **Found:** the game object's virtual `set_screen_size` (vtable 0x00665A20
  + 0x68, `sub_005ED2D0`) gets prog_info +0x8F4/+0x8FC (the registry's
  "Screen Size", the 800/1024/1280 switches, Options -> `sub_0042C100`). The
  game screen (vtable 0x0065CE24) lays itself out in `sub_004F5210` from
  `screen_coords.txt` (DRS 'bina' 50006), with a column per width chosen by
  an exact match (else 800). Its draw `sub_004FEDB0` blits one
  screen-sized interface SLP at 0,0 (`sub_005415D0`); the unit-info panel
  (`sub_005D5D00`) and the resource counters (`sub_005D4930`) pick their
  800/1024/1280 variants by exact widths. All of these are indirect calls,
  so the host takes them over; nothing in the lifted C changes.
- **Left:** the sides of the interface are black (stretching or tiling the
  art's edges would look better); panels hidden at layout time and placed
  when shown (some popups) may still use base-layout places; switching the
  viewed player (`sub_0042F880`, debug and scenario tests) reloads the art
  by the screen's real size; switching to WxH from the in-game Options is
  untested, because after an in-game switch to 1024x768 the menu button no
  longer answers, with or without `--res` (a track 1 lead).
- **Risk:** done; low for the rest.

### 4. Clone Campaigns
- **Known:** the Steam install has `battlegrounds_cc.exe` without the
  SteamStub wrapper (no `.bind` section, `.text` entropy 6.5 against 8.0
  for the wrapped `battlegrounds_x1.exe`), PE stamp `0x3CF57142`
  (2002-05-29), the same 14 DLLs and 258 imports as the base game, image
  0x3B1000 bytes (fits below 0x00A5A000). `host.c` has no exe-specific
  address but the image base.
- **Work:** confirm `battlegrounds_cc.exe` is the same program as the
  unwrapped `battlegrounds_x1.exe` (`tools/steamstub_swgb.py`, then
  compare); teach `run_lift.py` a second build: its own catalog, its own
  generated sources and target, the ~50 per-exe seeds (the `THIS_COD`
  blitter's jump tables and the rest) found again, its EH frames; the
  `_x1` data (`genie_x1.dat`, `*_x1.drs`, `language_x1.dll`). Every softdd
  fix is shared.
- **Risk:** medium, mostly time: a second 4M-line lift and build. It
  unlocks most community work (Expanding Fronts, the widescreen patches)
  as references.

### 5. Higher frame rates
- **Known (measured 2026-11-21, [docs/framerate.md](docs/framerate.md)):**
  drawing and simulation are separate. In game the engine draws one frame
  (one Blt to the primary, `sub_00471210`) per pass of the game screen's
  idle, `sub_004F6EA0`, behind a gate of `[screen+0x1258]` ms (1 ms, from
  the startup settings): ~920 frames a second headless here. The world
  update (`sub_0061AB70`) steps only when 50 ms have passed, by the real
  time elapsed (at most 100 ms) times the speed: 20 steps a second, and
  the game clock gains 1500 game ms a wall second at Normal speed at any
  frame rate from 10 up. Of the ~920 frames, only 8-30 a second differ
  from the one before (one per step, and the scroll). `0x00428A80` is the
  base game idle; its `timeGetTime` calls time its own housekeeping, not frames.
- **Done:** `--fps N` (the gate set per pass, the host waiting between
  frames instead of spinning a core) and `--fps-log` (frames, changed
  frames, steps, the game clock, DirectDraw and `timeGetTime` calls by
  caller), in `src/runtime/framerate.c`. Game speed unchanged at 60, 30
  and 10 (game clock and training progress); at 5 the 100 ms step cap
  halves it. `--fps 60` takes 9% of a core in game against 98%.
- **Next:** more frames do not move units more smoothly: they move once a
  step. Smooth motion needs interpolated drawing (objects drawn at their
  position plus velocity times the time since the step: the view's object
  pass, sync untouched) or a smaller minimum step (the `0x32` at
  `0x0061ACAA`; single player only, and untested AI and pathing at small
  steps). Both are changes to lifted code.
- **Risk:** low for `--fps`; medium-high for smoother motion.

### 6. Cutscenes
- **Done (branch `track/cutscenes`):** the films play, headless and in
  `--record` too ([docs/movies.md](docs/movies.md)). `ICInfo` says yes to
  Indeo 5 when ffmpeg is on `PATH`; `MCIWndCreateA` makes the host's own
  child window (`src/runtime/movie.c`), which answers the MCIWnd messages
  the game sends (`GETPALETTE`, `PLAY`, `GETMODE`, `STOP`, `PAUSE`,
  `RESUME`, `WM_CLOSE`). Two child `ffmpeg` processes decode the video
  (raw frames on a pipe) and the sound (PCM to `waveOut`, whose position is
  the clock); softdd's `compose()` prints the window into its frames (16-bit,
  or dithered into the film palette with the game's `8BITVIDEO`). The
  game's own skipping works (a click; Enter, Space, Esc). A missing or
  broken film is skipped, as before. `--films DIR` finds films elsewhere.
  Tested with the Saga's Clone Campaigns `xlogo1.avi` and `xintro.avi` in
  place of the base game's `logo1` and `intro`: every frame shown, the
  intro's last frame 20 ms after its time over 70.7 s.
- **Left:** disc 2's own films (`logo1`, `intro`, and the campaigns'
  `INTRO` and `celebration`): untested without the disc, though the path and
  format are the same. `--record` has no sound.
- **Risk:** low now.

### 7. Linux and macOS
- **Known:** pcrecomp's `win32hle` implements Win32 on libc, POSIX and
  SDL2: DirectDraw, DirectSound, GDI with fonts, windows and messages, a
  scaling SDL2 screen. Tiberian Sun and Red Alert 2 run on it natively on
  Linux (Red Alert 2 on macOS too), with no Wine.
- **Gaps:** EDIT controls (win32hle has none; the text fields and their
  composition use `WM_PRINT`, `SetWindowRgn` and class styles); MCIWnd
  (shared with 6, if the decoder lives in the host); `host.c`'s Win32
  specifics (window subclassing, `GetGUIThreadInfo`, the private desktop
  for headless runs); softdd against win32hle's DirectDraw (keep one); the
  game's `WIN32.dll`, Steam's winmm wrapper that plays the CD music from
  Ogg Vorbis (libvorbisfile); a 32-bit gcc or clang build of 4M lines.
- **Risk:** large but plausible: the hard part exists; the gaps are
  bounded.

## Later
- DirectPlay multiplayer over LAN.

## Done
- Text in 8-bit surfaces' DCs (labels, buttons, the HUD) in the palette in
  use.
- Text fields: the game's EDIT controls composed into softdd's frames, with
  the caret; no ghost box; focus kept across a trip to another window.

## Out of scope
- Shipping any game file or generated source.
- The Zone.com service, which no longer exists.
