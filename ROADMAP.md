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
- **Known:** the game offers three 4:3 sizes and picks the mode from them;
  softdd draws any size. The community does this for Clone Campaigns by
  patching the exe (Expanding Fronts, the All-In-One patch, the WSGF
  patch); their changes are a map of which constants matter.
- **Work:** find where the Screen Size choice becomes `SetDisplayMode`'s
  arguments and the checks on it; a host option (`--res WxH`, or the
  desktop's size) that replaces the 1280x1024 entry; then the layout: the
  HUD's interface art is drawn for 800, 1024 and 1280 wide, so the panels
  need placing (or a filled gap) at other widths, and the map view's
  clipping, scrolling limits and the minimap need checking.
- **Risk:** medium. The mode is easy; a HUD that looks right is the work.

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
