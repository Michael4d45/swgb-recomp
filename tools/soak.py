#!/usr/bin/env python3
"""Long headless games against the computer, to find what a short one never runs.

    py -3 tools/soak.py [--mode skirmish|monkey|campaign] [--runs N] [--minutes M]
                        [--jobs J] [--seed S] [--run EXE] [--game DIR] [--out DIR]

Every run is `--headless --multi` (never a window, never the user's game):

  skirmish  Single Player -> Standard Game, with the setup varied per run from
            the seed (1-7 computer players, their civilizations, the
            location, map size and difficulty), Start Game, then an idle
            player's few habits: the command center trains workers, the view
            jumps home, a shot every five minutes. The computers play.
  monkey    the same start, then random input over the whole screen: clicks
            and right-clicks on the map, the command grid, the HUD's
            buttons (the game menu, objectives, chat, diplomacy), the
            minimap, letter and function keys, Escape and Enter. The menu's
            Quit, Restart and Load are never clicked on purpose.
  campaign  Campaigns -> one of the six campaigns -> its first mission ->
            OK, the briefing, Escape for the Objectives, then the monkey (a
            first mission has no command center for the idle habits;
            --idle-campaign for them anyway).

In every mode the game speed is set to Fast (--speed) and the game is saved
once, 40% of the way in (--no-save).

Each run's log, its --keys script and its shots go to OUT/<run>/; the
summary prints every unresolved indirect call, fault and not-lifted stub
with the keys that came before it, and how long the game ran. Exit codes of
the host: 4 the watchdog ended it (the run survived), 0 the game exited,
2 a not-lifted stub, 3 a fault, 5 an unresolved indirect call (coverage.c).

Then `py -3 tools/seeds_from_logs.py` turns the unresolved addresses into
seeds (it reads work/soak/**/*.log).

Coordinates are the 800x600 screens (the menus, the setup screen, the HUD):
a --game folder whose Screen Size is 1024 or 1280 is moved off by the HUD's
layout, so the tool checks the first SetDisplayMode in the game and warns.
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
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# ---- the screens, in 800x600 game pixels ------------------------------------
MAIN_SINGLE = (345, 250)            # main menu: Single Player
SP_STANDARD = (600, 168)            # Single Player: Standard Game
SP_CAMPAIGNS = (603, 112)           # Single Player: Campaigns
SETUP_START = (118, 568)            # Standard Game: Start Game
# The setup screen's drop-downs: the arrow, then the first item and the step.
DD_OTHERS = ((770, 211), (690, 236), 21)        # 1..7 computer players
DD_LOCATION = ((770, 124), (680, 149), 21)      # Forced Deployment .. Shoreline (16 shown)
DD_SIZE = ((770, 153), (690, 178), 21)          # Tiny, Small, Medium, Normal, Large, Giant
DD_DIFFICULTY = ((770, 182), (690, 207), 21)    # Easiest .. Hardest
CIV_ROW0, CIV_ROW_STEP = 169, 24                # computer player rows; the arrow at x 249
CIV_ITEMS = 7                                   # 6 civilizations and Random, x 290, from row + 25
LOCATIONS = ['Forced Deployment', 'Forest', 'Fortress', 'Ice Lake', 'Islands', 'LandMass', 'Large Sea',
             'Motherlode', 'Nova Assault', 'Nova Lake', 'Precipice', 'Raiders', 'Rivers', 'Savannah',
             'Sea', 'Shoreline']
SIZES = ['Tiny', 'Small', 'Medium', 'Normal', 'Large', 'Giant']
DIFFICULTIES = ['Easiest', 'Easy', 'Moderate', 'Hard', 'Hardest']
CIVS = ['Galactic Empire', 'Gungans', 'Rebel Alliance', 'Royal Naboo', 'Trade Federation', 'Wookiees',
        'Random']
# Campaigns: the six planets, then the first mission and the difficulty dialog's OK.
CAMPAIGNS = {'Attichitcuk': (160, 322), 'OOM-9': (187, 212), 'Boss Nass': (272, 118),
             'Darth Vader': (512, 118), 'Princess Leia': (595, 212), 'Chewbacca': (625, 322)}
CAMPAIGN_MISSION1 = (150, 140)
CAMPAIGN_OK = (318, 481)             # the difficulty dialog (Easy is chosen)
OBJECTIVES_OK = (589, 502)           # the mission's opening Objectives (Escape closes it too)
# In the game (the 800x600 HUD).
MAP_VIEW = (0, 20, 799, 425)        # the map, under the resource bar's row and above the panel
COMMAND_GRID = [(x, y) for y in (452, 495, 538, 577) for x in (25, 70, 115, 152)]
HUD_RIGHT = {'menu': (767, 478), 'objectives': (767, 501), 'chat': (767, 525),
             'diplomacy': (767, 550), 'last': (767, 576)}
MINIMAP = (585, 515, 120)           # centre and half-width of the diamond
MINIMAP_BUTTONS = [(664, 480), (692, 480), (470, 565)]
TOP_ICONS = [(190, 441), (212, 441), (233, 441), (254, 441)]
GAME_MENU = {'save': (408, 218), 'options': (408, 317), 'about': (408, 350), 'cancel': (408, 390)}
OPTIONS_FAST, OPTIONS_OK = (135, 248), (305, 455)     # the game menu's Options: Speed
SAVE_OK = (178, 568)                 # the Save Game screen; the name field has the focus
COMMAND_CENTER = (400, 170)         # where the view starts: the command center
MONKEY_KEYS = list('abcdefghijklmnopqrstuvwxyz0123456789') + \
    ['esc', 'enter', 'space', 'tab', 'up', 'down', 'left', 'right', 'home', 'end', 'bksp'] + \
    ['f1', 'f2', 'f3', 'f4', 'f5', 'f6', 'f7', 'f8', 'f9', 'f11']   # F10 opens the menu, kept off

# What the host prints when the game reaches code it cannot run.
EVENTS = re.compile(r'ICALL: unresolved|ITAIL: unresolved|=== fault|\[not-lifted\]|=== unresolved|'
                    r'\[coverage\]|\[eh32\] funclet .* not lifted|STUB:')
EXIT = {0: 'exited', 2: 'not-lifted', 3: 'fault', 4: 'survived (watchdog)', 5: 'unresolved icall'}


class Script:
    def __init__(self):
        self.steps = []

    def at(self, t, what):
        self.steps.append((round(t, 2), what))

    def click(self, t, xy, button='click'):
        self.at(t, '%s:%d:%d' % (button, xy[0], xy[1]))

    def text(self):
        return '\n'.join('%g:%s' % s for s in sorted(self.steps, key=lambda s: s[0]))


def pick(dd, k, s, t):
    """Open a drop-down and click its k-th item. Two seconds."""
    (ax, ay), (ix, iy), step = dd
    s.click(t, (ax, ay))
    s.click(t + 1.2, (ix, iy + step * k))
    return t + 2.5


def setup_skirmish(s, rnd, t):
    """Main menu -> Standard Game -> a varied setup -> Start Game. Returns (t, what)."""
    s.click(t, MAIN_SINGLE)
    s.click(t + 15, SP_STANDARD)
    t += 33
    others = rnd.randint(1, 7)
    size = rnd.randint(max(0, (others + 1 + 1) // 2 - 1), 5)   # big enough for the players, more or less
    loc = rnd.randrange(len(LOCATIONS))
    diff = rnd.randrange(len(DIFFICULTIES))
    t = pick(DD_OTHERS, others - 1, s, t)
    t = pick(DD_LOCATION, loc, s, t)
    t = pick(DD_SIZE, size, s, t)
    t = pick(DD_DIFFICULTY, diff, s, t)
    civs = []
    for p in range(others):
        k = rnd.randrange(CIV_ITEMS)
        civs.append(CIVS[k])
        y = CIV_ROW0 + CIV_ROW_STEP * p
        s.click(t, (249, y))
        s.click(t + 1.2, (290, y + 25 + 21 * k))
        t += 2.5
    s.click(t + 1, SETUP_START)
    what = {'others': others, 'location': LOCATIONS[loc], 'size': SIZES[size],
            'difficulty': DIFFICULTIES[diff], 'civs': civs}
    return t + 1, what


def setup_campaign(s, rnd, t, name=None):
    name = name or rnd.choice(sorted(CAMPAIGNS))
    s.click(t, MAIN_SINGLE)
    s.click(t + 15, SP_CAMPAIGNS)
    s.click(t + 30, CAMPAIGNS[name])
    s.click(t + 40, CAMPAIGN_MISSION1)
    s.click(t + 46, CAMPAIGN_OK)
    return t + 46, {'campaign': name, 'mission': 1}


def fast_speed(s, t):
    """The game menu -> Options -> Speed: Fast -> OK. Six seconds."""
    s.click(t, HUD_RIGHT['menu'])
    s.click(t + 2, GAME_MENU['options'])
    s.click(t + 4, OPTIONS_FAST)
    s.click(t + 5, OPTIONS_OK)
    return t + 6


def save_game(s, t, name):
    """The game menu -> Save -> a name -> OK. Saving runs the whole world through the writer."""
    s.click(t, HUD_RIGHT['menu'])
    s.click(t + 2, GAME_MENU['save'])
    for k, ch in enumerate(name):
        s.at(t + 5 + 0.3 * k, ch)
    s.click(t + 6 + 0.3 * len(name), SAVE_OK)
    return t + 20


def habits(s, rnd, t0, t1, shots, out):
    """An idle player: train a worker now and then, jump home, a shot every five minutes."""
    t = t0
    while t < t1:
        s.at(t, 'h')                                   # the command center, selected and centred
        s.click(t + 1, COMMAND_GRID[0])                # its first button: a worker
        s.click(t + 3, (rnd.randint(150, 650), rnd.randint(60, 400)), 'rclick')  # rally / gather point
        t += rnd.uniform(25, 45)


def monkey(s, rnd, t0, t1, out):
    """Random input over the game screen and the HUD."""
    t = t0
    while t < t1:
        r = rnd.random()
        if r < 0.30:
            s.click(t, (rnd.randint(*MAP_VIEW[0::2]), rnd.randint(*MAP_VIEW[1::2])))
        elif r < 0.45:
            s.click(t, (rnd.randint(*MAP_VIEW[0::2]), rnd.randint(*MAP_VIEW[1::2])), 'rclick')
        elif r < 0.62:
            s.click(t, rnd.choice(COMMAND_GRID))
        elif r < 0.66:
            cx, cy, w = MINIMAP
            s.click(t, (cx + rnd.randint(-w // 2, w // 2), cy + rnd.randint(-w // 4, w // 4)),
                    rnd.choice(['click', 'rclick']))
        elif r < 0.68:
            s.click(t, rnd.choice(MINIMAP_BUTTONS + TOP_ICONS))
        elif r < 0.71:
            # A HUD panel (objectives, chat, diplomacy, ...): a click or two inside it, then Escape.
            s.click(t, HUD_RIGHT[rnd.choice(['objectives', 'chat', 'diplomacy', 'last'])])
            for k in range(rnd.randint(0, 3)):
                s.click(t + 1.5 + k, (rnd.randint(150, 650), rnd.randint(120, 450)))
            s.at(t + 5, 'esc')
            t += 5
        elif r < 0.72:
            # The game menu: Options or About, then out again; never Quit, Restart or Load.
            s.click(t, HUD_RIGHT['menu'])
            s.click(t + 1.5, GAME_MENU[rnd.choice(['options', 'about', 'cancel'])])
            s.at(t + 5, 'esc')
            s.at(t + 6, 'esc')
            t += 6
        elif r < 0.95:
            s.at(t, rnd.choice(MONKEY_KEYS))
        else:
            s.at(t, 'h')                       # home: keeps the view where there is something to click
        t += rnd.uniform(0.4, 2.0)
    for ts in range(int(t0) + 30, int(t1), 300):
        s.at(ts, 'shot=%s' % os.path.join(out, 'shot_%04d.bmp' % ts))


def ensure_game(template, dst):
    """A game folder of its own: hard links to the template (as worktree_env.ps1), copies of
    what the game writes."""
    if os.path.isdir(dst):
        return
    for dirpath, _, files in os.walk(template):
        rel = os.path.relpath(dirpath, template)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        top = rel.split(os.sep)[0]
        for f in files:
            src, d = os.path.join(dirpath, f), os.path.join(dst, rel, f)
            if top in ('SaveGame', 'Scenario') or (rel == '.' and f.lower().startswith('player.nf')):
                shutil.copy2(src, d)
            else:
                os.link(src, d)


def analyse(log, keys_text):
    text = open(log, encoding='utf-8', errors='replace').read()
    lines = text.splitlines()
    keyed = [(i, l) for i, l in enumerate(lines) if l.startswith('[keys]')]
    events = []
    for i, l in enumerate(lines):
        if EVENTS.search(l):
            before = [k for j, k in keyed if j < i][-8:]
            events.append({'line': l.strip(), 'context': [x.rstrip() for x in lines[i:i + 30]],
                           'keys_before': before})
    m = re.search(r'\[watchdog\] (\d+) s', text)
    last_key = keyed[-1][1] if keyed else ''
    modes = re.findall(r'SetDisplayMode\((\d+), (\d+)', text)
    frames = re.findall(r'(\d+) frames', text)
    # --firsthit (a SWGB_TRACE build): the first entry to each lifted function, timed by the
    # [keys] step before it (stdout and stderr share the log, in order).
    first, now = [], 0.0
    for l in lines:
        if l.startswith('[keys] '):
            mm = re.match(r'\[keys\] ([0-9.]+)s', l)
            if mm:
                now = float(mm.group(1))
        elif l.startswith('[first] '):
            first.append((now, l.split()[-1]))
    fh = None
    if first:
        by5 = {}
        for t, _ in first:
            by5[int(t // 300) * 5] = by5.get(int(t // 300) * 5, 0) + 1
        fh = {'functions': len(first), 'new_per_5_minutes': {'%d-%d' % (k, k + 5): v for k, v in sorted(by5.items())},
              'timeline': [['%.1f' % t, va] for t, va in first]}
    return {'events': events, 'watchdog': bool(m), 'last_key': last_key,
            'modes': ['%sx%s' % mm for mm in modes], 'frames': int(frames[-1]) if frames else None,
            'firsthit': fh}


def run_one(args, i, mode, rnd_seed, out_root, game):
    rnd = random.Random(rnd_seed)
    name = '%02d-%s-%d' % (i, mode, rnd_seed)
    out = os.path.join(out_root, name)
    os.makedirs(out, exist_ok=True)
    s = Script()
    if mode == 'campaign':
        t, what = setup_campaign(s, rnd, 20)
        start = t + 100                  # the briefing (~35 s), then the mission loads
        # Its Objectives dialog: Escape closes it. Not its OK button: once the dialog is gone,
        # that spot is the minimap, and the view jumps into unexplored black.
        for k in range(4):
            s.at(start - 30 + 10 * k, 'esc')
    else:
        t, what = setup_skirmish(s, rnd, 20)
        start = t + 80                   # the map loads
    end = args.minutes * 60 + start
    s.at(start, 'shot=%s' % os.path.join(out, 'shot_start.bmp'))
    t = start + 2
    if args.speed == 'fast':
        t = fast_speed(s, t)
        what['speed'] = 'fast'
    save_at = start + (end - start) * 0.4 if args.save else None
    if save_at:
        what['save'] = 'soak%d' % rnd_seed
    if mode == 'monkey' or (mode == 'campaign' and not args.idle_campaign):
        if save_at:
            monkey(s, rnd, t, save_at - 3, out)
            save_game(s, save_at, what['save'])
            monkey(s, rnd, save_at + 22, end - 5, out)
        else:
            monkey(s, rnd, t, end - 5, out)
    else:
        if save_at:
            habits(s, rnd, t, save_at - 3, None, out)
            save_game(s, save_at, what['save'])
            habits(s, rnd, save_at + 22, end - 5, None, out)
        else:
            habits(s, rnd, t, end - 5, None, out)
        for ts in range(int(start) + 30, int(end), 300):
            s.at(ts, 'shot=%s' % os.path.join(out, 'shot_%04d.bmp' % ts))
    s.at(end - 2, 'shot=%s' % os.path.join(out, 'shot_end.bmp'))
    keys = os.path.join(out, 'keys.txt')
    open(keys, 'w', newline='\n').write(s.text())
    log = os.path.join(out, 'run.log')
    cmd = [args.run, '--run', '--multi', '--headless', '--exe', args.exe, '--game', game,
           '--watchdog', str(int(end + 5)), '--keys', '@' + keys] + args.extra.split()
    meta = {'name': name, 'mode': mode, 'seed': rnd_seed, 'setup': what, 'minutes': args.minutes,
            'game': game, 'cmd': cmd, 'started': datetime.datetime.now().isoformat(timespec='seconds')}
    t0 = time.time()
    with open(log, 'w', encoding='utf-8', errors='replace') as f:
        p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=out)
        meta['pid'] = p.pid
        rc = p.wait()
    meta['seconds'] = round(time.time() - t0)
    meta['exit'] = rc
    meta['outcome'] = EXIT.get(rc, 'exit %d' % rc)
    meta.update(analyse(log, s.text()))
    for f in sorted(os.listdir(out)):
        if f.endswith('.bmp') and shutil.which('ffmpeg'):
            png = os.path.join(out, f[:-4] + '.png')
            if subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', os.path.join(out, f), png]).returncode == 0:
                os.remove(os.path.join(out, f))
    json.dump(meta, open(os.path.join(out, 'summary.json'), 'w'), indent=1)
    return meta


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mode', default='skirmish', choices=['skirmish', 'monkey', 'campaign', 'mixed'])
    ap.add_argument('--runs', type=int, default=1)
    ap.add_argument('--minutes', type=float, default=25)
    ap.add_argument('--jobs', type=int, default=1, help='runs at once (each with its own game folder)')
    ap.add_argument('--seed', type=int, default=None)
    ap.add_argument('--idle-campaign', action='store_true', help='the idle habits in a campaign, not the monkey')
    ap.add_argument('--speed', default='fast', choices=['fast', 'normal'],
                    help='the game speed (the game menu: Options); fast plays more game per hour')
    ap.add_argument('--no-save', dest='save', action='store_false',
                    help='skip the save at 40%% of the run (the game menu: Save)')
    ap.add_argument('--run', default=r'A:\Games\star\builds\coverage\swgbrecomp.exe')
    ap.add_argument('--exe', default=r'A:\Games\star\swgb-recomp\work\Battlegrounds.exe')
    ap.add_argument('--game', default=r'A:\Games\star\games\coverage',
                    help='the game folder; job k > 0 uses a copy, DIR-soakK (hard links)')
    ap.add_argument('--extra', default='',
                    help='more host options, e.g. "--firsthit 0x401000 0x651000" with a SWGB_TRACE build')
    ap.add_argument('--out', default=os.path.join(ROOT, 'work', 'soak',
                                                  datetime.datetime.now().strftime('%Y%m%d-%H%M%S')))
    args = ap.parse_args()
    sys.stdout.reconfigure(line_buffering=True)    # a run's line as it ends, into a redirected log too
    args.out = os.path.abspath(args.out)        # the game runs in its own folder
    seed = args.seed if args.seed is not None else random.randrange(1 << 20)
    rnd = random.Random(seed)
    os.makedirs(args.out, exist_ok=True)
    games = [args.game] + ['%s-soak%d' % (args.game.rstrip('\\/'), k) for k in range(1, args.jobs)]
    for g in games[1:]:
        ensure_game(args.game, g)
    modes = ['skirmish', 'monkey', 'campaign']
    plan = [(i, args.mode if args.mode != 'mixed' else modes[i % 3], rnd.randrange(1 << 20))
            for i in range(args.runs)]
    print('soak: %d run(s), %s, %g min each, %d at once, seed %d -> %s'
          % (args.runs, args.mode, args.minutes, args.jobs, seed, args.out))
    results, lock = [], threading.Lock()
    free = list(games)

    def worker(item):
        with lock:
            g = free.pop()
        try:
            m = run_one(args, item[0], item[1], item[2], args.out, g)
        finally:
            with lock:
                free.append(g)
        with lock:
            results.append(m)
            print('  %-28s %-20s %5ds  %d event(s)  %s' % (m['name'], m['outcome'], m['seconds'],
                                                          len(m['events']), json.dumps(m['setup'])))
            for e in m['events'][:6]:
                print('      ' + e['line'])
    threads = []
    pending = list(plan)
    while pending or threads:
        threads = [t for t in threads if t.is_alive()]
        while pending and len(threads) < args.jobs:
            th = threading.Thread(target=worker, args=(pending.pop(0),))
            th.start()
            threads.append(th)
        time.sleep(1)
    json.dump(sorted(results, key=lambda m: m['name']), open(os.path.join(args.out, 'summary.json'), 'w'), indent=1)
    bad = [m for m in results if m['exit'] not in (0, 4) or m['events']]
    hours = sum(m['seconds'] for m in results) / 3600
    print('%d run(s), %.2f game-hours, %d with a problem; %s' % (len(results), hours, len(bad), args.out))
    for m in results:
        if m['modes'] and m['modes'][0] != '800x600':
            print('  warning: %s ran at %s: the HUD coordinates are for 800x600' % (m['name'], m['modes'][0]))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
