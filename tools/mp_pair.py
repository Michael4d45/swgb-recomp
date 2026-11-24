"""A LAN game between two recomp instances on this machine, played and checked.

    py -3 tools\\mp_pair.py [--minutes M] [--activity idle|habits|busy] [--seed S] [--cc]
                            [--run EXE] [--exe GUEST] [--game DIR] [--out DIR] [--port N]

--cc plays Clone Campaigns (swgbrecomp_cc.exe, battlegrounds_x1.exe): the main menu's Multiplayer
button and the Multiplayer screen's Create (beside a Restore) are elsewhere.

Two headless --multi runs of the host (docs/multiplayer.md): A hosts, B joins over 127.0.0.1,
both through the host's own DirectPlay (src/runtime/dplay.c). A goes Multiplayer -> Create and
clicks I'm Ready; B goes Multiplayer, picks the game in the list, Join, I'm Ready; A clicks Start
Game (a Random Map, two players, the setup screen's defaults). Then both play M minutes:

  idle    nothing after the start
  habits  each player, every 25-45 s: H (the command center), its first button (a worker),
          a right-click somewhere (soak.py's idle player)
  busy    habits, plus every 2-6 s a left or right click on the map or a command button

B's game folder is DIR-mp2 (hard links to DIR, as soak.py makes them); each folder is a
different player profile only in its registry hive, so both players have the profile's name.

What it checks, from the two logs:
  - both ran to the watchdog (exit 4): no fault, not-lifted stub or unresolved call;
  - neither lost the session or a player before the end;
  - the game showed no out-of-sync message ("[mp] the game loaded string ...");
  - the game's own sync records ("[sync] slot: nine words", dplay.c) agree: every record both
    instances logged for the same player and turn is the same in both, and within each log every
    player's record for a turn is the same.
Exit code 0 when all hold, 1 otherwise. Logs, keys and shots (every five minutes) in --out.
"""
import argparse
import datetime
import json
import os
import random
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import soak  # noqa: E402  (its screens, its Script, ensure_game)

MAIN_MULTI = (275, 390)          # the main menu: Multiplayer
MAIN_MULTI_CC = (290, 405)       # ... in Clone Campaigns
MP_CREATE = (602, 452)           # the Multiplayer screen: Create
MP_CREATE_CC = (515, 452)        # ... in Clone Campaigns, beside its Restore
MP_LAN = (560, 134)              # ... the connection list: Local (LAN) TCP/IP Connection
MP_JOIN = (602, 402)             # ... Join
MP_ROW0 = (600, 238)             # ... the first game in the list
SETUP_READY = (88, 517)          # the multiplayer setup screen: I'm Ready!
SETUP_START = (118, 568)         # ... Start Game (the host's)
GAME_DEFAULT = r'A:\Games\star\games\multiplayer'
B_DELAY = 5                      # B starts this many seconds after A
GAME_FROM = 160                  # by then both are in the game (A's clock)


def script(rnd, who, minutes, activity, out):
    s = soak.Script()
    d = 0 if who == 'a' else B_DELAY   # B's clock runs B_DELAY behind A's
    s.click(20, MAIN_MULTI)
    if who == 'a':
        s.click(34, MP_CREATE)
        s.click(51, SETUP_READY)
        s.click(81, SETUP_START)
    else:
        # the LAN connection: its games appear by themselves. A folder that last used it
        # (the Windows farm's) has it selected already; a new one has "Internet", whose
        # Show Games asks for an address.
        s.click(40, MP_LAN)
        s.click(47, MP_ROW0)
        s.click(52, MP_ROW0)   # again: Clone Campaigns' list drops the selection when it refreshes
        s.click(53, MP_JOIN)
        s.click(63, SETUP_READY)
    s.at(GAME_FROM - d - 10, 'shot=%s' % os.path.join(out, 'start.bmp'))
    end = GAME_FROM - d + minutes * 60
    for ts in range(GAME_FROM - d + 300, int(end), 300):
        s.at(ts, 'shot=%s' % os.path.join(out, 'shot_%04d.bmp' % ts))
    s.at(end - 5, 'shot=%s' % os.path.join(out, 'end.bmp'))
    if activity in ('habits', 'busy'):
        soak.habits(s, rnd, GAME_FROM - d, end - 10, None, out)
    if activity == 'busy':
        t = GAME_FROM - d + 2
        while t < end - 10:
            r = rnd.random()
            if r < 0.4:
                s.click(t, (rnd.randint(0, 799), rnd.randint(25, 420)))
            elif r < 0.75:
                s.click(t, (rnd.randint(0, 799), rnd.randint(25, 420)), 'rclick')
            else:
                s.click(t, rnd.choice(soak.COMMAND_GRID))
            t += rnd.uniform(2, 6)
    return s, end


SYNC = re.compile(r'^\[sync\]\s+(\d+): ((?:[0-9A-F]{8} ?){9})$')


def sync_records(log):
    recs = []
    for line in open(log, encoding='utf-8', errors='replace'):
        m = SYNC.match(line.strip())
        if m:
            recs.append((int(m.group(1)), tuple(int(x, 16) for x in m.group(2).split())))
    return recs


def compare(ra, rb):
    """A record is one player's checkpoint: word 0 the player (1, 2, ...), word 1 the world time
    (ms), word 2 the turn (every 40th), word 3 the world time again, words 4-8 what the game
    compares (checksums, and a word that is 0). Each instance holds every player's record for a
    turn. Two checks: across the logs, the records for one (player, turn); within each log, every
    player's record for one turn, all but word 0."""
    def index(recs):
        d = {}
        for _, w in recs:
            d.setdefault((w[0], w[2]), set()).add(w)
        return d
    ia, ib = index(ra), index(rb)
    shared = sorted(set(ia) & set(ib))
    bad = [(k, sorted(ia[k]), sorted(ib[k])) for k in shared if ia[k] != ib[k]]
    for name, recs in (('a', ra), ('b', rb)):
        turns = {}
        for _, w in recs:
            turns.setdefault(w[2], set()).add(w[1:])
        bad += [(('within ' + name, t), sorted(v), []) for t, v in sorted(turns.items()) if len(v) > 1]
    return shared, bad


def check(log):
    text = open(log, encoding='utf-8', errors='replace').read()
    ev = [l for l in text.splitlines() if soak.EVENTS.search(l) or '[mp] the game loaded string' in l]
    lost = [l for l in text.splitlines() if 'session lost' in l or ') left' in l or 'gone' in l]
    return ev, lost


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--minutes', type=float, default=20)
    ap.add_argument('--activity', choices=['idle', 'habits', 'busy'], default='habits')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--cc', action='store_true', help='Clone Campaigns')
    ap.add_argument('--run', default=None, help=r'the host (A:\Games\star\builds\multiplayer\swgbrecomp.exe; '
                    r'--cc: ...\multiplayer-cc\swgbrecomp_cc.exe)')
    ap.add_argument('--exe', default=None, help=r'A:\Games\star\swgb-recomp\work\Battlegrounds.exe '
                    r'(--cc: battlegrounds_x1.exe there)')
    ap.add_argument('--game', default=GAME_DEFAULT)
    ap.add_argument('--out', default=None)
    ap.add_argument('--port', type=int, default=2300)
    ap.add_argument('--extra', default='', help='more host switches for both runs')
    args = ap.parse_args()
    global MAIN_MULTI, MP_CREATE
    if args.cc:
        MAIN_MULTI, MP_CREATE = MAIN_MULTI_CC, MP_CREATE_CC
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
    out = os.path.abspath(args.out or os.path.join(os.getcwd(), 'mp-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S')))
    args.game = os.path.abspath(args.game)   # each run starts in its own folder
    game_b = args.game.rstrip('\\/') + '-mp2'
    soak.ensure_game(args.game, game_b)
    procs = {}
    ends = {}
    for who, game in (('a', args.game), ('b', game_b)):
        d = os.path.join(out, who)
        os.makedirs(d, exist_ok=True)
        rnd = random.Random(args.seed * 2 + (who == 'b'))
        s, end = script(rnd, who, args.minutes, args.activity, d)
        keys = os.path.join(d, 'keys.txt')
        open(keys, 'w').write(s.text())
        wd = int(end + 20) if who == 'a' else int(end + 10)   # B stops first: A sees a player leave, not a lost host
        ends[who] = wd
        cmd = [args.run, '--run', '--multi', '--headless', '--exe', args.exe, '--game', game,
               '--watchdog', str(wd), '--keys', '@' + keys, '--mp-port', str(args.port)] + args.extra.split()
        procs[who] = subprocess.Popen(cmd, cwd=d, stdout=open(os.path.join(d, 'run.log'), 'w'), stderr=subprocess.STDOUT)
        print('%s: pid %d, %d s' % (who, procs[who].pid, wd), flush=True)
        if who == 'a':
            time.sleep(B_DELAY)
    rc = {}
    for who, p in procs.items():
        try:
            rc[who] = p.wait(timeout=ends[who] + 120)
        except subprocess.TimeoutExpired:
            p.kill()   # only the PIDs this tool started
            rc[who] = 'killed'
    for f in [os.path.join(out, w, n) for w in 'ab' for n in os.listdir(os.path.join(out, w)) if n.endswith('.bmp')]:
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', f, f[:-4] + '.png'])
        os.remove(f)
    la, lb = os.path.join(out, 'a', 'run.log'), os.path.join(out, 'b', 'run.log')
    ra, rb = sync_records(la), sync_records(lb)
    shared, bad = compare(ra, rb)
    eva, losta = check(la)
    evb, lostb = check(lb)
    if rc.get('b') == 4:   # B leaving at its watchdog, before A's, is planned
        losta = [l for l in losta if not l.startswith('[dplay] player') and 'client 0 ' not in l]
    summary = {
        'exit': rc, 'sync_records': {'a': len(ra), 'b': len(rb), 'shared': len(shared), 'differ': len(bad)},
        'events': {'a': eva, 'b': evb}, 'lost': {'a': losta, 'b': lostb},
        'turns_checked': len({k[1] for k in shared}),
        'first_differences': [{'key': str(k), 'a': [' '.join('%08X' % x for x in w) for w in va],
                               'b': [' '.join('%08X' % x for x in w) for w in vb]} for k, va, vb in bad[:5]],
    }
    ok = rc.get('a') == 4 and rc.get('b') == 4 and not eva and not evb and not bad and shared and not losta and not lostb
    summary['pass'] = bool(ok)
    json.dump(summary, open(os.path.join(out, 'summary.json'), 'w'), indent=1)
    print(json.dumps(summary, indent=1))
    print('PASS' if ok else 'FAIL', out)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
