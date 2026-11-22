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
- **Work:** check 1280x1024 the same way; a scripted run per size in
  `docs/testing.md`; screenshots against the original.
- **Risk:** low.

### 2. Presentation: scaling, fullscreen, pacing
- **Known:** windowed, the frame is `StretchDIBits`-ed to the whole client
  area (85% of the screen height at 4:3, or `--scale N`), so a window
  resized to another shape distorts it, and there is no smoothing choice
  or fullscreen. A primary without a back buffer presents on every
  `Unlock`/`ReleaseDC`: hundreds of presents a second in the menus.
- **Work:** aspect-correct letterboxing; nearest, integer and smooth
  scaling (pcrecomp's `win32hle/screen.c` has these modes for SDL2 to
  borrow from); a borderless fullscreen toggle (Alt+Enter) that never
  changes the desktop's mode; coalesce presents to the display's refresh.
  Mouse mapping (`to_game`) and the text-field composition follow the
  letterbox.
- **Risk:** low. All of it is in `softdd.c` and `host.c`.

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
- **Known:** nothing measured yet. present counts are not frames (see 2).
  Expanding Fronts advertises 60 fps, so the original runs below that in
  some states.
- **Work:** count real frames in a skirmish (Flips per second against the
  wall clock); find the engine's limiter (the `timeGetTime` loop around
  `0x00428A80` is a first lead); see what is tied to it (simulation speed,
  animation, scrolling). Raise the cap only where drawing is independent
  of the simulation tick.
- **Risk:** medium-high. If the simulation steps per drawn frame, a higher
  rate changes game speed and multiplayer sync; a 2D sprite engine has
  nothing to interpolate.

### 6. Cutscenes
- **Known:** the game plays its films through `MCIWndCreateA` (MSVFW32).
  The AVIs are Indeo 5 (`IV50`), which Windows 10 and 11 no longer ship
  a decoder for, and an MCIWnd is a child window, which only reaches the
  picture through softdd's composition. The Steam install has only Clone
  Campaigns' `xintro.avi` and `xlogo1.avi`; the base game's campaign films
  are on disc 2 (`Game\Avi`), which Steam does not have.
- **Work:** shim the MCIWnd calls the game makes; decode the video and
  audio with FFmpeg's libraries (Indeo 5 is supported) into softdd frames
  and DirectSound; skip on Esc or a click; the same path headless and in
  `--record`. Test with the Clone Campaigns intro, then disc 2's films.
- **Risk:** medium. A new subsystem, and base-game films need disc 2 to
  test.

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
