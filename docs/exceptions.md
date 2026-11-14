# Exceptions in the lift

Galactic Battlegrounds is MSVC6 C++ with the CRT linked in, and has three
kinds of exception frame. What each needs from a static lift, and where
pcrecomp does it:

| Frame | In this game | What runs it |
|---|---|---|
| C++ frame, `push ebp; mov ebp, esp` first | 1 | `eh32.c` dispatch; handlers' ebp = entry esp - 4 |
| C++ frame, frame pointer omitted (`push -1; push handler; mov eax, fs:[0]` at entry) | 488 | the same; handlers' ebp = entry esp |
| `__try/__except` (`_except_handler3` at `0x00636C3C`) | 14, all in the CRT | `seh32.c` over the guest's `fs:[0]` chain, guest code end to end |

None of the 702 FuncInfo records in the game has a try block: its C++
frames only destroy locals on the way past. So the game itself never
catches a C++ exception; the frames matter because their unwind funclets
must run if anything throws through them.

## How a lift continues inside a function

An `__except` block is entered by `_except_handler3` *calling* an address
inside the function that installed the frame, with that function's ebp; the
block then resets esp from `[ebp-18h]`. In lifted code that call misses the
dispatch table (the block is a label, not a function). `eh32.frame()` reads
the scope table for those addresses at lift time; the body gets them as
labels and a `RECOMP_EH_FRAME(off, list)` landing pad, and the runtime turns
the miss into a `longjmp` back into the body that owns that ebp. The same
path takes a guest `__CxxFrameHandler`'s jump to a catch's continuation.

`run_lift.py` drops those addresses from the catalog: disasm32 takes the
scope table's pointers for function starts, and dispatched as functions the
blocks would run as calls and "return" into `_except_handler3`.

`native32_guest_seh = 1` in `host.c` turns the guest chain on.

## Verified

- pcrecomp `tools/lift/msvceh_probe/run.cmd`: an MSVC-compiled probe (C++
  catch, nested try, rethrow, `__except`, `__finally`) lifted and run on
  native32, 5/5.
- This game: the 489 frames with a body get landing pads (3 of them with
  somewhere to continue: the CRT's `__except` blocks).
- Not verified: a C++ catch in a frame-pointer-omitted function, end to
  end. Today's compilers keep ebp in functions with try blocks, so the
  probe cannot produce one; the case is covered by eh32's unit tests only.
