#!/usr/bin/env python3
"""One test runner for the recomp, in levels (docs/testing.md, "Test levels").

    py -3 tools/test.py --level unit|smoke|functional|soak|real
                        [--build DIR] [--cc | --no-cc] [--jobs N] [--only PATTERN,...]
                        [--results DIR] [--baselines DIR] [--update-baselines]
                        [--soak-scale F] [--list] [--real-screen]

  unit        L0  seconds   no game files, no lift: the host's C units and the tools' own
                            tests. Runs on GitHub Actions (.github/workflows/unit.yml).
  smoke       L1  ~2 min    + conformance (base and Clone Campaigns), a menu and a
                            skirmish shot each, no ICALL, no fault; the lift's function
                            count, errors and byte-identical C against its baseline
  functional  L2  ~20 min   + present_check's five groups, the game's three resolutions,
                            --res 1920x1080, the films, --record with its sound (a film,
                            a skirmish, a victory), a short skirmish per set of
                            civilizations with a save and a load, the frame-rate figures
                            (CPU and presents a second) of a fixed skirmish against
                            thresholds, menu shots against stored baselines
  soak        L3  hours     + tools/soak.py's modes for both games: skirmish, monkey,
                            campaigns, defeat, allied, victory then load, chain, fight
  real        L4  ~5 min    the real screen: tools/present_check_real.ps1. It takes over
                            the mouse and keyboard; it runs only with --real-screen.

A level runs the levels below it too; --just runs only its own checks. Every
game is --headless --multi, in a game folder of its own under GAMES/testing-*
(hard links to the shared install, as worktree_env.ps1 makes them), at most
--jobs at once. Only the processes this tool starts are ever stopped, by PID.

Out of it: RESULTS\\<time>-<level>\\summary.json, report.md and report.html
(the screenshots beside them), one folder per check with its logs. Exit 0
when nothing failed, 1 otherwise. A SKIP (no Clone Campaigns host, no lift,
no game data) does not fail a run; the report lists it.

Baselines (screenshots, the lift's hashes, the frame-rate figures) are kept
outside the repository (--baselines; the defaults of every path come from
tools/swgb/runner.py's paths()): they are made from the game's files. A missing baseline is recorded by the first run;
--update-baselines records them all again (after an intended change).
"""
import argparse
import concurrent.futures
import datetime
import fnmatch
import hashlib
import html
import json
import os
import queue
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from swgb import runner, screens  # noqa: E402
from swgb.runner import EVENTS, EXIT, WIN, Script, ensure_game, kill_tree, png_of  # noqa: E402
from swgb.screens import MAIN_SINGLE, SETUP_START  # noqa: E402

PATHS = runner.paths()          # every default path (SWGB_STAR, tools/local.json, else this checkout)
LEVELS = ['unit', 'smoke', 'functional', 'soak', 'real']
ALIASES = {'l0': 'unit', 'l1': 'smoke', 'l2': 'functional', 'l3': 'soak', 'l4': 'real',
           'nightly': 'soak', 'merge': 'functional', 'gate': 'functional'}

# ---- the two games ---------------------------------------------------------------------------
# Their screens are tools/swgb/screens.py's (LAYOUT: at 800x600).
GAMES = {
    'base': {'title': 'Galactic Battlegrounds', 'host': runner.host_name('base'), 'exe': runner.GUEST_EXES['base'],
             'entry': 0x00632881, 'genie': 'genie', 'gen': 'gen', 'stats': 'lift_stats.json'},
    'cc': {'title': 'Clone Campaigns', 'host': runner.host_name('cc'), 'exe': runner.GUEST_EXES['cc'],
           'entry': 0x00636601, 'genie': 'genie_x1', 'gen': 'gen_cc', 'stats': 'lift_stats_cc.json'},
}
LAYOUT = {k: screens.layout(k) for k in GAMES}


# ---- results ---------------------------------------------------------------------------------
class Result:
    def __init__(self, check, root, attempt=0):
        self.check = check
        self.name = check.name
        # each try in a folder of its own: <check> first, then <check>-retry1, -retry2 (--retry),
        # so a retry never overwrites the first try's logs and shots, nor passes on its leftovers
        self.attempt = attempt
        self.dir = os.path.join(root, re.sub(r'[^A-Za-z0-9_.-]+', '_', check.name) + ('-retry%d' % attempt if attempt else ''))
        self.status = 'PASS'
        self.lines = []
        self.metrics = {}
        self.images = []                     # (path relative to the results folder, caption)
        self.own_lines, self.own_images = self.lines, self.images   # this try's alone (merge_tries)
        self.tries = []                      # the earlier tries of this check, first first
        self.seconds = 0
        self.game = None                     # the game folder, while it holds one
        self.root = root

    def log(self, msg):
        self.lines.append(msg)

    def fail(self, msg):
        self.status = 'FAIL' if self.status != 'ERROR' else self.status
        self.lines.append('FAIL: ' + msg)

    def skip(self, msg):
        self.status = 'SKIP'
        self.lines.append('SKIP: ' + msg)

    def image(self, path, caption):
        if path and os.path.exists(path):
            self.images.append((os.path.relpath(path, self.root).replace('\\', '/'), caption))

    def as_dict(self):
        return {'name': self.name, 'level': self.check.level, 'game': self.check.game, 'status': self.status,
                'seconds': round(self.seconds, 1), 'lines': self.lines, 'metrics': self.metrics,
                'images': [{'path': p, 'caption': c} for p, c in self.images], 'dir': os.path.relpath(self.dir, self.root),
                'tries': [{'try': t.attempt + 1, 'status': t.status, 'dir': os.path.relpath(t.dir, self.root)}
                          for t in self.tries + [self]]}


def merge_tries(old, new):
    """new is a retry of old: its report lines and shots become every try's, each try headed
    by its status and its folder (old's folder is untouched, so both sets of logs stay)."""
    new.tries = old.tries + [old]
    lines, images = [], []
    for t in new.tries + [new]:
        lines.append('try %d: %s, in %s/' % (t.attempt + 1, t.status, os.path.basename(t.dir)))
        lines += ['  ' + l for l in t.own_lines]
        images += [(p, 'try %d: %s' % (t.attempt + 1, c)) for p, c in t.own_images]
    new.lines, new.images = lines, images
    if new.status == 'PASS':
        new.status = 'FLAKY'
    return new


def retry_failed(ctx, results, times, run):
    """--retry: a game check (not a soak) that failed is run again, up to `times` times, by
    run(check, attempt) -> Result. Passing then, it is FLAKY, which does not fail the run but
    is listed. Each retry replaces its check's result in `results`, keeping the earlier tries'
    lines and shots (merge_tries)."""
    for k in range(1, times + 1):
        again = [r for r in results if r.status == 'FAIL' and r.check.game and r.check.level != 'soak']
        if not again or ctx.cancelled:
            break
        print('retry %d: %d failed check(s): %s' % (k, len(again), ', '.join(r.name for r in again)))
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, ctx.a.jobs)) as ex:
            retried = list(ex.map(lambda r: run(r.check, k), again))
        for old, new in zip(again, retried):
            results[results.index(old)] = merge_tries(old, new)
    return results


class Check:
    def __init__(self, name, level, fn, game=None, minutes=1.0, needs=(), doc=''):
        self.name, self.level, self.fn, self.game, self.minutes, self.needs = name, level, fn, game, minutes, needs
        self.doc = doc or (fn.__doc__ or '').strip().split('\n')[0]


CHECKS = []


def check(name, level, game=None, minutes=1.0, needs=()):
    def deco(fn):
        CHECKS.append(Check(name, level, fn, game, minutes, needs))
        return fn
    return deco


# ---- the run's context: paths, game folders, the processes started -----------------------------
class Ctx:
    def __init__(self, a):
        self.a = a
        self.results = a.results
        self.baselines = a.baselines
        self.procs = set()
        self.lock = threading.Lock()
        self.pools = {}
        self.cancelled = False

    def host(self, kind):
        return os.path.join(self.a.build_cc if kind == 'cc' and self.a.build_cc else self.a.build, GAMES[kind]['host'])

    def exe(self, kind):
        return self.a.exe if kind == 'base' else self.a.exe_cc

    def has(self, kind):
        return os.path.exists(self.host(kind)) and os.path.exists(self.exe(kind)) and os.path.isdir(self.a.game_template)

    # Game folders: GAMES\testing-<kind>-<k>, made once, kept between runs (their profiles and
    # saves are real copies; everything else is a hard link to the shared install).
    def pool(self, kind):
        with self.lock:
            if kind not in self.pools:
                q = queue.Queue()
                for k in range(self.a.jobs):
                    q.put(os.path.join(self.a.games, '%s-%s-%d' % (self.a.prefix, kind, k)))
                self.pools[kind] = q
            return self.pools[kind]

    def acquire(self, kind):
        folder = self.pool(kind).get()
        ensure_game(self.a.game_template, folder, self.a.cc_profiles if kind == 'cc' else None)
        return folder

    def release(self, kind, folder):
        self.pool(kind).put(folder)

    def popen(self, cmd, **kw):
        p = runner.popen(cmd, **kw)
        with self.lock:
            self.procs.add(p)
        return p

    def done(self, p):
        with self.lock:
            self.procs.discard(p)

    def stop_all(self):
        """Ctrl-C: end every process tree this tool started, by PID. Nothing else."""
        self.cancelled = True
        with self.lock:
            procs = list(self.procs)
        for p in procs:
            kill_tree(p)


def run(ctx, r, cmd, timeout, log_name, cwd=None, env=None):
    """A process of this tool's (the host or another tool), its output in r.dir/log_name.
    Returns (exit code, the output). Past the timeout its tree is ended (-1)."""
    os.makedirs(r.dir, exist_ok=True)
    log = os.path.join(r.dir, log_name)
    t0 = time.time()
    with open(log, 'w', encoding='utf-8', errors='replace') as f:
        f.write('# ' + (subprocess.list2cmdline(cmd) if WIN else ' '.join(map(str, cmd))) + '\n')
        f.flush()
        p = ctx.popen(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=cwd or r.dir, env=env)
        try:
            rc = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            kill_tree(p)
            p.wait()
            rc = -1
        finally:
            ctx.done(p)
    r.metrics.setdefault('process_seconds', 0)
    r.metrics['process_seconds'] += round(time.time() - t0)
    return rc, open(log, encoding='utf-8', errors='replace').read()


def run_host(ctx, r, kind, steps, watchdog, tag='run', extra=(), env=None):
    """The host, headless and --multi, in the check's game folder, with a --keys script
    (a list of (seconds, step)). Returns (exit code, log text)."""
    os.makedirs(r.dir, exist_ok=True)
    keys = os.path.join(r.dir, tag + '_keys.txt')
    with open(keys, 'w', newline='\n') as f:
        f.write('\n'.join('%g:%s' % s for s in sorted(steps, key=lambda s: s[0])) + '\n')
    cmd = runner.host_cmd(ctx.host(kind), ctx.exe(kind), r.game, watchdog, keys, '--files', *extra)
    rc, text = run(ctx, r, cmd, watchdog + 90, tag + '.log', env=env)
    r.log('%s: exit %d (%s), %s' % (tag, rc, EXIT.get(rc, '?'), os.path.join(os.path.basename(r.dir), tag + '.log')))
    return rc, text


def events(r, text, tag='run'):
    """Every unresolved call, fault, stub: each fails the check."""
    ev = [l.strip() for l in text.splitlines() if EVENTS.search(l)]
    for e in ev[:10]:
        r.fail('%s: %s' % (tag, e))
    r.metrics[tag + '_events'] = len(ev)
    return ev


def expect_exit(r, rc, want=(4,), tag='run'):
    if rc not in want:
        r.fail('%s: exit %d (%s), wanted %s' % (tag, rc, EXIT.get(rc, '?'), ' or '.join('%d (%s)' % (w, EXIT[w]) for w in want)))


# ---- screenshots -------------------------------------------------------------------------------
def blank(png, frac=0.9):
    """True when nearly the whole picture is one colour (a black screen)."""
    import numpy as np
    from PIL import Image
    a = np.asarray(Image.open(png).convert('L'))
    vals, counts = np.unique(a, return_counts=True)
    return counts.max() / a.size >= frac


def compare(ctx, r, png, key, tol=0.02, region=None, thresh=24):
    """The picture against its baseline (BASELINES\\<key>.png): the share of pixels that differ
    by more than thresh grey levels must be at most tol. region: (x0, y0, x1, y1), only there.
    A missing baseline is recorded (and passes); --update-baselines records it again."""
    import numpy as np
    from PIL import Image
    if not png:
        r.fail('%s: no shot' % key)
        return None
    base = os.path.join(ctx.baselines, key + '.png')
    if ctx.a.update_baselines or not os.path.exists(base):
        os.makedirs(os.path.dirname(base), exist_ok=True)
        shutil.copyfile(png, base)
        r.log('%s: baseline recorded (%s)' % (key, base))
        return 0.0
    a = Image.open(png).convert('L')
    b = Image.open(base).convert('L')
    if a.size != b.size:
        r.fail('%s: %dx%d, the baseline is %dx%d' % (key, a.size[0], a.size[1], b.size[0], b.size[1]))
        return None
    x = np.asarray(a, dtype=np.int16)
    y = np.asarray(b, dtype=np.int16)
    if region:
        x0, y0, x1, y1 = region
        x, y = x[y0:y1, x0:x1], y[y0:y1, x0:x1]
    d = np.abs(x - y) > thresh
    frac = float(d.mean())
    r.metrics['diff_' + key.replace('/', '_')] = round(frac, 4)
    if frac > tol:
        diff = png[:-4] + '_diff.png'
        img = np.zeros(d.shape + (3,), dtype=np.uint8)
        img[..., 0] = d * 255
        img[..., 1] = (x // 3).clip(0, 255)
        Image.fromarray(img).save(diff)
        r.image(diff, '%s: what differs from the baseline (red)' % key)
        r.fail('%s: %.1f%% of the picture differs from the baseline (at most %.1f%%)' % (key, frac * 100, tol * 100))
    else:
        r.log('%s: %.2f%% differs from the baseline (at most %.1f%%)' % (key, frac * 100, tol * 100))
    return frac


# ---- the --fps-log figures ---------------------------------------------------------------------
FPS = re.compile(r'\[fps\]\s+([0-9.]+)s mode (\d+) cpu\s+([0-9.]+)% presents ([0-9.]+)/s changed ([0-9.]+)/s')


def fps_figures(text, settle=20):
    """--fps-log's seconds in game (mode 4), from settle seconds after the first: the
    median CPU (% of one core) and presents a second."""
    rows = [(float(t), int(m), float(c), float(p), float(ch)) for t, m, c, p, ch in FPS.findall(text)]
    game = [x for x in rows if x[1] == 4]
    if not game:
        return None
    t0 = game[0][0] + settle
    game = [x for x in game if x[0] >= t0] or game
    return {'seconds': len(game), 'cpu_median': round(statistics.median(x[2] for x in game), 1),
            'cpu_p90': round(sorted(x[2] for x in game)[int(len(game) * 0.9) - 1 if len(game) > 1 else 0], 1),
            'presents_median': round(statistics.median(x[3] for x in game), 1),
            'changed_median': round(statistics.median(x[4] for x in game), 1)}


# ==== L0: unit ===================================================================================
def msvc_env(ctx):
    """The environment of an x86 MSVC prompt (vcvarsall amd64_x86), found with vswhere."""
    with ctx.lock:
        if getattr(ctx, '_msvc', None):
            return ctx._msvc
        env = dict(os.environ)
        if os.environ.get('VSCMD_ARG_TGT_ARCH') != 'x86' or not shutil.which('cl'):
            vswhere = os.path.join(os.environ['ProgramFiles(x86)'], 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
            vs = subprocess.run([vswhere, '-latest', '-products', '*', '-property', 'installationPath'],
                                capture_output=True, text=True).stdout.strip()
            if not vs:
                raise RuntimeError('Visual Studio not found (vswhere)')
            bat = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvarsall.bat')
            out = subprocess.run('"%s" amd64_x86 >nul && set' % bat, shell=True, capture_output=True, text=True).stdout
            env = dict(l.split('=', 1) for l in out.splitlines() if '=' in l)
        ctx._msvc = env
        return env


def find_pcrecomp(a):
    rt = os.path.join('runtime', 'native32', 'native32.h') if WIN else os.path.join('runtime', 'win32hle', 'win32hle.h')
    for p in [a.pcrecomp, os.environ.get('PCRECOMP'), os.path.join(os.path.dirname(ROOT), 'pcrecomp'),
              PATHS.star and os.path.join(PATHS.star, 'pcrecomp')]:
        if p and os.path.exists(os.path.join(p, rt)):
            return os.path.abspath(p)
    return None


RT = 'src/runtime/'
FLAT = ['/DRECOMP_FLAT_MEMORY', '/DRECOMP_LOCAL_REGS', '/D_CRT_SECURE_NO_WARNINGS']
# Linked at the guest's own base, not relocatable: pacing_test's .bss spans the engine's
# fixed addresses (the game object pointer), which no allocation of its own could be sure of.
HIGH = ['/BASE:0x00400000', '/DYNAMICBASE:NO', '/FIXED']
GDI = ['user32.lib', 'gdi32.lib', 'd3d11.lib', 'dxgi.lib', 'dxguid.lib']
# name: sources, compiler flags, libraries, arguments, what it checks
UNITS = {
    'scaler': ([ 'tools/present_bench.c', RT + 'present.c', RT + 'scaler.c', RT + 'present_d3d.c'], ['/O2'], GDI, ['--quick'],
               'the scaler: bit-exact against the reference scaler, every size and filter, after a band and a whole frame change'),
    'present': (['tools/present_test.c', RT + 'present.c', RT + 'scaler.c', RT + 'present_d3d.c'], [], GDI, [],
                'the letterbox layout, the mouse through it, and present pacing at 60-600 Hz'),
    'coverage': (['tools/coverage_test.c', RT + 'coverage.c'], FLAT, ['user32.lib'], [],
                 'the unresolved-call stop: exit 5 and the seed line; SWGB_ICALL_CONTINUE; eh32 sees it first'),
    'resolve': (['tests/unit/resolve_test.c', RT + 'resolve.c'], FLAT, ['user32.lib'], [],
                'the indirect-call cache: lookups kept, funclets and __local_unwind2 bypassed'),
    'inflate': (['tests/unit/inflate_test.c', RT + 'inflate.c', 'third_party/miniz/miniz.c'], FLAT, [], [],
                'the deflate decoder: any chunking, two streams, a restart, corrupt data'),
    'pacing': (['tests/unit/pacing_test.c'], FLAT, ['user32.lib', 'winmm.lib'] + HIGH, [],
               '--fps pacing: passes a second, the 16/17 ms gate, the refresh default, no catch-up, the menus'),
    'pacing_cc': (['tests/unit/pacing_test.c'], FLAT + ['/DSWGB_CC'], ['user32.lib', 'winmm.lib'] + HIGH, [],
                  '--fps pacing at Clone Campaigns\' addresses'),
    'smooth': (['tests/unit/smooth_test.c'], FLAT, ['user32.lib', 'winmm.lib'] + HIGH, [],
               '--smooth: drawn between steps, never ahead, stops, teleports, other callers, nothing written'),
    'smooth_cc': (['tests/unit/smooth_test.c'], FLAT + ['/DSWGB_CC'], ['user32.lib', 'winmm.lib'] + HIGH, [],
                  "--smooth at Clone Campaigns' addresses"),
    'films': (['tests/unit/films_test.c', RT + 'movie_core.c'], ['/D_CRT_SECURE_NO_WARNINGS'], ['user32.lib', 'gdi32.lib', 'winmm.lib', 'shlwapi.lib'], [],
              'the films: AVI headers, --films names, the film palette, dithering, the A/V clock, drawing into 8 bits'),
    'keys': (['tests/unit/keys_test.c', RT + 'keyscript.c'], ['/D_CRT_SECURE_NO_WARNINGS'], ['user32.lib'], [],
             'the --keys parser: every step kind, @FILE, drags, modifiers, live steps; window places, on:NAME waits'),
    'profile': (['tests/unit/profile_test.c', RT + 'profile_core.c'], FLAT, [], [],
                '--profile\'s shared core: return addresses, the stack scan, counting samples, the file, its options'),
    'cdaudio': (['tests/unit/cdaudio_test.c'], ['/D_CRT_SECURE_NO_WARNINGS'], ['winmm.lib'], [],
                'the CD music: Ogg decode and seek, disc tracks/positions, play/notify/stop, CD & wave-out mixer lines, cubic taper, effects isolation'),
    'recaudio': (['tests/unit/recaudio_test.c', RT + 'recaudio.c'], ['/D_CRT_SECURE_NO_WARNINGS'], ['user32.lib', 'ole32.lib'], [],
                 '--record\'s sound and sound-slider proxy over a fake DirectSound on a simulated clock: one-shot, volume/pan, rewrite, stream tap, drift, effects gain, WAV length'),
}


# Linux and macOS: the units of the code the win32hle host (src/linux) shares, built 32-bit
# with clang (or gcc) as build-linux.sh builds the host. The rest test the Windows host's
# own presenter, cache and players (native32, GDI, Direct3D), which the win32hle host
# does not use: pcrecomp's runtime/win32hle has its own selftests for those parts.
HLE = ['-DSWGB_WIN32HLE', '-DRECOMP_FLAT_MEMORY', '-DRECOMP_LOCAL_REGS']
UNITS_POSIX = {
    'keys': ([], ['-lpthread']),
    'films': ([], ['-lm']),
    'profile': (HLE, []),
    'inflate': (HLE + ['-w'], []),
    'coverage': (HLE, []),
    'cdaudio': (HLE, ['-lpthread', '-lm']),
}


def posix_cc():
    cc = os.environ.get('CC') or shutil.which('clang') or shutil.which('gcc') or 'cc'
    return [cc] + ([] if sys.platform == 'darwin' else ['-m32'])


def unit_posix(ctx, r, name, srcs, args, what):
    if name not in UNITS_POSIX:
        return r.skip('the Windows host\'s own (native32, GDI, Direct3D): not in the win32hle host')
    flags, libs = UNITS_POSIX[name]
    pc = find_pcrecomp(ctx.a)
    if not pc:
        return r.fail('no pcrecomp checkout (--pcrecomp, PCRECOMP, ../pcrecomp): its headers are needed')
    os.makedirs(r.dir, exist_ok=True)
    exe = os.path.join(r.dir, name + '_test')
    incs = ['-I' + os.path.join(ROOT, 'src', 'runtime'), '-I' + os.path.join(ROOT, 'third_party', 'miniz'),
            '-I' + os.path.join(pc, 'runtime', 'win32hle'), '-I' + os.path.join(pc, 'runtime', 'recomp32')]
    cmd = posix_cc() + ['-O1', '-Wall', '-Wno-unused-parameter', '-o', exe] + flags + incs + \
        [os.path.join(ROOT, s) for s in srcs] + libs
    rc, out = run(ctx, r, cmd, 300, 'build.log')
    if rc:
        for l in [l for l in out.splitlines() if 'error' in l][:8]:
            r.log(l.strip())
        return r.fail('the build failed (%s)' % os.path.join(os.path.basename(r.dir), 'build.log'))
    rc, out = run(ctx, r, [exe] + args, 300, 'run.log')
    for l in out.splitlines():
        if l.startswith('FAIL') or 'FAILED' in l:
            r.log(l.rstrip())
    if rc or not re.search(r'^OK\s*$', out, re.M):
        r.fail('%s: exit %d' % (name, rc))
    else:
        r.log('OK: ' + what)


def unit_check(name):
    srcs, flags, libs, args, what = UNITS[name]

    def fn(ctx, r):
        if not WIN:
            missing = [s for s in srcs if not os.path.exists(os.path.join(ROOT, s))]
            if missing:
                return r.skip('not in this checkout: %s' % ', '.join(missing))
            return unit_posix(ctx, r, name, srcs, args, what)
        missing = [s for s in srcs if not os.path.exists(os.path.join(ROOT, s))]
        if missing:
            return r.skip('not in this checkout: %s' % ', '.join(missing))
        pc = find_pcrecomp(ctx.a)
        if not pc:
            return r.fail('no pcrecomp checkout (--pcrecomp, PCRECOMP, ..\\pcrecomp): its headers are needed')
        os.makedirs(r.dir, exist_ok=True)
        exe = os.path.join(r.dir, name + '_test.exe')
        incs = ['/I' + os.path.join(ROOT, 'src', 'runtime'), '/I' + os.path.join(ROOT, 'third_party', 'miniz'),
                '/I' + os.path.join(pc, 'runtime', 'native32'), '/I' + os.path.join(pc, 'runtime', 'recomp32')]
        env = msvc_env(ctx)
        cl = shutil.which('cl', path=env.get('PATH') or env.get('Path')) or 'cl'
        cmd = [cl, '/nologo', '/W3', '/wd4996', '/Fe' + exe, '/Fo' + r.dir + '\\'] + flags + incs + \
              [os.path.join(ROOT, s) for s in srcs] + ['/link', '/INCREMENTAL:NO'] + libs
        rc, out = run(ctx, r, cmd, 300, 'build.log', env=env)
        if rc:
            tail = [l for l in out.splitlines() if ' error ' in l or 'fatal' in l][:8]
            for l in tail:
                r.log(l.strip())
            return r.fail('the build failed (%s)' % os.path.join(os.path.basename(r.dir), 'build.log'))
        rc, out = run(ctx, r, [exe] + args, 300, 'run.log')
        for l in out.splitlines():
            if l.startswith(('FAIL', '  pacing', '  --fps', '  refresh', '  hitch', '  plain', '  --smooth', '  stopping')) or 'FAILED' in l:
                r.log(l.rstrip())
        if rc or not re.search(r'^OK\s*$', out, re.M):
            r.fail('%s: exit %d' % (name, rc))
        else:
            r.log('OK: ' + what)
    fn.__doc__ = what
    return fn


for _n in UNITS:
    CHECKS.append(Check('unit/' + _n, 'unit', unit_check(_n), None, 0.2))


@check('unit/tools', 'unit', minutes=0.1)
def unit_tools(ctx, r):
    """the tools' own tests (tests/unit/test_tools.py): soak.py's scripts and log reading, lift_check, this runner's image and --fps-log readers"""
    rc, out = run(ctx, r, [sys.executable, '-m', 'unittest', '-v', 'tests.unit.test_tools'], 120, 'run.log', cwd=ROOT)
    for l in out.splitlines():
        if l.startswith(('FAIL:', 'ERROR:', 'Ran ')):
            r.log(l)
    if rc:
        r.fail('unittest: exit %d' % rc)


# ==== L1: smoke ==================================================================================
def lift_check_fn(kind):
    def fn(ctx, r):
        import lift_check
        gen = os.path.join(ctx.a.main, 'src', 'recomp', GAMES[kind]['gen'])
        stats = os.path.join(ctx.a.main, 'work', GAMES[kind]['stats'])
        base = os.path.join(ctx.baselines, 'lift', kind + '.json')
        if ctx.a.update_baselines and os.path.exists(base):
            os.remove(base)
        rep = lift_check.check(gen, stats, base)
        if rep.get('skipped'):
            return r.skip(rep['skipped'])
        for l in rep['lines']:
            r.log(l)
        for p in rep['problems']:
            r.fail(p)
        for k in ('functions', 'bodies', 'identical', 'changed', 'gone', 'new'):
            if k in rep:
                r.metrics[k] = rep[k]
        os.makedirs(r.dir, exist_ok=True)
        json.dump(rep, open(os.path.join(r.dir, 'lift.json'), 'w'), indent=1)
    fn.__doc__ = 'the %s lift: function count, 0 errors and stubs, each function\'s C byte-identical to the baseline' % GAMES[kind]['title']
    return fn


for _k in GAMES:
    CHECKS.append(Check('lift/' + _k, 'smoke', lift_check_fn(_k), None, 0.1, needs=('cc',) if _k == 'cc' else ()))


def conformance_fn(kind):
    def fn(ctx, r):
        cmd = [sys.executable, os.path.join(HERE, 'conformance.py'), ctx.host(kind), '--game', r.game, '--exe', ctx.exe(kind)]
        if kind == 'cc':
            cmd.insert(2, '--cc')
        rc, out = run(ctx, r, cmd, 180, 'conformance.log', cwd=ROOT)
        for l in out.splitlines():
            if re.match(r'(PASS|FAIL|SKIP): |\d+/\d+', l):
                r.log(l)
        if 'SKIP' in out:
            return r.skip('conformance.py found no game data')
        if rc:
            r.fail('conformance: exit %d' % rc)
    fn.__doc__ = 'conformance.py: boot, genie, directdraw, menu, exit (%s)' % GAMES[kind]['title']
    return fn


for _k in GAMES:
    CHECKS.append(Check('conformance/' + _k, 'smoke', conformance_fn(_k), _k, 0.4))


def skirmish_steps(kind, out, t_menu=18):
    """The main menu's shot, Single Player, Standard Game, Start Game, the map's shot, the
    command center clicked and shot (docs/testing.md, "Into a skirmish")."""
    lay = LAYOUT[kind]
    s = Script()
    s.shot(t_menu, os.path.join(out, 'menu.bmp'))
    start = screens.to_skirmish(s, lay, 21, 800)
    s.shot(start - 8, os.path.join(out, 'setup.bmp'))       # 10 s after Standard Game
    s.shot(start + 44, os.path.join(out, 'map.bmp'))
    s.click(start + 47, lay.command_center)
    s.shot(start + 50, os.path.join(out, 'selected.bmp'))
    return s.steps, start + 54


def skirmish_fn(kind):
    def fn(ctx, r):
        os.makedirs(r.dir, exist_ok=True)
        steps, end = skirmish_steps(kind, r.dir)
        rc, text = run_host(ctx, r, kind, steps, end)
        expect_exit(r, rc)
        events(r, text)
        modes = re.findall(r'SetDisplayMode\((\d+)(?:, |x)(\d+)', text)
        if not modes or modes[0] != ('800', '600'):
            r.fail('the first SetDisplayMode is %s, not 800x600' % ('x'.join(modes[0]) if modes else 'none'))
        menu, setup, mp, sel = (png_of(os.path.join(r.dir, n + '.bmp')) for n in ('menu', 'setup', 'map', 'selected'))
        for p, cap in ((menu, 'the main menu'), (setup, 'Standard Game'), (mp, 'the map, loaded'), (sel, 'the command center selected')):
            r.image(p, cap)
            if not p:
                r.fail('no shot: %s' % cap)
            elif blank(p):
                r.fail('%s is blank' % cap)
        compare(ctx, r, menu, '%s/menu' % kind, tol=0.02)
        # the setup screen keeps the folder's last settings: only its title and buttons
        compare(ctx, r, setup, '%s/setup' % kind, tol=0.03, region=(0, 480, 400, 600))
        # the map and the HUD's art (each civilization has its own) differ every game: not compared
        if 'open data\\%s.dat -> ok' % GAMES[kind]['genie'] not in text.lower().replace('/', '\\') and \
                not re.search(r'open data\\%s\.dat -> ok' % GAMES[kind]['genie'], text, re.I):
            r.fail('data\\%s.dat was not read' % GAMES[kind]['genie'])
    fn.__doc__ = 'menu -> Standard Game -> a skirmish, its shots, no ICALL, no fault (%s)' % GAMES[kind]['title']
    return fn


for _k in GAMES:
    CHECKS.append(Check('skirmish/' + _k, 'smoke', skirmish_fn(_k), _k, 1.9))


# ==== L2: functional =============================================================================
PRESENT_GROUPS = {'A': 6, 'B': 3.4, 'C': 2.7, 'D': 3.7, 'E': 4.3}     # minutes, measured


def present_fn(group):
    def fn(ctx, r):
        cmd = [sys.executable, os.path.join(HERE, 'present_check.py'), '--only', group, '--run', ctx.host('base'),
               '--game', r.game, '--exe', ctx.exe('base'), '--out', r.dir]
        rc, out = run(ctx, r, cmd, 40 * 60, 'present_check.log', cwd=ROOT)
        cases = re.findall(r'^(PASS|FAIL): (.*)$', out, re.M)
        r.metrics['cases'] = len(cases)
        r.metrics['passed'] = sum(1 for s, _ in cases if s == 'PASS')
        for s, c in cases:
            if s == 'FAIL':
                r.fail(c)
        r.log('%d/%d cases passed' % (r.metrics['passed'], len(cases)))
        if rc or not cases:
            r.fail('present_check.py --only %s: exit %d' % (group, rc))
        events(r, '\n'.join(open(os.path.join(dp, f), encoding='utf-8', errors='replace').read()
                            for dp, _, fs in os.walk(r.dir) for f in fs if f.endswith('.log') and f != 'present_check.log'))
    fn.__doc__ = 'present_check.py group %s (the presenter headless: letterbox, edge scrolling, fullscreen, pacing)' % group
    return fn


for _g, _m in PRESENT_GROUPS.items():
    CHECKS.append(Check('present/' + _g, 'functional', present_fn(_g), 'base', _m, needs=('win',)))


def res_fn(size):
    def fn(ctx, r):
        cmd = [sys.executable, os.path.join(HERE, 'res_check.py'), str(size), '--out', r.dir, '--game', r.game,
               '--exe', ctx.exe('base'), '--run', ctx.host('base')]
        rc, out = run(ctx, r, cmd, 600, 'res_check.log', cwd=ROOT)
        for l in out.splitlines():
            if l.startswith(('SetDisplayMode', 'menus restored', 'PASS', 'FAIL')):
                r.log(l)
        if rc:
            r.fail('res_check.py %d: exit %d' % (size, rc))
        if size != 800 and 'menus restored to 800x600 after Quit: yes' not in out:   # 800: no mode change at all
            r.fail('the menus did not come back at 800x600')
        log = os.path.join(r.dir, 'run.log')
        if os.path.exists(log):
            events(r, open(log, encoding='utf-8', errors='replace').read())
        for name in ('a_start', 'b_selected', 'g_minimap', 'h_menu', 'j_menus'):
            p = os.path.join(r.dir, name + '.png')
            if os.path.exists(os.path.join(r.dir, name + '.bmp')):
                os.remove(os.path.join(r.dir, name + '.bmp'))
            r.image(p, name)
            if not os.path.exists(p):
                r.fail('no shot %s' % name)
        jm = os.path.join(r.dir, 'j_menus.png')
        # Main Menu on the score screen leads to the Single Player menu
        compare(ctx, r, jm if os.path.exists(jm) else None, 'base/sp_menu', tol=0.03)
    fn.__doc__ = 'res_check.py %d: the game\'s own Screen Size in a skirmish, the menus back at 800x600' % size
    return fn


for _s in (800, 1024, 1280):
    CHECKS.append(Check('res/%d' % _s, 'functional', res_fn(_s), 'base', 4.0))


def res_mode_fn(kind, what, args, shots, doc):
    """res_check.py ingame or popups: its PASS and FAIL lines, its shots in the report."""
    def fn(ctx, r):
        cmd = [sys.executable, os.path.join(HERE, 'res_check.py'), what, '--out', r.dir, '--game', r.game,
               '--exe', ctx.exe(kind), '--run', ctx.host(kind)] + list(args) + (['--cc'] if kind == 'cc' else [])
        rc, out = run(ctx, r, cmd, 600, 'res_check.log', cwd=ROOT)
        lines = re.findall(r'^(PASS|FAIL): (.*)$', out, re.M)
        for s, l in lines:
            r.log('%s: %s' % (s, l))
            if s == 'FAIL':
                r.fail(l)
        r.metrics['passed'] = sum(1 for s, _ in lines if s == 'PASS')
        if rc or not lines:
            r.fail('res_check.py %s: exit %d' % (what, rc))
        log = os.path.join(r.dir, 'run.log')
        if os.path.exists(log):
            events(r, open(log, encoding='utf-8', errors='replace').read())
        for f in os.listdir(r.dir) if os.path.isdir(r.dir) else []:
            if f.endswith('.bmp') and os.path.exists(os.path.join(r.dir, f[:-4] + '.png')):
                os.remove(os.path.join(r.dir, f))
        for name in shots:
            r.image(os.path.join(r.dir, name + '.png'), name)
    fn.__doc__ = doc
    return fn


# In-game switches (the Menu button after each), and every window over the map at sizes where
# the game places it itself (1024x768) and where the host does (--res: widescreen.c).
for _k in GAMES:
    CHECKS.append(Check('res/ingame/' + _k, 'functional', res_mode_fn(
        _k, 'ingame', ['--res', '1920x1080'],
        ['01_1024x768_menu', '02_1920x1080_menu', '03_800x600_menu', 'i_score'],
        'res_check.py ingame: Options in the game to 1024x768, 1920x1080, 800x600; the Menu button '
        'answers after each, its menu centred (%s)' % GAMES[_k]['title']), _k, 3.5))
for _k, _args, _tag in (('base', ['--size', '1024'], '1024'), ('base', ['--res', '2560x1440'], '2560x1440'),
                        ('cc', ['--res', '1920x1080'], '1920x1080')):
    CHECKS.append(Check('popups/%s-%s' % (_k, _tag), 'functional', res_mode_fn(
        _k, 'popups', _args, ['b_menu', 'b_objectives', 'b_chat', 'b_diplomacy', 'b_techtree', 'c_save_typed', 'd_load', 'e_about'],
        'res_check.py popups at %s: menu, Objectives, Chat, Diplomacy, About centred; the tech tree; Save and '
        'Load screens at the size that fits, centred (%s)' % (_tag, GAMES[_k]['title'])), _k, 3.5))

def hud_band(w, h):
    """The interface at --res WxH: (its base width, its rows under the map)."""
    bw = screens.hud_base(w, h)
    return bw, screens.HUD[bw]['rows']


def side_column(d, band):
    """widescreen_side_column: the art's column (0 = its edge) shown d columns out from it."""
    k = d % (2 * band)
    return k if k < band else 2 * band - 1 - k


def hud_sides(png):
    """The interface band of a --res shot: its mean grey, the sides' mean grey, their share of
    black pixels, and the share of side pixels equal to the art's pixel that --hud-sides fill
    mirrors there (1.0: every side pixel is the art's own)."""
    import numpy as np
    from PIL import Image
    a = np.asarray(Image.open(png).convert('RGB'), dtype=np.int16)
    h, w, _ = a.shape
    bw, bh = hud_band(w, h)
    dx, band = (w - bw) // 2, bw // 100
    rows = a[h - bh:]
    hud, left, right = rows[:, dx + 10:dx + bw - 10], rows[:, :dx], rows[:, dx + bw:]
    grey = rows.mean(axis=2)
    sides = np.concatenate([grey[:, :dx], grey[:, dx + bw:]], axis=1)
    lsrc = [dx + side_column(d, band) for d in range(dx)][::-1]            # the left side, x = 0..dx-1
    rsrc = [dx + bw - 1 - side_column(d, band) for d in range(w - dx - bw)]
    match = np.concatenate([(left == rows[:, lsrc]).all(axis=2), (right == rows[:, rsrc]).all(axis=2)], axis=1)
    return {'hud_mean': round(float(hud.mean()), 1), 'sides_mean': round(float(sides.mean()), 1),
            'sides_black': round(float((sides < 8).mean()), 4), 'sides_mirror': round(float(match.mean()), 4),
            'base': bw, 'rows': bh, 'dx': dx}


def sides_crop(png, out):
    """The two sides of the interface band side by side, as a PNG."""
    from PIL import Image
    im = Image.open(png).convert('RGB')
    w, h = im.size
    bw, bh = hud_band(w, h)
    dx = (w - bw) // 2
    c = Image.new('RGB', (w - bw, bh))
    c.paste(im.crop((0, h - bh, dx, h)), (0, 0))
    c.paste(im.crop((dx + bw, h - bh, w, h)), (dx, 0))
    c.save(out)
    return out


def view_marker_moved(png_a, png_b, x0, y0, x1, y1):
    """The minimap's view frame (its white outline, and the cursor beside it) in two shots: the
    mean x of the white pixels only shot A has, and of those only shot B has (the art's own white
    is in both). None for a shot with fewer than 20."""
    import numpy as np
    from PIL import Image
    wa, wb = [(np.asarray(Image.open(p).convert('RGB'), dtype=np.int16)[y0:y1, x0:x1] > 220).all(axis=2) for p in (png_a, png_b)]
    out = []
    for m in (wa & ~wb, wb & ~wa):
        xs = np.nonzero(m)[1]
        out.append(float(xs.mean()) + x0 if len(xs) >= 20 else None)
    return tuple(out)


def layout_text(path):
    """SWGB_RES_DUMP's panel tree without the panels' addresses: vtable and rect per panel."""
    if not path or not os.path.exists(path):
        return None
    with open(path) as f:
        return ''.join(re.sub(r'^(\s*)[0-9A-F]{8} ', r'\1', l.rstrip()) + '\n' for l in f if l.strip())


def read_text(path):
    with open(path) as f:
        return f.read()


def widescreen_fn(kind, res='1920x1080', sides='fill'):
    def fn(ctx, r):
        W, H = map(int, res.split('x'))
        lay = screens.layout(kind, (W, H))
        out = r.dir
        os.makedirs(out, exist_ok=True)
        bw = lay.base
        # two points on the minimap, left and right of its centre: its view frame follows them
        mx, my, reach = lay.minimap
        ma, mb = (mx - 120, my), (mx + 120, my)
        s = Script()
        start = screens.to_skirmish(s, lay, 13, 1280)
        s.shot(18, os.path.join(out, 'options.bmp'))     # Options, the Screen Size buttons
        s.shot(start + 57, os.path.join(out, 'map.bmp'))
        s.shot(start + 59, os.path.join(out, 'map2.bmp'))
        s.click(start + 61, ma)
        s.shot(start + 63, os.path.join(out, 'mini_a.bmp'))
        s.click(start + 65, mb)
        s.shot(start + 67, os.path.join(out, 'mini_b.bmp'))
        dump = os.path.join(out, 'layout_dump.txt')
        env = dict(os.environ, SWGB_RES_DUMP=dump)
        extra = ['--res', res] + (['--hud-sides', sides] if sides != 'fill' else [])
        rc, text = run_host(ctx, r, kind, s.steps, start + 71, extra=extra, env=env)
        expect_exit(r, rc)
        events(r, text)
        modes = ['%sx%s' % m for m in re.findall(r'SetDisplayMode\((\d+)(?:, |x)(\d+)', text)]
        r.log('SetDisplayMode: ' + ', '.join(modes))
        if res not in modes:
            r.fail('no SetDisplayMode(%d, %d)' % (W, H))
        opt = png_of(os.path.join(out, 'options.bmp'))
        shots = [png_of(os.path.join(out, n + '.bmp')) for n in ('map', 'map2', 'mini_a', 'mini_b')]
        mp = shots[0]
        r.image(opt, 'Options: the third size reads %s' % res.replace('x', ' x '))
        r.image(mp, 'a skirmish at %s, --hud-sides %s' % (res, sides))
        compare(ctx, r, opt, '%s/options_%s' % (kind, '1920' if res == '1920x1080' else res), tol=0.03)
        if not mp:
            r.fail('no shot of the map')
            return
        from PIL import Image
        if Image.open(mp).size != (W, H):
            r.fail('the shot is %dx%d' % Image.open(mp).size)
            return
        if blank(mp):
            r.fail('the map is blank')
            return
        # the HUD at the bottom, centred; beside it the art's own edges, mirrored (or black). The
        # civilization (so the art) is random each game: the sides are checked against the art
        # in the same shot, not against a stored picture.
        f = hud_sides(mp)
        r.metrics.update({k: f[k] for k in ('hud_mean', 'sides_mean', 'sides_black', 'sides_mirror')})
        r.log('interface: base %d, mean grey %.0f; sides: mean grey %.0f, %.1f%% black, %.1f%% the art mirrored'
              % (f['base'], f['hud_mean'], f['sides_mean'], f['sides_black'] * 100, f['sides_mirror'] * 100))
        if f['hud_mean'] < 20:
            r.fail('no HUD at the bottom (mean grey %.0f)' % f['hud_mean'])
        if sides == 'black':
            if f['sides_mean'] > 8:
                r.fail('--hud-sides black: the sides are not black (mean grey %.0f)' % f['sides_mean'])
        else:
            if f['sides_black'] > 0.3 or f['sides_mean'] < 20:   # the art's own outlines: 7-12% black
                r.fail('--hud-sides fill: the sides are black (%.0f%% black, mean grey %.0f)' % (f['sides_black'] * 100, f['sides_mean']))
            if f['sides_mirror'] < 0.97:   # a panel drawn over the art's last columns (800 wide: 2%)
                r.fail('--hud-sides fill: only %.1f%% of the sides are the art mirrored' % (f['sides_mirror'] * 100))
        # steady: the sides the same two seconds later
        if shots[1]:
            import numpy as np
            a1 = np.asarray(Image.open(sides_crop(mp, os.path.join(out, 'sides1.png'))))
            a2 = np.asarray(Image.open(sides_crop(shots[1], os.path.join(out, 'sides2.png'))))
            same = float((a1 == a2).all(axis=2).mean())
            r.metrics['sides_steady'] = round(same, 4)
            if same < 0.999:
                r.fail('the sides changed between two frames (%.2f%% the same)' % (same * 100))
        else:
            r.fail('no second shot of the map')
        # the minimap answers where it is drawn: its view frame follows a click left, then right
        if bw == 1280:
            if shots[2] and shots[3]:
                px, py, pw, ph = lay.minimap_panel
                xa, xb = view_marker_moved(shots[2], shots[3], px, py, px + pw, py + ph)
                r.image(shots[3], 'the minimap clicked right of its centre')
                r.log('minimap view frame: x %s after the click at x %d, %s after the click at x %d'
                      % (xa and round(xa), ma[0], xb and round(xb), mb[0]))
                if xa is None or xb is None or abs(xa - ma[0]) > 30 or abs(xb - mb[0]) > 30:
                    r.fail('the minimap view frame is not where it was clicked (x %s for %d, %s for %d)' % (xa, ma[0], xb, mb[0]))
            else:
                r.fail('no shots after the minimap clicks')
        # every panel's place (so every click): the same with --hud-sides fill and black
        lay = layout_text(dump)
        key = os.path.join(ctx.baselines, kind, 'layout_%s.txt' % res)
        if lay is None:
            r.fail('no layout dump (SWGB_RES_DUMP)')
        elif ctx.a.update_baselines or not os.path.exists(key):
            os.makedirs(os.path.dirname(key), exist_ok=True)
            with open(key, 'w') as fo:
                fo.write(lay)
            r.log('layout recorded as the baseline (%s)' % key)
        elif read_text(key) != lay:
            r.fail('the panels are not where the baseline has them (%s against %s)' % (dump, key))
        else:
            r.log('layout: %d panels, as the baseline' % lay.count('\n'))
    fn.__doc__ = '--res %s --hud-sides %s: Options names it, the HUD centred, %s beside it, the minimap answers, the layout (%s)' % (
        res, sides, 'the art mirrored' if sides == 'fill' else 'black', GAMES[kind]['title'])
    return fn


for _k in GAMES:
    CHECKS.append(Check('widescreen/' + _k, 'functional', widescreen_fn(_k), _k, 2.1))
CHECKS.append(Check('widescreen/base_black', 'functional', widescreen_fn('base', '1920x1080', 'black'), 'base', 2.1))
CHECKS.append(Check('widescreen/base_3440', 'functional', widescreen_fn('base', '3440x1440'), 'base', 2.1))
CHECKS.append(Check('widescreen/cc_2560', 'functional', widescreen_fn('cc', '2560x1440'), 'cc', 2.1))


def films_fn(kind):
    def fn(ctx, r):
        if not shutil.which('ffmpeg'):
            return r.skip('no ffmpeg on PATH: the films are skipped by the host')
        films = r.game
        if not any(os.path.exists(os.path.join(films, f)) for f in ('xlogo1.avi', 'xintro.avi')):
            return r.skip('no xlogo1.avi or xintro.avi in the game folder')
        out = r.dir
        os.makedirs(out, exist_ok=True)
        s = [(0, 'on:logo1'), (4, 'shot=%s' % os.path.join(out, 'logo.bmp')),
             (0, 'on:intro'), (30, 'shot=%s' % os.path.join(out, 'intro.bmp'))] if kind == 'base' else \
            [(0, 'on:xlogo1'), (6, 'shot=%s' % os.path.join(out, 'logo.bmp')),
             (0, 'on:xintro'), (30, 'shot=%s' % os.path.join(out, 'intro.bmp'))]
        # on:NAME waits for the file (it takes no time): kept in order, unsorted
        keys = os.path.join(out, 'run_keys.txt')
        with open(keys, 'w', newline='\n') as f:
            f.write('\n'.join(w if w.startswith('on:') else '%g:%s' % (t, w) for t, w in s) + '\n')
        cmd = [ctx.host(kind), '--run', '--multi', '--headless', '--files', '--exe', ctx.exe(kind), '--game', r.game,
               '--watchdog', '110', '--films', films, '--keys', '@' + keys]
        rc, text = run(ctx, r, cmd, 200, 'run.log')
        expect_exit(r, rc)
        events(r, text)
        played = re.findall(r'\[movie\] (\S+): (\d+) frames shown, (\d+) dropped', text)
        for name, shown, dropped in played:
            r.log('%s: %s frames shown, %s dropped' % (name, shown, dropped))
            r.metrics[name] = int(shown)
            if int(shown) < 100:
                r.fail('%s: only %s frames shown' % (name, shown))
            if int(dropped) > max(10, int(shown) // 20):
                r.fail('%s: %s frames dropped' % (name, dropped))
        if not played:
            r.fail('no film played (%s)' % '; '.join(l for l in text.splitlines() if '[movie]' in l)[:300])
        for n, cap in (('logo', 'the logo film'), ('intro', 'the intro film')):
            p = png_of(os.path.join(out, n + '.bmp'))
            r.image(p, cap)
            if p and blank(p, 0.97):
                r.fail('%s: the shot is blank' % cap)
    fn.__doc__ = '--films: the logo and the intro play through FFmpeg, frames shown and dropped (%s)' % GAMES[kind]['title']
    return fn


for _k in GAMES:
    CHECKS.append(Check('films/' + _k, 'functional', films_fn(_k), _k, 1.9))


NEWPLAYER_NAME = 'enter'
WIN_CHEAT = screens.WIN_CHEAT
SKIRMISH_AI = re.compile(r'open data\\load\\[^ ]+\.per -> ok', re.I)   # the computer players' scripts
GAME_WON = re.compile(r'open sound\\stream\\won\d\.mp3 -> ok', re.I)  # the victory's music


def enter_fn(kind):
    # Enter in both of the game's text fields, as on Windows: New Player's name (Enter
    # accepts the dialog, like OK) and the chat line (Enter sends it: the victory cheat).
    # The folder's profile is moved aside, so Single Player opens New Player.
    def fn(ctx, r):
        lay = LAYOUT[kind]
        out = r.dir
        os.makedirs(out, exist_ok=True)
        aside = os.path.join(out, 'profiles')
        os.makedirs(aside, exist_ok=True)
        for f in os.listdir(r.game):
            if f.lower().startswith('player.nf'):
                shutil.move(os.path.join(r.game, f), os.path.join(aside, f))
        try:
            s = [(20, 'move:%d:%d' % MAIN_SINGLE), (21, 'click:%d:%d' % MAIN_SINGLE),
                 (25, 'shot=%s' % os.path.join(out, 'newplayer.bmp'))]
            s += [(26 + 0.5 * k, ch) for k, ch in enumerate(NEWPLAYER_NAME)]
            t = 26 + 0.5 * len(NEWPLAYER_NAME) + 1
            s += [(t, 'enter'), (t + 4, 'shot=%s' % os.path.join(out, 'single_player.bmp')),
                  (t + 6, 'move:%d:%d' % lay.sp_standard), (t + 7, 'click:%d:%d' % lay.sp_standard),
                  (t + 17, 'move:%d:%d' % SETUP_START), (t + 18, 'click:%d:%d' % SETUP_START)]
            t += 65                                      # the map is up (skirmish_steps: 44 s after Start)
            s += [(t, 'shot=%s' % os.path.join(out, 'map.bmp')), (t + 1, 'enter')]
            s += [(t + 2 + 0.3 * k, ch) for k, ch in enumerate(WIN_CHEAT)]
            t += 3 + 0.3 * len(WIN_CHEAT)
            s += [(t, 'enter'), (t + 12, 'shot=%s' % os.path.join(out, 'won.bmp'))]
            rc, text = run_host(ctx, r, kind, s, t + 15)
        finally:
            for f in os.listdir(r.game):
                if f.lower().startswith('player.nf'):
                    os.remove(os.path.join(r.game, f))
            for f in os.listdir(aside):
                shutil.move(os.path.join(aside, f), os.path.join(r.game, f))
        expect_exit(r, rc)
        events(r, text)
        for n, cap in (('newplayer', 'New Player, the name typed'), ('single_player', 'after Enter: Single Player'),
                       ('map', 'the skirmish'), ('won', 'after the chat\'s Enter: the victory')):
            r.image(png_of(os.path.join(out, n + '.bmp')), cap)
        if not SKIRMISH_AI.search(text):
            return r.fail('no skirmish started: Enter did not accept New Player (no profile, no Standard Game)')
        r.log('New Player: Enter accepted the name; the skirmish started')
        if not GAME_WON.search(text):
            return r.fail('no victory: the chat\'s Enter did not send "%s"' % WIN_CHEAT)
        r.log('the chat: Enter sent "%s", the game was won' % WIN_CHEAT)
    fn.__doc__ = 'Enter accepts New Player\'s name and sends the chat line (the victory cheat) (%s)' % GAMES[kind]['title']
    return fn


for _k in GAMES:
    CHECKS.append(Check('enter/' + _k, 'functional', enter_fn(_k), _k, 2.0))


MUSIC_TRACK_S = 2.0      # tests/data/tone.ogg


def music_fn(kind):
    def fn(ctx, r):
        # A disc of 2-second tracks (tests/data/tone.ogg as Track02..Track50), so the game's
        # one long play, tracks 2-49, ends inside the check and its notification is seen.
        disc = os.path.join(r.dir, 'disc')
        os.makedirs(disc, exist_ok=True)
        tone = os.path.join(ROOT, 'tests', 'data', 'tone.ogg')
        for t in range(2, 51):
            shutil.copyfile(tone, os.path.join(disc, 'Track%02d.ogg' % t))
        steps, _ = skirmish_steps(kind, r.dir)
        rc, text = run_host(ctx, r, kind, steps, 200, extra=('--music', disc))
        expect_exit(r, rc)
        events(r, text)
        lines = [l.strip() for l in text.splitlines() if l.startswith('[cdaudio]')]
        r.metrics['cdaudio_lines'] = len(lines)
        if not any(l.startswith('[cdaudio] disc: tracks 1-50') for l in lines):
            return r.fail('the disc was not read as tracks 1-50: %s' % (lines[:2],))
        if '[cdaudio] status item 3 -> 0x32' not in lines:
            r.fail('the game did not get 50 tracks from MCI_STATUS')
        if not any(l.startswith('[cdaudio] output: ') for l in lines):
            r.fail('no output was opened')
        if not any(l.startswith('[cdaudio] music volume ') for l in lines):
            r.fail('the game never set the CD line\'s volume')
        plays = [(i, float(m.group(1))) for i, l in enumerate(lines)
                 for m in [re.match(r'\[cdaudio\] play from track 2 0:00 to track 50 0:00, notify at ([\d.]+) s', l)] if m]
        if not plays:
            return r.fail('the game never played tracks 2-49: %s' % '; '.join(l for l in lines if ' play ' in l)[:300])
        i0, t0 = plays[0]
        r.log('in game: tracks 2-49 from %.1f s (the menus play none: the game\'s own MP3s)' % t0)
        if t0 < 40:
            r.fail('the play began at %.1f s, before Start Game' % t0)
        tracks = []
        for l in lines[i0 + 1:]:
            m = re.match(r'\[cdaudio\] track (\d+) \(audio\) at ([\d.]+) s', l)
            if m:
                tracks.append((int(m.group(1)), float(m.group(2))))
            if l.startswith('[cdaudio] played to'):
                break
        nums = [n for n, _ in tracks]
        if nums != list(range(2, 50)):
            r.fail('tracks played %s, not 2-49 in order' % nums[:60])
        gaps = [b - a for (_, a), (_, b) in zip(tracks[1:], tracks[2:])]
        if gaps:
            r.metrics['track_gap_min'], r.metrics['track_gap_max'] = round(min(gaps), 2), round(max(gaps), 2)
            r.log('tracks 3-49 each %.2f-%.2f s apart (each is %.1f s)' % (min(gaps), max(gaps), MUSIC_TRACK_S))
            if min(gaps) < MUSIC_TRACK_S - 0.3 or max(gaps) > MUSIC_TRACK_S + 0.5:
                r.fail('a track did not last %.1f s: %.2f-%.2f s between them' % (MUSIC_TRACK_S, min(gaps), max(gaps)))
        end = [(i, float(m.group(1))) for i, l in enumerate(lines)
               for m in [re.match(r'\[cdaudio\] played to track 50 0:00 at ([\d.]+) s', l)] if m and i > i0]
        if not end:
            return r.fail('the play never reached track 50')
        i1, t1 = end[0]
        r.metrics['play_seconds'] = round(t1 - t0, 1)
        r.log('played 48 tracks in %.1f s (%.1f s of audio)' % (t1 - t0, 48 * MUSIC_TRACK_S))
        if abs((t1 - t0) - 48 * MUSIC_TRACK_S) > 3:
            r.fail('48 tracks took %.1f s, not %.0f' % (t1 - t0, 48 * MUSIC_TRACK_S))
        if '[cdaudio] notify successful' not in lines[i1:i1 + 3]:
            r.fail('no MM_MCINOTIFY successful after the play\'s end')
        if not [p for p in plays if p[0] > i1]:
            r.fail('the game did not play tracks 2-49 again after the notification')
        else:
            r.log('then the notification, and the game played tracks 2-49 again')
    fn.__doc__ = 'the CD music: tracks 2-49 from Start Game, each on time, the end\'s notify, the game\'s repeat (%s)' % \
        GAMES[kind]['title']
    return fn


# Clone Campaigns: the farm's profile has its music at zero, so the game never opens the CD.
CHECKS.append(Check('music/base', 'functional', music_fn('base'), 'base', 3.6))


def mp4_streams(mp4):
    """ffprobe's streams of a file: {'video': {...}, 'audio': {...}} (the first of each)."""
    out = subprocess.run(['ffprobe', '-v', 'error', '-show_streams', '-of', 'json', mp4],
                         capture_output=True, text=True).stdout
    streams = {}
    for s in json.loads(out or '{}').get('streams', []):
        streams.setdefault(s.get('codec_type'), s)
    return streams


def audio_db_per_second(mp4, rate=8000):
    """The sound of a file as RMS dBFS per second (mono, decoded by ffmpeg); -99 for silence."""
    import array
    import math
    raw = subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-i', mp4, '-map', '0:a:0', '-ac', '1', '-ar', str(rate),
                          '-f', 's16le', '-'], capture_output=True).stdout
    a = array.array('h', raw[:len(raw) // 2 * 2])
    out = []
    for i in range(0, len(a), rate):
        w = a[i:i + rate]
        r = math.sqrt(sum(x * x for x in w) / max(1, len(w))) / 32768
        out.append(round(20 * math.log10(r), 1) if r > 0 else -99.0)
    return out


def record_fn(kind):
    def fn(ctx, r):
        if not WIN:
            return r.skip('--record has no sound on the win32hle host yet (docs/recording.md, "Not done")')
        if not shutil.which('ffmpeg') or not shutil.which('ffprobe'):
            return r.skip('no ffmpeg or ffprobe on PATH: --record needs them')
        lay = LAYOUT[kind]
        out = r.dir
        os.makedirs(out, exist_ok=True)
        x = '' if kind == 'base' else 'x'
        films = any(os.path.exists(os.path.join(r.game, f)) for f in ('xlogo1.avi', 'xintro.avi'))
        # the logo film played through, the intro skipped (Esc), then as skirmish_steps: Single
        # Player, Standard Game, Start; the command center clicked and its hotkey (unit sounds);
        # the win cheat, and the victory music over the score screen
        s = ['on:%slogo1' % x, 'on:%sintro' % x, '3:esc'] if films else []
        t = 12 if films else 18
        s += ['%g:shot=%s' % (t, os.path.join(out, 'menu.bmp')),
              '%g:move:%d:%d' % ((t + 2,) + MAIN_SINGLE), '%g:click:%d:%d' % ((t + 3,) + MAIN_SINGLE),
              '%g:move:%d:%d' % ((t + 17,) + lay.sp_standard), '%g:click:%d:%d' % ((t + 18,) + lay.sp_standard),
              '%g:move:%d:%d' % ((t + 37,) + SETUP_START), '%g:click:%d:%d' % ((t + 38,) + SETUP_START),
              '%g:shot=%s' % (t + 82, os.path.join(out, 'map.bmp')),
              '%g:move:400:170' % (t + 84), '%g:click:400:170' % (t + 85), '%g:h' % (t + 88),
              '%g:click:400:170' % (t + 90), '%g:h' % (t + 92), '%g:enter' % (t + 94)]
        for k, ch in enumerate(WIN_CHEAT):
            s.append('%g:%s' % (t + 95 + 0.3 * k, ch))
        s += ['%g:enter' % (t + 98.7), '%g:shot=%s' % (t + 110, os.path.join(out, 'score.bmp'))]
        keys = os.path.join(out, 'run_keys.txt')
        with open(keys, 'w', newline='\n') as f:
            f.write('\n'.join(s) + '\n')
        mp4 = os.path.join(out, 'record.mp4')
        cmd = [ctx.host(kind), '--run', '--multi', '--headless', '--files', '--exe', ctx.exe(kind), '--game', r.game,
               '--watchdog', '160', '--record', mp4, '--keys', '@' + keys] + (['--films', r.game] if films else [])
        rc, text = run(ctx, r, cmd, 300, 'run.log')
        expect_exit(r, rc)
        events(r, text)
        if not os.path.exists(mp4):
            return r.fail('no %s' % os.path.basename(mp4))
        st = mp4_streams(mp4)
        v, a = st.get('video'), st.get('audio')
        if not a:
            return r.fail('the recording has no sound stream (%s)' % ', '.join(st) or 'no streams')
        vd, ad = float(v.get('duration', 0)) if v else 0, float(a.get('duration', 0))
        r.metrics.update(video_s=vd, audio_s=ad)
        r.log('video %.3f s, sound %.3f s (%s, %s Hz, %s channels)' % (vd, ad, a.get('codec_name'), a.get('sample_rate'), a.get('channels')))
        if abs(vd - ad) > 2 / 30:
            r.fail('the sound is %.3f s, the video %.3f s: more than two frames apart' % (ad, vd))
        m = re.search(r'\[recaudio\] ([0-9.]+) s of sound, (\d+) buffers \((\d+) plays\), (\d+) streams', text)
        if m:
            r.metrics.update(buffers=int(m.group(2)), plays=int(m.group(3)), streams=int(m.group(4)))
            r.log('recaudio: %s buffers, %s plays, %s streams' % m.group(2, 3, 4))
            if int(m.group(3)) < 5:
                r.fail('only %s DirectSound plays recorded: the clicks and units make more' % m.group(3))
        else:
            r.fail('no [recaudio] summary in the log')
        if '[recaudio] muxed into' not in text:
            r.fail('the sound was not muxed in')
        db = audio_db_per_second(mp4)
        loud = [d for d in db if d > -60]
        r.metrics['rms_db'] = max(db) if db else -99
        r.log('sound dBFS per 10 s (loudest second): ' + ' '.join('%d:%.0f' % (i * 10, max(db[i * 10:i * 10 + 10]))
                                                                   for i in range((len(db) + 9) // 10)))
        if len(loud) < len(db) // 4:
            r.fail('the sound is silent most of the time (%d of %d seconds above -60 dBFS)' % (len(loud), len(db)))
        if films:
            if not re.search(r'\[movie\] playing .*xlogo1\.avi', text):
                r.fail('the logo film did not play')
            elif max(db[:12] or [-99]) < -30:
                r.fail('the logo film is silent in the recording (loudest %.0f dBFS in its first 12 s)' % max(db[:12] or [-99]))
        won = re.search(r'stream\\won\d\.mp3 -> ok', text, re.I)
        if not won:
            r.fail('no victory music: the cheat did not end the game')
        elif max(db[-15:] or [-99]) < -30 or sorted(db[-15:])[len(db[-15:]) // 2] < -40:
            r.fail('the victory music is silent in the recording (its last 15 s: %s)' % db[-15:])
        for n, cap in (('map', 'the map'), ('score', 'the score screen')):
            p = png_of(os.path.join(out, n + '.bmp'))
            r.image(p, cap)
    fn.__doc__ = ('--record with its sound: the logo film, a skirmish (clicks, hotkeys), the win cheat\'s victory music; '
                  'a sound stream as long as the video, loud where they play (%s)' % GAMES[kind]['title'])
    return fn


for _k in GAMES:
    CHECKS.append(Check('record/' + _k, 'functional', record_fn(_k), _k, 3.2))


OPTIONS_MUSIC_UP, OPTIONS_MUSIC_DOWN = (451, 254), (451, 334)   # the main menu's Options: the sliders' arrows,
OPTIONS_SOUND_UP, OPTIONS_SOUND_DOWN = (529, 254), (529, 334)   # 10% a click


def stereo_windows(mp4, a, b):
    """The sound of a file over [a, b) seconds: (left rms, right rms, left's zero crossings a second)."""
    import array
    import math
    raw = subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-ss', str(a), '-t', str(b - a), '-i', mp4, '-map', '0:a:0',
                          '-ac', '2', '-ar', '44100', '-f', 's16le', '-'], capture_output=True).stdout
    s = array.array('h', raw[:len(raw) // 4 * 4])
    left, right = s[0::2], s[1::2]
    if not left:
        return 0.0, 0.0, 0.0
    rl = math.sqrt(sum(x * x for x in left) / len(left)) / 32768
    rr = math.sqrt(sum(x * x for x in right) / len(right)) / 32768
    zc = sum(1 for i in range(1, len(left)) if (left[i - 1] < 0) != (left[i] < 0))
    return rl, rr, zc / 2 / (len(left) / 44100)


def sliders_fn(kind):
    def fn(ctx, r):
        import math
        if not WIN:
            return r.skip('--record has no sound on the win32hle host yet (docs/recording.md, "Not done")')
        if not shutil.which('ffmpeg') or not shutil.which('ffprobe'):
            return r.skip('no ffmpeg or ffprobe on PATH: --record needs them')
        lay = LAYOUT[kind]
        disc = os.path.join(r.dir, 'disc')
        os.makedirs(disc, exist_ok=True)
        tone = os.path.join(ROOT, 'tests', 'data', 'tone.ogg')    # 441 Hz left, 882 right, at 1/16 of full scale
        for t in range(2, 51):
            shutil.copyfile(tone, os.path.join(disc, 'Track%02d.ogg' % t))
        # Options: the sound slider to the top (17-22 s: the menu's MP3 at full), to the
        # bottom (26.5-33 s: silence), the music slider to the bottom then the top; then a
        # skirmish, its music the fixture's tone while the sound slider stays at the bottom
        s = [(12, 'move:%d:%d' % screens.MAIN_OPTIONS), (12.5, 'click:%d:%d' % screens.MAIN_OPTIONS)]
        s += [(13 + 0.3 * k, 'click:%d:%d' % OPTIONS_SOUND_UP) for k in range(12)]
        s += [(22 + 0.3 * k, 'click:%d:%d' % OPTIONS_SOUND_DOWN) for k in range(12)]
        s += [(26 + 0.3 * k, 'click:%d:%d' % OPTIONS_MUSIC_DOWN) for k in range(12)]
        s += [(30 + 0.3 * k, 'click:%d:%d' % OPTIONS_MUSIC_UP) for k in range(12)]
        s += [(34.5, 'click:%d:%d' % screens.SCREEN_RADIO[800])]
        s += [(36, 'move:%d:%d' % MAIN_SINGLE), (37, 'click:%d:%d' % MAIN_SINGLE),
              (51, 'move:%d:%d' % lay.sp_standard), (52, 'click:%d:%d' % lay.sp_standard),
              (71, 'move:%d:%d' % SETUP_START), (72, 'click:%d:%d' % SETUP_START),
              (140, 'shot=%s' % os.path.join(r.dir, 'map.bmp'))]
        mp4 = os.path.join(r.dir, 'record.mp4')
        saved_vols = None
        hive_sub = None
        if WIN:
            import winreg
            hive_sub = (r'Software\swgb-recomp\hives\%s\Software\LucasArts Entertainment Company LLC'
                        r'\Star Wars Galactic Battlegrounds\1.0') % re.sub(r'[\\/:]', '_', os.path.abspath(r.game))
            saved_vols = {}
            try:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, hive_sub) as k:
                    for name, default in (('Sound Volume', 0), ('Music Volume', 5000)):
                        try:
                            saved_vols[name] = winreg.QueryValueEx(k, name)[0]
                        except OSError:
                            saved_vols[name] = default
            except OSError:
                saved_vols = {'Sound Volume': 0, 'Music Volume': 5000}
        try:
            rc, text = run_host(ctx, r, kind, s, 145, extra=('--music', disc, '--record', mp4))
        finally:
            if WIN and saved_vols is not None and hive_sub:
                try:
                    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, hive_sub, 0, winreg.KEY_SET_VALUE) as k:
                        for name, val in saved_vols.items():
                            winreg.SetValueEx(k, name, 0, winreg.REG_DWORD, int(val))
                except OSError:
                    pass
        expect_exit(r, rc)
        events(r, text)
        r.image(png_of(os.path.join(r.dir, 'map.bmp')), 'the skirmish')
        lines = [l.strip() for l in text.splitlines() if l.startswith('[cdaudio]')]
        for want in ('[cdaudio] effects volume 100% (0.0 dB)', '[cdaudio] effects volume 1% (-120.0 dB)',
                     '[cdaudio] music volume 1% (-120.0 dB)', '[cdaudio] music volume 100% (0.0 dB)'):
            if want not in lines:
                return r.fail('no "%s": the game did not set its line there' % want)
        r.log('the sliders: effects 100%, then 1% (-120 dB); music 1% (-120 dB), then 100% (0 dB)')
        if not os.path.exists(mp4):
            return r.fail('no %s' % os.path.basename(mp4))
        full, _, _ = stereo_windows(mp4, 17, 22)
        bottom, bottom_r, _ = stereo_windows(mp4, 26.5, 33)
        r.metrics.update(menu_full=round(full, 4), menu_bottom=round(max(bottom, bottom_r), 6))
        r.log('the menu\'s music (DirectSound): rms %.4f with the sound slider at the top, %.6f at its bottom' % (full, max(bottom, bottom_r)))
        if full < 0.005:
            r.fail('the menu\'s music is not heard with the sound slider at the top (rms %.4f)' % full)
        if max(bottom, bottom_r) > 0.0005:
            r.fail('the sound slider\'s bottom is not silent (rms %.6f)' % max(bottom, bottom_r))
        played = [float(m.group(1)) for m in re.finditer(r'\[cdaudio\] play from track 2 0:00 to track 50 0:00, notify at ([\d.]+) s', text)]
        if not played:
            return r.fail('the game never played the CD')
        # the log's clock starts at the first MCI_OPEN, a few seconds into the recording: the
        # window is well inside the play, which lasts 96 s
        rl, rr, hz = stereo_windows(mp4, 125, 135)
        want = 1448 / 32768                                          # the fixture's left rms
        r.metrics.update(music_rms=round(rl, 4), music_hz=round(hz))
        r.log('in the skirmish, the sound slider at its bottom: left rms %.4f (the track %.4f), %.0f Hz, right %.4f' % (rl, want, hz, rr))
        if abs(20 * math.log10(max(rl, 1e-9) / want)) > 1.5:
            r.fail('the music is %.1f dB from the track\'s own level: the sound slider moved it' % (20 * math.log10(max(rl, 1e-9) / want)))
        if abs(hz - 441) > 15:
            r.fail('the left channel is at %.0f Hz, not the track\'s 441: something else is heard' % hz)
    fn.__doc__ = ('the sliders: the sound slider\'s bottom silences the menu\'s music, the music slider\'s top with the '
                  'sound slider at its bottom plays the CD at its own level (%s)' % GAMES[kind]['title'])
    return fn


CHECKS.append(Check('audio/base', 'functional', sliders_fn('base'), 'base', 3.0))


def civ_skirmish(kind, my_civ, civs, out, minutes, save_name):
    """Standard Game: me as my_civ, one computer per civ in civs, a Small map, Easiest; Start,
    Fast speed, then the idle player and a save 40% of the way in. Returns (steps, end)."""
    lay = LAYOUT[kind]
    s = Script()
    t = screens.to_setup(s, lay, 13, 800)
    t = screens.pick(s, t, screens.DD_OTHERS, len(civs) - 1)
    t = screens.pick(s, t, screens.DD_SIZE, 1)
    t = screens.pick(s, t, screens.DD_DIFFICULTY, 0)
    t = screens.set_civ(s, lay, t, screens.MY_CIV_ROW, my_civ)
    for p, c in enumerate(civs):
        t = screens.set_civ(s, lay, t, screens.computer_row(p), c)
    t += 2
    s.at(t, 'shot=%s' % os.path.join(out, 'setup.bmp'))
    s.click(t + 1, SETUP_START)
    start = t + 70
    end = start + minutes * 60
    s.at(start, 'shot=%s' % os.path.join(out, 'start.bmp'))
    t = screens.fast_speed(s, lay, start + 2)
    save_at = start + (end - start) * 0.4
    for k, ts in enumerate(range(int(t) + 3, int(save_at) - 3, 30)):
        s.at(ts, 'h')
        s.click(ts + 1, lay.command_grid[0])
    screens.save_game(s, lay, save_at, save_name)
    s.at(save_at + 21, 'shot=%s' % os.path.join(out, 'saved.bmp'))
    for ts in range(int(save_at) + 25, int(end) - 5, 30):
        s.at(ts, 'h')
        s.click(ts + 1, lay.command_grid[0])
    s.at(end - 2, 'shot=%s' % os.path.join(out, 'end.bmp'))
    return s.steps, end


def load_steps(kind, row, out, minutes):
    s = Script()
    t0 = screens.set_screen(s, 12, 800)
    s.at(t0 + 27, 'shot=%s' % os.path.join(out, 'load_list.bmp'))
    start = screens.saved_game(s, LAYOUT[kind], t0, row) + 62
    s.at(start, 'shot=%s' % os.path.join(out, 'loaded.bmp'))
    end = start + minutes * 60
    for ts in range(start + 5, int(end) - 5, 20):
        s.at(ts, 'h')
    s.at(end - 2, 'shot=%s' % os.path.join(out, 'load_end.bmp'))
    return s.steps, end


def civs_fn(kind, my_civ, civs, load):
    def fn(ctx, r):
        out = r.dir
        os.makedirs(out, exist_ok=True)
        name = 'aatest%s' % ('cc' if kind == 'cc' else 'gb')
        saved = os.path.join(r.game, 'SaveGame')
        for f in os.listdir(saved) if os.path.isdir(saved) else []:
            if f.lower().startswith(name):               # our own save from an earlier run: a real copy
                os.remove(os.path.join(saved, f))
        steps, end = civ_skirmish(kind, my_civ, civs, out, ctx.a.civ_minutes, name)
        rc, text = run_host(ctx, r, kind, steps, end + 5)
        expect_exit(r, rc)
        events(r, text)
        r.metrics['civs'] = [my_civ] + civs
        for n, cap in (('setup', 'the setup: %s against %s' % (my_civ, ', '.join(civs))), ('start', 'the map'),
                       ('saved', 'after the save'), ('end', 'the end')):
            p = png_of(os.path.join(out, n + '.bmp'))
            r.image(p, cap)
            if not p:
                r.fail('no shot: %s' % cap)
        files = [f for f in os.listdir(saved) if f.lower().startswith(name)] if os.path.isdir(saved) else []
        if not files:
            return r.fail('no save %s in %s' % (name, saved))
        r.log('saved: %s' % ', '.join(files))
        if not load:
            return
        names = sorted(f.rsplit('.', 1)[0].lower() for f in os.listdir(saved) if f.lower().endswith(('.gax', '.gal', '.ga1', '.ga2')))
        row = names.index(name) if name in names else 0
        if row > 15:
            return r.fail('the save is row %d of the list: past what the list shows' % row)
        steps, end = load_steps(kind, row, out, 1.5)
        rc, text = run_host(ctx, r, kind, steps, end + 5, tag='load')
        expect_exit(r, rc, tag='load')
        events(r, text, 'load')
        if not re.search(r'open savegame\\%s\.\w+ -> ok' % name, text, re.I):
            r.fail('the save was not opened (%s)' % name)
        for n, cap in (('load_list', 'Saved Game: the list'), ('loaded', 'the save, loaded'), ('load_end', 'played on')):
            p = png_of(os.path.join(out, n + '.bmp'))
            r.image(p, cap)
            if not p or blank(p):
                r.fail('%s: no shot, or blank' % cap)
    fn.__doc__ = 'a short skirmish: %s against %s, saved%s (%s)' % (my_civ, ', '.join(civs), ' and loaded' if load else '',
                                                                    GAMES[kind]['title'])
    return fn


CIV_SETS = {
    'base': [('Galactic Empire', ['Gungans', 'Rebel Alliance'], True),
             ('Royal Naboo', ['Trade Federation', 'Wookiees'], False)],
    'cc': [('Republic', ['Confederacy', 'Galactic Empire'], True),
           ('Gungans', ['Rebel Alliance', 'Royal Naboo'], False),
           ('Trade Federation', ['Wookiees', 'Republic'], False)],
}
for _k, _sets in CIV_SETS.items():
    for _i, (_me, _them, _load) in enumerate(_sets):
        CHECKS.append(Check('civs/%s-%d' % (_k, _i + 1), 'functional', civs_fn(_k, _me, _them, _load), _k, 8.4 if _load else 5.0))


def fixed_setup(s, lay):
    """The frame-rate check's setup (perf_ab.py's too): one Easiest computer, a Tiny Savannah
    map. Returns when it is set."""
    t = screens.to_setup(s, lay, 13, 800)
    t = screens.pick(s, t, screens.DD_OTHERS, 0)          # one computer
    t = screens.pick(s, t, screens.DD_LOCATION, 13)       # Savannah
    t = screens.pick(s, t, screens.DD_SIZE, 0)            # Tiny
    return screens.pick(s, t, screens.DD_DIFFICULTY, 0)   # Easiest


def perf_fn(kind):
    def fn(ctx, r):
        """a fixed skirmish at --fps 60: CPU and presents a second against tests/perf_thresholds.json"""
        out = r.dir
        os.makedirs(out, exist_ok=True)
        s = Script()
        t = fixed_setup(s, LAYOUT[kind])
        s.click(t + 2, SETUP_START)
        start = t + 72
        end = start + ctx.a.perf_minutes * 60
        s.at(start, 'shot=%s' % os.path.join(out, 'start.bmp'))
        for ts in range(int(start) + 10, int(end) - 5, 30):  # the view moves now and then: frames change
            s.at(ts, 'h')
            s.at(ts + 15, 'move:%d:300' % (200 + (ts // 30 % 3) * 200))
        rc, text = run_host(ctx, r, kind, s.steps, end, extra=['--fps', '60', '--fps-log'])
        expect_exit(r, rc)
        events(r, text)
        r.image(png_of(os.path.join(out, 'start.bmp')), 'the fixed skirmish')
        f = fps_figures(text)
        if not f:
            return r.fail('no --fps-log seconds in game')
        r.metrics.update(f)
        th = json.load(open(os.path.join(ROOT, 'tests', 'perf_thresholds.json')))[kind]
        r.log('in game %d s: CPU %.1f%% of a core (median; 90th %.1f%%), presents %.1f/s, changed frames %.1f/s'
              % (f['seconds'], f['cpu_median'], f['cpu_p90'], f['presents_median'], f['changed_median']))
        if f['cpu_median'] > th['cpu_max']:
            r.fail('CPU %.1f%% of a core, the threshold is %.1f%%' % (f['cpu_median'], th['cpu_max']))
        if not th['presents_min'] <= f['presents_median'] <= th['presents_max']:
            r.fail('%.1f presents a second, outside %g..%g' % (f['presents_median'], th['presents_min'], th['presents_max']))
        # against the last accepted figures: worse by more than a quarter is reported, not failed
        base = os.path.join(ctx.baselines, 'perf', kind + '.json')
        if ctx.a.update_baselines or not os.path.exists(base):
            os.makedirs(os.path.dirname(base), exist_ok=True)
            json.dump(f, open(base, 'w'), indent=1)
            r.log('figures recorded as the baseline (%s)' % base)
        else:
            b = json.load(open(base))
            r.metrics['cpu_baseline'] = b['cpu_median']
            if f['cpu_median'] > b['cpu_median'] * 1.25 + 1:
                r.log('NOTE: CPU %.1f%% against %.1f%% in the baseline (more than a quarter up)' % (f['cpu_median'], b['cpu_median']))
    fn.__doc__ = 'a fixed skirmish at --fps 60: CPU %% and presents/s against thresholds (%s)' % GAMES[kind]['title']
    return fn


for _k in GAMES:
    CHECKS.append(Check('perf/' + _k, 'functional', perf_fn(_k), _k, 4.8))


def smooth_fn(kind):
    def fn(ctx, r):
        """--smooth: a worker walks to a rally point at --fps 120; drawn between steps"""
        import smooth_check
        out = r.dir
        os.makedirs(out, exist_ok=True)
        lay = LAYOUT[kind]
        s = Script()
        t = fixed_setup(s, lay)
        s.click(t + 2, SETUP_START)
        start = t + 72
        s.at(start, 'h')                                 # the command center, the view on it
        for k in range(3):
            s.click(start + 1 + k, lay.command_grid[0])  # a worker, three times
        s.at(start + 4, 'move:660:110')
        s.at(start + 4.5, 'rclick:660:110')             # its rally point
        s.at(start + 5, 'move:400:300')                 # off the screen's edges: the view stays
        # whoever stands at the rally point, selected, walks back and forth (its frame and bar too)
        for k, (x, y) in enumerate([(250, 330), (660, 110), (250, 330)]):
            t0 = start + 40 + 12 * k
            if k == 0:
                s.at(t0, 'move:560:40')
                s.at(t0 + 0.5, 'ldown:560:40')
                s.at(t0 + 1, 'move:770:190')
                s.at(t0 + 1.5, 'lup:770:190')
            s.at(t0 + 2, 'move:%d:%d' % (x, y))
            s.at(t0 + 2.5, 'rclick:%d:%d' % (x, y))
            s.at(t0 + 3, 'move:400:300')
        s.at(start + 70, 'shot=%s' % os.path.join(out, 'end.bmp'))
        end = start + 75
        log = os.path.join(out, 'draw.log')
        env = dict(os.environ, SWGB_SMOOTH_LOG=log)
        rc, text = run_host(ctx, r, kind, s.steps, end, extra=['--smooth', '--fps', '120'], env=env)
        expect_exit(r, rc)
        events(r, text)
        r.image(png_of(os.path.join(out, 'end.bmp')), "the walk's end")
        if '[smooth] first object drawn between steps' not in text:
            r.fail('no object was drawn between steps')
        res = smooth_check.analyse(log) if os.path.exists(log) else []
        if not res:
            return r.fail('no object moved (draw.log)')
        m = res[0]
        r.metrics.update({k: m[k] for k in ('steps', 'frames', 'plain_max', 'plain_still', 'smooth_max', 'smooth_p95', 'smooth_still')})
        r.log('the longest walk: %d steps, %d frames; plain: up to %.1f px a frame, still %.0f%% of frames; '
              '--smooth: up to %.1f px (95%%: %.1f), still %.0f%%' % (
                  m['steps'], m['frames'], m['plain_max'], 100 * m['plain_still'],
                  m['smooth_max'], m['smooth_p95'], 100 * m['smooth_still']))
        if m['steps'] < 10:
            r.fail('the longest walk is %d steps' % m['steps'])
        if m['smooth_p95'] > 3.0:
            r.fail('--smooth: 95%% of frames move up to %.1f px (want 3 or less)' % m['smooth_p95'])
        if m['smooth_still'] >= m['plain_still']:
            r.fail('--smooth: still in %.0f%% of frames, the plain draw %.0f%%' % (100 * m['smooth_still'], 100 * m['plain_still']))
        os.remove(log)   # tens of MB
    fn.__doc__ = '--smooth: a worker walks at --fps 120, drawn between the steps (%s)' % GAMES[kind]['title']
    return fn


for _k in GAMES:
    CHECKS.append(Check('smooth/' + _k, 'functional', smooth_fn(_k), _k, 3.5))
# ==== multiplayer: tools/mp_pair.py (docs/multiplayer.md) ========================================
# name, level, game, mp_pair.py arguments, minutes of game, its own port (four from it: two checks
# at once must not find each other's games), the check's budget in minutes. Each takes its game
# folder and DIR-mp2.. beside it (hard links).
MP_CHECKS = [
    ('mp/base-3p', 'functional', 'base', ['--players', '3', '--activity', 'busy', '--chat', '--drop', 'c@1'], 2, 2340, 9),
    ('mp/cc-restore', 'functional', 'cc', ['--players', '2', '--activity', 'busy', '--save', '0.75', '--restore'], 1.5, 2344, 13),
    ('soak/mp-base-4p', 'soak', 'base', ['--players', '4', '--activity', 'busy', '--chat'], 20, 2348, 30),
    ('soak/mp-base-quit', 'soak', 'base', ['--players', '3', '--activity', 'busy', '--quit', 'b@5'], 12, 2352, 22),
    ('soak/mp-base-host-drop', 'soak', 'base', ['--players', '3', '--activity', 'busy', '--drop', 'a@4'], 8, 2356, 18),
    ('soak/mp-base-restore', 'soak', 'base', ['--players', '3', '--activity', 'busy', '--save', '8', '--restore'], 15, 2360, 45),
    ('soak/mp-cc-4p', 'soak', 'cc', ['--players', '4', '--activity', 'busy', '--chat', '--drop', 'd@10'], 20, 2364, 30),
    ('soak/mp-cc-restore', 'soak', 'cc', ['--players', '3', '--activity', 'busy', '--save', '8', '--restore'], 15, 2368, 45),
]


def mp_fn(name, kind, extra, minutes, port, budget):
    def fn(ctx, r):
        cmd = [sys.executable, os.path.join(HERE, 'mp_pair.py')] + (['--cc'] if kind == 'cc' else []) + extra + \
              ['--minutes', '%g' % minutes, '--run', ctx.host(kind), '--exe', ctx.exe(kind), '--game', r.game,
               '--out', r.dir, '--port', str(port), '--seed', str(ctx.a.seed)]
        rc, out = run(ctx, r, cmd, budget * 60, 'mp_pair.log', cwd=ROOT)
        summ = os.path.join(r.dir, 'summary.json')
        if not os.path.exists(summ):
            return r.fail('mp_pair.py wrote no summary (exit %d)' % rc)
        res = json.load(open(summ))
        for g in res if isinstance(res, list) else [res]:
            sr = g['sync_records']
            r.log('%s: %d players, exit %s, sync records %s (%d shared, %d differ), %d turns checked%s' % (
                g['phase'], g['players'], g['exit'], {k: v for k, v in sr.items() if k not in ('shared', 'differ')},
                sr['shared'], sr['differ'], g['turns_checked'],
                ', left: %s' % g['left'] if g.get('left') else ''))
            if g.get('chat'):
                r.log('chat: %d lines received, %d drawn of %d' % (sum(v['received'] for v in g['chat'].values()),
                                                                 sum(bool(v['drawn']) for v in g['chat'].values()), len(g['chat'])))
            for p in g['problems'][:8]:
                r.fail('%s: %s' % (g['phase'], p))
            r.metrics['%s_shared_records' % g['phase']] = sr['shared']
        for d in sorted(os.listdir(r.dir)):
            for f in ('start.png', 'end.png'):
                if os.path.exists(os.path.join(r.dir, d, f)) and d in ('a', 'b', 'r_a'):
                    r.image(os.path.join(r.dir, d, f), '%s: %s' % (d, f[:-4]))
        if rc and r.status == 'PASS':
            r.fail('mp_pair.py: exit %d' % rc)
    fn.__doc__ = 'mp_pair.py %s, %g minutes (%s)' % (' '.join(extra), minutes, GAMES[kind]['title'])
    return fn


for _n, _lv, _k, _e, _m, _p, _b in MP_CHECKS:
    CHECKS.append(Check(_n, _lv, mp_fn(_n, _k, _e, _m, _p, _b), _k, _b))


# ==== L3: soak ===================================================================================
# name, game, soak.py arguments, minutes of game per run (before --soak-scale); for `missions`,
# per mission played. The missions before --first are passed through (started and won at once).
SOAKS = [
    ('skirmish', 'base', ['--mode', 'skirmish'], 30),
    ('monkey', 'base', ['--mode', 'monkey'], 30),
    ('campaign', 'base', ['--mode', 'campaign'], 25),
    ('defeat', 'base', ['--mode', 'defeat'], 20),
    ('allied', 'base', ['--mode', 'allied', '--allies', '6'], 15),
    ('victory-load', 'base', ['--mode', 'victory', '--after', '10'], 15),
    ('chain', 'base', ['--mode', 'chain', '--first', '1', '--last', '3'], 15),
    ('fight', 'base', ['--mode', 'fight', '--allies', '4'], 25),
    ('missions-oom9', 'base', ['--mode', 'missions', '--campaign', 'OOM-9', '--first', '6'], 8),
    ('missions-boss-nass', 'base', ['--mode', 'missions', '--campaign', 'Boss Nass', '--first', '2'], 6),
    ('missions-chewbacca', 'base', ['--mode', 'missions', '--campaign', 'Chewbacca', '--first', '2'], 6),
    ('missions-darth-vader', 'base', ['--mode', 'missions', '--campaign', 'Darth Vader', '--first', '2'], 6),
    ('missions-princess-leia', 'base', ['--mode', 'missions', '--campaign', 'Princess Leia', '--first', '2'], 6),
    ('missions-training', 'base', ['--mode', 'missions', '--campaign', 'Attichitcuk', '--first', '2', '--unlocked'], 6),
    ('ingame-load', 'base', ['--mode', 'ingame-load'], 20),
    ('monkey-1024', 'base', ['--mode', 'monkey', '--screen', '1024'], 25),
    ('monkey-1280', 'base', ['--mode', 'monkey', '--screen', '1280'], 25),
    ('skirmish', 'cc', ['--mode', 'skirmish', '--civs', 'Confederacy,Republic', '--my-civ', 'Republic'], 30),
    ('monkey', 'cc', ['--mode', 'monkey'], 30),
    ('campaign-confederacy', 'cc', ['--mode', 'campaign', '--campaign', 'Confederacy'], 25),
    # Galactic Republic opens once Confederacy is won: missions passes through Confederacy first.
    ('campaign-republic', 'cc', ['--mode', 'missions', '--campaign', 'Galactic Republic', '--first', '1', '--last', '1'], 25),
    ('skirmish-b', 'cc', ['--mode', 'skirmish', '--civs', 'Gungans,Wookiees,Trade Federation', '--my-civ', 'Confederacy'], 30),
    ('missions-confederacy', 'cc', ['--mode', 'missions', '--campaign', 'Confederacy', '--first', '2'], 6),
    ('missions-republic', 'cc', ['--mode', 'missions', '--campaign', 'Galactic Republic', '--first', '2'], 6),
    ('ingame-load', 'cc', ['--mode', 'ingame-load'], 20),
]
# soak.py's other endgame modes (defeat .. fight, chain) script the base game's screens and
# campaign: it refuses them with --cc.


def soak_missions(extra):
    """missions, chain: (played, passed through), from the campaign's table in screens.py."""
    if '--mode' not in extra or extra[extra.index('--mode') + 1] not in ('missions', 'chain'):
        return None
    camp = extra[extra.index('--campaign') + 1] if '--campaign' in extra else 'OOM-9'
    boxes = {**screens.BASE_MISSIONS, **screens.CC_MISSIONS}[camp]
    first = int(extra[extra.index('--first') + 1]) if '--first' in extra else 1
    last = int(extra[extra.index('--last') + 1]) if '--last' in extra else len(boxes)
    if '--unlocked' in extra:
        return last - first + 1, 0
    # Galactic Republic opens once Confederacy is won: soak.py passes through all of it first.
    before = len(screens.CC_MISSIONS['Confederacy']) if camp == 'Galactic Republic' else 0
    return last - first + 1, first - 1 + before


def soak_budget(name, extra, m):
    """Minutes a soak.py run may take: its game time and the menus, briefings and loads."""
    ms = soak_missions(extra)
    if ms:
        return ms[0] * (m + 6.5) + ms[1] * 4 + 3     # a game per mission: its menus too
    if name == 'victory-load':
        return m + 10 + 10
    return m + 10


def soak_fn(name, kind, extra, minutes):
    def fn(ctx, r):
        # whatever --soak-scale says: a defeat comes about 11 minutes in; a fight's first wave
        # sets off at about 4 and wins 1-8 minutes later (a 12-minute trial ran out at 0 enemy pixels)
        m = max({'defeat': 15, 'fight': 20}.get(name, 3), minutes * ctx.a.soak_scale)
        cmd = [sys.executable, os.path.join(HERE, 'soak.py')] + (['--cc'] if kind == 'cc' else []) + extra + \
              ['--minutes', '%g' % m, '--run', ctx.host(kind), '--exe', ctx.exe(kind), '--game', r.game,
               '--out', r.dir, '--seed', str(ctx.a.seed)]
        budget = soak_budget(name, extra, m)
        rc, out = run(ctx, r, cmd, budget * 60 + 600, 'soak.log', cwd=ROOT)
        if re.search(r'refused with --cc|--cc: --mode', out):
            return r.skip(out.strip().splitlines()[-1])
        summ = os.path.join(r.dir, 'summary.json')
        runs = json.load(open(summ)) if os.path.exists(summ) else []
        for m_ in runs:
            r.log('%s: %s, %ds, %d event(s), endings %s' % (m_['name'], m_['outcome'], m_['seconds'], len(m_['events']),
                                                         m_.get('endings')))
            for e in m_['events'][:6]:
                r.fail(e['line'])
            r.metrics.setdefault('game_seconds', 0)
            r.metrics['game_seconds'] += m_['seconds']
            for f in sorted(os.listdir(os.path.join(r.dir, m_['name']))) if os.path.isdir(os.path.join(r.dir, m_['name'])) else []:
                if f.endswith('.png') and (f.startswith(('shot_start', 'shot_end', 'score', 'game_over')) or f == 'fight-score.png'):
                    r.image(os.path.join(r.dir, m_['name'], f), '%s: %s' % (m_['name'], f[:-4]))
        if not runs:
            r.fail('soak.py wrote no summary (exit %d)' % rc)
        elif rc:
            r.fail('soak.py: exit %d (a run with a problem)' % rc)
        if name == 'victory-load' and runs and not any(k == 'victory' for _, k in runs[0].get('endings', [])):
            r.fail('no victory music: the cheat did not end the game')
        if name == 'fight' and runs and not any(k == 'victory' for _, k in runs[0].get('endings', [])):
            r.fail('no victory by combat in %g minutes' % m)
        ms = soak_missions(extra)
        if ms and runs:
            # One game per mission (soak.py). A won mission opens its end briefing; a lost one
            # (the game's own defeat while the monkey played) is a game outcome, not a fault,
            # and the missions after it stay shut in a fresh profile.
            won = sorted({w for m_ in runs for w in m_.get('missions_won', [])})
            r.metrics['missions_won'] = len(won)
            r.log('missions won: %s' % ', '.join(won))
            lost = False
            for m_ in runs:
                if m_.get('result') == 'lost':
                    lost = True
                    r.log('%s: lost (the game declared a defeat)' % m_['name'])
                elif m_.get('result') != 'won':
                    if lost:
                        r.log('%s: not open after the defeat' % m_['name'])
                    else:
                        r.fail('%s: neither won nor lost' % m_['name'])
        if name == 'ingame-load' and runs and not runs[0].get('save_reads'):
            r.fail('the in-game Load did not read the save back')
        if name == 'defeat' and runs and not any(k == 'defeat' for _, k in runs[0].get('endings', [])):
            r.fail('no defeat')
    fn.__doc__ = 'soak.py %s for %d minutes (%s)' % (' '.join(extra), minutes, GAMES[kind]['title'])
    return fn


for _n, _k, _e, _m in SOAKS:
    CHECKS.append(Check('soak/%s-%s' % (_k, _n), 'soak', soak_fn(_n, _k, _e, _m), _k,
                        soak_budget(_n, _e, _m) + (2 if _n == 'victory-load' else 0)))


# ==== L4: the real screen ========================================================================
@check('real/present', 'real', game='base', minutes=6, needs=('win',))
def real_present(ctx, r):
    """present_check_real.ps1: the real window, mouse and screen (takes them over for ~5 minutes)"""
    if not ctx.a.real_screen:
        return r.skip('needs the user at the screen: run with --real-screen when they say so')
    cmd = ['powershell', '-ExecutionPolicy', 'Bypass', '-File', os.path.join(HERE, 'present_check_real.ps1'), '-Yes',
           '-Exe', ctx.host('base'), '-Game', r.game, '-Lift', ctx.exe('base'), '-Out', r.dir]
    rc, out = run(ctx, r, cmd, 20 * 60, 'present_check_real.log', cwd=ROOT)
    for l in out.splitlines():
        if re.match(r'\s*(PASS|FAIL)', l):
            r.log(l.strip())
    if rc:
        r.fail('present_check_real.ps1: exit %d' % rc)


# ==== the run ====================================================================================
def select(a):
    top = LEVELS.index(a.level)
    chosen = []
    for c in CHECKS:
        lv = LEVELS.index(c.level)
        if a.just and lv != top:
            continue
        if lv > top or (c.level == 'real' and a.level != 'real'):
            continue
        if a.level == 'real' and c.level != 'real' and not a.with_lower:
            continue
        if c.game == 'cc' or 'cc' in c.needs:
            if a.cc is False:
                continue
        if a.only and not any(fnmatch.fnmatch(c.name, p) or p in c.name for p in a.only):
            continue
        chosen.append(c)
    return chosen


def run_check(ctx, c, attempt=0):
    r = Result(c, ctx.results, attempt)
    os.makedirs(r.dir, exist_ok=True)
    t0 = time.time()
    kind = c.game
    try:
        if ctx.cancelled:
            r.skip('cancelled')
        elif 'win' in c.needs and not WIN:
            r.skip('the Windows host\'s presenter (GDI/Direct3D, the real screen): win32hle has its own (screen.c)')
        elif kind and not ctx.has(kind) and kind == 'cc' and ctx.a.cc is True:
            r.fail('--cc, and no Clone Campaigns host or game data: %s, %s' % (ctx.host(kind), ctx.exe(kind)))
        elif kind and not ctx.has(kind):
            r.skip('no %s host or game data: %s, %s' % (GAMES[kind]['title'], ctx.host(kind), ctx.exe(kind)))
        elif 'cc' in c.needs and not ctx.has('cc') and ctx.a.cc is not True:
            r.skip('no Clone Campaigns build here')
        else:
            if kind:
                r.game = ctx.acquire(kind)
                r.log('game folder: %s' % r.game)
            try:
                c.fn(ctx, r)
            finally:
                if kind:
                    ctx.release(kind, r.game)
    except Exception:                                    # noqa: BLE001 -- a broken check is reported, not fatal
        r.status = 'ERROR'
        r.lines.append(traceback.format_exc())
    r.seconds = time.time() - t0
    return r


def take_lock(a):
    """One run at a time in a set of game folders: two games in one folder share its saves,
    profile and registry hive (two runs at once made present_check fail every edge)."""
    os.makedirs(a.games, exist_ok=True)
    lock = os.path.join(a.games, '%s.lock' % a.prefix)
    try:
        pid = int(open(lock).read().split()[0])
        if WIN:
            alive = subprocess.run(['tasklist', '/FI', 'PID eq %d' % pid, '/NH'], capture_output=True, text=True).stdout
        else:
            try:
                alive = '%d ' % pid + open('/proc/%d/cmdline' % pid, 'rb').read().replace(b'\0', b' ').decode(errors='replace')
            except OSError:
                alive = ''
        if pid != os.getpid() and str(pid) in alive and 'python' in alive.lower():
            print('another test.py run (PID %d) is using %s: wait for it, or pass --prefix NAME'
                  % (pid, os.path.join(a.games, a.prefix + '-*')))
            return False
    except (OSError, ValueError, IndexError):
        pass
    with open(lock, 'w') as f:
        f.write('%d %s\n' % (os.getpid(), datetime.datetime.now().isoformat(timespec='seconds')))
    import atexit
    atexit.register(lambda: os.path.exists(lock) and open(lock).read().startswith('%d ' % os.getpid()) and os.remove(lock))
    return True


def git(*args):
    try:
        return subprocess.run(['git', '-C', ROOT] + list(args), capture_output=True, text=True).stdout.strip()
    except OSError:
        return ''


def sha256(path):
    if not os.path.exists(path):
        return None
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()[:16]


def report(ctx, results, meta):
    out = ctx.results
    counts = {s: sum(1 for r in results if r.status == s) for s in ('PASS', 'FLAKY', 'FAIL', 'ERROR', 'SKIP')}
    meta['counts'] = counts
    meta['ok'] = not counts['FAIL'] and not counts['ERROR']
    json.dump(dict(meta, checks=[r.as_dict() for r in results]), open(os.path.join(out, 'summary.json'), 'w'), indent=1)
    # Markdown
    md = ['# Test run: %s (%s)' % (meta['level'], 'PASS' if meta['ok'] else 'FAIL'), '',
          '%s, %s, %.1f min, %d jobs. Build `%s`. Commit `%s`%s.' % (
              meta['started'], meta['host'], meta['seconds'] / 60, meta['jobs'], meta['build'], meta['commit'],
              ' (with local changes)' if meta['dirty'] else ''), '',
          '**%d passed, %d flaky (failed, then passed), %d failed, %d errors, %d skipped.**' % (
              counts['PASS'], counts['FLAKY'], counts['FAIL'], counts['ERROR'], counts['SKIP']), '',
          '| Check | Level | Result | Time | What |', '|---|---|---|---|---|']
    for r in results:
        md.append('| %s | %s | **%s** | %.0f s | %s |' % (r.name, r.check.level, r.status, r.seconds, r.check.doc.replace('|', '/')))
    for r in results:
        md += ['', '## %s: %s' % (r.name, r.status), '']
        md += ['    ' + l for ll in r.lines for l in ll.splitlines()]
        if r.metrics:
            md += ['', '`' + json.dumps(r.metrics) + '`']
        for p, cap in r.images:
            md += ['', '![%s](%s)' % (cap, p)]
    open(os.path.join(out, 'report.md'), 'w', encoding='utf-8').write('\n'.join(md) + '\n')
    # HTML: the same, with the shots in a grid
    e = html.escape
    rows = ''.join('<tr class="%s"><td><a href="#%s">%s</a></td><td>%s</td><td><b>%s</b></td><td>%.0f s</td><td>%s</td></tr>'
                   % (r.status.lower(), e(r.name), e(r.name), r.check.level, r.status, r.seconds, e(r.check.doc)) for r in results)
    secs = ''
    for r in results:
        imgs = ''.join('<figure><a href="%s"><img src="%s" loading="lazy"></a><figcaption>%s</figcaption></figure>'
                       % (e(p), e(p), e(c)) for p, c in r.images)
        secs += ('<section id="%s" class="%s"><h2>%s <span>%s</span></h2><pre>%s</pre>%s<div class="shots">%s</div></section>'
                 % (e(r.name), r.status.lower(), e(r.name), r.status, e('\n'.join(r.lines)),
                    '<p class="m">%s</p>' % e(json.dumps(r.metrics)) if r.metrics else '', imgs))
    page = '''<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Test run %(level)s</title><style>
:root{--bg:#fff;--fg:#1b1b1b;--mut:#666;--line:#ddd;--pass:#1a7f37;--fail:#cf222e;--skip:#9a6700}
@media (prefers-color-scheme:dark){:root{--bg:#111;--fg:#e8e8e8;--mut:#999;--line:#333;--pass:#3fb950;--fail:#f85149;--skip:#d29922}}
body{background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif;margin:0 auto;max-width:1200px;padding:16px}
table{border-collapse:collapse;width:100%%}td,th{border-bottom:1px solid var(--line);padding:4px 8px;text-align:left;vertical-align:top}
tr.pass b,section.pass h2 span{color:var(--pass)}tr.fail b,tr.error b,section.fail h2 span,section.error h2 span{color:var(--fail)}
tr.skip b,section.skip h2 span,tr.flaky b,section.flaky h2 span{color:var(--skip)}pre{white-space:pre-wrap;color:var(--mut);font-size:12px}
.shots{display:flex;flex-wrap:wrap;gap:8px}figure{margin:0;width:280px}img{width:100%%;border:1px solid var(--line)}
figcaption{font-size:12px;color:var(--mut)}.m{font-family:monospace;font-size:12px;color:var(--mut)}a{color:inherit}
</style></head><body><h1>%(title)s</h1><p>%(sub)s</p><table><tr><th>Check</th><th>Level</th><th>Result</th><th>Time</th><th>What</th></tr>%(rows)s</table>%(secs)s</body></html>'''
    open(os.path.join(out, 'report.html'), 'w', encoding='utf-8').write(page % {
        'level': e(meta['level']), 'title': 'Test run: %s &mdash; %s' % (e(meta['level']), 'PASS' if meta['ok'] else 'FAIL'),
        'sub': e('%s, %.1f min. %d passed, %d flaky, %d failed, %d errors, %d skipped. Build %s, commit %s.' % (
            meta['started'], meta['seconds'] / 60, counts['PASS'], counts['FLAKY'], counts['FAIL'], counts['ERROR'], counts['SKIP'],
            meta['build'], meta['commit'])),
        'rows': rows, 'secs': secs})
    return meta['ok']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--level', default='smoke', help='unit, smoke, functional, soak or real (L0-L4)')
    ap.add_argument('--just', action='store_true', help='only this level\'s checks, not the ones below it')
    ap.add_argument('--with-lower', action='store_true', help='--level real: the lower levels too')
    ap.add_argument('--build', default=PATHS.build,
                    help='the folder with swgbrecomp.exe (and swgbrecomp_cc.exe); Linux: build-linux/')
    ap.add_argument('--build-cc', default=None, help='the folder with swgbrecomp_cc.exe, when not --build')
    cc = ap.add_mutually_exclusive_group()
    cc.add_argument('--cc', dest='cc', action='store_true', default=None, help='Clone Campaigns too (a missing host fails)')
    cc.add_argument('--no-cc', dest='cc', action='store_false', help='the base game only')
    ap.add_argument('--jobs', type=int, default=0,
                    help='games at once (default: 5, at most %d: SWGB_MAX_JOBS or local.json\'s max_jobs)' % PATHS.max_jobs)
    ap.add_argument('--only', default='', help='checks by name or glob, comma-separated (e.g. "skirmish/*,unit/keys")')
    ap.add_argument('--list', action='store_true', help='list the checks and stop')
    ap.add_argument('--results', default=None, help='the results folder (default %s)' % os.path.join(PATHS.results, '<time>-<level>'))
    ap.add_argument('--baselines', default=PATHS.baselines)
    ap.add_argument('--update-baselines', action='store_true', help='record every baseline again from this run')
    ap.add_argument('--main', default=PATHS.main, help='the main checkout: the lift (src/recomp/gen*) and work/lift_stats*.json')
    ap.add_argument('--exe', default=PATHS.exe('base'))
    ap.add_argument('--exe-cc', default=PATHS.exe('cc'))
    ap.add_argument('--game-template', default=PATHS.template, help='the shared install the game folders link to')
    ap.add_argument('--games', default=PATHS.games, help='where the testing-* game folders go')
    ap.add_argument('--prefix', default='testing', help='the game folders are GAMES/PREFIX-<base|cc>-<k>: a second run at once needs another')
    ap.add_argument('--cc-profiles', default=PATHS.cc_profiles,
                    help='Clone Campaigns: the player.nf* (an 800x600 profile) its game folders start with')
    ap.add_argument('--pcrecomp', default=None)
    ap.add_argument('--soak-scale', type=float, default=1.0, help='soak: each run\'s minutes times this')
    ap.add_argument('--civ-minutes', type=float, default=2.5, help='functional: minutes of each civilization skirmish')
    ap.add_argument('--perf-minutes', type=float, default=2.5, help='functional: minutes of the frame-rate skirmish')
    ap.add_argument('--seed', type=int, default=20261122, help='soak.py --seed')
    ap.add_argument('--retry', type=int, default=1, help='run a failed game check again this many times (FLAKY if it passes)')
    ap.add_argument('--real-screen', action='store_true',
                    help='L4: the user said the screen, mouse and keyboard may be taken over now')
    a = ap.parse_args()
    a.level = ALIASES.get(a.level.lower(), a.level.lower())
    if a.level not in LEVELS:
        ap.error('--level: one of %s' % ', '.join(LEVELS))
    a.only = [p for p in a.only.split(',') if p]
    a.jobs = min(PATHS.max_jobs, a.jobs or 5)
    a.build = os.path.abspath(a.build)
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    a.results = os.path.abspath(a.results or os.path.join(PATHS.results, '%s-%s' % (stamp, a.level)))
    checks = select(a)
    if a.list:
        for c in checks:
            print('%-28s %-10s %-4s %5.1f min  %s' % (c.name, c.level, c.game or '', c.minutes, c.doc))
        print('%d checks, about %.0f game-minutes' % (len(checks), sum(c.minutes for c in checks if c.game)))
        return 0
    sys.stdout.reconfigure(line_buffering=True)
    if any(c.game for c in checks) and not take_lock(a):
        return 2
    os.makedirs(a.results, exist_ok=True)
    ctx = Ctx(a)
    print('test: %s, %d checks, %d games at once, build %s -> %s' % (a.level, len(checks), a.jobs, a.build, a.results))
    t0 = time.time()
    results = []
    # the units and the lift first (no game folder), then the games, longest first
    plain = [c for c in checks if not c.game]
    games = sorted((c for c in checks if c.game), key=lambda c: -c.minutes)
    lock = threading.Lock()

    def one(c, attempt=0):
        r = run_check(ctx, c, attempt)
        with lock:
            if not attempt:                  # a retry replaces its first try (retry_failed)
                results.append(r)
            print('  %-5s %-28s %6.0f s  %s' % (r.status, r.name + ('  (retry %d)' % attempt if attempt else ''), r.seconds,
                                               next((l for l in r.lines if l.startswith(('FAIL', 'SKIP'))), '')[:110]))
        return r

    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
            list(ex.map(one, plain))
        # a thread per game slot: each kind has --jobs folders, and at most --jobs games run at once
        with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            list(ex.map(one, games))
        # a game check that failed is run again (--retry), in a folder of its own
        retry_failed(ctx, results, a.retry, one)
    except KeyboardInterrupt:
        print('interrupted: ending the processes this run started')
        ctx.stop_all()
    order = {c.name: k for k, c in enumerate(checks)}
    results.sort(key=lambda r: order.get(r.name, 1 << 30))
    meta = {'level': a.level, 'started': datetime.datetime.fromtimestamp(t0).isoformat(timespec='seconds'),
            'seconds': round(time.time() - t0), 'jobs': a.jobs, 'build': a.build, 'host': os.environ.get('COMPUTERNAME') or ('' if WIN else os.uname().nodename),
            'commit': git('rev-parse', '--short', 'HEAD'), 'branch': git('rev-parse', '--abbrev-ref', 'HEAD'),
            'dirty': bool(git('status', '--porcelain', '--untracked-files=no')),
            'hosts': {k: sha256(ctx.host(k)) for k in GAMES}, 'results': a.results}
    ok = report(ctx, results, meta)
    c = meta['counts']
    print('%s: %d passed, %d flaky, %d failed, %d errors, %d skipped in %.1f min; %s' % (
        'PASS' if ok else 'FAIL', c['PASS'], c['FLAKY'], c['FAIL'], c['ERROR'], c['SKIP'], meta['seconds'] / 60,
        os.path.join(a.results, 'report.html')))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
