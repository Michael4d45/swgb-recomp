# Lifting Battlegrounds.exe

What `run_lift.py` and the host do beyond the stock pcrecomp pipeline, and
why. Everything here was found on this title; the general parts went into
pcrecomp, the title-specific parts stay here.

## The code is in two sections

`.text` (0x00401000-0x00645000) and `THIS_COD` (0x00645000-0x00651000, matrix
and blit routines). The driver this one was copied from read only the section
that holds the start of the code range, so every function in `THIS_COD`
lifted to an empty body. An empty body never pops its return address, so the
first call into one shifted the caller's stack by four bytes:

```
=== fault 0xC0000005 at 0x60576D39, thread 30604 ===
  write of 0x00000068 (null/low)
  in lifted sub_00425E20, last native call (none)
```

`sub_00470080` (a constructor) called `sub_0064E780` in `THIS_COD`, came back
with `esp` four bytes off, and popped zero into its caller's `esi`. The
driver now lays every section out at its address before decoding, and a
function that decodes to nothing is a lift error, not a silent `{ }`.

## The deflate decoder is the host's

`genie.dat`, scenarios, campaigns and saved games are raw deflate. Ensemble's
decoder (`sub_0062343B`) is hand-written assembly built as a coroutine: when
its input runs out or its output fills, it copies the guest stack between its
entry and its current depth into its state block (`0x00622D7D`), and the next
call pushes that copy back and jumps to the saved return address
(`0x00622DCD`). A lift cannot resume that: return addresses in a lifted guest
stack are placeholders, and where the code really was is on the host stack,
long gone by the next call. The first decode wrote through a null pointer in
the reader that called it (`sub_004D5000`).

So `run_lift.py` lists it in `HLE`, and `src/runtime/inflate.c` does the same
contract with miniz: `(in, in_off, &in_len, out, out_off, &out_len, state,
final)`, cdecl, `in_len`/`out_len` back as consumed/produced; 0 = call again,
-1 = end of stream, -2 = corrupt. A fresh state block (its resume index at
+0xA958 is 0) starts a new stream.

## DirectDraw

`softdd.c` is X-Wing's software DirectDraw without its Direct3D half. The game
looks for 8-bit modes from 800x600 up in `EnumDisplayModes` and calls
DirectDraw unusable without them ("Could not initialize graphics system"), so
every mode is offered at 8 and 16 bits. 8-bit frames are recorded through the
primary's palette.

A surface's pixels have 4 KB of slack before and after them. The game's span
blitters read and write whole dwords around a span's ends: THIS_COD's
palette remap (`sub_0064AC8D`) does `mov eax, [edi-3]` at x = 2 of the first
row, which reads the byte before the surface. In real DirectDraw a surface
sits in the heap or in video memory and that byte exists. A DIB section's
bits start on a 64 KB boundary, so that read faulted. Two soak runs died this
way, at 13 and 25 minutes (`read of 0x1A00FFFF`, `0x19F7FFFF`, with `edi`
two bytes into the back buffer), whenever that span reached the top-left of
the view. So `surf_new` maps its own section with the slack and makes the
DIB from the same section at that offset, so GDI and the game share the
pixels.

## Seeds

`RUN_SEEDS` in `run_lift.py` (committed) and `work/run_seeds.json` (local,
from `tools/seeds_from_logs.py` or pcrecomp's `seed_from_log.py` over a run's
log) add entries the catalog missed: `0x00427090`, called through a pointer
from `0x00424425`; the blitter's jump-table targets (below); and the entries
in the next section.

## Pointers the catalog dropped

A skirmish against computers died a minute or two in:

```
ICALL: unresolved VA 0x005648A0 from 0x00406E20
=== fault 0xC0000005 ... read of 0x00000000 (null/low)
  in lifted sub_005C45C0
```

`0x005648A0` is a slot of the vtable at `0x0066100C` and the start of a
real function (`mov eax, [esp+10h] ... call 0x0040AC40; ret 10h`). The
catalog has no entry there because a false function ran over it.
`sub_005641F0` ends in `ret`, then its two switch tables: a dword jump
table at `0x0056474C`, a byte index table (`0x0C 0x0C ...`) at `0x00564780`,
and a second jump table at `0x00564878`. disasm32 decoded the index bytes as
code (`or al, 0Ch` over and over), made entries of them at `0x00564800` and
`0x0056480C`, and that false body ran on through the tables into the real
function at `0x005648A0`. Its data scan then found the vtable slot and
dropped it, because the target was inside an instruction of the false body
(the `interior` check in `find_data_code_pointers`). The rule that a run of
code pointers outranks a decoded body (`table = s.is_code and run3`) only
applies to tables inside code sections, not to a vtable in `.rdata`.

`tools/code_ptr_seeds.py` finds every such case. It looks at each aligned
dword in the data sections and each `push imm32` and `mov r/m32, imm32` in
code. For every value in the code range that is not a catalog entry (270 of
them), it decodes the target and looks at the code around it:

| Class | How it is told | Count | Seeded |
|---|---|---|---|
| `vtable` | in a run of 2+ code pointers in data; the target decodes to a `ret` or `jmp` with nothing a compiler never emits; it starts after a `ret`, `jmp`, padding or a switch table | 3 | yes |
| `pointer` | a lone pointer, or an immediate stored or pushed by code, with the same decode and boundary | 16 | yes |
| `seh` | a filter or handler of an `_except_handler3` scope table (`push -1; push table; push handler`) | 14 | no: eh32 resumes there as a label, and run_lift refuses a seed that is a resume point |
| `cxx_eh` | a catch handler or unwind action of a FuncInfo | 0 | no (they are catalog entries already) |
| `switch` | an arm of a `jmp [reg*4 + table]` | 0 | no |
| `label` | a branch target inside the body that holds it | 2 | no |
| `noise` | anything else: ASCII and UTF-16 text, floats (`0x437A0000` is 250.0), `0x0061FFFF`-style constants, padding | 235 | no |

No class rests on the numbers alone; each one was checked by hand:

- **vtable**: `0x005648A0`; `0x005C6D90` (vtable `0x00664550`), a function
  with an SEH prologue behind `sub_005C63B0`'s switch table, the same false
  body problem (`sub_005C6D61` and `sub_005C6D6F` are jump-table bytes);
  `0x004C39B0` (vtables `0x00659C18` and `0x006649C4`): `int3; mov al, 1;
  ret 4`.
- **pointer**: zlib's default `zalloc`/`zfree` and friends,
  `0x00625E80`-`0x00626140`, stored by `mov [ebx+18h], imm32` in code the
  catalog never reached (`0x00623A84`-`0x006248A4`).
- The SEH filters are all catalog entries already, so only handler
  continuations are missing, and eh32 handles those.
- An unaligned scan found only floats.

The binary has no RTTI (no `/GR`), so a vtable is a run of code pointers
with nothing before it. The script still notes a COL when one is there.

The 18 new entries are in `RUN_SEEDS`. Re-lifted: 20,802 -> 20,820
functions, 0 errors and 176 with no terminator before and after, and the C
of every function that existed before is byte-for-byte the same. The tool
writes `work/code_ptr_seeds.json` with the seeds and, for each rejected
address, its class and the reason.

**The fix that belongs in pcrecomp** (read-only here; written up for its
maintainers):

1. `find_data_code_pointers`: a run of three or more code pointers in a
   *data* section should outrank a decoded body too (`table = run3`, not
   `s.is_code and run3`). A vtable slot inside a false body is exactly the
   case `interior` cannot judge, because the body is the thing in doubt.
2. Recognise switch index tables as data: `movzx r32, byte [reg + T]` or
   `mov r8, [reg + T]` followed by `jmp [reg*4 + U]` makes `T` up to `U`
   (and `U` up to its last in-range dword) data in code. Then no entry is
   ever made inside them, and nothing runs over the function behind them.
   `code_ptr_seeds.py` uses the same rule to accept a function that starts
   right after a table.
3. As a check, a catalog entry whose first instructions repeat one opcode
   on bytes under 0x40 (`or al, 0Ch` x N) is almost always a table.

## When lifted code meets an address with no function

pcrecomp's `RECOMP_ICALL` prints `ICALL: unresolved VA` and goes on with
`eax = 0`, as if the callee had returned. Nothing pops the callee's
arguments, and the caller goes on with a zero that it uses as a pointer.
The fault comes later, somewhere unrelated (the null read in `sub_005C45C0`
above). `src/runtime/coverage.c` chains `recomp_eh_jump_hook`, which both
`RECOMP_ICALL` and `RECOMP_ITAIL` ask before they report a miss. eh32 sees
the address first: a handler's continuation still resumes as before. Any
other miss stops the run on the spot:

```
ICALL: unresolved VA 0x005648A0 from 0x00406E20
=== unresolved indirect call, thread 15392 ===
  target 0x005648A0, from lifted sub_00406E20, last native call (none)
  inside the game's image, no lifted function starts there; nearest below: sub_0056480C (+0x94)
  if it is a function (a vtable slot, a callback), it is an entry the catalog
  missed: `py -3 tools\seeds_from_logs.py --merge` adds it to work\run_seeds.json, ...
  eax=... (registers), stack: ...
last indirect calls (newest first): ...
```

The exit code is 5. Running the target was considered and rejected. The
original x86 is in the image, but run natively it would use the real TEB's
`fs:[0]` chain instead of the guest's, call imports through the IAT outside
native32's bridges, and reach the host's replacements (the inflate
coroutine) as the original code. pcrecomp has no 32-bit interpreter to fall
back to. `SWGB_ICALL_CONTINUE=1` keeps the old behaviour, reported once per
address, to collect several misses from one run whose later behaviour is no
longer trustworthy. `tools/coverage_test.c` checks both modes and the eh32
chain without the game.

From the log to the lift:

```
py -3 tools\seeds_from_logs.py            # launcher logs (A:\Games\star\logs\play-*.log), work\*.log, work\soak\**
py -3 tools\seeds_from_logs.py --merge    # adds the new ones to work\run_seeds.json

py -3 run_lift.py --all                   # then rebuild
```

It reads both encodings of a log (PowerShell's `Tee` writes UTF-16). It
names the catalog body that holds each address, skips addresses that are
already seeds or entries (a build older than the lift), and does not seed
addresses outside the code sections, which are bad pointers, not missing
entries. Before committing a seed to `RUN_SEEDS`, run
`tools/code_ptr_seeds.py` and look at the address.

## Dialogs draw on WM_PAINT

Clicking Single Player with no player profile opens the New Player dialog.
The game draws a dialog when its window gets `WM_PAINT` (it calls
`InvalidateRect` and waits), and `--headless` used to strip `WS_VISIBLE`
from every window so nothing reached the screen; a hidden window gets no
`WM_PAINT`, so the picture froze. `--headless` now runs the host again on a
private desktop (pcrecomp `native32_headless`) and leaves the windows
visible there. Scripted keys go to the window that has the focus (the
dialog's edit control).

## The blitter's jump tables

The first frame of a map died with `ITAIL: unresolved VA 0x00646465 from
0x0064F3C0`: `THIS_COD`'s span blitters jump through tables of code
addresses (`jmp [eax*4 + 0x0064D440]` and others, 0x0064C300-0x0064E780).
The 44 targets that were not functions are seeds in `run_lift.py`.

## The CD letter

The game stores the play disc's letter in `CDPath` (HKLM, virtualised to
HKCU's VirtualStore) the first time it finds it, and checks only that
letter afterwards ("You must insert a game CD"). The host finds the real
drive with disc 2 in it (`CD_DRIVE`, `--cd E`, or the first CD-ROM drive with
`Game\Avi`) and stores that letter in `CDPath` before the game starts, so a
disc image mounted on a new letter still passes.

## Who called: the host stack

Lifted functions call each other as C functions, so the call chain of
any import is on the host stack, as return addresses into the lifted
code. The generated C has no frame pointers (`/O1`), so the stack is
scanned instead: a value counts when it falls in lifted code just after a
call instruction, and maps back to the lifted function that holds it.
The dispatch table's pointers are an incremental link's jump stubs
(`E9 rel32`); the bodies are where they jump. `FPS_STACK` with
`--fps-log` does this at DirectDraw and `timeGetTime` calls
([framerate.md](framerate.md)); it found the game's frame path in one run.

## Clone Campaigns

The expansion is its own program, lifted by the same pipeline into its own
tree and linked into its own host:

```
py -3 tools\steamstub_swgb.py <Steam>\Game\battlegrounds_x1.exe work\battlegrounds_x1.exe
py -3 ..\pcrecomp\tools\disasm\disasm32.py work\battlegrounds_x1.exe -o work\functions_x1.json
py -3 run_lift.py --build cc --all          # -> src\recomp\gen_cc\
build.cmd                                   # also builds swgbrecomp_cc.exe
build\swgbrecomp_cc.exe --run --headless    # --exe defaults to work\battlegrounds_x1.exe
```

### Which exe

Steam's install has two: `battlegrounds_x1.exe`, SteamStub-wrapped (a
`.bind` section, `.text` encrypted), and `battlegrounds_cc.exe`, not
wrapped. Both have PE stamp `0x3CF57142` (2002-05-29), and unwrapped
(`tools/steamstub_swgb.py`, a SteamStub 3.x `C0DEC0DE` unwrapper over
pcrecomp's `tools/drm/steamstub.py`) `battlegrounds_x1.exe` is
`battlegrounds_cc.exe` with the same sections at the same places. But 338
bytes differ, and `battlegrounds_cc.exe` is a community mod of it:

| Where | `battlegrounds_x1.exe` | `battlegrounds_cc.exe` |
|---|---|---|
| `0x0063667B`, the CRT start | `mov [0x007A3A28], eax` | `jmp 0x006539B9`, a cave in `THIS_COD`: `pushad; LoadLibraryA("uage.dll"); popad` (the string is the tail of `language.dll`), then back |
| 15 places in `.text`: `0x00426234`, `0x0045ACB9`, `0x00463E9E`, `0x0046A914`, `0x004F670B`, `0x0050EDCB`, `0x005D853E` and others | 1280 (`0x500`), 1024 (`0x400`), 1200 (`0x4B0`); `cmp eax, 3` at `0x00428058` | 2560, 1440, 1441; `cmp eax, 5`: the largest Screen Size made 2560x1440 (a map for track 3, widescreen) |
| `.data` | `interfac.drs`, `interfac_x1.drs` | `wide1280.drs`, `wide1280_x1.drs` |
| `0x005FD4FA`, 8 times | `cmp byte [esp+eax+24h], 1; jbe` | `and byte [esp+eax+24h], 80h; je` |
| `0x0064DC8D` (a `THIS_COD` blitter) | 25 bytes of code | `nop`s |

`uage.dll` (2024) hooks the running code, and a static lift has no such
code to hook. So the lift is of the plain program, the unwrapped
`battlegrounds_x1.exe`. `run_lift.py --build cc` checks its stamp, refuses
a wrapped copy (a `.bind` section) and refuses `battlegrounds_cc.exe` by a
digest of `.text` and `THIS_COD`, since its stamp is the same.

### Its own addresses

The expansion's `.text` is 0x3D60 bytes longer than the base game's, so
everything after its first change moved. What `run_lift.py` keeps per build:

| | Base game | Clone Campaigns | How it was found |
|---|---|---|---|
| Entry | `0x00632881` | `0x00636601` | the PE header |
| The deflate decoder (`HLE`) | `0x0062343B` | `0x006271AB` | its first 48 bytes, one match; the state block is `0xAD90` bytes in both (the `Inf32Dat` section), so `inflate.c` is unchanged |
| The pointer-called seed | `0x00427090` | `0x00427920` | its first 128 bytes, with absolute addresses and branch displacements masked: one match |
| `THIS_COD` | `0x00645000` | `0x00648000` | the same 0xB9B9 bytes but for 4,550 absolute addresses, every one moved by +0x3000 (into `THIS_COD`) or +0x2000 (into data) |
| The blitter's jump tables | `0x0064C300-0x0064E780` | `0x0064F300-0x00651780` | 720 targets, all of them a catalog entry or a seed (the base game's 43 table seeds plus 0x3000) |

pcrecomp's disasm32 now finds most of those seeds itself (41 of the 45 are
already catalog entries); they stay in `run_lift.py` for an older catalog.
The C++ and SEH frames are found in the catalog at lift time
(`eh32.frame`), so nothing about them is per build: 13 functions with
somewhere to continue and 19 resume points, the same as the base game.

### Pointers the catalog dropped, in x1

`tools/code_ptr_seeds.py --exe work/battlegrounds_x1.exe --catalog
work/functions_x1.json --out work/code_ptr_seeds_cc.json` classifies x1's
262 candidates: 234 noise, 14 `seh`, 2 labels, 2 vtable slots and 10
pointers. All 12 seeds are real and are in `CC_SEEDS`:

- `0x00571380`, a vtable slot (`0x00664E64`) behind a switch's index and
  jump tables (`0x00571350`-`0x0057137F`) that disasm32 decoded as code: a
  false `sub_00571372` running to `0x00571AFD` swallowed the real function,
  as the base game's `sub_0056480C` swallowed `0x005648A0`. Unseeded, the
  first call through that slot is an unresolved indirect call.
- `0x004C3EF0` (`int3; mov al, 1; ret 4`, in three vtables), the base
  game's `0x004C39B0`.
- Ten of zlib's allocator and stream callbacks (`0x00629BF0`,
  `0x00629DD0`-`0x00629EB0`), stored into a `z_stream` by code the catalog
  does not reach, the base game's `0x00625E80`-`0x00626140`.

The base game's other seeds have x1 counterparts that are already catalog
entries (`0x005648A0` is `0x00566EF0`, `0x005C6D90` is `0x005CA950`, six of
the zlib callbacks). `tools/x1_addrs.py` finds a base address in x1 (below);
it found every one of these.

The lift: 21,069 functions (the base game: 20,820), 0 stubs, 0 lift errors,
182 with no terminator, 5.5M lines of C in 53 files. The catalog: 21,057
entries, 95.3% of the code range.

### The host

The lifted code and its dispatch table are linked into the host, so one
host runs one program: `swgbrecomp_cc.exe` is a second target
(`cmake/clone_campaigns.cmake`, built only when `src\recomp\gen_cc` has a
lift in it). It reads the base target's sources, definitions, libraries and
link options, so every runtime file (softdd, the presenter, frame pacing,
the indirect-call cache, the films, the coverage stop) is the same code in
both, and it has the same 16 MB stack. `SWGB_CC` picks three things in
`host.c`: the exe's name (the guest's own
`GetModuleFileNameA` and its icons come from `battlegrounds_x1.exe` in the
game folder), its registry key (`...Star Wars Galactic Battlegrounds: Clone
Campaigns\1.0`, where `CDPath` goes), and what is on its disc. The
expansion looks on every CD-ROM drive, C to Y, for
`gamedata\game\battlegrounds_x1.exe` (`0x00424930`-`0x00424A2C`) and starts
without it, as the base game does on Steam.

The rest of what the runtime knows about the engine is in
`src/runtime/guest_addrs.h`, per exe. Frame pacing and the profiler read
the game object; `--res` hooks the game screen's virtual functions and
calls a few of the engine's own. In x1 the functions moved, and two classes
grew: the game screen by 0x18 bytes from about `+0x1084` (its frame gate
`+0x1258/+0x125C` is `+0x1270/+0x1274`, its interface flag `+0x10A4` is
`+0x10BC`, two of its art shapes moved), the game object not at all.

| | base | x1 | how |
|---|---|---|---|
| game object pointer | `0x006A0FD4` | `0x006A3684` | 2,446 uses, one answer |
| game screen vtable | `0x0065CE24` | `0x0065FE48` | the store in its constructor (`0x004F1820`, `0x004F2BA0`) |
| its destructor, draw, handle_size, paint (`+0x00`, `+0x3C`, `+0x5C`, `+0x60`) | `0x004F46B0`, `0x004FEDB0`, `0x004F5210`, `0x004F6A70` | `0x004F5A30`, `0x00500230`, `0x004F6600`, `0x004F7E60` | the vtable's slots |
| set_screen_size, interface (game vtable `+0x68`, `+0x14`) | `0x005ED2D0`, `0x005E2C50` | `0x005F1030`, `0x005E68B0` | the vtable's slots |
| resources, unit-info draw (`+0x3C`) | `0x005D4930`, `0x005D5D00` | `0x005D84D0`, `0x005D98A0` | the vtables' slots |
| TPanel::set_rect | `0x004B5660` | `0x004B5BB0` | the screen vtable's `+0x0C`; 59 vtables hold each |
| draw area fill, lock, unlock | `0x004736C0`, `0x00471FD0`, `0x00472080` | `0x00474160`, `0x00472A70`, `0x00472B20` | 16, 6 and 4 call sites in the aligned functions |
| game code ends (the CRT) | `0x0062E000` | `0x00631D70` | bytes |

Struct offsets were checked by aligning every method of the class in both
exes (every slot of the screen's and the game's vtables, and the functions
that set the program info's sizes) and reading the displacement each
aligned instruction uses. Matching a function by its first bytes alone is
not enough: the first 256 bytes of the screen's draw match, and the fields
it reads further on moved. A masked match of a short function can also be
false: the screen's destructor matched `0x004185A0` at 40 bytes, and the
vtable says `0x004F5A30`. `tools/x1_addrs.py` needs 64 bytes or more.

### Running it

```
build\swgbrecomp_cc.exe --run --headless --multi --watchdog 180 --keys "15:move:340:240,16:click:340:240,20:a,21:b,23:move:323:352,24:click:323:352,30:move:600:228,31:click:600:228,37:move:118:568,38:click:118:568,100:move:335:170,101:click:335:170,104:move:420:330,107:a,170:shot=cc-skirmish.bmp"
```

With no player profile: Single Player, a name in the New Player dialog, OK,
Standard Game, Start Game, the command center selected and a Worker trained
with its hotkey. The menus are at the base game's places. On the way:

- **The data.** `genie_x1.dat` inflates through the same host decoder;
  `gamedata_x1.drs` and the `_x1` interface, terrain, graphics and sound
  archives open over the base game's; `language_x1.dll` loads from the game
  folder (`SetDllDirectoryA`). Nothing in the host had to change for them.
- **The films.** It looks for `avi\xintro.avi` and `avi\xlogo1.avi` in the
  game folder, then `game\avi\` on the disc. Steam has them at the top of
  `Game`, so they are skipped (track 6 plays them).
- **The stack, at `/Od`.** A first build at `/Od` (the machine was short
  of memory for an `/O1` one) overflowed the host's 16 MB stack when the
  command center was selected (`0xC00000FD` in `sub_00651E40`). The
  `THIS_COD` sprite decoder is threaded code: `0x00651E40` reads a command
  byte and jumps through a table to its handler (`0x00648006` copies a
  pixel), and each handler jumps back through `[0x007A4044]`. Each jump is
  a host call in the lift, so a sprite's commands nest, and what each one
  costs is the two functions' frames. At `/Od` they are `sub esp, 320h` and
  `sub esp, 48h`, about 890 bytes a command with the saved registers and
  return addresses; at `/O1`, `sub esp, 24h` and `sub esp, 1Ch` plus four
  pushes each, about 100. The `/O1` build plays the same run on 16 MB, as
  the base game's does with the same decoder (`0x0064EE40`, `0x00645006`).
  Taking the nesting away (an indirect tail jump that returns to a
  trampoline instead of calling) would be pcrecomp's lifter.
- **A profile saved by the widescreen mod.** Steam's `player.nf1` was last
  saved by `battlegrounds_cc.exe` with its largest Screen Size, which that
  mod makes 2560x1440. The plain `battlegrounds_x1.exe` asks softdd for
  2560x1440 and lays the game out for 800x600 in its top left corner. With
  a fresh profile it is 800x600 throughout.
- **The view scrolled left with the mouse resting at x = 30** (on the
  command panel), in the base game too: the headless cursor. Gone since
  the presentation track's virtual cursor (checked again on 2026-11-22).
