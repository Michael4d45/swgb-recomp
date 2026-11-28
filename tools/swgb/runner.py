"""How the tools run the host: where its files are (paths), its command line, its game folders,
its --keys scripts and shots, what fails a run, its exit codes, and how a run is stopped."""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
from dataclasses import dataclass, fields

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(TOOLS)
WIN = os.name == 'nt'
HOSTS = {'base': 'swgbrecomp', 'cc': 'swgbrecomp_cc'}
GUEST_EXES = {'base': 'Battlegrounds.exe', 'cc': 'battlegrounds_x1.exe'}

# What the host prints when the game reaches code it cannot run: each one fails a run.
EVENTS = re.compile(r'ICALL: unresolved|ITAIL: unresolved|=== fault|\[not-lifted\]|=== unresolved|'
                    r'\[coverage\]|\[eh32\] funclet .* not lifted|STUB:')
# The host's exit codes (-1: a tool ended a run that went past its time).
EXIT = {0: 'exited', 2: 'not-lifted stub', 3: 'fault', 4: 'survived (watchdog)', 5: 'unresolved icall',
        -1: 'timed out'}


# ---- where things are ----------------------------------------------------------------------------
def host_name(kind):
    """The host's file name in a build folder."""
    return HOSTS[kind] + ('.exe' if WIN else '')


@dataclass(frozen=True)
class Paths:
    star: str           # the shared workspace (SWGB_STAR, or local.json's "star"); None without one
    main: str           # the checkout with the lift (src/recomp/gen*), work/*.exe and game/
    build: str          # the folder with the hosts
    games: str          # the game folders the tools make (hard links to MAIN/game)
    baselines: str      # test.py's baselines
    results: str        # test.py's results
    cc_profiles: str    # a folder with a Clone Campaigns profile at 800x600 (player.nf*) for its game
                        # folders; None: the install's own (GAMES/clone-campaigns when there is one)
    max_jobs: int       # games at once, at most, whatever a tool is asked

    @property
    def template(self):
        """The installed game every game folder is made from."""
        return os.path.join(self.main, 'game')

    def host(self, kind):
        return os.path.join(self.build, host_name(kind))

    def exe(self, kind):
        return os.path.join(self.main, 'work', GUEST_EXES[kind])

    def game(self, name, kind):
        """A tool's own game folder, GAMES/NAME-KIND, made on first use."""
        dst = os.path.join(self.games, '%s-%s' % (name, kind))
        ensure_game(self.template, dst, self.cc_profiles if kind == 'cc' else None)
        return dst


def linked_main(root=ROOT):
    """The repository's main checkout when ROOT is a linked worktree (git worktree add), else
    None: its tools/local.json then holds for every worktree of it."""
    dotgit = os.path.join(root, '.git')
    if not os.path.isfile(dotgit):
        return None
    with open(dotgit) as f:
        gitdir = f.read().split('gitdir:', 1)[1].strip()
    gitdir = os.path.join(root, gitdir)
    common = os.path.join(gitdir, 'commondir')
    if not os.path.isfile(common):          # a submodule's checkout, not a worktree
        return None
    with open(common) as f:
        return os.path.dirname(os.path.normpath(os.path.join(gitdir, f.read().strip())))


def local_config(root=ROOT):
    """tools/local.json (git ignores it) of this checkout, or else of the main checkout this one
    is a worktree of: {} when there is none."""
    for r in (root, linked_main(root)):
        p = r and os.path.join(r, 'tools', 'local.json')
        if p and os.path.isfile(p):
            with open(p) as f:
                cfg = json.load(f)
            unknown = set(cfg) - {f.name for f in fields(Paths)}
            if unknown:
                raise ValueError('%s: unknown keys %s (known: %s)' % (p, ', '.join(sorted(unknown)),
                                                                        ', '.join(f.name for f in fields(Paths))))
            return cfg
    return {}


def paths(env=os.environ, root=ROOT):
    """Every default path of the tools, from one place.

    A shared workspace (SWGB_STAR, or "star" in tools/local.json) is laid out as
    docs/parallel-work.md describes: STAR/swgb-recomp is the main checkout, STAR/builds/
    integration its build, and STAR/games, STAR/test-baselines, STAR/test-results. Without
    one, everything is this checkout's: build/ (build-linux/, build-macos/), game/, work/,
    and work/games, work/test-baselines, work/test-results. local.json may set any field of
    Paths itself; SWGB_MAX_JOBS sets max_jobs (default: the machine's cores)."""
    cfg = local_config(root)
    star = env.get('SWGB_STAR') or cfg.get('star')
    if star:
        d = dict(main=os.path.join(star, 'swgb-recomp'), build=os.path.join(star, 'builds', 'integration'),
                 games=os.path.join(star, 'games'), baselines=os.path.join(star, 'test-baselines'),
                 results=os.path.join(star, 'test-results'))
    else:
        work = os.path.join(root, 'work')
        d = dict(main=root, games=os.path.join(work, 'games'), baselines=os.path.join(work, 'test-baselines'),
                 results=os.path.join(work, 'test-results'),
                 build=os.path.join(root, 'build' if WIN else 'build-macos' if sys.platform == 'darwin' else 'build-linux'))
    d.update(star=star, max_jobs=os.cpu_count() or 1)
    d.update({k: v for k, v in cfg.items() if k != 'star'})
    if 'cc_profiles' not in d:              # the folder docs/testing.md has one made in, once it is
        cc = os.path.join(d['games'], 'clone-campaigns')
        d['cc_profiles'] = cc if os.path.isdir(cc) else None
    if env.get('SWGB_MAX_JOBS'):
        d['max_jobs'] = int(env['SWGB_MAX_JOBS'])
    return Paths(**d)


# ---- game folders --------------------------------------------------------------------------------
MARKER = 'swgb-testing.txt'                 # written last: a folder without it is (re)completed


def ensure_game(template, dst, profiles=None):
    """A game folder of its own: hard links to TEMPLATE (never written: the game only reads
    them), real copies of what the game writes (SaveGame, Scenario, player.nf*), as
    worktree_env.ps1 makes them. A copy that was cut short is completed on the next call.
    PROFILES: a folder whose player.nf* to use instead of the template's (Clone Campaigns:
    the Steam install's profile was saved by its widescreen-mod exe at 2560x1440, and the
    game's Options dialog switches to that size)."""
    if os.path.exists(os.path.join(dst, MARKER)):
        return
    if not os.path.isdir(template):
        raise FileNotFoundError('no game to make %s from: %s' % (dst, template))
    for dirpath, _, files in os.walk(template):
        rel = os.path.relpath(dirpath, template)
        top = rel.split(os.sep)[0]
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in files:
            src, d = os.path.join(dirpath, f), os.path.join(dst, rel, f)
            if os.path.exists(d):
                continue
            if top in ('SaveGame', 'Scenario') or (rel == '.' and f.lower().startswith('player.nf')):
                shutil.copy2(src, d)
            else:
                os.link(src, d)
    if profiles:
        seed_profiles(dst, profiles)
    with open(os.path.join(dst, MARKER), 'w') as f:
        f.write('template: %s\nprofiles: %s\n' % (template, profiles or template))


def seed_profiles(dst, profiles):
    """The player profiles (player.nf*) of folder PROFILES, copied into DST."""
    found = [f for f in os.listdir(profiles) if f.lower().startswith('player.nf')] if os.path.isdir(profiles) else []
    if not found:
        raise FileNotFoundError('no player profile (player.nf*) in %s: make one as docs/testing.md says' % profiles)
    for f in found:
        shutil.copy2(os.path.join(profiles, f), os.path.join(dst, f))


# ---- a run ----------------------------------------------------------------------------------------
def host_cmd(host, exe, game, watchdog, keys, *extra):
    """The host's command line for a scripted run: headless, --multi (its own registry hive and
    no one-copy check: other games run beside it), the --keys script in file KEYS."""
    return [host, '--run', '--multi', '--headless', '--exe', exe, '--game', game,
            '--watchdog', str(int(watchdog)), '--keys', '@' + keys] + list(extra)


def popen(cmd, **kw):
    """A process of a tool's, in a process group of its own off Windows (kill_tree ends it with
    its children)."""
    if not WIN:
        kw['start_new_session'] = True
    return subprocess.Popen(cmd, **kw)


def kill_tree(p):
    """End process P and its children, by its PID: never by name (the user may be playing)."""
    if p.poll() is not None:
        return
    if WIN:
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(p.pid)], capture_output=True)
    else:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError:              # it ended between the poll and the kill
            pass


def events(text):
    """The lines of a log that fail a run."""
    return [l.strip() for l in text.splitlines() if EVENTS.search(l)]


def modes(text):
    """The SetDisplayMode calls of a log, as (w, h): softdd's (W, H, B) and win32hle's WxHxB."""
    return [(int(x), int(y)) for x, y in re.findall(r'SetDisplayMode\((\d+)(?:, |x)(\d+)', text)]


class Script:
    """A --keys script: (seconds, step) pairs, written in time order. A click moves the cursor
    to its place itself."""

    def __init__(self):
        self.steps = []

    def at(self, t, what):
        self.steps.append((round(t, 2), what))

    def click(self, t, xy, button='click'):
        self.at(t, '%s:%d:%d' % (button, xy[0], xy[1]))

    def move(self, t, xy):
        self.at(t, 'move:%d:%d' % (xy[0], xy[1]))

    def shot(self, t, path):
        """The game's frame as a BMP at PATH."""
        self.at(t, 'shot=' + path)
        return path

    def end(self):
        return max((t for t, _ in self.steps), default=0)

    def text(self):
        return '\n'.join('%g:%s' % st for st in sorted(self.steps, key=lambda st: st[0]))

    def write(self, path):
        with open(path, 'w', newline='\n') as f:
            f.write(self.text() + '\n')
        return path


# ---- shots ----------------------------------------------------------------------------------------
def png_of(bmp):
    """The host's BMP as a PNG beside it (the BMP removed). None when there is none."""
    if not os.path.exists(bmp):
        return None
    from PIL import Image
    png = bmp[:-4] + '.png'
    # No name kept for the image: PIL maps a BMP's file, and on Windows the map (alive as long as
    # the image is) keeps the BMP from being removed.
    Image.open(bmp).save(png)
    os.remove(bmp)
    return png


def to_png(folder):
    """Every BMP in FOLDER as a PNG (png_of)."""
    return [png_of(os.path.join(folder, f)) for f in sorted(os.listdir(folder)) if f.lower().endswith('.bmp')]


if __name__ == '__main__':
    # `py -3 tools/swgb/runner.py`: paths() as JSON, for the PowerShell tools (worktree_env.ps1, ...)
    P = paths()
    print(json.dumps(dict({f.name: getattr(P, f.name) for f in fields(Paths)}, template=P.template), indent=1))
