# Linux and macOS, native

The plan for running the recompiled game as a native Linux program (then a
native Mac one) with no Wine: the same lifted C as the Windows build, on
pcrecomp's `runtime/win32hle` instead of `runtime/native32`. This is how
Tiberian Sun, Red Alert 2 and Civilization III run on Linux, and Red Alert 2
on macOS (their `src/linux`). This page has the design, every gap with its
fix, the build steps and the effort; [linux-bringup.md](linux-bringup.md) is
the checklist for continuing on a Linux machine. **State (2026-11-22): linked and run on Linux (`-m32`). Main-menu GDI text
works with pcrecomp `swgb-linux-palette` (8bpp TextOut). See
[linux-bringup.md](linux-bringup.md).**

## The design

| | Windows (today) | Linux and macOS |
|---|---|---|
| Lifted C | `src/recomp/gen`, MSVC `/O1` | the same files, gcc or clang `-O1` |
| Runtime | pcrecomp `native32`: imports go to the real DLLs | pcrecomp `win32hle`: imports are answered in C on libc, POSIX and SDL2 |
| Host | `src/runtime/host.c` | `src/linux/main.c`, `src/linux/shims.c` |
| DirectDraw | `softdd.c` (Win32 GDI underneath) | win32hle's `ddraw.c`, shown by its `screen.c` |
| Memory | 32-bit process, guest address = host address | Linux `-m32`: the same (flat). macOS and 64-bit Linux: the guest's 4 GB is one block of the host's (based) |
| Shared, portable | `inflate.c` (through `guest.h`), `keyscript.c` | the same files |

The Windows host stays as it is. The native host is a second, separate
host, as in the sibling projects. The two hosts share every piece that has
no Win32 in it: the deflate decoder (`inflate.c`) and the `--keys` grammar
(`keyscript.c`). `src/runtime/guest.h` picks the runtime for those, and
gives them `GUEST_PTR(va)`, which is a cast on Windows and `hle_ptr` (flat
or based) on win32hle.

`CMakeLists.txt` sends Linux and macOS to `cmake/native.cmake` before its
Windows-only checks, so the Windows configuration is unchanged.

### DirectDraw: win32hle's, not softdd

softdd stays the Windows DirectDraw, and the native build uses win32hle's.
softdd is written against Win32 itself: DIB sections, memory DCs, GDI text
into 8-bit surfaces, `StretchDIBits`, and `WM_PRINT` into the frame for the
text fields. Host C on win32hle cannot call those, because every Win32 call
there is a guest-ABI shim. win32hle's `ddraw.c` already has what the game
needs: 8- and 16-bit surfaces, palettes, colour key, stretch, flip, a
virtual display mode, and `GetDC` through its own GDI. Its `screen.c`
already has what Track 2 is adding to softdd: sharp, smooth, CRT, nearest
and integer scaling, letterboxing, F11 fullscreen and mouse mapping. Making
softdd portable would mean writing a GDI for it, which win32hle already is.

softdd's palette-true 8-bit text and EDIT compose are done on this track;
remaining gaps are below.
`--shot` and `--record` are the native host's own (`main.c`, through
`hle_dd_frame`), as in Red Alert 2's.

### The lifted C under clang (preferred) or gcc

`./build-linux.sh` picks **clang** when `CC` is unset. clang's `musttail`
makes `RECOMP_ITAIL` a real `jmp`; gcc's fallback `(fn)(); return` is a
call, so THIS_COD's blitter jump tables grow the C stack and SIGSEGV a few
frames into Standard Game (~23k frames). gcc 15+ has `musttail` too but
rejects it in any function that contains `setjmp` (`RECOMP_EH_FRAME`), so
it cannot host those ITAILs. Use `CC=gcc` only for bring-up that stays on
the main menu.

Checked 2026-11-21 against the shared lift (`A:\Games\star\swgb-recomp\src\recomp\gen`,
54 files, 238 MB of C), with Ubuntu 24.04's gcc 13 in WSL:

- `-fsyntax-only`, 64-bit based (`RECOMP_MEM_BASE`, the macOS model): **all
  54 files clean, no errors**. The generated C has no `__declspec`, no
  inline asm, no MSVC intrinsics. `recomp_types.h` already has gcc's side of
  every MSVC-specific spot: statement expressions, `x86intrin.h`, and
  `cpuid`.
- `-O1 -c` of `recomp_0000.c` (2.9 MB): 11 s. If time scales with size, the
  whole lift is about 15 minutes on one core and 2 to 3 minutes on eight.
  MSVC takes more than 30 minutes.
- `-m32` (flat) runs with clang or gcc-multilib. Red Alert 2 and Tiberian Sun
  build their lifts both ways from the same generator.
- `src/linux/*.c`, `keyscript.c` and `inflate.c` against `win32hle.h`:
  clean under `-Wall -Wextra`.

The flags are Red Alert 2's: `-fno-strict-aliasing -fwrapv` (x86 integer
and aliasing semantics), and on win32hle the indirect-call caches and the
x87 ring (`SWGB_FAST_ICALLS`). The lift writes `_ST(i)`, which the ring
needs.

## The import census

`Battlegrounds.exe` makes 258 imports from 14 DLLs. win32hle answers 237 of
them by name or ordinal (WSOCK32's by its `SHIM2` aliases). The other 21
are answered in `src/linux/shims.c`, registered ahead of win32hle:

| Import | Answer here | Final answer |
|---|---|---|
| `MCIWndCreateA` (MSVFW32) | NULL: no movie, as on a PC without the films | Track 6's player (below) |
| `ICInfo` (MSVFW32) | FALSE: no codecs | the same |
| `mciSendCommandA` (WINMM) | `MCIERR_INVALID_DEVICE_NAME`: no CD music | CD audio from the Ogg tracks (below) |
| `DPLAYX` #1 | stub `IDirectPlay4` (Create must succeed or boot MessageBox-exits) | multiplayer still empty |
| `DPLAYX` #2, #4 | empty enum / `DPERR_UNAVAILABLE` | the same: DirectPlay MP out of scope |
| `GetFileVersionInfo*` / `VerQueryValueA` | FIXEDFILEINFO 4.06.03.0516 (DirectX 6.1a) | the same, or win32hle VERSION later |
| `IsDBCSLeadByte`, `GetPriorityClass`, `WinExec` | code page 1252, normal priority, refused | the same |
| `GetLastActivePopup`, `GetWindowThreadProcessId`, `IsClipboardFormatAvailable`, `SystemParametersInfoA`, `SetClassLongA`, `GetCaretBlinkTime`, `DrawTextExA`, `SetSysColors`, `IsIconic` | the smallest true answer (`DrawTextExA` is `DrawTextA`) | the same, or moved into win32hle |
| `CreateICA` (GDI32) | a 1x1 32-bit DC | the same |
| `ImmGetOpenStatus`, `ImmGetDefaultIMEWnd` | no input method | the same |

`shims.c` also does, the win32hle way, what `host.c` does on Windows:
`GetDriveTypeA` and `GetLogicalDrives` show the play disc as a CD-ROM,
`CreateFileA` and `OpenFile` are wrapped for `--files` and `on:NAME`, and
`MessageBoxA` answers itself on a test run.

## The gaps, and their fixes

### 1. `host.c`'s Win32 specifics

Each one, and what it becomes on win32hle:

| `host.c` | Why on Windows | Native |
|---|---|---|
| `GetModuleHandleA`, `GetModuleFileNameA`, `GetCommandLineA`, `LoadIconA`, `LoadCursorA` shims | the process is the host, the module the guest | win32hle's module table: the image is added as `C:\Battlegrounds.exe` (`hle_module_add`), resources come from it, `hle_set_command_line` |
| Window subclassing (`host_wndproc`) | the fullscreen window, windowed: always active, mouse scaled from the client area, clicks given to the text fields | `screen.c` maps the mouse onto the virtual display. win32hle hit-tests the field itself (`hle_window_at`). **Still to check:** whether losing focus pauses the game (win32hle's `WM_ACTIVATEAPP` on SDL focus) |
| `GetGUIThreadInfo` in the `--keys` thread | keys go to the focused text field | `hle_input_key` already posts to the focus window |
| `native32_headless`'s private desktop | windows that are visible get `WM_PAINT`, but on no screen | win32hle generates `WM_PAINT` itself, and `hle_screen_open(headless)` opens no window |
| The `--keys` thread (`PostMessageA`, `g_vk_until`) | scripted input | `keyscript.c` (the same grammar) and `hle_input_mouse`/`hle_input_key` (`main.c`) |
| `CreateMutexA` with `--multi` | one copy per desktop | win32hle's named objects are per process: `--multi` is accepted and does nothing |
| `cd_pick` (drive letters, `RegSetKeyValueA`) | the play disc | `--cd DIR` or `$SWGB_CD` (disc 2 mounted, or copied) becomes `D:`, and `CDPath` is seeded in win32hle's registry |
| `mute_process` (WASAPI session) | test runs are silent | `hle_dsound_set_master(0)`, and SDL's dummy audio driver when headless |
| Vectored exception handler, `TerminateProcess` | the fault report | `sigaction` on an alternate stack, `_exit` |
| `SetDllDirectoryA` (`language.dll`) | the string DLL beside the game | win32hle's `LoadLibraryA` maps a resource DLL from the game folder |

**What both hosts share.** `host.c` parses `--keys` with `keyscript.c`
(main did that), and `keyscript.c` builds without Win32, so the Linux host
runs the same grammar: modifiers, right clicks, drags, window places,
`@FILE` scripts and `--keys-live`. The other runtime files the native host
compiles from `src/runtime` are `coverage.c` (the unresolved-call stop, exit
5), `widescreen.c` (`--res`), `dplay.c` (DirectPlay), `inflate.c` and
`keyscript.c`; each picks its platform with `SWGB_WIN32HLE` (`guest.h`,
`winport.h`). `src/linux` has its own `framerate.c` (`--fps`, `--fps-log`),
`profile.c` (`--profile`) and `movie.c` (`--films`), with the Windows host's
options, logic and log lines, over SDL and POSIX instead of softdd and Win32.

### 2. Text fields (EDIT controls) — done

The brief said win32hle has no EDIT control. It has one: `user32.c`'s
`edit_proc` keeps the text, the selection, the focus, `EM_*` and `EN_*`,
and paints on `WM_PAINT` / `WM_PRINT` (font, `WM_CTLCOLOREDIT` colours,
selection, blinking caret). `src/linux/fields.c` sets `hle_dd_frame_hook`
to softdd's `compose`: a copy of the primary with each visible EDIT
`WM_PRINT`ed in. New Player shows the typed name.

### 3. Text in 8-bit surfaces — done

The game draws its labels, buttons and HUD text with GDI into 8-bit
surfaces' DCs. softdd needed the current palette loaded into the DC to get
the right colours (fixed on main, `sync_dib_palette`). win32hle's
`gdidc.c` used to map a colour to an 8-bit index as a grey ramp, and
`pack()` truncated an RGB value to its blue byte.

**Fix (pcrecomp `swgb-linux-palette`):** `ddraw.c`'s `GetDC` hands the
surface's palette (else the primary's) to `hle_gdi_surface_dc_pal` as a
live `PALETTEENTRY` table; `gdidc.c` nearest-matches `SetTextColor` against
it. Main-menu hangar labels are white with the black drop shadow.

### 4. Movies (MCIWnd) — done

`src/linux/movie.c` plays the films as `src/runtime/movie.c` does on
Windows: `ICInfo` says Indeo 5 is there when `ffmpeg` is on the PATH, and
`MCIWndCreateA` makes a child window (class `swgb.movie`, its window
procedure a shim) that answers the game's MCI messages. Two `ffmpeg`
processes decode the video (BGRA) and the sound (s16 stereo, into
`hle_audio_stream_*`, which is the clock). The frame is drawn where the text
fields are, in `fields.c`'s compose of the primary, dithered into the film
palette the game installs (`MCIWNDM_GETPALETTE`, a real `CreatePalette`), so
headless shots and `--record` have it. `--films DIR` finds `NAME.avi` or
`xNAME.avi` there, as on Windows. Measured: the logo 220 frames and the
intro 2120, none dropped.

### 5. Music: `WINMM`'s CD audio, and Steam's `WIN32.dll`

The lifted `Battlegrounds.exe` (v1.1, `0x3C03FD69`) imports the real
`WINMM.dll` and plays its music from disc 2's audio tracks through
`mciSendCommandA` (`cdaudio`). Steam's `WIN32.dll` is imported only by the
Clone Campaigns executables (`battlegrounds_cc.exe`, `battlegrounds_x1.exe`,
Track 4). It is a winmm wrapper: 151 exports, the waveOut and timer ones
forwarded, and the MCI CD calls answered by playing `Music\TrackNN.ogg`
through `libvorbisfile-3.dll`.

So it does not need to be lifted. win32hle binds imports by name whatever
the DLL, so `WIN32.dll`'s waveOut and `time*` imports already land on
win32hle's winmm. What remains for both executables is one module: **MCI
`cdaudio` over the Steam release's Ogg tracks.** It handles `MCI_OPEN`,
`MCI_SET` (TMSF), `MCI_STATUS` (number of tracks, length, position, mode),
`MCI_PLAY` from/to, `MCI_STOP`, `MCI_PAUSE`, `MCI_CLOSE` and the notify
message. It decodes with stb_vorbis (one public-domain file in
`third_party`, so no 32-bit libvorbis package is needed) and plays on
`hle_audio_stream_*` natively, or on waveOut on Windows.

The Windows build gains from it too: a Steam install has no audio CD, so
today the recompiled game has no music there. Effort: 2 days, testable on
Windows first.

### 6. Exceptions

`host.c` sets `native32_guest_seh = 1`. The game has 702 C++ frames and 14
`__try` frames in its CRT, and the lift gives them landing pads (setjmp,
`RECOMP_EH_FRAME`: portable). win32hle's `RaiseException` ends the run
("guest exceptions are not handled"). The game never catches a C++
exception itself (`docs/exceptions.md`), so a normal run should never
raise. If one does, the fix is to port `native32/seh32.c` and `eh32.c`'s
dispatch, which is driven by guest data, onto win32hle's `RaiseException`
and `RtlUnwind`. That is a pcrecomp change, about 2 days, done only if a
run needs it.

### 7. Things found only by running it

The sibling ports each found a dozen of these (`civ3/docs/linux.md`). This
game's likely ones:

- **The registry.** On Windows the game reads the installer's keys (paths,
  `CDPath`). win32hle's registry starts empty, seeded by the host:
  `main.c` sets `CDPath`. Others will show up as reads that fail.
- **Case and paths.** `hle_path.c` matches components without case, but the
  game also builds paths from `GetCurrentDirectoryA` and from registry
  values.
- **Activation.** See subclassing above.
- **Timing.** `timeGetTime`, `Sleep(0)` and `GetVerticalBlankStatus` pacing
  (softdd fakes a 60 Hz blank; check `ddraw.c`'s).

Each is the usual permissive-bind loop: run, read the stub's name or the
fault report, answer it.

### 8. DirectPlay — the Windows host's own

`src/runtime/dplay.c` (DirectPlay 4 over TCP/UDP, [multiplayer.md](multiplayer.md))
builds on win32hle too. `winport.h` gives it the Win32 it uses (sockets,
threads, events, critical sections, UTF-16 names). Each method is a shim
(`IDirectPlay4::dp_Open` and so on) whose vtable slots are shim addresses; the
shim lets the machine go (`hle_block_begin`: `Open` and `EnumSessions` wait),
calls the method with the guest's arguments, and pops them. The game's
callbacks go through `hle_call_guest` with the machine taken back first, and
a player's event is the game's own win32hle event (`hle_set_event`).
`CoCreateInstance(CLSID_DirectPlay)` reaches it through
`hle_com_register_class`. 32-bit only (the game's pointers are the host's).
`shims.c` still answers VERSION with 4.06.03.0516 for the boot check. A
Linux build's join stamp differs from a Windows build's, so the two refuse
to play each other (lockstep needs the same code).

### 9. macOS

A Mac runs 64-bit programs only, so the guest's memory is one 4 GB block at
`0x100000000000` (`SWGB_MEM_BASE`, `cmake/native.cmake`). The lifted C
already compiles that way (above), and win32hle is written for it: every
guest pointer goes through `hle_ptr` and `hle_va`. In this repo,
`inflate.c` was the one host file that cast guest addresses to pointers,
and it now uses `GUEST_PTR`. `main.c` and `shims.c` use only win32hle's
accessors. Red Alert 2 needed nothing more for the Mac than its build
script and the fixed base. It needs a Mac to try: Xcode's command line
tools and Homebrew's `cmake ninja pkgconf sdl2 sdl2_ttf`.

## Building

You need a lift (`run_lift.py`, on any OS with Python and capstone), and the
game folder as `game/`.

**Linux** (Debian or Ubuntu, including WSL2):

```
sudo dpkg --add-architecture i386 && sudo apt update
sudo apt install gcc-multilib cmake ninja-build pkg-config fonts-liberation \
                 libsdl2-dev:i386 libsdl2-ttf-dev:i386
./build-linux.sh
build-linux/swgbrecomp --run --game game --cd /media/$USER/SWGB2
```

`CMAKE_ARGS=-DWITH_FFMPEG=ON` adds movies through ffmpeg (also
`libavformat-dev:i386 libavcodec-dev:i386 libswresample-dev:i386
libavutil-dev:i386`). `-DSWGB_GEN_LIB=path/libswgb_gen.a` links a lift built
in another worktree, as `swgb_gen.lib` does on Windows.

**macOS:**

```
brew install cmake ninja pkgconf sdl2 sdl2_ttf
./build-macos.sh
build-macos/swgbrecomp --run --game game
```

The command line is the Windows host's: `--headless`, `--record`,
`--frames`, `--shot`, `--keys` (inline or `@FILE`), `--keys-live`,
`--watchdog`, `--files`, `--multi`, `--exe`, `--game`, `--args`, `--res`,
`--fps`, `--fps-log`, `--smooth` (built, not yet run here), `--films`, `--profile` (`--profile-window`,
`--profile-hz`), `--mp-port`, `--mp-join`, `--mp-log`, plus `--cd DIR` (a
folder, not a letter), `--fullscreen` and
`--scale sharp|smooth|crt|nearest|integer` (F11 and F12 in the window). The
presenter's own switches (`--scale N`, `--filter`, `--refresh`,
`--present`, `--present-log`) and native32's (`--callbacks`,
`--native-trace`) are Windows ones. `tools/profile_report.py` reads a Linux
profile with `nm`.

**Clone Campaigns** builds the same way: unwrap and catalog its exe
(`python3 tools/steamstub_swgb.py <Steam>/Game/battlegrounds_x1.exe
work/battlegrounds_x1.exe`, then pcrecomp's `disasm32.py`, 14 minutes),
`python3 run_lift.py --build cc --all`, and `./build-linux.sh` also builds
`build-linux/swgbrecomp_cc` (`SWGB_CC`: its exe, registry key and disc).

## What is done (2026-11-23)

- `src/runtime/guest.h`; `inflate.c` on it (no change on Windows).
- `src/linux/main.c`, `shims.c`, `eh.c`, `fields.c`: the native host;
  VERSION for the 6.1a check; `recomp_eh_push`/`pop`; EDIT fields and films
  composed into the primary.
- Rebased on main (test.py, soak, the resolve cache and profiler, Clone
  Campaigns, DirectPlay), and the Windows host's features on Linux too:
  the full `--keys` grammar and `--keys-live` (`keyscript.c`, shared), the
  unresolved-call stop (`coverage.c`, exit 5), `--res` (`widescreen.c`,
  through win32hle's `hle_lookup_manual_hook`, pcrecomp `swgb-linux-palette`),
  `--fps`/`--fps-log` (`framerate.c`), `--profile` (`profile.c`), `--films`
  (`movie.c`), DirectPlay (`dplay.c`), Clone Campaigns' host
  (`swgbrecomp_cc`), the hot files at `-O2` (`GEN_HOT_FILES`).
- `tools/test.py`, `conformance.py`, `res_check.py`, `soak.py`,
  `mp_pair.py`, `profile_report.py` run on Linux ([testing.md](testing.md),
  "On Linux").
- `cmake/native.cmake`, `cmake/linux-i386.cmake` (no longer FORCEs the
  compiler into the cache: a regenerate then dropped the toolchain and built
  x86-64), `build-linux.sh`, `build-macos.sh`.

Not done: music (gap 5), macOS (needs a Mac), `setup.sh`'s native path.
Enter in New Player clears the name instead of accepting it (click OK;
`HLE_EDIT_ENTER_CLICK=1` turns Enter into that click, but it also swallowed
the chat's Enter, so it is off by default).

### Found by the soak (2026-11-23)

- **The minimap was black, and the terrain plain.** The game writes some
  graphics resources to the temp folder and reads them back as bitmaps
  (`sub_004BE820`: the minimap's `map800.bmp`, resource 50411, and one more
  at start-up). win32hle's temp folder is `C:\WINDOWS\TEMP\`, under the
  game folder here, which has none: the minimap got no size
  (`[panel+0x12C]`, `+0x130` stayed 0) and never drew. The host now maps a
  folder of the run's own (`$TMPDIR/swgbrecomp-XXXXXX`, removed at exit) as
  `T:` and answers `GetTempPathA` with it (`shims.c`). The minimap shows the
  map and every player's colour, and the terrain has its dirt and scenery.
- **After the chat, the game stopped drawing.** Enter sends the chat; the
  game deletes the chat dialog panel while its field still has the focus.
  `DestroyWindow` takes the focus away first on Windows (the field gets
  `WM_KILLFOCUS`, which lets the panel tree release it); win32hle just
  forgot the focus, and the game screen's message handler went on swallowing
  `WM_PAINT`: the map froze with "Game Paused" and the chat line on it while
  the world ran on. pcrecomp's `destroy()` now moves the focus first.
- **The game window is never told it lost the focus or the activation**, as
  `host_wndproc` does on Windows (`shims.c` subclasses it): Genie pauses on
  `WM_KILLFOCUS`, and its own chat field taking the focus is not the window
  losing it.

The game's own cursor is drawn into the frame in game. `GetClipper` on a
surface with no clipper must return `DDERR_NOCLIPPERATTACHED`
(`0x88760238`, `MAKE_DDHRESULT(568)`). win32hle returned `0x88760082`
(`DDERR_INVALIDOBJECT`). `sub_0049F080` and `sub_0049F630` treat any other
failure as fatal and return after the save-under blit, before `BltFast` of
the mcursor sprite, so the arrow never appeared and a saved rectangle (a
piece of the chat box, after a chat) could stay where the cursor was.
pcrecomp `ddraw.c` now returns the real code. A headless skirmish shot
shows the arrow on the map, over the panel, and, after the chat closes and
the pointer moves, a clean background where it was. In a window the host's
SDL pointer is still shown on the menu, where Genie does not blit mcursor,
and hidden once the game calls `ShowCursor(FALSE)`. `SWGB_MSG_TRACE=1`
(focus, key and paint messages) and `SWGB_CHILD_TRACE=1` (the visible
child windows) are the debugging aids used here.

## Effort

From the sibling ports' history and the gaps above, for one person who knows
the code. The first item needs the toolchain installed.

| Step | Days |
|---|---|
| Toolchain, first full compile and link (flat, `-m32`) | 0.5 |
| Boot to the main menu: the permissive-bind loop (registry, paths, activation, timing) and 8-bit text (gap 3) | 3 to 4 |
| Text fields (gap 2, pcrecomp and here) | 2 to 3 |
| A skirmish played with the mouse and keys; sound | 3 to 5 |
| Music: MCI `cdaudio` over Ogg, shared with Windows (gap 5) | 2 |
| Movies, after Track 6 (gap 4) | 1 to 2 |
| `setup.sh` native path, `conformance.py` and `docs/testing.md` on Linux | 2 |
| **Linux, playable** | **about 3 to 4 weeks** |
| macOS (64-bit based), with a Mac | 1 more week |

The risk is in the bring-up loop: it may be shorter (Red Alert 2 needed
little beyond Tiberian Sun's fixes), or longer if the game raises
exceptions (gap 6) or depends on Windows' window activation in ways
win32hle does not model.
