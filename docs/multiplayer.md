# Multiplayer (LAN)

Two recomp instances play a multiplayer game against each other through a
DirectPlay of the host's own (`src/runtime/dplay.c`). Tested on one machine
over loopback (`tools/mp_pair.py`), base game. Results are under Tests below.

```
build\swgbrecomp.exe --run                       the host's DirectPlay (the default)
build\swgbrecomp.exe --run --dplay native        Windows' DirectPlay (dplayx), as before
build\swgbrecomp.exe --run --mp-port 2400        the port: TCP and UDP, 2400-2403 (default 2300)
build\swgbrecomp.exe --run --mp-join 192.168.1.7 also look for games at this address (or a name; commas between)
build\swgbrecomp.exe --run --mp-log              every probe answered, and more
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
  2300-2303, so a second host on one machine works). Clients use ephemeral
  ports only. Nothing is near 44620-48715.
- **Players and system messages.** The host numbers players. Joins and
  leaves reach the other processes as `DPSYS_CREATEPLAYERORGROUP` /
  `DPSYS_DESTROYPLAYERORGROUP`, with the names inside the message as dplayx
  lays them out. `SetSessionDesc` and `EnableNewPlayers` send
  `DPSYS_SETSESSIONDESC`. A host that goes away is `DPSYS_SESSIONLOST` to the
  others.
- **Drops.** A closed connection is a drop at once. A silent one is a drop
  after 30 s: there is a ping each second each way, and a pulled cable sends
  no FIN.
- **Threads.** The game's threads call the methods (native32 has already let
  go of the machine, as for any native call). Our network thread reads the
  sockets and queues messages, and sets the event a player was created with.
  Guest code (the EnumSessions callback) runs only on the game's thread,
  inside EnumSessions, and never with our lock held.
- **Not done:** groups, player and group data (the game uses none), host
  migration (the game's sessions don't ask for it), passwords, NAT (out of
  scope). Every method the game doesn't call is there with its real signature
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
  --run-b EXE` sets it and gives B that host, to see whether two builds of
  one source (say the lifted C at `/O1` and `/O2`) simulate alike.

Measured: no differences in any run (Tests below).

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

Works (headless, loopback, two players):

- the Multiplayer screen: the connection list, Show Games, the list of games
  ("<name>'s Game"), Join, Create;
- the setup screen on both sides: both players, their ping (6-7 ms),
  civilizations, player numbers, "CD 1.1" versions, the settings (only the
  host can change them), I'm Ready, the host's Start Game;
- the start: `EnableNewPlayers(FALSE)` closes the session to new joins, and
  both load the same map (Rebel Alliance and Gungans on the screenshots) and
  play;
- leaving: the client's Close is `DPSYS_DESTROYPLAYERORGROUP` on the host. The
  host going away is `DPSYS_SESSIONLOST` on the client, which then destroys its
  player, closes and initializes a new connection for the Multiplayer screen
  (seen when a watchdog ended the host first).

Not tried yet: chat (typed text in the setup screen and the game), changing
settings while a client is in, more than two players, computer players in a
multiplayer game, saving and restoring a multiplayer game, a client dropping
mid-game.

## Firewall

A host must accept TCP and UDP on 2300-2303 from the LAN. Windows Firewall
keys its rules by the exe's full path. This machine already has one for each
build folder that ever listened. Most are Allow (the user's answers to the
prompt). `builds\multiplayer\swgbrecomp.exe` has a **Block** rule, created at
the first run of this track. Loopback is never filtered, so the tests here
don't care, but a game between two machines needs an Allow rule for the host
machine's exe. Changing rules is the user's decision.

## Tests

```
py -3 tools\mp_pair.py [--minutes 20] [--activity idle|habits|busy] [--seed S] [--run EXE] [--run-b EXE] [--game DIR] [--out DIR]
```

Two headless `--multi` runs over loopback. A creates; B (game folder
`DIR-mp2`, hard links) joins; both click I'm Ready; A starts (a Random Map, the
setup screen's defaults). Then both play `--minutes`. With `habits`, every
25-45 s each player selects the command center, trains a worker and
right-clicks somewhere. `busy` adds a left or right click on the map or a
command button every 2-6 s. It passes when:

- both ran to their watchdog (exit 4): no fault, no not-lifted stub, no
  unresolved call;
- neither lost the session or a player before the planned end (B stops 10 s
  before A);
- no out-of-sync string was loaded;
- the sync records agree, across the logs and within each.

Shots every five minutes and at the end, `summary.json` in `--out`.

| Run | Game time | Result |
|---|---|---|
| `--minutes 5 --activity busy` | 5 min | PASS: 36 checkpoints (72 records) both logs share, none differ; about 3,150 messages each way |
| `--minutes 25 --activity busy --seed 7` | 25 min (46.9 min of game time at Normal, turn 6,210) | PASS: 155 checkpoints (310 records), none differ; 13,350 messages (216 KB) each way; no fault, drop or out-of-sync message. B trained workers up to its population limit ("You need to build more shelters"). |
| `--cc --minutes 5 --activity busy --seed 3` | 5 min, Clone Campaigns | PASS: 36 checkpoints, none differ |
| `--cc --minutes 20 --activity busy --seed 11` | 20 min, Clone Campaigns (37.9 min of game time) | PASS: 125 checkpoints (250 records), none differ; 10,800 messages each way; no fault, drop or out-of-sync message |
| A this build, B the same source linked a minute earlier | the join | refused as another build; B shows "Unable to join game." |
| `--run-b`: A the lifted C at `/O1`, B at `/O2` (2026-11-26), `--minutes 25 --activity busy --seed 5` | 25 min (46.9 min of game time, turn 6,210) | PASS: let in with `SWGB_MP_ANY_BUILD`; 156 checkpoints (312 records), none differ; no fault, drop or out-of-sync message ([performance.md](performance.md#the-whole-lift-at-o2-2026-11-26-now-the-default)) |
| the same, `--cc --minutes 20 --activity busy --seed 11` | 20 min, Clone Campaigns (38.2 min of game time, turn 5,050) | PASS: 127 checkpoints (254 records), none differ |

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
the comm handler's address is in `guest_addrs.h`. Its screens differ (Create is
beside a new Restore button; `mp_pair.py --cc` knows). Restore, which loads a
saved multiplayer game, is untried.

**No `dplayx.dll` on disk.** Our DirectPlay never calls dplayx, but two
things still need the file. native32 resolves the game's imports by ordinal
(`DPLAYX.dll#1`) through the DLL's export names, and the game's startup check
reads the file's version. A machine without the DirectPlay component, or
Linux, needs both answered by the host: shims by ordinal, and a version answer
for that one file.

**The game itself:** chat, more than two players, computer players in a
multiplayer game, a client dropping mid-game, saved multiplayer games, a game
played to its end (the score screen on both sides).

**The original exe** (above): `--dplay native`, a dplaysvr that can own
47624, the screen, and the sync comparison with one log from the original.
That log needs another way to read the original's records (a debugger, or the
game's own `oos` save).
