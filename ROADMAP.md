# Roadmap

## Next
- Play a skirmish for real: train workers, build, a few minutes against the AI.
- Campaign 1 mission 1.
- Movies: the intro and campaign films go through `MCIWndCreateA` from the
  play disc; check they play, and that headless runs keep them off screen.
- Child windows (the game's text fields are real Win32 edit controls) do not
  appear in softdd's frames; draw them into the picture.
- The mouse cursor (the game draws its own from `mcursors`) in recordings.
- **pcrecomp-ingame into pcrecomp main**, then Setup and the launcher on `main`.
- netlab QA on a test VM (it reports SKIP without game data).

## Later
- **Linux and macOS**: a Wine route (clang-cl or mingw cross-build, as in
  redalert2-recomp's `setup.sh --wine`), tried and tested before `setup.sh` offers it.
- DirectPlay multiplayer over LAN.
- A presenter with scaling (X-Wing's `gpu.c`) instead of `StretchDIBits`.
- The Clone Campaigns expansion (`battlegrounds_x1.exe`).

## Out of scope
- Shipping any game file or generated source.
- The Zone.com service, which no longer exists.
