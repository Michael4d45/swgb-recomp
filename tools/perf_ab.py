#!/usr/bin/env python3
"""Two builds of the host, the same scripted headless games, in turn: CPU and frames, median and spread.

    py -3 tools\\perf_ab.py --build O1=DIR --build O2=DIR [--scenario NAME,...] [--runs N] [--pairs]
                            [--cc] [--game DIR] [--save FILE] [--profile] [--out DIR]
    py -3 tools\\perf_ab.py --build O1=DIR --make-save MINUTES --out DIR

Made for docs/performance.md's question "is a build of the lifted C at other flags faster?"
(-DGEN_OPT=/O2 against /O1). Every run is `--headless --multi --fps-log --present-log`, with
SWGB_PAINT_HEADLESS=1 so the presenter's scaling and copy are in the figures, each build in a game
folder of its own (hard links to --game, as soak.py makes them). The builds take turns: in round
k the first build runs first when k is even, last when odd; --pairs runs a round's builds at once
instead (they then share whatever else the machine is doing).

Scenarios (seconds of the host's own clock; the figures are --fps-log's, from FROM to TO):

  menus        the main menu, untouched, 20-70 s
  sk800        test.py's frame-rate skirmish (one Easiest computer, Tiny Savannah), at the default
               pacing (the refresh), the view moved now and then: 3 minutes of game
  sk800-off    the same, --fps off: frames drawn as fast as the game draws them
  sk1440       sk800 at --res 2560x1440 --fullscreen
  sk1440-off   sk1440, --fps off
  late         --save (a late game: --make-save) loaded from Single Player -> Saved Game, then
               3 minutes of an idle player at the default pacing
  late-off     the same, --fps off

Figures per run (medians of the window's seconds): CPU (% of one core, the process), frames drawn
a second ("presents" in --fps-log), frames shown a second (--present-log), changed frames a second,
the simulation's steps a second, and CPU ms a drawn frame (CPU / drawn: in the `-off` scenarios,
which draw as fast as they can, the cost of a frame). Per build and scenario: the median of the
runs and their range. --profile adds one
`--profile` run of each build and scenario and tools/profile_report.py's module table.

--make-save M: a Standard Game against 7 computers (me and 3 allies against 4, Moderate, a Normal
Savannah map, Fast), M minutes of an idle player, then saved as `late` (copied to --out); its
log's events are reported as soak.py would.
"""
import argparse
import datetime
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import soak  # noqa: E402
from swgb import runner, screens  # noqa: E402

FPS = re.compile(r'\[fps\]\s+([0-9.]+)s mode (\d+) cpu\s+([0-9.]+)% presents ([0-9.]+)/s changed ([0-9.]+)/s'
                 r'(?:.*? steps ([0-9.]+)/s)?')
SAVE = re.compile(r'.*\.ga[x0-9]$', re.I)     # saved games: .ga1 (the base game), .ga2, ...
SHOWN = re.compile(r'\[present\] ([0-9.]+) s: (\d+) presents, (\d+) shown')
GAME_MINUTES = 3


def skirmish(kind, out, size=800):
    """test.py's perf skirmish: (steps, the game's start, the end). A game folder remembers its
    Screen Size (its private HKCU), so every scenario in a game sets its own (main menu, Options)."""
    s = runner.Script()
    t = screens.to_setup(s, screens.layout(kind), 12, size)
    t = screens.pick(s, t, screens.DD_OTHERS, 0)          # one computer
    t = screens.pick(s, t, screens.DD_LOCATION, 13)       # Savannah
    t = screens.pick(s, t, screens.DD_SIZE, 0)            # Tiny
    t = screens.pick(s, t, screens.DD_DIFFICULTY, 0)      # Easiest
    s.click(t + 2, screens.SETUP_START)
    start = t + 72
    end = start + GAME_MINUTES * 60
    s.at(start, 'shot=%s' % os.path.join(out, 'start.bmp'))
    for ts in range(int(start) + 10, int(end) - 5, 30):
        s.at(ts, 'h')
        s.at(ts + 15, 'move:%d:300' % (200 + (ts // 30 % 3) * 200))
    return s, start + 20, end - 5


def loaded(kind, out, size=800):
    s = runner.Script()
    lay = screens.layout(kind)
    t = screens.saved_game(s, lay, screens.set_screen(s, 12, size), 0)   # row 0: the bench folder's one save
    start = t + 72
    s.at(start, 'shot=%s' % os.path.join(out, 'start.bmp'))
    end = start + GAME_MINUTES * 60
    t = start + 5
    while t < end - 10:                              # the idle player: home, a worker, now and then
        s.at(t, 'h')
        s.click(t + 1, lay.command_grid[0])
        t += 30
    return s, start + 20, end - 5


def menus(kind, out, size=None):
    s = runner.Script()
    s.at(65, 'shot=%s' % os.path.join(out, 'start.bmp'))
    return s, 20, 70


WIDE = ['--res', '2560x1440', '--fullscreen']
SCENARIOS = {   # name: (script, host switches, Screen Size: 800, or 1280 for the third, --res)
    'menus': (menus, [], None),
    'sk800': (skirmish, [], 800),
    'sk800-off': (skirmish, ['--fps', 'off'], 800),
    'sk1440': (skirmish, WIDE, 1280),
    'sk1440-off': (skirmish, WIDE + ['--fps', 'off'], 1280),
    'late': (loaded, [], 800),
    'late-off': (loaded, ['--fps', 'off'], 800),
    'late1440': (loaded, WIDE, 1280),
}


def figures(text, t0, t1):
    rows = [m for m in FPS.findall(text) if t0 <= float(m[0]) <= t1]
    if not rows:
        return None
    med = lambda xs: round(statistics.median(xs), 2) if xs else None   # noqa: E731
    f = {'seconds': len(rows),
         'cpu': med([float(r[2]) for r in rows]),
         'drawn': med([float(r[3]) for r in rows]),
         'changed': med([float(r[4]) for r in rows]),
         'modes': sorted({int(r[1]) for r in rows})}
    steps = [float(r[5]) for r in rows if r[5]]
    if steps:
        f['steps'] = med(steps)
    # CPU ms a drawn frame, over the window (not a median of seconds)
    drawn = sum(float(r[3]) for r in rows)
    if drawn:
        f['ms_frame'] = round(sum(float(r[2]) for r in rows) * 10 / drawn, 3)
    shown = [(float(a), int(c)) for a, _, c in SHOWN.findall(text)]
    # --present-log's lines carry the seconds of their own window, not the host's clock: take the
    # same share of them as the window is of the run.
    if shown:
        f['shown'] = med([c / a for a, c in shown[len(shown) * 1 // 4:] if a > 0])
    return f


def run_one(build, exe, kind, game, scenario, out, profile=False):
    os.makedirs(out, exist_ok=True)
    plan, extra, size = SCENARIOS[scenario]
    s, t0, t1 = plan(kind, out, size)
    keys = s.write(os.path.join(out, 'keys.txt'))
    wd = int(t1 + 10)
    cmd = runner.host_cmd(os.path.join(build, runner.host_name(kind)), exe, game, wd, keys,
                          '--fps-log', '--present-log', *extra)
    if profile:
        cmd += ['--profile', os.path.join(out, 'profile.txt'), '--profile-window', '%d:%d' % (t0, t1)]
    env = dict(os.environ, SWGB_PAINT_HEADLESS='1')
    log = os.path.join(out, 'run.log')
    p = runner.popen(cmd, cwd=out, env=env, stdout=open(log, 'w'), stderr=subprocess.STDOUT)
    return p, log, (t0, t1), wd


def finish(p, log, window, wd):
    try:
        rc = p.wait(timeout=wd + 120)
    except subprocess.TimeoutExpired:
        runner.kill_tree(p)                          # only the PID this tool started
        rc = 'killed'
    text = open(log, encoding='utf-8', errors='replace').read()
    f = figures(text, *window) or {}
    modes = runner.modes(text)
    f.update(exit=rc, events=len(runner.events(text)), mode='%dx%d' % modes[-1] if modes else None)
    runner.to_png(os.path.dirname(log))
    return f


def summarize(results):
    """{(build, scenario): [figures]} -> {scenario: {build: {key: [median, lo, hi, n]}}}"""
    table = {}
    for (b, sc), runs in results.items():
        row = {}
        good = [r for r in runs if r.get('exit') == 4 and not r.get('events')]
        for k in ('cpu', 'drawn', 'shown', 'changed', 'steps', 'ms_frame'):
            xs = [r[k] for r in good if r.get(k) is not None]
            if xs:
                row[k] = [round(statistics.median(xs), 2), min(xs), max(xs), len(xs)]
        row['runs'] = len(runs)
        row['bad'] = len(runs) - len(good)
        table.setdefault(sc, {})[b] = row
    # Each later build against the first, round by round (the runs of a round share the machine's
    # load): the median and range of the ratio of CPU ms a frame.
    for sc, by in table.items():
        labels = [b for b, s_ in results if s_ == sc]
        first = labels[0]
        for b in labels[1:]:
            ra = [(x, y) for x, y in zip(results[(first, sc)], results[(b, sc)])
                  if x.get('ms_frame') and y.get('ms_frame') and x.get('exit') == y.get('exit') == 4]
            rs = [y['ms_frame'] / x['ms_frame'] for x, y in ra]
            if rs:
                by[b]['ratio'] = [round(statistics.median(rs), 3), round(min(rs), 3), round(max(rs), 3), len(rs)]
    return table


def report(table, builds):
    lines = []
    for sc, by in table.items():
        lines.append('## %s' % sc)
        lines.append('| build | CPU %% | drawn/s | shown/s | changed/s | steps/s | CPU ms/frame | ms/frame vs %s | runs (bad) |'
                     % builds[0])
        lines.append('|---|---|---|---|---|---|---|---|---|')
        for b in builds:
            r = by.get(b)
            if not r:
                continue
            cell = lambda k: ('%g (%g-%g)' % tuple(r[k][:3])) if k in r else '-'   # noqa: E731
            lines.append('| %s | %s | %s | %s | %s | %s | %s | %s | %d (%d) |' % (
                b, cell('cpu'), cell('drawn'), cell('shown'), cell('changed'), cell('steps'), cell('ms_frame'),
                cell('ratio'), r['runs'], r['bad']))
        lines.append('')
    return '\n'.join(lines)


def make_save(build, exe, kind, game, minutes, out):
    """7 computers, 4 against 4, played idle for `minutes`, saved as `late`."""
    os.makedirs(out, exist_ok=True)
    s = runner.Script()
    lay = screens.layout(kind)
    t, what = soak.setup_fixed(s, lay, 20, out, others=7, size=3, diff=2, teams=[1, 1, 1, 1, 2, 2, 2, 2], loc=13)
    start = t + 70
    t = screens.fast_speed(s, lay, start)
    import random
    rnd = random.Random(7)
    end = start + minutes * 60
    soak.habits(s, lay, rnd, t + 2, end - 25)
    soak.shots(s, start, end, out)
    screens.save_game(s, lay, end - 20, 'late')
    keys = s.write(os.path.join(out, 'keys.txt'))
    log = os.path.join(out, 'run.log')
    cmd = runner.host_cmd(os.path.join(build, runner.host_name(kind)), exe, game, end + 15, keys, '--fps-log', '--files')
    p = runner.popen(cmd, cwd=out, stdout=open(log, 'w'), stderr=subprocess.STDOUT)
    print('make-save: pid %d, %d s' % (p.pid, end + 15), flush=True)
    f = finish(p, log, (start + 60, end - 30), int(end + 15))
    saved = [n for n in os.listdir(os.path.join(game, 'SaveGame')) if SAVE.match(n) and n.lower().startswith('late.')]
    if saved:
        shutil.copy2(os.path.join(game, 'SaveGame', saved[0]), os.path.join(out, saved[0]))
    f.update(what, save=os.path.join(out, saved[0]) if saved else None)
    print(json.dumps(f, indent=1))
    return 0 if f.get('save') and f['exit'] == 4 and not f['events'] else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', action='append', required=True, help='LABEL=DIR (the folder with the host)')
    ap.add_argument('--scenario', default='menus,sk800,sk800-off,sk1440,sk1440-off')
    ap.add_argument('--runs', type=int, default=3)
    ap.add_argument('--pairs', action='store_true', help="run a round's builds at once")
    ap.add_argument('--cc', action='store_true', help="Clone Campaigns' host and exe")
    ap.add_argument('--exe', default=None)
    ap.add_argument('--game', default=None, help='the template game folder (default: game/ of the main checkout)')
    ap.add_argument('--save', default=None, help='late: the save to load (--make-save writes one)')
    ap.add_argument('--profile', action='store_true', help='one --profile run per build and scenario as well')
    ap.add_argument('--make-save', type=float, default=None, metavar='MINUTES')
    ap.add_argument('--prefix', default='perfab', help='game folders: GAMES/PREFIX-KIND-LABEL (runner.paths())')
    ap.add_argument('--out', default=None)
    a = ap.parse_args()
    kind = 'cc' if a.cc else 'base'
    P = runner.paths()
    exe = a.exe or P.exe(kind)
    a.game = a.game or P.template
    builds = [b.split('=', 1) for b in a.build]
    out = os.path.abspath(a.out or os.path.join(os.getcwd(), 'perfab-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S')))
    games = {}
    for label, _ in builds:
        g = os.path.join(P.games, '%s-%s-%s' % (a.prefix, kind, label))
        runner.ensure_game(a.game, g, P.cc_profiles if kind == 'cc' else None)
        games[label] = g
    if a.make_save is not None:
        label, build = builds[0]
        return make_save(build, exe, kind, games[label], a.make_save, out)
    scenarios = [s for s in a.scenario.split(',') if s]
    for sc in scenarios:
        if sc not in SCENARIOS:
            ap.error('no scenario %s (%s)' % (sc, ', '.join(SCENARIOS)))
    if any(sc.startswith('late') for sc in scenarios):
        if not a.save or not os.path.exists(a.save):
            ap.error('late needs --save FILE')
    results = {}

    def place_save(sc):
        for g in games.values():
            d = os.path.join(g, 'SaveGame')
            for n in os.listdir(d):
                if SAVE.match(n):
                    os.remove(os.path.join(d, n))
            if sc.startswith('late'):
                shutil.copy2(a.save, os.path.join(d, os.path.basename(a.save)))

    plan = [(sc, k, False) for k in range(a.runs) for sc in scenarios]
    if a.profile:
        plan += [(sc, 'p', True) for sc in scenarios]
    for sc, k, prof in plan:
        place_save(sc)
        order = builds if (k == 'p' or k % 2 == 0) else builds[::-1]
        started = []
        for label, build in order:
            d = os.path.join(out, sc, '%s-%s' % (label, k))
            started.append((label, d, run_one(build, exe, kind, games[label], sc, d, prof)))
            print('%s %s run %s: pid %d' % (sc, label, k, started[-1][2][0].pid), flush=True)
            if not a.pairs:
                label_, d_, args_ = started.pop()
                f = finish(*args_)
                if not prof:
                    results.setdefault((label_, sc), []).append(f)
                json.dump(f, open(os.path.join(d_, 'figures.json'), 'w'), indent=1)
                print('  ', json.dumps(f), flush=True)
        for label_, d_, args_ in started:
            f = finish(*args_)
            if not prof:
                results.setdefault((label_, sc), []).append(f)
            json.dump(f, open(os.path.join(d_, 'figures.json'), 'w'), indent=1)
            print('  ', label_, json.dumps(f), flush=True)
        if prof:
            for label, _ in builds:
                d = os.path.join(out, sc, '%s-p' % label)
                rep = subprocess.run([sys.executable, os.path.join(HERE, 'profile_report.py'),
                                      os.path.join(d, 'profile.txt'), '--top', '30'],
                                     capture_output=True, text=True)
                open(os.path.join(d, 'profile_report.txt'), 'w').write(rep.stdout + rep.stderr)
        table = summarize(results)
        json.dump({'builds': dict(builds), 'table': table,
                   'runs': {'%s/%s' % k: v for k, v in results.items()}}, open(os.path.join(out, 'summary.json'), 'w'), indent=1)
        open(os.path.join(out, 'report.md'), 'w').write(report(table, [b for b, _ in builds]))
    print(report(summarize(results), [b for b, _ in builds]))
    print(out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
