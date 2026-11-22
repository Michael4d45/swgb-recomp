# Performance

Where the CPU goes, what was changed, and how to measure it. Measured
headless (`--headless --multi`, pcrecomp's private desktop) on 2026-11-22,
v1.1, a 20-thread machine, the display at 180 Hz; the scripted skirmish of
[testing.md](testing.md) at 800x600 and at `--res 2560x1440 --fullscreen`
(what `play.ps1` runs on a 2560x1440 monitor). Headless runs normally never
draw into the window; these were run with `SWGB_PAINT_HEADLESS=1`, so the
presenter's scaling and GDI copy are in the numbers too.

## In short

| Scenario | | CPU (process, % of a core) | frames drawn/s | shown/s | CPU ms a drawn frame |
|---|---|---|---|---|---|
| Menus, idle | before | 97% | 4 | 4 | (idle loop: ~130,000 passes/s) |
| | after | 10% (game thread 6%) | 4 | 4 | (idle loop: ~1,000 passes/s) |
| Skirmish 800x600 | before, default | 97% | 901 | 180 | 1.08 |
| | before, `--fps 180` | 39% | 184 | 179 | 2.12 |
| | **after, default** | **18%** | 184 | 181 | 0.98 |
| | after, `--fps off` | 97% | 876 | 180 | 1.11 |
| Skirmish 2560x1440 fullscreen | before, default | 99% | 150 | 96 | 6.6 |
| | before, `--fps 180` (`play.ps1`) | 98% | 115 | 111 | 8.5 |
| | **after, default** | **38%** (55% with `--fps-log`) | 185 | 180 | 2.1 |
| | after, `--fps off` | 97% | 335 (530-800 not painting) | 179 | 2.9 (1.2-1.8) |
| Loading a map | before / after | 97% / 69-77% | (coalesced) | | under a second either way |

"Before" is `main` at c4eed56 plus the profiler and the `--fps-log` fix;
"after" is this track. `--fps-log`'s own frame compare costs ~15% at
2560x1440, so the 1440 "after" is given without it as well. The uncapped
1440 rows vary with the random map (the view's contents). A drawn frame is
one Blt to the primary; "shown" is what the presenter drew into the window
(one a refresh at most).

So: in the menus the game no longer spins a core; in game it draws at the
display's refresh by default (it drew 5 frames for every one shown); and at
2560x1440 the player gets the full 180 frames a second for 38% of a core,
where `play.ps1`'s `--fps 179` got 111 for a whole core.

## What was changed

1. **The idle loop waits** (`framerate.c`). The game's message loop calls
   `PeekMessageA` and, when there is no message, its idle, with no `Sleep`
   anywhere. In the menus that was ~130,000 idle passes a second, each
   polling every DirectSound buffer's status (`IDirectSoundBuffer::GetStatus`,
   vtable +0x24, from `sub_00544540`) and `timeGetTime` (~600,000 calls a
   second from `0x00428A80`). The host now waits up to 1 ms for input
   between passes (`MsgWaitForMultipleObjects`, so a key or a click ends the
   wait at once): 1,000 passes a second, input never later than before.
2. **The frame rate defaults to the refresh** (`framerate.c`, `present.c`).
   Track 5's `--fps N` pacing is the default, at the refresh the presenter
   paces to: a frame drawn faster than that is never shown. The simulation
   steps on the wall clock (every 50 ms, [framerate.md](framerate.md)), so
   game speed is unchanged from 10 frames a second up. `--fps N` still sets
   it; `--fps off` is the old behaviour. The refresh is looked up again
   every 2 s (a window moved to another monitor) instead of on every frame.
3. **Indirect calls are cached** (`resolve.c`). Every C++ virtual call and
   function pointer of the lifted code resolves its guest address through
   `recomp_lookup`, a binary search of 20,802 functions: 31% of the game
   thread in the menus, 13% in game. The host's `recomp_lookup_manual`
   (asked first) answers from a 4,096-entry direct-mapped table filled from
   `recomp_lookup` itself; only answers that are the dispatch table's for
   that address are kept, and the cache is bypassed while the caller is a
   `__local_unwind2` (native32 answers those differently). pcrecomp has a
   cache of its own (`RECOMP_RESOLVE_CACHE`), but it is compiled into the
   generated code. `SWGB_NO_RESOLVE_CACHE=1` turns this one off.
4. **The presenter draws only what changed** (`scaler.c`, `present.c`).
   Each shown frame was converted to 32 bits, scaled and copied to the
   window whole: at 2560x1440 that was 57% of the game thread (`scale` 27%,
   `to32` 15%, the GDI copy 15%). Now the frame is compared with the last
   row by row, and only changed rows are converted, scaled and drawn (DWM
   keeps the rest of the window's picture); the scaler is separable (each
   source row scaled across once, picture rows copied or blended with SSE2);
   at 1:1 the converted frame is the picture. It is bit for bit the old
   scaler's (`tools/present_bench.c`). The whole picture is drawn at a
   resize, a mode or display change, a filter change, and once a second.
5. **A WM_PAINT is not damage** (`host.c`, `present.c`). The game draws
   every frame through its own `UpdateWindow` -> `WM_PAINT`, and the host
   treated each one as damage: it killed and re-armed the pacing timer
   (4.5% + 1.4% of the game thread) and asked the monitor's refresh again
   (`EnumDisplaySettings`) on every frame. A paint now only asks that the
   current frame be shown again (the 100 ms timer does it if nothing new
   comes); a resize or display change still lays out and draws everything.
6. **`--fps-log` measures less of itself**: it found changed frames with a
   64-bit hash of every word (`_allmul` on 32-bit), 41% of the game thread
   at 2560x1440; it now compares with a copy of the last frame.

### The presenter, per frame (`tools/present_bench.c`)

The scaler alone, milliseconds a frame on this machine (old = the one-pass
scaler before this track; "band" = 40 changed rows, a cursor moving;
"same" = an unchanged frame). "gdi" is `SetDIBitsToDevice` into a memory DC
of the picture's size, an estimate of the copy to the window (only changed
rows are copied now).

| frame | picture | filter | old | new, all rows | new, band | new, same | gdi, all rows |
|---|---|---|---|---|---|---|---|
| 800x600 | 1440x1080 | sharp | 2.94 | 1.26 | 0.10 | 0.03 | 0.34 |
| 800x600 | 1920x1440 | sharp | 5.38 | 2.02 | 0.16 | 0.03 | 0.83 |
| 800x600 | 2880x2160 | sharp | 10.73 | 3.63 | 0.24 | 0.03 | 2.38 |
| 1024x768 | 1920x1440 | sharp | 5.77 | 2.40 | 0.17 | 0.05 | 0.77 |
| 2560x1440 | 2560x1440 | sharp | 3.41 | 1.11 | 0.24 | 0.21 | 1.32 |
| 2560x1440 | 3840x2160 | sharp | 16.24 | 8.47 | 0.43 | 0.20 | 3.20 |
| 2560x1440 | 3840x2160 | smooth | 23.73 | 16.00 | 0.84 | 0.40 | 3.25 |

In game 9 to 30 frames a second differ from the one before (the world steps
20 times a second; the cursor, which the game draws itself, moves more
often), so most shown frames cost a compare. `nearest` and `integer` are
scaled by GDI (`StretchDIBits`), cheap at any size, and are also skipped when
nothing changed.

## Where the time goes now

`--profile` (below), the game thread, while it runs:

- **Menus:** 6% of a core; what is left is the idle's own work (x87 helpers
  `fp_push_impl`/`fp_pop_impl` and `sub_00629DA0` and its neighbours).
- **In game, 800x600, 180 frames a second:** 18% of a core. The compare of
  the frame (`scaler_run`, 14% of the samples), softdd's Blt of the frame to
  the primary (`memmove`, 13%), the flag helpers of the generated code
  (`recomp_cond`, `recomp_cond_cf`: 17%), the lifted code itself (24%),
  window calls (10%).
- **In game, 2560x1440:** 38% of a core; the game's own frame is about
  2 ms: lifted code and its flag helpers (`sub_006094D0`, the map view's
  drawing, is 21% on its own), softdd's Blt, the compare.

`recomp_cond` and `recomp_cond_cf` are `static inline` in pcrecomp's
`recomp_types.h`, compiled into every generated file; at `/O1` they are not
inlined, so every lifted conditional branch whose flags come from another
block is a call. The experiment below tried `/O2` on the hot files, and
`__forceinline`.

## The generated code at /O2

The 53 generated files (3.9M lines) are built at `/O1` because `/O2` on all
of them stalled a build farm (Shogo's lift). Tried here on what the profiles
say is hot:

- `profile_report.py --hot` lists the lifted functions that cover 90% of
  the lifted code's own time (with the helpers they call): 186 functions
  over both resolutions, 86,000 lines, in 32 of the files; 4 files
  (`recomp_0040.c`, `0051`, `0050`, `0022`) hold 93% of it at 2560x1440.
- **Compile time is not the problem for these:** at `/O2 /Ob2` the four
  whole files took 27 s (`recomp_0040.c`, 224,000 lines), 20 s, 11 s and 4 s
  each, single-threaded (the 186 functions alone: 128 s, one chunk of them
  taking most of it).
- **The gain is small.** With the 186 functions at `/O2` (linked first) the
  flag helpers' time fell by 60% (0.31 to 0.12 and 0.66 to 0.27 ms a frame
  at 2560x1440, uncapped) and the whole frame by 6% and 23% in two runs; at
  800x600 nothing measurable (1.17 to 1.18 ms). With the four files at
  `/O2` in a lib of its own, capped at 180 at 2560x1440 with the presenter:
  38% (36, 41, 37) against 35% (39, 32, 35) of a core, helpers 0.49 to
  0.30 ms a frame on average: within the run-to-run spread of the random
  maps.
- So `CMakeLists.txt` gets `GEN_HOT_FILES` (generated file names) and
  `GEN_HOT_OPT` (`/O2`), set to those four files. It applies only to a full
  build of the generated code (not with `SWGB_GEN_LIB`), costs nothing in
  compile time (at `/O1` the four took 31, 29, 17 and 5 s; at `/O2` 27,
  20, 11 and 4 s) and buys a few percent at 2560x1440. A re-lift moves
  functions between files: pick them again with `--hot`
  (`-DGEN_HOT_FILES=` to turn it off).
- **`__forceinline` on the helpers is worse:** with it, `recomp_0040.c` at
  `/O1` took 237 s to compile instead of 31 (tried in a pcrecomp worktree,
  not kept). `/O1` inlining big functions is what is slow; `/O2` is not.
  These numbers also suggest `/O2` for the whole lift may no longer stall
  (the files here compile as fast at `/O2` as at `/O1`); that needs a full
  build of the generated code to confirm.

## The GPU: `--present d3d11`

Only the last step can move to the GPU: the game draws everything itself
into an 8-bit frame in memory (lifted software rendering). `present_d3d.c`
takes over from the converted frame: the changed rows are uploaded to a
texture, the GPU scales it into the letterbox (the `sharp` filter as a pixel
shader, `smooth` bilinear, `nearest`/`integer` point sampling), and a
flip-model swap chain hands it to DWM. Anything that fails (no Direct3D 11,
no `d3dcompiler_47.dll`, a lost device twice) falls back to GDI for the run.

On the real screen (2026-11-22) it passes `present_check_real.ps1` 38/38
(windowed shapes, bars, fullscreen, edge scrolling read from screen
captures), and a text field (New Player) shows what is typed. It does not
save CPU here, because after change-only drawing the presenter is a small
part of a frame:

| 2560x1440 fullscreen, in game, process CPU | GDI | Direct3D 11 |
|---|---|---|
| `--res 2560x1440` (1:1) | 34%, 38%, 40%, 40% | 34%, 38%, 40% |
| 1280x1024 scaled to 1800x1440 | 18%, 22%, 25% | 20%, 21%, 30% |
| the same, the view scrolling (~20 changed frames a second) | 19%, 20% | 20%, 22% |
| frames shown a second (of 180) | 176-180 | 178-180 |

So GDI stays the default and Direct3D 11 is opt-in: for flip-model
presentation (DWM shows the swap chain without its own copy), a 4K monitor
with a smaller frame (the CPU scaler's worst case, 8.5 ms for a whole frame),
or a machine where GDI is slow. The game thread's remaining cost is the game
itself (above).

## Measuring

```
build\swgbrecomp.exe --run --headless --multi --profile p.txt [--profile-window A:B] [--profile-hz N]
py -3 tools\profile_report.py p.txt [--mode 4] [--top 25] [--callers NAME] [--hot OUT] [--folded OUT]
```

`--profile FILE` (`src/runtime/profile.c`): a thread of the host's own
suspends the game thread at a random point of every millisecond, reads EIP,
copies the top of its host stack and lets it go. The lifted code is C on the
host stack, one C function per guest function, so the stack holds the chain
of lifted callers: values just after a call instruction into the executable
are kept as frames (scanned: no frame pointers, so a few stale ones come
along). Each sample also records the game's mode, whether the thread ran
since the last sample (its cycle count), and the Windows call in progress
(`g_cur_import`). The file has raw addresses and the module list, rewritten
every 10 s and at the end (the watchdog, `ExitProcess`).
`--profile-window A:B`: only seconds A to B.

`tools/profile_report.py` names the addresses with DbgHelp: the host's PDB
(lifted functions are `sub_<VA>`, so the guest address is in the name;
`recomp_types.h` helpers compiled into the generated code are marked
`(gen)` by their source file) and the system DLLs' exports (the names of
syscall stubs in `win32u.dll` are only the nearest export: read them as "a
window or GDI call"). Tables: self and inclusive time by function, self time
by module (the executable split into lifted code, its helpers and the
host), the Windows call in progress, and the lifted functions. Samples
inside framerate.c's `wait_ms` count as waits. `--mode 4` keeps the game,
`--mode 2` the menus. Sampling costs the game thread a few percent.

Also: `--fps-log` (frames drawn, changed and steps a second, CPU) and
`--present-log` (presents and frames shown a second) from
[framerate.md](framerate.md) and [building.md](building.md);
`SWGB_PAINT_HEADLESS=1` to include the presenter in a headless run;
`SWGB_NO_RESOLVE_CACHE=1` and `--fps off` for the old behaviour;
`--present d3d11` for the Direct3D presenter;
`tools/present_bench.c` for the scaler (its first lines have the `cl`
command; "OK" when the pictures match the old scaler's).

## Not done, or left

- **On the real screen** (2026-11-22, with the user's go-ahead):
  `tools/present_check_real.ps1` passes 38/38 on this build (edge scrolling
  in every window shape and fullscreen, the cursor, 180 shown a second at
  180 Hz). The scripted 2560x1440 fullscreen skirmish, in game: before,
  with `--fps 180`, 95% of a core for 143 frames drawn and 128 shown a
  second; after, 45% for 185 drawn and 180 shown.
- Callbacks into the game's window procedure go through an exception each
  (native32's VEH, which also calls `getenv` on every one); in game that is
  ~2 a frame. pcrecomp's.
- `/O2` for the whole lift (above): one full build of the generated code to
  time it and measure.
- Loading: a map loads in under a second here; its progress presents are
  held back by the pacing and cost little.
