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
