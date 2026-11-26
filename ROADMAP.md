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
  showed 105), the 100 ms timer redrawing only for a text field; the window
  shrinking a little at every mode change (the game sizes its window from
  `GetWindowRect` and `GetClientRect`: both are now the mode's, and its own
  moves of the window are left to softdd).
- **Tested without a person:** `tools/present_check.py` (headless, a virtual
  cursor the game reads back as it reads the real one): edge scrolling on all
  four sides and in each bar in every window shape, in fullscreen, at
  `--res 1920x1080` and after an Options size change; fullscreen kept at a
  mode change; the cursor back in the window; pacing near the refresh.
  `tools/present_check_real.ps1` is the same on the real screen, for the user.
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
- **Done (2026-11-21, 2026-11-22):** a second lift and host. Steam's
  `battlegrounds_x1.exe`, unwrapped (`tools/steamstub_swgb.py`), is the
  program; `battlegrounds_cc.exe` is the same build with a community
  widescreen mod in it (2560x1440, `wide1280*.drs`, a `uage.dll` loader at
  the entry), so it is refused. `run_lift.py --build cc`: 21,069 functions,
  0 lift errors, `src/recomp/gen_cc`; its seeds found again (the `THIS_COD`
  tables, the pointer-called one, `tools/code_ptr_seeds.py`'s 12).
  `swgbrecomp_cc.exe` (`cmake/clone_campaigns.cmake`) is the base host's
  sources, options and libraries with `SWGB_CC`: the engine's addresses and
  the game screen's moved fields in `src/runtime/guest_addrs.h`
  (`tools/x1_addrs.py`), so frame pacing, the profiler and `--res` work on
  it. `/O1`, its hot files `/O2`, 16 MB stack: the `/Od` stack overflow was
  `/Od`'s frames. Conformance `--cc` 5/5; `soak.py --cc`: two 28-minute
  skirmishes with the Republic and the Confederacy (one to its end) and both
  Episode II campaigns' first missions, 1.9 game-hours, no fault, no
  unresolved call. The films play with `--films` at the game folder.
- **Next:** the Episode II campaigns past mission 1; the base game's six
  campaigns under the expansion's exe; `--res` at more sizes; a launcher
  entry (the integrator).
- **Shared, found here:** a profile saved by the widescreen mod's exe makes
  the plain one set 2560x1440 and draw 800x600 in its corner: pick a Screen
  Size in Options once. The sprite decoder's indirect tail jumps nest on
  the host stack (pcrecomp's lifter could return to a trampoline instead).
- **Risk:** low. It is the same engine, and every host and softdd fix is
  shared.

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
  SDL2: DirectDraw, DirectSound, GDI with fonts, windows and their controls,
  a scaling SDL2 screen. Tiberian Sun, Red Alert 2 and Civilization III run
  on it natively on Linux (Red Alert 2 on macOS too), with no Wine. The
  whole lift passes gcc's front end as it is (54 files, 64-bit based, no
  errors), and gcc `-O1` builds it in minutes. win32hle answers 237 of the
  game's 258 imports; `src/linux/shims.c` answers the other 21.
- **Design** ([docs/linux-macos.md](docs/linux-macos.md)): a second host,
  `src/linux`, on win32hle. Its DirectDraw and `screen.c` replace softdd
  there, and softdd stays the Windows one. Portable pieces are shared
  (`inflate.c` through `guest.h`, the `--keys` grammar in `keyscript.c`).
  `cmake/native.cmake` is the build, reached from `CMakeLists.txt` on Linux
  and macOS.
- **Done:** the native host (`-m32`, clang) boots, plays and passes the
  harness on Linux: conformance 5/5, `tools/test.py` smoke and the base
  game's functional checks (films, `--res`, the three resolutions, `--fps`
  perf at the Windows figures, civilization skirmishes saved and loaded).
  Main's Windows features run there too: the `--keys` grammar and
  `--keys-live`, the unresolved-call stop, `--res`, `--fps`/`--fps-log`,
  `--profile`, `--films`, DirectPlay and Clone Campaigns' host.
- **Open:** the soak's `fight` razes the enemy's base but no victory comes
  (docs/linux-macos.md, "Found by the soak"). The in-game cursor is drawn
  (win32hle `GetClipper` now returns the real `DDERR_NOCLIPPERATTACHED`;
  the old code made `sub_0049F080` return before the sprite blit).
- **Work:** `setup.sh`'s native path; macOS (64-bit based, needs a Mac;
  `dplay.c` there needs guest-pointer conversion). Music is done (below).
- **Estimate:** macOS one more week.
- **Risk:** medium. The hard part exists, and the rest is the bring-up loop
  the sibling ports went through.

### Music (CD audio from the Steam Ogg tracks)
- **Known:** the game plays its in-game music from disc 2's audio tracks
  through MCI `cdaudio` and sets its loudness on the mixer's CD line; Steam
  has `Music\Track02-50.ogg` instead. Measured (docs/music.md): every game
  start plays tracks 2-49 in one play with notify and repeats it at the
  end; the menus, victory and defeat use the game's own MP3 streams, not
  the CD. Clone Campaigns' `WIN32.dll` (ogg-winmm) would play the Oggs on
  Windows, but its music is off on this machine (`Music Volume` 9900), so it
  never opens the CD here.
- **Done (2026-11-26):** `src/runtime/cdaudio.c`, both hosts: MCI open, set
  (ms, MSF, TMSF), status, play from/to, seek, stop, pause, close and
  `MM_MCINOTIFY` (successful, aborted, superseded), over the Oggs with
  stb_vorbis, on `waveOut` (Windows) or win32hle's audio stream; the
  mixer's CD line is the music slider (the wave line, the effects' slider,
  stays WINMM's). `--music off|native|DIR`. Test runs stay muted. Tests:
  `unit/cdaudio` and `music/base` (tracks on time, the notification, the
  game's repeat), on Windows and Linux (WSL).
- **Open:** a listening test on real speakers; Clone Campaigns with its
  music on (its slider), which no test profile has.

### Stability / coverage
- **Goal:** a complete skirmish against computer players, and a campaign
  mission, with no crash.
- **Found:** the catalog lost real functions that are reached only through
  a pointer. A switch's index table decoded as code made a false function
  that ran over the real function behind it, and the data scan then dropped
  the vtable slot as "inside a body". `0x005648A0` ended skirmishes about two
  minutes in.
- **Done:** every code pointer in data, and every code-pointer immediate,
  classified (`tools/code_ptr_seeds.py`; docs/lifting.md, "Pointers the
  catalog dropped"). 19 seeds: 3 vtable slots, 16 callbacks. 251 rejected:
  SEH continuations, labels and noise. The lift is 20,820 functions, and
  no existing function's C changed. An unresolved indirect call now stops
  the run at the miss with exit code 5 and names the address, instead of
  faulting later in an unrelated function (`src/runtime/coverage.c`).
  `tools/seeds_from_logs.py --merge` turns a player's log into a seed.
  `tools/soak.py` runs long headless games: varied skirmishes, a monkey,
  a campaign's first mission, at Fast speed with a save mid-game.
- **Soak results (2026-11-22, headless, about 7 game-hours):** 14 runs of
  25-30 minutes: skirmishes with 1-7 computers on Tiny to Giant maps, the
  monkey, and the first mission of three campaigns, all at Fast speed with a
  save. Before the fix, 2 of 6 faulted in a THIS_COD span blitter that reads
  the byte before the back buffer (at 13 and 25 minutes). softdd now puts
  4 KB of slack around every surface, and all 8 runs since went the full
  time. No unresolved indirect call in any run. A traced pair
  (`--firsthit`) shows why long games matter: new code still turns up at
  minutes 5-28 (the computers' attacks, the save, blitter variants). Also
  fixed: the fault report's indirect-call ring was empty after 2^31 calls.
- **Games to the end (about 4.4 more game-hours, no events):** natural
  defeats against Hardest computers (three: at 7-12 minutes), each with the
  score screen; a victory after 22 minutes of a 7-computer game (the chat
  cheat), the score screen's tabs, Main Menu, and that game's save loaded
  from Saved Game and played 15 minutes more; OOM-9's missions 1-5 played in
  a row and won, with their briefings, triggers, score screens and Scenario
  Menu; two older saves (one a campaign's) loaded and played.
- **A victory by combat (merged main, three runs):** waves of Simon the Killer
  Ewok (`simonsays`) aimed by a live controller at the enemy's colour on the
  minimap (`--keys-live`, `soak.py --mode fight`) took the base apart in one to
  three and a half minutes at Fast speed. The game declared each victory itself (`won1.mp3`, the score
  screen crowning the player). No fault and no unresolved call. `--keys` gained
  drags (`ldown`/`lup`: box select) and `ctrl+`/`shift+`.
- **pcrecomp (for its maintainers):** a vtable run in a data section should
  outrank a decoded body, and switch index tables should be data
  (docs/lifting.md).
- **Left:** missions 6 and later, and the other
  campaigns past mission 1; the in-game Load; the 1024 and 1280 HUDs under
  the monkey (its clicks are 800x600); the pcrecomp fixes above.
- **Risk:** medium: each miss is cheap to fix once seen, but only play
  finds them.

### 8. Performance
- **Done (2026-11-22, headless, [docs/performance.md](docs/performance.md)):**
  a sampling profiler (`--profile FILE`, `tools/profile_report.py`: self and
  inclusive time by lifted function, host function, module and Windows
  call). The menus wait for input instead of spinning (97% of a core to
  10%); the frame rate defaults to the display's refresh (`--fps off`: the
  game's own); indirect calls are cached (`resolve.c`: the dispatch binary
  search was 13-31% of the game thread); the presenter draws only the rows
  that changed, with a separable, bit-exact scaler (`scaler.c`); a WM_PAINT
  no longer re-arms the pacing timer and re-reads the refresh every frame.
  Skirmish at 800x600: 97% of a core to 18%; at 2560x1440 fullscreen
  (`play.ps1`'s case): 98% for 111 frames shown a second to 38% for 180.
- **Tried:** `/O2` on the hottest generated files (`GEN_HOT_FILES`, now the
  four hot files by default): as fast to compile as `/O1`, a few percent at
  2560x1440, nothing at 800x600 (pcrecomp's flag helpers, not inlined at
  `/O1`). `__forceinline` on the helpers made `/O1` 7x slower to compile:
  not kept.
- **Real screen (2026-11-22, with the user's go-ahead):**
  `present_check_real.ps1` 38/38; 2560x1440 fullscreen in game: 95% of a core
  for 128 frames shown a second before, 45% for 180 after.
- **GPU presenter:** `--present d3d11` (`present_d3d.c`), opt-in: real-screen
  check 38/38 and text fields fine, but the same CPU as change-only GDI here.
- **Left:** `/O2` for the whole lift (one full build to time it); callbacks
  into the window procedure without an exception each (pcrecomp).
- **Risk:** low.

### 9. Multiplayer (LAN)
- **Known:** the game uses DirectPlay 4 (dplayx) for a small set of calls, plus
  IDirectPlay's EnableNewPlayers ([docs/multiplayer.md](docs/multiplayer.md)).
  Windows' own DirectPlay works through native32's bridge but can't host on a
  machine whose WSL mirrored networking holds port 47624, and isn't on Linux.
- **Done (2026-11-22, headless, loopback):** a DirectPlay of the host's own
  (`dplay.c`; `--dplay native` keeps Windows'): sessions over TCP through the
  host process, UDP discovery on port 2300. Two recomps find the game, join,
  meet in the setup screen, start and play. The game's own sync checkpoints are
  logged (`[sync]`) and compared by `tools/mp_pair.py`.
  Results: 25 minutes of the base game and 20 of Clone Campaigns, every
  checkpoint the same on both sides; a different build is refused at the join.
- **Left:** two real machines (a firewall Allow rule on the host's exe); chat, more players,
  drops mid-game, saved multiplayer games; answering the startup version check
  and the ordinal imports without dplayx.dll on disk (Linux); playing the
  original exe (`--dplay native`, a working dplaysvr, the x87 transcendentals).
- **Risk:** medium: out-of-sync shows only in long games and across machines.

## Later

## Done
- Text in 8-bit surfaces' DCs (labels, buttons, the HUD) in the palette in
  use.
- Text fields: the game's EDIT controls composed into softdd's frames, with
  the caret; no ghost box; focus kept across a trip to another window.

## Out of scope
- Shipping any game file or generated source.
- The Zone.com service, which no longer exists.
