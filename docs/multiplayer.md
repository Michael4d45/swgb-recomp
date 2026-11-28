# Multiplayer (LAN)

Two to four recomp instances play a multiplayer game against each other
through a DirectPlay of the host's own (`src/runtime/dplay.c`): chat, a player
dropping or quitting, the host dropping (host migration), saving and restoring.
Tested on one machine over loopback (`tools/mp_pair.py`), base game and Clone
Campaigns. Results are under Tests below.

```
build\swgbrecomp.exe --run                       the host's DirectPlay (the default)
build\swgbrecomp.exe --run --dplay native        Windows' DirectPlay (dplayx), as before
build\swgbrecomp.exe --run --mp-port 2400        the port: TCP and UDP, 2400-2403 (default 2300)
build\swgbrecomp.exe --run --mp-join 192.168.1.7 also look for games at this address (or a name; commas between)
build\swgbrecomp.exe --run --mp-log              every probe answered, and more
build\swgbrecomp.exe --run --mp-cross            the lift's build stamp: a Windows and a Linux host meet
```

In the game: Multiplayer, then **Local (LAN) TCP/IP Connection** or **Internet
TCP/IP Connection For DirectPlay** (both are the same TCP/IP here), then
Create or Join.

## What the game uses

### Imports

| DLL | Import | What for |
|---|---|---|
| `DPLAYX.dll` | #1 `DirectPlayCreate` | the version check at startup (`GUID_NULL`, then QueryInterface to IDirectPlay4), each service provider the connection list offers, and Create |
| | #2 `DirectPlayEnumerateA` | the connection list on the Multiplayer screen |
| | #4 `DirectPlayLobbyCreateA` | an IDirectPlayLobby2A for `CreateAddress` only |
| `ole32.dll` | `CoCreateInstance` | `CLSID_DirectPlay` / `IID_IDirectPlay4` (one call site, `0x0043B900`): Show Games and Join |
| | `CoCreateGuid` | the game's own GUIDs |
| `WSOCK32.dll` | `WSAStartup`, `gethostname`, `gethostbyname`, `WSACleanup` | the machine's own address (`0x0042DC31`..); real winsock, untouched |

At startup the game also reads the file version of `C:\WINDOWS\SYSTEM32\DPLAYX.dll`
(`sub_00477DA0`) and stops with "requires DirectPlay 6.1a or higher" if the
check fails.

### Interfaces and methods

The game asks for **IDirectPlay4**, the Unicode interface (IID `0AB1C530-…`).
It passes ANSI strings through it anyway (`"<name>'s Game"` as a `WCHAR*`, the player's profile name).
dplayx reads those as WCHARs up to a `0x0000` and hands the same bytes back,
so the game reads its own text. Ours does the same.

What it calls, from a static scan of the lift and confirmed at run time
(`[dplay] first ...` in a log):

| Interface | Methods |
|---|---|
| IDirectPlay4 | `QueryInterface`, `Release`, `InitializeConnection`, `EnumSessions` (500 ms, `DPENUMSESSIONS_ASYNC \| AVAILABLE`, again and again while the list is up), `Open` (create, join), `Close`, `CreatePlayer`, `DestroyPlayer`, `GetCaps`, `GetPlayerCaps`, `GetPlayerName`, `GetSessionDesc`, `GetMessageCount`, `Receive` (polled; peek first), `Send`, `CancelMessage` |
| IDirectPlay (the first one) | `EnableNewPlayers`: QueryInterface for `IID_IDirectPlay` at 13 places (`0x00436640`..`0x004411B9`, `0x0043C480`), FALSE when the game starts |
| IDirectPlayLobby2A | `CreateAddress(DPSPGUID_TCPIP, DPAID_INet, ...)` |

It uses no groups, no player data, no `SendEx`, no secure sessions, no lobby
launch. Its own protocol runs over `Send` to `DPID_ALLPLAYERS` (0) or one
player. The traffic is small: in a two-player game, about 7 messages a second
each way, 17 bytes on average.

### How it reached DirectPlay before

Nothing in `host.c` touched it. native32 bound the imports to the real
`dplayx.dll`, and the game called the COM methods through the native bridge
like any other native function. The callbacks (EnumSessions,
DirectPlayEnumerate) came back into lifted code through native32's
non-executable-page trampoline. That part works: with `--dplay native` the
Multiplayer screen lists the service providers and enumerates sessions.

## Windows' DirectPlay cannot host on this machine

`Open(DPOPEN_CREATE)` fails with `DPERR_GENERIC` (`0x80004005`). It fails in
the recomp, and in a ten-line 32-bit test program that does nothing but
`DirectPlayCreate(TCP/IP)` + `Open(CREATE)`, so the lift is not the cause.

The cause is the port. DirectPlay's TCP/IP provider needs `dplaysvr.exe` to own
UDP and TCP port **47624**. This machine runs WSL 2 with
`networkingMode=mirrored`, which gives Linux the ephemeral ports
**44620-48715** (`/proc/sys/net/ipv4/ip_local_port_range`). Windows will not
bind any of them: every bind fails with `WSAEADDRINUSE`, though `netstat` shows
no owner. dplaysvr starts and listens on nothing, so a session cannot be
created. The installed original game is in the same position. Enumeration
alone works, which is why the Multiplayer screen looked healthy. `wsl
--shutdown`, or a WSL configuration without mirrored networking, would free the
port. That is the user's choice.

Hooking the TCP/IP provider's winsock imports showed the order: it binds UDP
2350 and TCP 2300, listens, then gives up before sending anything, closing the
sockets.

## Providing DirectPlay: (a) or (b)

**(a) Windows' optional "DirectPlay" component** (dplayx + dpwsockx +
dplaysvr). It's already installed here.

- For: no code. It speaks the real wire protocol, so a recomp could play the
  original exe.
- Against: an optional feature someone has to turn on. On this machine it
  can't host while WSL runs (above). Fixed ports 47624 and 2300-2400. It
  doesn't exist on Linux or macOS. Its threads and timers are a black box when
  something goes wrong.

**(b) Our own DirectPlay 4 in the host** (`dplay.c`, about 1,500 lines).

- For: no Windows feature to install. It runs wherever winsock runs (and on
  Linux the same code over BSD sockets: the Linux track's `win32hle` has no
  DirectPlay at all). Ports of our choosing. Every message is in our logs. The
  game uses a small part of the API (above).
- Against: our wire protocol, not Microsoft's, so a recomp using it cannot
  play the original exe. More code to keep.

**Recommendation: (b) by default, (a) behind `--dplay native`.** Two recomps
are the case that matters, and (b) works everywhere, this machine included.
Playing against the original needs (a) on both sides plus everything in
"Recomp against the original" below.

## The host's DirectPlay

- **Topology: a star.** The process that creates the session is its host.
  Every other process keeps one TCP connection to it, and the host relays every
  message, so messages between two processes arrive in the order they were
  sent. Every `Send` is delivered as if `DPSEND_GUARANTEED`; the game's own
  layer doesn't need it, but it doesn't hurt.
- **Finding games.** `EnumSessions` sends a UDP probe (`SWGBDPE1` + the
  application GUID) to port 2300-2303 on 127.0.0.1, 255.255.255.255, the
  broadcast address of each interface (`SIO_GET_INTERFACE_LIST`), the address
  from `InitializeConnection` (the game's "Internet" connection) and each
  `--mp-join` address. The host answers (`SWGBDPS1`) with its session and TCP
  port. Sessions with joins disabled or no room are hidden, as in dplayx. An
  asynchronous enumeration goes on between the game's calls, as DirectPlay's
  does: the network thread probes once a second until `STOPASYNC`, `Open` or
  `Close`. Clone Campaigns asks for the list only every five seconds or so,
  and a session is listed for four seconds after its last answer, so probing
  only inside `EnumSessions` made its game blink in and out of the list.
- **Ports.** The host listens on TCP and UDP 2300 (the next free of
  2300-2303, so a second host on one machine works). Each client connects from
  an ephemeral port and listens on one more, any free TCP port, for a host
  migration (below). Nothing asks for 44620-48715.
- **Players and system messages.** The host numbers players. Joins and
  leaves reach the other processes as `DPSYS_CREATEPLAYERORGROUP` /
  `DPSYS_DESTROYPLAYERORGROUP`, with the names inside the message as dplayx
  lays them out. `SetSessionDesc` and `EnableNewPlayers` send
  `DPSYS_SETSESSIONDESC`.
- **Host migration.** The game opens its session with flags `0x44`:
  `DPSESSION_KEEPALIVE | DPSESSION_MIGRATEHOST`. When the host goes, the
  others carry on and one of them becomes the host, as with dplayx. Each
  client listens on a port of its own from its join, and the host tells every
  client where the others listen (`T_PEERS`, after each join and leave). When
  a client's connection to the host closes, it destroys the old host's players
  (`DPSYS_DESTROYPLAYERORGROUP`), and the process with the lowest player id
  left takes its own listener as the host's and gets `DPSYS_HOST`; the others
  connect to it and name their players (`T_REJOIN`). A `Send` while the host is
  going waits (`g_backlog`) and goes out once the connection it is for is back;
  a client not back within 15 s is dropped. Only a client with nowhere to go
  (no peer list, or the new host unreachable) gets `DPSYS_SESSIONLOST`.
- **Drops.** A closed connection is a drop at once. A silent one is a drop
  after 30 s: there is a ping each second each way, and a pulled cable sends
  no FIN. The game itself then waits for the dropped player's turns and shows
  its **Multiplayer Status** dialog ("Player Dropped"), where each player votes
  Wait, Continue Game or Save and Exit; the game goes on when the others vote
  Continue Game. The dialog also opens when a player only lags (seen with four
  games on one busy machine).
- **Chat.** The game's chat line is a message of its own (`'C'`, 2, ...,
  the sender's player number at 12, `Y`/`N` for each of the eight players it
  goes to at 14-21, the text from 29). The host logs each one the game takes in
  (`[mp] chat from player N (DPID) to DPID (YYYNNNNN): "text"`), and the game
  draws it at the left of the map in the sender's colour.
- **Threads.** The game's threads call the methods (native32 has already let
  go of the machine, as for any native call). Our network thread reads the
  sockets and queues messages, and sets the event a player was created with.
  Guest code (the EnumSessions callback) runs only on the game's thread,
  inside EnumSessions, and never with our lock held.
- **Not done:** groups, player and group data (the game uses none),
  passwords, NAT (out of scope). A host migration while players are still
  joining (in the setup screen) is untried; the new host does not answer
  probes, so nobody new can find the game. Every method the game doesn't call is there with its real signature
  (native32's bridge takes the stack purge from the callee). It returns
  `DPERR_UNSUPPORTED` and says so in the log the first time.

## Lockstep and out-of-sync

The Genie engine runs the whole simulation on every machine and exchanges only
commands, a turn at a time. Every 40 turns (about 18 s of game time) each
player puts a checkpoint into its turn message, and every machine compares
them. The communications handler (`G_COMM`, `0x006A0F30`; Clone Campaigns
`0x006A35E0`) keeps them at `+0x1600`: 54 records of nine dwords from `+0x38`
(the `this` of `sub_00448EF0`):

| Word | |
|---|---|
| 0 | the player (1, 2, …) |
| 1, 3 | the world time, ms |
| 2 | the turn |
| 4, 5, 7, 8 | checksums: 4 changes slowly (units?), 5 and 8 each turn |
| 6 | 0 so far |

The check itself is `sub_00440260`, which logs `SaveGameChecksum: %ld, Me: %ld,
oosWorldTime: %d, IsPlayerOutOfSync: %d` and shows "Out of sync detected…" /
"Game stopped because it was out of sync!" (strings 2411, 2012 and the others
in 2406-2413, 9800, 9801).

The host prints each record once it has held still for a second
(`[sync] slot: nine words`), and a line when the game loads one of those
strings (`[mp] the game loaded string ...`). `tools/mp_pair.py` compares two
logs. Across the logs, the records for the same player and turn must be equal.
Within each log, every player's record for a turn must be equal (all but word
0).

### Recomp against recomp (the same build)

Both instances run the same machine code, so nothing should differ. Specifically:

- **Floating point.** The lift keeps the x87 stack in C `double`s and computes
  in them. The generated C is built `/fp:precise` (the default; no `/fp:fast`,
  no contraction) with SSE2, so each operation rounds the same way everywhere.
  The CRT is linked statically (`CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded`), so
  `sin`, `cos`, `atan2` and `pow` (what the lift calls for `fsin`, `fcos`,
  `fpatan`, `fyl2x`/`f2xm1`) are the same code in both. One thing is not
  checked: whether the CRT picks a different code path on a different CPU (it
  does on x64, with FMA3). Two machines with different CPUs are the test.
- **Time.** `timeGetTime` passes through unchanged: `framerate.c` only paces
  frames (the game screen's gate). In a multiplayer game the turn timer runs
  the simulation, and the game's own turn protocol keeps the machines together.
  `--fps` changes how often frames are drawn, not what is simulated.
- **Random numbers.** Whatever the engine seeds them from has to be the same
  on every machine, or the original would go out of sync too. Nothing in the
  host feeds the game a seed of its own.
- **The indirect-call cache** (`resolve.c`) and the coverage stop only decide
  how a call reaches its target, not which target. They can't change the
  simulation.
- **Different builds** are the risk. Another compiler version, other
  optimization flags or another CRT could round a library function
  differently. So the join handshake carries a build stamp: the host exe's
  link time and size, and the guest exe's link time. The host refuses any
  other ("a client with another build ...: refused" in its log), and the
  joiner sees the game's own "Unable to join game." Two links of the same
  source count as different builds: copy one build to both machines.
  For a test only, `SWGB_MP_ANY_BUILD=1` on the hosting side lets another
  build in ("... let in (SWGB_MP_ANY_BUILD)" in its log); `mp_pair.py
  --run b=EXE` sets it and gives B that host, to see whether two builds of
  one source (say the lifted C at `/O1` and `/O2`) simulate alike.

Measured: no differences in any run (Tests below).

### Windows and Linux

The Windows host (MSVC, `/fp:precise`, SSE2, its static CRT) and the Linux
host (`src/linux`, clang `-m32` on win32hle, glibc's libm) run the same lift,
but they are different builds, so the build stamp keeps them apart. With
`--mp-cross` on both sides the stamp is the lift's instead (every address in
the dispatch table, and the guest exe's link time), and they meet; a side
without it refuses the other ("... needs --mp-cross on both").

Whether they then compute the same is the question. Measured on 200,000
random inputs each (a small C program built both ways, comparing the bits):

| | sin | cos | tan | atan2 | pow | fmod | sqrt |
|---|---|---|---|---|---|---|---|
| MSVC `/O2` against `/O1` | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| clang `-m32 -msse2 -mfpmath=sse`, glibc | 472 | 403 | 742 | 14 | 308 | 0 | 0 |
| clang `-m32` (the x87), glibc | 76,602 | 75,414 | 75,331 | 67,374 | 104,694 | 82,359 | 32,512 |

So the Linux host's build now asks for SSE2 doubles
(`cmake/linux-i386.cmake`: `-msse2 -mfpmath=sse -ffp-contract=off`): with the
x87 every operation could round differently, even `fmod` and `sqrt`. What is
left is the transcendental functions, about one input in 300: MSVC's CRT and
glibc are each within an ulp, not the same ulp. The lift calls them at 12
`fsin`, 12 `fcos`, 7 `fptan`, 28 `fpatan` and 4 `pow` sites; whether any of
them feeds the simulation (and not only drawing) is what a long game tells.
If one does, the fix is one implementation for both (a portable libm, e.g.
FreeBSD's msun, compiled into both hosts and the lift's calls pointed at it),
which needs the Windows lift rebuilt.

Played on one machine through WSL (Tests below): Windows hosting with a Linux
player for 15 minutes (three players, busy, chat: 285 records in each log, 95
turns, none differ), and Linux hosting a Windows player for 5 (80 records,
none differ). So far, no difference has reached the simulation.

```
set WSL_CHECKOUT=/home/me/swgb-recomp      the WSL checkout, with build-linux/, game/ and work/
py -3 tools\mp_pair.py --players 3 --wsl b --host-arg all=--mp-cross --minutes 15 --activity busy --chat ...
```

`--wsl b` runs player b's Linux host through `wsl.exe`, in a game folder of
its own under the WSL checkout's `work/games/` (hard links to its `game/`:
win32hle's `LoadLibraryA` does not find `language.dll` on a `/mnt` drive),
with stderr merged into stdout inside Linux (through `wsl.exe` the two reached
the log with their own file positions and overwrote each other).

Found on the Linux host, not in `dplay.c` (left for the Linux track):

- **Chat lines and the Multiplayer Status dialog are not drawn.** The game
  takes every line in (`[mp] chat from ...` in its log) and its vote counts,
  but no shot shows the text or the dialog; the same game on Windows draws
  both. The panel there also shows `UNIT-WORKERA1` where Windows shows the
  unit's name, so a string lookup may be what is missing.
- **Two Linux hosts cannot start a game without the play disc.** The setup
  screen shows their version as `1.1` where a Windows host shows `CD 1.1`,
  and Start Game does nothing. With a Windows player in the game it starts.

### Recomp against the original

Not possible yet, and not tested:

1. **The wire.** The original speaks Microsoft's DirectPlay protocol through
   dplayx, and ours speaks its own. Interoperating needs `--dplay native` in
   the recomp, on a machine where dplaysvr can own 47624.
2. **Floating point.** The original's x87 runs with the precision control
   Windows sets (`0x027F`, 53 bits), so `+ - * / sqrt` round like the lift's
   doubles. But:
   - the transcendental instructions (`fsin` ×12, `fcos` ×12, `fptan` ×7,
     `fpatan` ×28 sites; `fyl2x`, `f2xm1`) are the CPU's microcode in the
     original and the CRT in the recomp. They can differ in the last bit.
   - the lift ignores the precision bits of `fldcw` (it keeps only the rounding
     mode, for `fistp` and `frndint`). A game that set 24-bit precision would
     diverge everywhere. None has shown up so far (33 `fldcw` sites, mostly the
     CRT's truncation for `_ftol`), but it has not been checked.
   - x87's wider exponent range and denormals in intermediates.

   Each of these is rare, but in lockstep one bit is enough. The test is the
   same sync-record comparison with the original on one side. That needs the
   screen (the original runs fullscreen) and a working dplaysvr, so it waits
   for the user.

## Lobby and session

Works (headless, loopback, two to four players):

- the Multiplayer screen: the connection list, Show Games, the list of games
  ("<name>'s Game"), Join, Create, and Restore (beside Create once
  `SaveGame\Multi` holds a multiplayer save; Clone Campaigns' template always
  has one);
- the setup screen on both sides: both players, their ping (6-7 ms),
  civilizations, player numbers, "CD 1.1" versions, the settings (only the
  host can change them), I'm Ready, the host's Start Game;
- the start: `EnableNewPlayers(FALSE)` closes the session to new joins, and
  both load the same map (Rebel Alliance and Gungans on the screenshots) and
  play;
- chat in the game: Enter, the text, Enter. Every other player's game takes
  the line in and draws it at the left of the map in the sender's colour
  (player 1 light blue, 2 light red, 3 green, 4 yellow); the Multiplayer Status
  dialog keeps them too;
- three and four players: each joins as the second did, and every sync record
  is the same in all logs;
- a player quitting (the game menu, Quit Current Game, Yes): its Close is
  `DPSYS_DESTROYPLAYERORGROUP` on the others, and they play on with no vote;
- a player resigning (the game menu, Resign, Yes): it stays in the session and
  the game, the others play on;
- a joiner's process killed: the host sees the connection close and destroys
  its player at once; the game shows Multiplayer Status ("Player Dropped"),
  the others vote Continue Game and play on, in sync;
- the host's process killed: the others migrate (above): the next player hosts,
  the third reconnects to it within a second, and after their Continue Game
  votes they play on, in sync;
- saving (the game menu, Save, OK): "Multiplayer Save - <date>" lands in each
  player's `SaveGame\Multi` (`.msx` in the base game, `.ms1` in Clone
  Campaigns);
- restoring: the host's Restore lists the saves (the newest first), OK opens
  the setup screen, the others join and ready as for a new game, and the game
  goes on from where it was saved: the restored game's first sync record has
  the save's world time (93 s into a game saved 45 s of real time in), and
  every record after it agrees.

Not tried yet: chat in the setup screen, changing settings while a client is
in, computer players in a multiplayer game, a host migration in the setup
screen, a restore with a player missing.

## Firewall

A host must accept TCP and UDP on 2300-2303 from the LAN. Windows Firewall
keys its rules by the exe's full path. This machine already has one for each
build folder that ever listened. Most are Allow (the user's answers to the
prompt). `builds\multiplayer\swgbrecomp.exe` has a **Block** rule, created at
the first run of this track. Loopback is never filtered, so the tests here
don't care, but a game between two machines needs an Allow rule for the host
machine's exe. Changing rules is the user's decision.

**WSL on the same machine** (mirrored networking, `hostAddressLoopback=true`,
WSL's own firewall on): checked with a small UDP echo and TCP accept on ports
2391-2395, both ways, no rule added:

| From | To | 127.0.0.1 | the LAN address (192.168.1.25) | 255.255.255.255 |
|---|---|---|---|---|
| Windows | a listener in WSL | UDP and TCP | UDP and TCP | nothing comes back |
| WSL | a listener on Windows | UDP and TCP | UDP and TCP | nothing comes back |

So a Windows and a Linux host on one machine find each other through the
loopback probe (`EnumSessions` always probes 127.0.0.1), and connect. A
broadcast does not reach the other side, so `--mp-join` is the way from
another machine. Mirrored networking shares the port space: whichever host
binds 2300 first has it, and the other takes 2301 (the probes cover
2300-2303).

From another machine to a game hosted in WSL, Hyper-V's firewall for WSL
filters inbound traffic. The rule that would let the game's ports in (run in
an elevated PowerShell; not added here):

```
New-NetFirewallHyperVRule -Name SWGB-recomp-MP -DisplayName "SWGB recomp multiplayer" -Direction Inbound -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -Protocol TCP -LocalPorts 2300-2303
New-NetFirewallHyperVRule -Name SWGB-recomp-MP-UDP -DisplayName "SWGB recomp multiplayer (UDP)" -Direction Inbound -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -Protocol UDP -LocalPorts 2300-2303
```

(A host migration to a process in WSL would also need its listening port,
which is any free one; with a host on Windows that is not an issue.)

## Tests

```
py -3 tools\mp_pair.py [--players 2-4] [--minutes 20] [--activity idle|habits|busy] [--seed S] [--cc]
                       [--chat] [--drop WHO@MIN] [--quit WHO@MIN] [--resign WHO@MIN]
                       [--save MIN [--restore]] [--wsl WHO] [--run [WHO=]EXE ...] [--host-arg WHO=SWITCHES ...] [--game DIR] [--out DIR] [--port N]
```

Headless `--multi` runs over loopback, players a, b, c, d. A creates; the
others (game folders `DIR-mp2`.., hard links) start five seconds apart and
join; all click I'm Ready; A starts (a Random Map, the setup screen's
defaults). Then all play `--minutes`. With `habits`, every 25-45 s each player
selects the command center, trains a worker and right-clicks somewhere. `busy`
adds a left or right click on the map or a command button every 2-6 s.

The menus are each run's `--keys` script, timed from the main menu's music
(`on:open.mp3`), so a slow start on a busy machine delays the clicks with it.
The game is played live (`--keys-live`): the tool watches the logs, takes the
game's start from A's first sync record (the players finish loading seconds
apart, but the lockstep game starts for all at once) and writes each player's
steps as they fall due. It also answers what it reads: a shot of a player the
moment a chat line arrives, and a Continue Game vote (three clicks, ten
seconds apart) each time a player's Multiplayer Status dialog opens (its chat
line is a window: `CreateWindowExA "" ... at 67,463 441x21`).

- `--chat`: two lines from every player ("mpchatb1"...), each checked on
  every other: in the log (`[mp] chat from ...`) and in the shot (pixels of the
  sender's colour at the left of the map).
- `--drop WHO@MIN`: that player's process is killed, by its PID. The others
  must see it leave within 45 s and keep logging sync records after it; if
  it was A, one of the others must take over as the host and the rest move to
  it.
- `--quit` / `--resign WHO@MIN`: the game menu's Quit Current Game or Resign.
- `--save MIN`: A saves; `--restore` then plays a second game, A restoring the
  newest save, which must start at the save's world time.
- `--wsl WHO`: that player runs the Linux host in WSL (below).

It passes when:

- every run reached its watchdog (exit 4): no fault, no not-lifted stub, no
  unresolved call (a dropped player is not checked);
- nobody lost the session or a player before that player's planned end
  (each joiner stops 20 s before the one before it), but the one dropped or
  quitting;
- no out-of-sync string was loaded;
- the sync records agree, across every pair of logs and within each;
- the chat, drop, quit and restore checks above.

Shots every five minutes and at the end, `summary.json` in `--out`.
`tools/test.py` runs it: `mp/base-3p` (3 players, chat, c dropped at 1 min)
and `mp/cc-restore` (save at 0:45, restore) at the functional level; four
players for 20 minutes, a quit, a host drop and longer restores in both games
at the soak level.

| Run | Game time | Result |
|---|---|---|
| `--minutes 5 --activity busy` | 5 min | PASS: 36 checkpoints (72 records) both logs share, none differ; about 3,150 messages each way |
| `--minutes 25 --activity busy --seed 7` | 25 min (46.9 min of game time at Normal, turn 6,210) | PASS: 155 checkpoints (310 records), none differ; 13,350 messages (216 KB) each way; no fault, drop or out-of-sync message. B trained workers up to its population limit ("You need to build more shelters"). |
| `--cc --minutes 5 --activity busy --seed 3` | 5 min, Clone Campaigns | PASS: 36 checkpoints, none differ |
| `--cc --minutes 20 --activity busy --seed 11` | 20 min, Clone Campaigns (37.9 min of game time) | PASS: 125 checkpoints (250 records), none differ; 10,800 messages each way; no fault, drop or out-of-sync message |
| A this build, B the same source linked a minute earlier | the join | refused as another build; B shows "Unable to join game." |
| `--run b=EXE`: A the lifted C at `/O1`, B at `/O2` (2026-11-26), `--minutes 25 --activity busy --seed 5` | 25 min (46.9 min of game time, turn 6,210) | PASS: let in with `SWGB_MP_ANY_BUILD`; 156 checkpoints (312 records), none differ; no fault, drop or out-of-sync message ([performance.md](performance.md#the-whole-lift-at-o2-2026-11-26-now-the-default)) |
| the same, `--cc --minutes 20 --activity busy --seed 11` | 20 min, Clone Campaigns (38.2 min of game time, turn 5,050) | PASS: 127 checkpoints (254 records), none differ |

2026-11-26, this machine shared with nine other agents' builds and games:

| Run | Game | Result |
|---|---|---|
| `--players 4 --minutes 3 --activity busy --chat` | base | PASS: 112 records in each of the four logs, none differ; 24 of 24 chat lines received and drawn. Multiplayer Status opened 6 times for a lagging player (no drop) and closed on the votes. |
| `--players 3 --minutes 3 --activity busy --drop c@1` | base | PASS: a and b saw c leave at once, voted Continue Game, played on: 65 records shared, none differ |
| `--players 3 --minutes 3 --activity busy --chat --drop a@1` | base | PASS: the host killed; b took over (`DPSYS_HOST`), c rejoined it within a second; 63 records after, none differ; 4 of 4 chat lines |
| the same, `--cc` | Clone Campaigns | PASS: the same, 63 records |
| `--players 3 --minutes 2.5 --activity busy --chat --quit b@1` | base | PASS: a and c saw b leave at once and played on with no vote; 58 records, none differ; chat 4 of 4 |
| `--players 3 --minutes 2.5 --activity busy --resign c@1` | base | PASS: c stays; 63 records in each log, none differ |
| `--players 2 --minutes 1.5 --activity busy --save 0.75 --restore` | base | PASS: the save in both `SaveGame\Multi`; the restored game starts at world time 93.2 s; 50 records after, none differ |
| the same, `--cc` | Clone Campaigns | PASS: restored at 93.6 s; 50 records, none differ |
| before the host migration: `--drop a@1` | base | the clients got `DPSYS_SESSIONLOST` and sat in Multiplayer Status, "Waiting For Vote", to the end |
| `--players 3 --minutes 15 --activity busy --chat --wsl b --host-arg all=--mp-cross --seed 5` | base, b on Linux (WSL) | sync PASS: 285 records in each log, 95 turns, none differ; 12 of 12 chat lines received; the 4 to b not drawn (Linux, above) |
| `--players 2 --minutes 5 --activity busy --chat --wsl a --host-arg all=--mp-cross --seed 6` | base, the host on Linux | sync PASS: 80 records, none differ; 4 of 4 chat lines received; the 2 to a not drawn |
| `--players 2 --minutes 2 --chat --wsl ab` | base, both on Linux | the game does not start (no play disc, above) |
| `tools/test.py --level functional --cc --prefix mp-coverage` | both | PASS: 37 of 37, `mp/base-3p` and `mp/cc-restore` among them |

## What's left

**Two real machines.** Nothing here has crossed a wire yet.
- The host machine needs a Windows Firewall Allow rule (TCP and UDP
  2300-2303) for its `swgbrecomp.exe`. This machine's
  `builds\multiplayer\swgbrecomp.exe` has a Block rule.
- Finding the game relies on each interface's broadcast address. Where that
  doesn't reach (Wi-Fi isolation, another subnet), use `--mp-join <host IP>`.
  An address the game itself passes to `InitializeConnection` is probed too.
  Whether its Internet TCP/IP connection asks for one is untried (with the LAN
  one it passes an empty address).
- Both machines need the same build (the handshake checks), and
  `battlegrounds.exe` 1.1 (`--exe`).
- What only two machines can show: different CPUs (does the CRT take another
  path?), real latency and loss, a pulled cable (the 30 s drop).

**Clone Campaigns.** Same code: `swgbrecomp_cc.exe` links `dplay.c` too, and
the comm handler's address is in `guest_addrs.h`. Its main menu's Multiplayer
button is elsewhere (`mp_pair.py --cc` knows). Restore works in both games.

**No `dplayx.dll` on disk.** Our DirectPlay never calls dplayx, but two
things still need the file. native32 resolves the game's imports by ordinal
(`DPLAYX.dll#1`) through the DLL's export names, and the game's startup check
reads the file's version. A machine without the DirectPlay component, or
Linux, needs both answered by the host: shims by ordinal, and a version answer
for that one file. (The Linux host already answers both: win32hle's shims by
ordinal, `src/linux/shims.c`.)

**The game itself:** computer players in a multiplayer game, chat in the
setup screen, a game played to its end (the score screen on every side), a
host migration in the setup screen, restoring with a player missing.

**Windows and Linux:** a long game across them, to see whether the CRT's and
glibc's `sin`, `cos`, `tan`, `atan2` and `pow` ever differ where the
simulation reads them (above); if they do, one libm for both.

**The original exe** (above): `--dplay native`, a dplaysvr that can own
47624, the screen, and the sync comparison with one log from the original.
That log needs another way to read the original's records (a debugger, or the
game's own `oos` save).
