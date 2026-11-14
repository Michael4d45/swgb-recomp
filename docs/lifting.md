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

## Seeds

`work/run_seeds.json` (from `pcrecomp tools/disasm/seed_from_log.py` over a
run's log) adds entries the catalog missed. One so far: `0x00427090`, called
through a pointer from `0x00424425`.

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
