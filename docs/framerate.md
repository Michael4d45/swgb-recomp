# Frame rate

How often Galactic Battlegrounds draws, how often it simulates, what ties
them together, the host's `--fps N`, and `--smooth` (units drawn between
steps). Measured headless with
`--fps-log` (below) in the scripted skirmish of [testing.md](testing.md),
v1.1, 800x600x8, on 2026-11-21.

## In short

- **Drawing and simulation are separate.** In game the engine draws one
  frame per pass of the game screen's idle, which a gate lets through at
  most once a millisecond: about 920 frames a second here. The world steps
  only when 50 ms of real time have passed since its last step: 20 steps a
  second, each the real time elapsed (at most 100 ms) times the game speed.
- **The game clock follows the wall clock, not the frame rate.** At Normal
  speed (1.5) it gains 1500 game ms a wall second at 920, 60, 30 and 10
  frames a second. Below 10 a step is cut to 100 ms and the game slows.
- **Extra frames show nothing new.** The picture changes when the world
  steps (20 times a second at most) or when the view scrolls; of the ~920
  frames drawn a second, 8 to 30 differ from the one before.
- **`--fps N`** sets the gate so frames start N times a second and lets the
  host sleep between them instead of spinning a core. Game speed is
  unchanged from 10 up (the table below). **Since the performance track this
  is the default, at the display's refresh** (`--fps off`: the game's own
  rate, as measured below); see [performance.md](performance.md).
- **`--smooth`** draws every object between its last two simulated
  positions, so at 120 frames a second a walking unit moves a pixel or so
  every frame instead of 3-7 pixels every sixth. Only the place a sprite is
  drawn changes, never the game's state: a multiplayer game with `--smooth`
  on one side only stays in sync. Off by default ([below](#--smooth-drawn-between-steps)).

## How a frame is made

The message loop (`sub_004255D0`, `0x00425655`) calls `PeekMessageA` with
`PM_REMOVE`, dispatches a message if there is one, and otherwise calls the
game's idle (game vtable `+0x104`). There is no `Sleep` and no other clock
than `timeGetTime` in the import table, so the loop never waits.

```
sub_004255D0  message loop: PeekMessageA, else idle
  sub_005EB5A0  TRIBE game idle (vtable 0x00665A20 + 0x104)
    sub_00428A80  base game idle: a 250 ms timer on the draw system ([game+0x68]),
                  comms (sub_00433040 on [game+0x1B4]), the current panel's idle
    sub_004F6EA0  game screen idle ([game+0x17B4])
      gate: return unless timeGetTime() - [screen+0x125C] >= [screen+0x1258]
      world vtable +0xD4 = sub_00601630 -> sub_0061AB70   world update
      [screen+0x125C] = timeGetTime()
      sub_00427F30  UpdateWindow -> WM_PAINT
        sub_00428F80  paint: panels draw (sub_004F6A70 ...), views (sub_0049F080, sub_0049F380)
          sub_00471210  Blt of the finished frame to the primary
```

The game object is `[0x006A0FD4]`; its mode is `[game+0x1DC]` (4 in game,
2 in the menus), its world `[game+0x420]`, its game screen `[game+0x17B4]`
(vtable `0x0065CE24`, made by `sub_004F1820`).

### The gate

`[screen+0x1258]` is copied from the startup settings, `[[game+0x24]+0x888]`,
when the game screen is made (`0x004F1B7D`); it is 1 ms here. So in game
the engine draws as often as it can, up to 1000 frames a second: about
920 here, the rest of each millisecond going to the idle loop
(~70,000 idle passes a second). The menus have no gate: they redraw only
when something changes (4 presents a second at rest), and their idle loop
spins at ~130,000 passes a second.

### The world step

`sub_0061AB70` (world `+0x10` is the game clock in ms, `+0x14` its value
before the last step, `+0x18` the last step, `+0x20` the real time of the
last step, `+0x24` the game speed as a float):

1. Asks the communications timer (`sub_00432190` on
   `[[world+0x68]+0x25C0]`) whether the world may step. In single player it
   says yes unless paused; in multiplayer this is the turn schedule (lockstep).
2. Takes `elapsed = timeGetTime() - [world+0x20]`:
   - over 100 ms: `elapsed = 100` (`0x0061AC9E`);
   - under 50 ms: no step, unless the counter at
     `[[[world+0xC]+0x1614]+4]` is above 0 (`0x0061ACAA`; an object made at
     `0x00431410`, not identified yet; 0 in every run here);
3. advances the clock by `elapsed x speed` and updates every object by that.

So at any frame rate from 20 up the world steps every 50 to 51 ms by
75 game ms (at Normal speed); between 10 and 20 it steps every frame by the
frame's time; under 10 every frame by 100 ms, and the game runs slow. The
frame rate never changes how far a unit moves in a second, only how finely
the steps fall (at 30 frames a second a step comes every 2 frames, 66 ms).

## Measurements

The scripted skirmish, then: the command center trains a worker at 68 s
with a rally point at 69 s; shots at fixed wall times; the screen edge
scrolls the view at 87-89 s. Maps are random, so the shots are compared by
what does not depend on the map: the HUD's training progress, and the
worker's walk to its rally point.

Means over 66-87 s of the run, one run each (`cpu` is of one core, from a
second pair of runs with the same script):

| | frames/s | changed/s | steps/s | game ms a step | game clock, ms per wall s | training at 70 / 74 / 78 / 82 s | cpu in game / menus |
|---|---|---|---|---|---|---|---|
| default (gate 1 ms) | 902 | 12.4 | 19.9 | 75.5 | 1495 | 8 / 32 / 56 / 80 % | 98 / 100 % |
| `--fps 60` | 60.8 | 13.1 | 18.1 | 82.7 | 1498 | 8 / 32 / 57 / 81 % | 9 / 0-12 % |
| `--fps 30` | 31.2 | 12.3 | 15.0 | 99.7 | 1494 | 8 / 32 / 57 / 81 % | |
| `--fps 10` | 11.8 | 8.9 | 10.0 | 149.4 | 1495 | 9 / 34 / 55 / 80 % | |
| `--fps 5` | 7.6 | 6.2 | 5.0 | 150.0 | **746** | **4 / 15 / 28 / 39 %** | |

- `frames` counts Blts to the primary; with `--fps` a few more than N
  are WM_PAINTs that come from the message loop rather than the gate.
- The game clock gains 1500 ms a wall second (Normal speed) at every rate
  from 10 up; the worker trains at the same pace to within a percent, the
  rounding of the HUD and of when the shot falls between steps.
- At 5 frames a second each step is cut to 100 ms (150 game ms): the
  clock and the training run at half speed, as the code says.
- With the gate at 1 ms the game spins a whole core in game and in the
  menus (~130,000 idle passes a second there); `--fps 60` draws the same
  changed frames for a tenth of the CPU.
- The worker's walk to its rally point looks the same in the shots at
  every rate, but the view's start differs on each random map, so the
  HUD's progress is the measure.

## Is a higher frame rate worth it?

Not for units, as the game draws them (`--smooth`, below, changes that).
They move once a step, 20 times a second, and a frame drawn
between two steps is the same picture: of the ~920 frames drawn a second,
8 to 13 differ from the one before at rest, 20 while a unit walks (one a
step), and 20 to 30 while the view scrolls. Whatever Expanding Fronts'
"60 fps" changes, this build already draws far more often than that; the
units cannot look smoother than their 20 steps a second.

What could make motion smoother, none of it host-only:

- **Interpolated drawing.** A sprite can be drawn anywhere: at draw time,
  place each moving object at its position plus its velocity times the
  time since the last step (or between its last two positions, a step
  late). That is a change to the view's object pass, not to the
  simulation, so multiplayer sync is untouched. Sprite animation frames
  would still change at their own rate.
- **Smaller steps.** Lowering the 50 ms minimum (`cmp eax, 0x32` at
  `0x0061ACAA`) to 16 ms would step 60 times a second in single player.
  Every timer in the game counts game milliseconds, so speed would not
  change, but rounding would (integer milliseconds at 1.5x: 16 ms steps
  are 24 game ms), and AI, pathing and attack timers have only ever run at
  75 ms steps. Multiplayer steps by turns and would need the same change
  on every machine. This is a change to lifted code (a re-lift or a
  patched constant), so out of this track's host-only rule.

## `--smooth`: drawn between steps

`src/runtime/smooth.c`, both games, both hosts (the Linux one built but
not yet run). Off unless `--smooth` is given.

### Where an object is drawn

Found with a trace of every indirect call in game (who calls what, how
often, with which `this`): one object virtual was called about 50 times a
frame, from one place.

```
the map view's tile pass (0x0060A8DF): for each visible tile
  sub_00496470(tile, area, x, y + elevation, ...)   the object pass:
      for each object on the tile the player can see (vtable +0x44),
      its draw, vtable +0x70:
    sub_0054AD10(area, x, y, colors)   RGE_Static_Object's draw: every object
        class's (9 vtables) but one, vtable 0x006600E0, whose sub_00556040
        ends in a direct call to it
      sprite at x + [obj+0x34], y + [obj+0x36]; shadow at +0x38, +0x3A
      the selection frame and hit-point bar (sub_0054B1B0, sub_00495BE0):
        corners from the object's size, relative to the same x, y
```

`x, y` is the tile's place on screen; `[obj+0x34..0x3A]` is the object's
offset in its tile, which the simulation sets when the object moves
(`sub_0054DB30`, new_location). The position itself is `[obj+0x48]`,
`+0x4C`, `+0x50`: floats, in tiles (x, y, height). A tile is
`[map+0xBF40]` by `[map+0xBF3E]` pixels, by half (48 and 24 here), and a
move of dx, dy, dz shows on screen as

```
sx = hw (dx + dy)      sy = hh (dy - dx - dz)
```

(new_location's arithmetic, and `sub_00495BE0`'s). `[world+0x20]` is the
wall time of the world's last step (`0x0061ACC6`). Clone Campaigns has the
same code at other addresses (`0x00496E80`, `0x0054C9E0`, `0x00557F60`;
`tools/x1_addrs.py` matched each byte for byte, so the offsets are the
same) and its step time at the same place (`0x0061E946`).

### What `--smooth` does

The object draw is an indirect call, so the host owns it
(`recomp_lookup_manual`, as `widescreen.c` owns the screen's). When the
caller is the object pass:

1. Each object's last two positions are kept by the host, keyed by its
   address and id (`[obj+0x10]`), one entry a world step.
2. A frame at t, between steps at t0 and t1 = t0 + T (T the last
   interval, about 50 ms), has f = (t - t1) / T, 0 to 1. An object that was
   at p0 and is at p1 is drawn at p0 + f (p1 - p0): its screen offset is
   (f - 1) times the step's move, through the formula above.
3. `x, y` on the guest stack get that offset for the call and are put back
   after it. Nothing else is written: not the object, not the world.

So the picture is one step (about 50 ms) behind the simulation, and it
never overshoots. A unit that stops is drawn up to where it stopped and
stays; one that turns, starts attacking or dies is drawn along the
straight line of its last step; a projectile is an object and is drawn the
same way. An object with no history (it came into view, it was just made,
its slot holds another object, a new game) or that moved more than 3 tiles
in a step (a teleport) is drawn where it is. A paused game (no steps) is
drawn where everything is.

Not changed: sprite animation frames (they change when the world steps),
the minimap, and the objects the object pass draws directly
(`sub_0054B150`, objects remembered under the fog: they do not move).
Clicks pick by the simulation, so a fast unit's drawn place is up to one
step's move (a few pixels) from where a click finds it.

### Measured

`SWGB_SMOOTH_LOG=FILE` (with or without `--smooth`) logs every object the
view draws, every frame: the time, the world's step, the object, its
position, the game's own screen position and what `--smooth` adds.
`tools/smooth_check.py FILE` reads it: for each object that moved, the
largest change of its screen position from one frame to the next, and the
share of frames it did not move, as the game draws it and as `--smooth`
does. `--record-fps N` records at N frames a second (30 by default).

The scripted skirmish (testing.md), a worker trained with a rally point and
then box-selected and sent back, `--fps 240`, recorded at 120 frames a
second, 2026-11-26, the walking workers of one run with and one without:

| | a step's move | largest change a frame | 95% of frames | frames it stood still |
|---|---|---|---|---|
| plain draw | 3.0-6.0 px | 4.5-6.3 px | 4.5-6.3 px | 92-98% |
| `--smooth` | 3.0-5.6 px | 1.4-2.2 px | 1.0-1.4 px | 52-91% |

The plain draw moves a walking unit 3-7 px every sixth frame and leaves it
still the other five; `--smooth` moves it 0-2 px every frame (a unit that
moves 0.4 px a frame still rounds to the same pixel most frames). In the
recordings (beside the build, not in git: `builds\smooth-motion\clips`,
`off.mp4`, `on.mp4` and a side-by-side of a selected worker's walk at 120
frames a second) the selection ellipse and hit-point bar move with the
sprite.

**The simulation is untouched.** `tools/mp_pair.py --activity busy
--minutes 6 --host-arg a=--smooth` (the host with `--smooth`, the joiner
without), 2026-11-26: both ran to their watchdog, no out-of-sync message,
86 sync records in each log over 43 turns, **all 86 the same**. The sync
records are the game's own checksums of the world, compared turn by turn.

**CPU.** `--fps 120 --profile`, the same skirmish: the game thread 15% of
a core with and without; the hook (`smooth_lookup`, `entry`, `offset_for`,
`draw`) 0.5% of the game thread's samples in game, under 0.1% of a core.
Without `--smooth` the host does not ask it at all (`smooth_hooked`).

`tests/unit/smooth_test.c` (`test.py`'s `unit/smooth`, `unit/smooth_cc`)
compiles `smooth.c` with a fake clock and fake game objects: a walk at 5,
7 and 8 ms frames moves at most 2 px a frame (the plain draw: 8 px at
once, still 90% of frames), is never drawn ahead of the object, and stops
exactly where it stopped; teleports, a new object in a slot, a new game
and a pause are drawn where they are; another caller of the draw is left
alone; the caller's argument slots and the object are as they were.
`smooth/base` and `smooth/cc` (L2) play the walk at `--fps 120` and read
the log: 95% of frames move 3 px or less, and fewer frames are still than
in the plain draw.

## `--fps N`

`src/runtime/framerate.c`. The default is the refresh the presenter paces
shown frames to (`--refresh`, or the window's monitor, 30 to 1000): a frame
drawn faster than that is never shown. `--fps off` changes nothing (the
game's own rate and its spin, as the measurements on this page were made).
Otherwise the host's
`PeekMessageA` (the game's loop, with the machine let go so sent messages
reach the window procedure) does, when there is no message:

- in game (mode 4 or 5, a screen with the game screen's vtable): writes the
  gate, `[screen+0x1258]`, so that the next pass starts at the next
  multiple of 1000/N ms (16.67 ms is 16 and 17 in turn; when a frame runs
  late the schedule restarts from now, without catching up), and waits for
  that time with `MsgWaitForMultipleObjects`, so input still wakes it;
- in the menus: waits up to 1 ms for input. They draw only on change, so
  this only stops the spin.

It calls `timeBeginPeriod(1)` so the waits are to the millisecond. 1 to
1000; under 10 the game slows (above). Presentation to the display (vsync,
pacing presents to the refresh) is track 2's, in softdd.

## `--fps-log`

Once a second, against the wall clock:

```
[fps]   88.1s mode 4 cpu  98% presents 916/s changed 21/s | world 35148 ms +1499/s speed 1.50
        steps 20.0/s of 75.0 ms (max 75) | gate 1 ms | blt>primary 912/s 00471210:912
        | blt 2775/s ... | lock 2009/s ... | timeGetTime 717000/s 00428A80:291000 ...
```

- `mode`: the game's mode (4 in game); `cpu`: the process's CPU time a
  second, in % of one core.
- `presents`: softdd's presents (not frames: a primary without a back
  buffer presents on every write); `changed`: presents whose picture
  differs from the one before.
- `world`: the game clock, its rate, the game speed; `steps`: world steps
  a second, their mean and largest size in game ms; `gate`: the game
  screen's gate.
- `blt>primary`, `blt`, `flip`, `lock`, `dc`: the game's DirectDraw calls on
  the primary and on other surfaces, with the lifted functions that made
  the most. In game one `blt>primary` is one frame.
- `timeGetTime`: calls a second, by calling function.

`FPS_STACK=<n>[:VA]` with `--fps-log` prints, every 3 s, the lifted
functions on the host stack at call `n` (0 `blt>primary` ... 7
`timeGetTime`, in `framerate.c`'s order), optionally only from function
`VA`. The stack is scanned (the generated code has no frame pointers), so
a few stale entries show up; the chain from the message loop down is the
part to read. It is how the chain above was found.
