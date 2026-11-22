# Frame rate

How often Galactic Battlegrounds draws, how often it simulates, what ties
them together, and the host's `--fps N`. Measured headless with
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
  unchanged from 10 up (the table below).

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

Not for units. They move once a step, 20 times a second, and a frame drawn
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

## `--fps N`

`src/runtime/framerate.c`. Without it nothing changes. With it, the host's
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
