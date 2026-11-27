"""A LAN game between two to four recomp instances, played and checked.

    py -3 tools\\mp_pair.py [--players N] [--minutes M] [--activity idle|habits|busy] [--seed S] [--cc]
                            [--chat] [--drop WHO@MIN] [--quit WHO@MIN] [--resign WHO@MIN]
                            [--save MIN [--restore]] [--wsl WHO]
                            [--run EXE] [--run-b EXE] [--exe GUEST] [--game DIR] [--out DIR] [--port N]
                            [--extra ARGS] [--extra-a ARGS] [--extra-b ARGS]

--cc plays Clone Campaigns (swgbrecomp_cc.exe, battlegrounds_x1.exe): the main menu's Multiplayer
button and the Multiplayer screen's Create (beside a Restore) are elsewhere.

--run-b EXE: B runs this host instead of --run (another build of the same source, e.g. the lifted C
at /O2 against /O1: docs/performance.md). The join normally refuses another build; every run then
gets SWGB_MP_ANY_BUILD=1, and the sync records say whether the two builds simulate alike.

--extra-a and --extra-b give one player switches the others do not have: --extra-a=--smooth
checks that drawing between steps (docs/framerate.md) leaves the simulation alone.

N headless --multi runs of the host (docs/multiplayer.md), players a, b, c, d: A hosts, the others
join, all through the host's own DirectPlay (src/runtime/dplay.c). A goes Multiplayer -> Create and
clicks I'm Ready; each other player starts B_DELAY seconds after the one before, goes Multiplayer,
picks the game in the list, Join, I'm Ready; A clicks Start Game (a Random Map, the setup screen's
defaults). Then all play M minutes:

  idle    nothing after the start
  habits  each player, every 25-45 s: H (the command center), its first button (a worker),
          a right-click somewhere (soak.py's idle player)
  busy    habits, plus every 2-6 s a left or right click on the map or a command button

The menus are each player's --keys script, timed from the main menu's music (--keys
"on:open.mp3"): a busy machine (ten agents share this one) delays the start, not the clicks. The
game is played live (--keys-live): this tool watches the logs and writes each player's steps as
they fall due, on one clock for all from the game's start (the first sync record in A's log: the
players finish loading at different times, but the lockstep game starts for all at once). A shot
is taken of a player the moment its log says a chat line arrived.

Each player's game folder is DIR-mpK (hard links to DIR, as soak.py makes them; A's is DIR); each
folder is a different player profile only in its registry hive, so every player has the profile's
name.

  --chat          every player sends two chat lines in the game ("mpchat" + player + number); each
                  must reach every other player (dplay.c logs each chat line the game takes in:
                  "[mp] chat from player N ...") and be drawn there (in a shot a few seconds later,
                  pixels of the sender's chat colour at the left of the map)
  --drop WHO@MIN  WHO's process is killed (its own PID) MIN minutes into the game. A joiner: the
                  game shows the others its Multiplayer Status ("Player Dropped"; vote Wait,
                  Continue Game or Save and Exit); they vote Continue Game, play on to the end
                  and stay in sync. A (the host): the others lose the session (there is no host
                  migration), and must neither fault nor hang.
  --quit WHO@MIN  WHO leaves cleanly MIN minutes in: the game menu, Quit Current Game, Yes, then
                  the score screen's Main Menu; its run ends at its watchdog
  --resign WHO@MIN  WHO resigns MIN minutes in (the game menu, Resign, Yes) and stays in the game
  --save MIN      A saves the game MIN minutes in (the game menu, Save, OK: the name the game
                  offers). A multiplayer save lands in every player's SaveGame\\Multi.
  --restore       a second game after the first: the same players, A restores the newest save
                  (the Multiplayer screen's Restore, there once a save is) and all play --minutes
                  more, checked the same way; the restored game must start where the save was
  --wsl WHO       WHO runs the Linux host in WSL (wsl.exe -d WSL_DISTRO), from WSL_CHECKOUT's
                  build-linux/ and work/ (a Windows <-> Linux game over the network between them)

What it checks, from the logs:
  - each ran to its watchdog (exit 4): no fault, not-lifted stub or unresolved call (a --drop'ed
    player was killed, and is not checked);
  - nobody lost the session or a player before that player's planned end, but for the --drop or
    --quit, which every other player sees within 45 s;
  - the game showed no out-of-sync message ("[mp] the game loaded string ...");
  - the game's own sync records ("[sync] slot: nine words", dplay.c) agree: every record two
    instances logged for the same player and turn is the same in both, and within each log every
    player's record for a turn is the same; after a joiner leaves, the others keep logging them.
Exit code 0 when all hold, 1 otherwise. Logs, keys and shots (every five minutes) in --out.
"""
import argparse
import datetime
import json
import os
import random
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import soak  # noqa: E402  (its screens, its Script, ensure_game)

MAIN_MULTI = (275, 390)          # the main menu: Multiplayer
MAIN_MULTI_CC = (290, 405)       # ... in Clone Campaigns
# The Multiplayer screen: Create, the width of the panel; with a multiplayer save in SaveGame\Multi
# (Clone Campaigns: always, the template's "Out of Sync Save"s), Create on the left and Restore
# on the right. (515, 452) is Create in both.
MP_CREATE = (515, 452)
MP_RESTORE = (682, 452)          # ... Restore: its Saved Game list, the newest first
MP_LAN = (560, 134)              # ... the connection list: Local (LAN) TCP/IP Connection
MP_JOIN = (602, 402)             # ... Join
MP_ROW0 = (600, 238)             # ... the first game in the list
SETUP_READY = (88, 517)          # the multiplayer setup screen: I'm Ready!
SETUP_START = (118, 568)         # ... Start Game (the host's)
# The game menu in a multiplayer game (F10): Quit Current Game, Resign, Save, Save And Exit,
# Options, About, Cancel. Single player's (soak.GAME_MENU) has no Resign: Save is a row higher.
MP_MENU = {'quit': (408, 178), 'resign': (408, 218), 'save': (408, 251), 'save_exit': (408, 284)}
MENU_YES = (305, 328)            # "Are you sure you want to resign?" (or quit): Yes
STATUS_CONTINUE = (445, 369)     # Multiplayer Status (a player dropped): vote Continue Game
STATUS_BOX = 'CreateWindowExA "" style 40000080 at 67,463 441x21'   # ... its chat line, opening
GAME_DEFAULT = r'A:\Games\star\games\multiplayer'
B_DELAY = 5                      # each joiner starts this many seconds after the one before
WHO = 'abcd'
ON_MENU = 'open.mp3'             # the main menu's music: the menus' times count from it
TO_MENU = 45                     # seconds from a start to its main menu, at most (it is ~18)
LOAD = 100                       # seconds from Start Game to the map, at most (it is ~60)
STEP = 15                        # each joiner's game ends this much before the one before it
WSL_DISTRO = os.environ.get('WSL_DISTRO', 'Ubuntu')
WSL_CHECKOUT = os.environ.get('WSL_CHECKOUT', '')   # a Linux path: the checkout with build-linux/


def start_at(n, restore=False):
    """When A clicks Start Game (seconds after its main menu): every joiner has clicked I'm Ready
    (the last at 45 + B_DELAY * (n - 1) on A's clock), with room for a slow one."""
    return 66 + B_DELAY * (n - 2) + (10 if restore else 0)


def menu_part(s, who, k, n, out, restore=False):
    """The menus, timed from this player's main menu."""
    start = start_at(n, restore)
    s.click(2, MAIN_MULTI)
    s.at(14 if who == 'a' else 28, 'shot=%s' % os.path.join(out, 'multiplayer.bmp'))
    if who == 'a' and restore:
        s.click(16, MP_RESTORE)
        s.at(22, 'shot=%s' % os.path.join(out, 'restore_list.bmp'))
        s.click(23, soak.SAVED_OK)    # the newest save is selected
        s.click(43, SETUP_READY)
    elif who == 'a':
        s.click(16, MP_CREATE)
        s.click(33, SETUP_READY)
    else:
        # the LAN connection: its games appear by themselves. A folder that last used it
        # (the Windows farm's) has it selected already; a new one has "Internet", whose
        # Show Games asks for an address.
        r = 10 if restore else 0      # A's Restore takes a list and a load first
        s.click(22 + r, MP_LAN)
        s.click(29 + r, MP_ROW0)
        s.click(34 + r, MP_ROW0)   # again: Clone Campaigns' list drops the selection when it refreshes
        s.click(35 + r, MP_JOIN)
        s.click(45 + r, SETUP_READY)
    s.at(start - 3 - B_DELAY * k, 'shot=%s' % os.path.join(out, 'setup.bmp'))
    if who == 'a':
        s.click(start, SETUP_START)


def chat_times(n, minutes, before=None):
    """Two chat lines from each player, spread over the game's first half (or the time before
    a player leaves: before, in seconds), a player at a time: [(seconds into the game, who,
    words)]."""
    out = []
    span = max(60, minutes * 60 / 2)
    if before:
        span = min(span, max(2 * n * 4, before - 30))
    step = span / (2 * n)
    for r in range(2):
        for k in range(n):
            out.append((20 + step * (r * n + k), WHO[k], 'mpchat%s%d' % (WHO[k], r + 1)))
    return out


def leave_at(args):
    """When a player drops or quits (seconds into the game), or None."""
    spec = args.drop or args.quit
    return spec[1] * 60 if spec else None


def game_end(k, minutes):
    """When player k's game part ends (seconds into the game): each joiner before the one before."""
    return minutes * 60 - STEP * k


def game_plan(rnd, who, k, n, args, out, phase=''):
    """The game: [(seconds from its start, step)], for --keys-live. (Shots of chat lines are
    taken as they arrive: play().)"""
    s = soak.Script()
    end = game_end(k, args.minutes)
    s.at(5, 'shot=%s' % os.path.join(out, 'start.bmp'))
    for ts in range(300, int(end), 300):
        s.at(ts, 'shot=%s' % os.path.join(out, 'shot_%04d.bmp' % ts))
    s.at(end - 5, 'shot=%s' % os.path.join(out, 'end.bmp'))
    quiet = []                    # (from, to): no activity (the chat line, a save, the menu, a vote)
    if args.chat:
        for t, w, words in chat_times(n, args.minutes, leave_at(args)):
            if w == who:
                soak.chat(s, t, words)
                quiet.append((t - 2, t + 5 + 0.3 * len(words)))
    if args.save is not None and not phase:
        t = args.save * 60
        if who == 'a':   # the game menu, Save, OK: the name the game offers ("Multiplayer Save - <date>")
            s.click(t, soak.HUD_RIGHT['menu'])
            s.click(t + 2, MP_MENU['save'])
            s.at(t + 5, 'shot=%s' % os.path.join(out, 'save_screen.bmp'))
            s.click(t + 6, soak.SAVE_OK)
        s.at(t + 21, 'shot=%s' % os.path.join(out, 'saved.bmp'))
        quiet.append((t - 2, t + 22))
    stop = end - 10
    for spec, how in ((args.quit, 'quit'), (args.resign, 'resign')):
        if spec and spec[0] == who and not phase:
            t = spec[1] * 60
            s.click(t, soak.HUD_RIGHT['menu'])
            s.at(t + 2, 'shot=%s' % os.path.join(out, how + '_menu.bmp'))
            s.click(t + 3, MP_MENU[how])
            s.at(t + 5, 'shot=%s' % os.path.join(out, how + '_sure.bmp'))
            s.click(t + 6, MENU_YES)
            s.at(t + 14, 'shot=%s' % os.path.join(out, how + '_after.bmp'))
            if how == 'quit':
                s.click(t + 16, soak.SCORE_MENU)
                s.at(t + 30, 'shot=%s' % os.path.join(out, how + '_menu_after.bmp'))
            stop = t - 2
    if args.drop and args.drop[0] != who and not phase:
        # the game waits for the dropped player's turns and shows Multiplayer Status, where
        # play() votes Continue Game each time it opens; no other clicks meanwhile
        t = args.drop[1] * 60
        s.at(t + 75, 'shot=%s' % os.path.join(out, 'drop_after.bmp'))
        quiet.append((t - 2, t + 75))

    def busy(t):
        return any(a <= t <= b for a, b in quiet)
    if args.activity in ('habits', 'busy'):
        h = soak.Script()
        soak.habits(h, rnd, 6, stop, None, out)
        s.steps += [st for st in h.steps if not busy(st[0])]
    if args.activity == 'busy':
        t = 8
        while t < stop:
            if not busy(t):
                r = rnd.random()
                if r < 0.4:
                    s.click(t, (rnd.randint(0, 799), rnd.randint(25, 420)))
                elif r < 0.75:
                    s.click(t, (rnd.randint(0, 799), rnd.randint(25, 420)), 'rclick')
                else:
                    s.click(t, rnd.choice(soak.COMMAND_GRID))
            t += rnd.uniform(2, 6)
    return sorted(s.steps, key=lambda st: st[0])


def script(who, k, n, args, out, phase=''):
    """One player's --keys script (the menus) and its watchdog (seconds)."""
    m = soak.Script()
    menu_part(m, who, k, n, out, restore=bool(phase))
    text = 'on:%s\n%s\n' % (ON_MENU, m.text())
    # A's watchdog: its menu, Start, the load, the game, a margin; each joiner's ends earlier
    # (it starts B_DELAY later and plays STEP less), so A sees players leave, not a lost host.
    wd = TO_MENU + start_at(n, bool(phase)) + LOAD + (30 if phase else 0) + args.minutes * 60 + 20
    return text, int(wd - (B_DELAY + STEP) * k)


SYNC = re.compile(r'^\[sync\]\s+(\d+): ((?:[0-9A-F]{8} ?){9})$')


def sync_records(log):
    recs = []
    for line in open(log, encoding='utf-8', errors='replace'):
        m = SYNC.match(line.strip())
        if m:
            recs.append((int(m.group(1)), tuple(int(x, 16) for x in m.group(2).split())))
    return recs


def index(recs):
    d = {}
    for _, w in recs:
        d.setdefault((w[0], w[2]), set()).add(w)
    return d


def compare(ra, rb):
    """A record is one player's checkpoint: word 0 the player (1, 2, ...), word 1 the world time
    (ms), word 2 the turn (every 40th), word 3 the world time again, words 4-8 what the game
    compares (checksums, and a word that is 0). Each instance holds every player's record for a
    turn. Two checks: across the logs, the records for one (player, turn); within each log, every
    player's record for one turn, all but word 0."""
    return compare_all({'a': ra, 'b': rb})


def compare_all(recs):
    """compare() for any number of logs: {who: records}. Returns (keys two or more logs share,
    differences)."""
    idx = {w: index(r) for w, r in recs.items()}
    shared, bad = set(), []
    names = sorted(recs)
    for i, x in enumerate(names):
        for y in names[i + 1:]:
            both = set(idx[x]) & set(idx[y])
            shared |= both
            bad += [(k, sorted(idx[x][k]), sorted(idx[y][k])) for k in sorted(both) if idx[x][k] != idx[y][k]]
    for name in names:
        turns = {}
        for _, w in recs[name]:
            turns.setdefault(w[2], set()).add(w[1:])
        bad += [(('within ' + name, t), sorted(v), []) for t, v in sorted(turns.items()) if len(v) > 1]
    return sorted(shared), bad


CHAT_RX = re.compile(r'^\[mp\] chat from player (\d+) \((\w+)\) to (\w+) \((\w+)\): "(.*)"$')
DEPART = re.compile(r'^\[dplay\] (?:player ([0-9A-F]{8}) \(.*\) left|(the host is gone: session lost)|'
                    r'connection \d+ silent for 30 s: dropped)(?: \(([\d.]+) s\))?$')
LEFT = re.compile(r'^\[dplay\] (?:Close \(client\)|DestroyPlayer [0-9A-F]{8}) \(([\d.]+) s\)')
ME = re.compile(r'^\[dplay\] CreatePlayer ".*" -> ([0-9A-F]{8})')
IN_GAME = re.compile(r'^\[sync\] ')


def check(log):
    """The log's faults and out-of-sync messages, the chat lines it took in ({text: player}),
    its own DPID, each departure it saw [(seconds since its start, line, DPID or 'host')], when
    it left itself (its own Close or DestroyPlayer, or None) and whether its game started."""
    lines = open(log, encoding='utf-8', errors='replace').read().splitlines()
    ev = [l for l in lines if soak.EVENTS.search(l) or '[mp] the game loaded string' in l]
    chats = {m.group(5): int(m.group(1)) for m in map(CHAT_RX.match, lines) if m}
    me, deps, left, started = None, [], None, False
    for l in lines:
        m = ME.match(l)
        if m and not me:
            me = m.group(1)
        m = DEPART.match(l)
        if m:
            deps.append((float(m.group(3) or 0), l, m.group(1) or 'host'))
        m = LEFT.match(l)
        if m and left is None and started:
            left = float(m.group(1))
        if IN_GAME.match(l):
            started = True
    return ev, chats, me, deps, left, started


MIGRATION = re.compile(r'^\[dplay\] (the host is gone \(.*|client \d+ .* is back after the host migration.*|'
                       r'\d+ message\(s\) held during the host migration sent)$')


def migration(log):
    """What a log says of a host migration (dplay.c): who took over, who came back."""
    return [l[8:] for l in open(log, encoding='utf-8', errors='replace').read().splitlines() if MIGRATION.match(l)]


# The colour the game draws a player's chat line in (player 1, 2, ...): measured on the shots.
CHAT_RGB = {1: (110, 166, 235), 2: (255, 100, 100), 3: (0, 255, 0), 4: (255, 255, 0)}


def chat_shown(png, player):
    """Whether a chat line from this player is drawn in the shot: the game shows the lines it
    receives at the left of the map view, each in its sender's colour (a line of eight letters
    and the name is about 430 pixels of it; units are painted in other shades)."""
    try:
        from PIL import Image
    except ImportError:
        return None
    if not png or not os.path.exists(png):
        return False
    im = Image.open(png).convert('RGB')
    w, h = im.size
    box = im.crop((0, int(h * 0.033), int(w * 0.6), int(h * 0.71)))
    rgb = CHAT_RGB.get(player)
    return sum(1 for p in box.getdata() if p == rgb) >= 150


def to_png(d):
    for n in os.listdir(d):
        if n.endswith('.bmp'):
            f = os.path.join(d, n)
            subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', f, f[:-4] + '.png'])
            os.remove(f)


def wsl_path(p):
    """A Windows path as WSL sees it (A:\\x -> /mnt/a/x)."""
    p = os.path.abspath(p)
    return '/mnt/%s%s' % (p[0].lower(), p[2:].replace('\\', '/'))


def for_host(args, who, step):
    """A step as that player's host reads it: a WSL player's shots go to WSL paths."""
    if who in args.wsl and step.startswith('shot='):
        return 'shot=' + wsl_path(step[5:])
    return step


def launch(args, who, k, game, out, keys, live, wd):
    log = open(os.path.join(out, 'run.log'), 'w')
    if who in args.wsl:
        root = WSL_CHECKOUT
        run = '%s/build-linux/%s' % (root, 'swgbrecomp_cc' if args.cc else 'swgbrecomp')
        exe = '%s/work/%s' % (root, 'battlegrounds_x1.exe' if args.cc else 'Battlegrounds.exe')
        # its game folder in Linux's own file system (win32hle's LoadLibraryA finds no language.dll
        # on a /mnt drive): work/games/<the Windows folder's name>, hard links to the checkout's game/
        lgame = '%s/work/games/%s' % (root, os.path.basename(game.rstrip('\\/')))
        subprocess.run(['wsl.exe', '-d', WSL_DISTRO, '--cd', root, '--exec', 'python3', '-c',
                        "import sys; sys.path.insert(0, 'tools'); import soak; soak.ensure_game('game', %r)" % lgame])
        # stderr into stdout in Linux: through wsl.exe the two reach the log with their own file
        # positions, and one wrote over the other
        cmd = ['wsl.exe', '-d', WSL_DISTRO, '--cd', wsl_path(out), '--exec', 'bash', '-c', 'exec "$@" 2>&1', 'bash',
               run, '--run', '--multi', '--headless',
               '--exe', exe, '--game', lgame, '--watchdog', str(wd), '--keys', '@' + wsl_path(keys), '--keys-live', wsl_path(live),
               '--mp-port', str(args.port)] + args.extra.split() + args.wsl_extra.split()
    else:
        run = args.run_b if who == 'b' and args.run_b else args.run
        cmd = [run, '--run', '--multi', '--headless', '--exe', args.exe, '--game', game,
               '--watchdog', str(wd), '--keys', '@' + keys, '--keys-live', live, '--mp-port', str(args.port)] +             args.extra.split() + own_switches(args, who).split()
    env = dict(os.environ)
    if args.run_b and os.path.abspath(args.run_b) != os.path.abspath(args.run):
        env['SWGB_MP_ANY_BUILD'] = '1'   # dplay.c: the join lets another build in
    return subprocess.Popen(cmd, cwd=out, stdout=log, stderr=subprocess.STDOUT, env=env)


def own_switches(args, who):
    """--extra-a / --extra-b: the switches only that player has."""
    return {'a': args.extra_a, 'b': args.extra_b}.get(who, '')


class Tail:
    """The lines a growing log has gained since the last read."""
    def __init__(self, path):
        self.path, self.pos, self.rest = path, 0, ''

    def lines(self):
        try:
            with open(self.path, 'rb') as f:
                f.seek(self.pos)
                b = f.read()
        except OSError:
            return []
        self.pos += len(b)
        text = self.rest + b.decode('utf-8', 'replace')
        out = text.split('\n')
        self.rest = out.pop()
        return [l.rstrip() for l in out]


def play(args, phase=''):
    """One game: launch, play it live, read the logs. Returns the summary (a dict)."""
    n = args.players
    procs, ends, starts, folders, outs, lives, plans = {}, {}, {}, {}, {}, {}, {}
    for k in range(n):
        who = WHO[k]
        outs[who] = os.path.join(args.out, phase + who)
        os.makedirs(outs[who], exist_ok=True)
    for k in range(n):
        who = WHO[k]
        game = args.game if k == 0 else args.game.rstrip('\\/') + '-mp%d' % (k + 1)
        if k:
            soak.ensure_game(args.game, game)
        folders[who] = game
        rnd = random.Random(args.seed * 4 + k + (100 if phase else 0))
        text, wd = script(who, k, n, args, outs[who], phase)
        text = '\n'.join(l.split(':', 1)[0] + ':' + for_host(args, who, l.split(':', 1)[1]) if 'shot=' in l else l
                         for l in text.split('\n'))
        plans[who] = game_plan(rnd, who, k, n, args, outs[who], phase)
        keys = os.path.join(outs[who], 'keys.txt')
        open(keys, 'w').write(text)
        lives[who] = os.path.join(outs[who], 'live.txt')
        open(lives[who], 'w').close()
        open(os.path.join(outs[who], 'game_plan.txt'), 'w').write('\n'.join('%g:%s' % st for st in plans[who]) + '\n')
        ends[who] = wd
        procs[who] = launch(args, who, k, game, outs[who], keys, lives[who], wd)
        starts[who] = time.time()
        print('%s%s: pid %d, %d s, %s%s' % (phase, who, procs[who].pid, wd, game, ' (WSL)' if who in args.wsl else ''),
              flush=True)
        if k < n - 1:
            time.sleep(B_DELAY)
    # the game, live: one clock for all, from its start (A's first sync record: world time 4.4 s,
    # logged once it has held still a second)
    tails = {w: Tail(os.path.join(outs[w], 'run.log')) for w in procs}
    nxt = {w: 0 for w in procs}
    g0, dropped, drop_wall, hung = None, None, None, set()
    chat_shots = {w: [] for w in procs}
    votes = {}
    while any(p.poll() is None for p in procs.values()):
        now = time.time()
        for w in procs:
            for l in tails[w].lines():
                if g0 is None and w == 'a' and l.startswith('[sync]'):
                    g0 = now - 6
                    print('%sthe game started (%.0f s after A)' % (phase, g0 - starts['a']), flush=True)
                if g0 is not None and STATUS_BOX in l and procs[w].poll() is None:
                    # Multiplayer Status opened (its chat line is a window): vote Continue Game,
                    # and again (a vote can come before the dialog takes clicks)
                    k = votes[w] = votes.get(w, 0) + 1
                    t_open = now - g0
                    plans[w][nxt[w]:] = sorted(plans[w][nxt[w]:] + [
                        (t_open + 2, 'shot=%s' % os.path.join(outs[w], 'status_%d.bmp' % k)),
                        (t_open + 3, 'click:%d:%d' % STATUS_CONTINUE),
                        (t_open + 13, 'click:%d:%d' % STATUS_CONTINUE),
                        (t_open + 23, 'click:%d:%d' % STATUS_CONTINUE)], key=lambda st: st[0])
                    print('%s: Multiplayer Status opened (%d), voting Continue Game' % (w, k), flush=True)
                m = CHAT_RX.match(l)
                if m and procs[w].poll() is None:   # the line shows for some seconds: a shot now
                    shot = os.path.join(outs[w], 'chat_%s.bmp' % m.group(5))
                    with open(lives[w], 'a') as f:
                        f.write('0:%s\n' % for_host(args, w, 'shot=' + shot))
                    chat_shots[w].append(m.group(5))
        if g0 is not None:
            t = now - g0
            for w in procs:
                due = []
                while nxt[w] < len(plans[w]) and plans[w][nxt[w]][0] <= t:
                    due.append(plans[w][nxt[w]][1])
                    nxt[w] += 1
                if due and procs[w].poll() is None:
                    with open(lives[w], 'a') as f:
                        f.write(''.join('0:%s\n' % for_host(args, w, st) for st in due))
            if args.drop and not phase and dropped is None and t >= args.drop[1] * 60:
                who = args.drop[0]
                if procs[who].poll() is None:
                    soak.kill_tree(procs[who])   # its own PID: never a name
                    dropped, drop_wall = who, time.time()
                    print('%s killed (pid %d) %.0f s into the game' % (who, procs[who].pid, t), flush=True)
                else:
                    dropped = ''
        for w, p in procs.items():   # past its watchdog and a margin: hung
            if p.poll() is None and now - starts[w] > ends[w] + 120:
                soak.kill_tree(p)   # only the PIDs this tool started
                hung.add(w)
                print('%s hung: killed' % w, flush=True)
        time.sleep(0.2)
    rc = {w: p.poll() for w, p in procs.items()}
    for w in hung:
        rc[w] = 'hung: killed'
    if dropped:
        rc[dropped] = 'dropped'
    for who in procs:
        to_png(outs[who])
    logs = {w: os.path.join(outs[w], 'run.log') for w in procs}
    recs = {w: sync_records(l) for w, l in logs.items()}
    shared, bad = compare_all(recs)
    ev, chats, ids, deps, left, started = {}, {}, {}, {}, {}, {}
    for w, l in logs.items():
        ev[w], chats[w], ids[w], deps[w], left[w], started[w] = check(l)
    pid_of = {v: w for w, v in ids.items() if v}            # DPID -> player
    leaver, leave_wall = None, None                         # who left mid-game, when (wall clock)
    if dropped:
        leaver, leave_wall = dropped, drop_wall
    elif args.quit and not phase:
        leaver = args.quit[0]
        leave_wall = starts[leaver] + left[leaver] if left[leaver] is not None else None
    planned = {w: starts[w] + ends[w] for w in procs}        # each watchdog, wall clock
    problems = []
    lost = {w: [] for w in procs}
    seen_leave = {w: None for w in procs}
    for w in procs:
        if not started[w]:
            problems.append('%s: the game never started (no sync record)' % w)
        if w == dropped:
            continue
        if rc.get(w) != 4:
            problems.append('%s: exit %s' % (w, rc.get(w)))
        if ev[w]:
            problems.append('%s: %s' % (w, ev[w][0]))
        for t, line, dpid in deps[w]:
            x = None if dpid == 'host' else pid_of.get(dpid)   # a lost session is never planned
            wall = starts[w] + t
            if x and x == leaver and leave_wall and wall >= leave_wall - 5:
                seen_leave[w] = seen_leave[w] or wall
            elif x and x != w and wall >= planned[x] - 15:
                pass                                       # its watchdog: the planned end
            elif w == leaver:
                pass                                       # what the leaver saw after it left
            else:
                lost[w].append(line)
        if lost[w]:
            problems.append('%s: %s' % (w, lost[w][0]))
    if args.quit and not phase and leave_wall is None:
        problems.append('%s never left (no Close or DestroyPlayer after the game started)' % args.quit[0])
    if leaver and leave_wall:
        for w in procs:
            if w != leaver and rc.get(w) == 4:
                t = seen_leave[w]
                if t is None:
                    problems.append('%s never saw %s leave' % (w, leaver))
                elif t > leave_wall + 45:
                    problems.append('%s saw %s leave %.0f s after it left' % (w, leaver, t - leave_wall))
    if bad:
        problems.append('%d sync records differ' % len(bad))
    if not shared:
        problems.append('no sync records shared')
    if leaver:   # the others played on (the host: after a host migration): records after it left
        after = max((r[1] for w in procs if w != leaver for _, r in recs[w]), default=0)
        before = max((r[1] for _, r in recs[leaver]), default=0)
        if after <= before:
            problems.append('no sync records after %s left' % leaver)
    chat_report = {}
    if args.chat:
        for t, sender, words in chat_times(n, args.minutes, leave_at(args)):
            for w in procs:
                if w == sender or leaver in (w, sender):   # the leaver may be gone by then
                    continue
                got = words in chats[w]
                png = os.path.join(outs[w], 'chat_%s.png' % words)
                drawn = chat_shown(png, chats[w].get(words, WHO.index(sender) + 1))
                chat_report['%s->%s %s' % (sender, w, words)] = {'received': got, 'drawn': drawn}
                if not got:
                    problems.append('%s never received "%s" from %s' % (w, words, sender))
                if drawn is False:
                    problems.append('%s: "%s" not drawn in %s' % (w, words, png))
    migr = {w: migration(l) for w, l in logs.items()}
    if leaver == 'a':   # the host went: one of the others hosts now, the rest came back to it
        if not any('hosts now' in l for w in procs if w != 'a' for l in migr[w]):
            problems.append('nobody took over as the host')
        for w in procs:
            if w != 'a' and not any('hosts now' in l or 'rejoined' in l for l in migr[w]):
                problems.append('%s did not move to the new host' % w)
    first = {w: next(((r[1], r[2]) for _, r in recs[w]), None) for w in procs}
    summary = {
        'phase': phase or 'game', 'players': n, 'exit': rc, 'wsl': sorted(args.wsl),
        'hosts': {w: args.run_b if w == 'b' and args.run_b else args.run for w in procs},
        'switches': {w: ' '.join((args.extra + ' ' + own_switches(args, w)).split()) for w in procs},
        'sync_records': dict({w: len(r) for w, r in recs.items()}, shared=len(shared), differ=len(bad)),
        'first_record': {w: {'world_ms': v[0], 'turn': v[1]} if v else None for w, v in first.items()},
        'events': ev, 'lost': lost, 'chat': chat_report, 'status_dialogs': votes, 'host_migration': {w: v for w, v in migr.items() if v},
        'left': {'who': leaver, 'seen_after_s': {w: round(t - leave_wall, 1) for w, t in seen_leave.items() if t}}
        if leaver and leave_wall else None,
        'turns_checked': len({k[1] for k in shared}),
        'first_differences': [{'key': str(k), 'x': [' '.join('%08X' % x for x in w) for w in va],
                               'y': [' '.join('%08X' % x for x in w) for w in vb]} for k, va, vb in bad[:5]],
        'problems': problems, 'pass': not problems, 'folders': folders,
    }
    if phase and not problems and all(v and v[0] < 10000 for v in first.values() if v):
        summary['problems'].append('the restored game starts at world time 0: a new game, not the save')
        summary['pass'] = False
    return summary


def who_at(s):
    w, _, m = s.partition('@')
    if w not in WHO or not m:
        raise argparse.ArgumentTypeError('WHO@MINUTES, WHO one of a, b, c, d')
    return (w, float(m))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--players', type=int, default=2, choices=[2, 3, 4])
    ap.add_argument('--minutes', type=float, default=20)
    ap.add_argument('--activity', choices=['idle', 'habits', 'busy'], default='habits')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--cc', action='store_true', help='Clone Campaigns')
    ap.add_argument('--chat', action='store_true', help='chat lines from every player, checked on every other')
    ap.add_argument('--drop', type=who_at, default=None, help='WHO@MIN: kill that player\'s process MIN minutes in')
    ap.add_argument('--quit', type=who_at, default=None, help='WHO@MIN: that player quits MIN minutes in')
    ap.add_argument('--resign', type=who_at, default=None, help='WHO@MIN: that player resigns MIN minutes in')
    ap.add_argument('--save', type=float, default=None, help='MIN: A saves the game MIN minutes in')
    ap.add_argument('--restore', action='store_true', help='then restore that save with the same players and play on')
    ap.add_argument('--wsl', default='', help='players (e.g. "b") that run the Linux host in WSL')
    ap.add_argument('--wsl-extra', default='', help='more host switches for the WSL players')
    ap.add_argument('--run', default=None, help=r'the host (A:\Games\star\builds\multiplayer\swgbrecomp.exe; '
                    r'--cc: ...\multiplayer-cc\swgbrecomp_cc.exe)')
    ap.add_argument('--exe', default=None, help=r'A:\Games\star\swgb-recomp\work\Battlegrounds.exe '
                    r'(--cc: battlegrounds_x1.exe there)')
    ap.add_argument('--game', default=GAME_DEFAULT)
    ap.add_argument('--out', default=None)
    ap.add_argument('--port', type=int, default=2300)
    ap.add_argument('--template', default=r'A:\Games\star\swgb-recomp\game' if os.name == 'nt' else None,
                    help='the shared install a missing --game folder is made from (hard links)')
    ap.add_argument('--profiles', default=r'A:\Games\star\games\clone-campaigns' if os.name == 'nt' else None,
                    help='--cc: the player.nf* a new --game folder starts with (an 800x600 profile)')
    ap.add_argument('--run-b', help='B runs this host instead of --run (another build: SWGB_MP_ANY_BUILD)')
    ap.add_argument('--extra', default='', help='more host switches for every run')
    ap.add_argument('--extra-a', default='', help='more host switches for A (the host) only')
    ap.add_argument('--extra-b', default='', help='more host switches for B only')
    args = ap.parse_args()
    if args.restore and args.save is None:
        ap.error('--restore needs --save')
    if args.drop and args.quit:
        ap.error('one of --drop and --quit')
    if any(w not in WHO[:args.players] for w in args.wsl):
        ap.error('--wsl: players among %s' % WHO[:args.players])
    if args.wsl and not WSL_CHECKOUT:
        ap.error('--wsl needs WSL_CHECKOUT (the Linux path of a checkout with build-linux/ and work/)')
    global MAIN_MULTI
    if args.cc:
        MAIN_MULTI = MAIN_MULTI_CC
    if os.name == 'nt':
        args.run = args.run or (r'A:\Games\star\builds\multiplayer-cc\swgbrecomp_cc.exe' if args.cc
                                else r'A:\Games\star\builds\multiplayer\swgbrecomp.exe')
        args.exe = args.exe or (r'A:\Games\star\swgb-recomp\work\battlegrounds_x1.exe' if args.cc
                                else r'A:\Games\star\swgb-recomp\work\Battlegrounds.exe')
    else:   # Linux and macOS: this checkout's build and lift, a game folder under work/
        root = os.path.dirname(HERE)
        build = os.path.join(root, 'build-macos' if sys.platform == 'darwin' else 'build-linux')
        args.run = args.run or os.path.join(build, 'swgbrecomp_cc' if args.cc else 'swgbrecomp')
        args.exe = args.exe or os.path.join(root, 'work', 'battlegrounds_x1.exe' if args.cc else 'Battlegrounds.exe')
        if args.game == GAME_DEFAULT:
            args.game = os.path.join(root, 'work', 'games', 'multiplayer')
        if not os.path.isdir(args.game):
            soak.ensure_game(os.path.join(root, 'game'), args.game)
    args.out = os.path.abspath(args.out or os.path.join(os.getcwd(), 'mp-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S')))
    args.game = os.path.abspath(args.game)   # each run starts in its own folder
    if not os.path.isdir(args.game) and args.template:   # A's folder, from the shared install
        soak.ensure_game(args.template, args.game)
        if args.cc and args.profiles and os.path.isdir(args.profiles):   # an 800x600 profile (test.py's)
            for f in os.listdir(args.profiles):
                if f.lower().startswith('player.nf'):
                    shutil.copy2(os.path.join(args.profiles, f), os.path.join(args.game, f))
    results = [play(args)]
    if args.restore:
        results.append(play(args, phase='r_'))
    ok = all(r['pass'] for r in results)
    json.dump(results if len(results) > 1 else results[0], open(os.path.join(args.out, 'summary.json'), 'w'), indent=1)
    print(json.dumps(results, indent=1))
    print('PASS' if ok else 'FAIL', args.out)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
