"""Scripted skirmishes at the game's resolutions, headless.

  py -3 tools/res_check.py 800|1024|1280 [--out DIR] [--game DIR] [--no-options]
  py -3 tools/res_check.py ingame [--res WxH] [--cc] [--out DIR] [--game DIR]
  py -3 tools/res_check.py popups [--size 800|1024|1280|wide] [--res WxH] [--cc] [--out DIR] [--game DIR]

800, 1024, 1280 (800x600, 1024x768, 1280x1024): the run picks the Screen Size on
the main menu's Options, starts a Standard Game and takes shots:

  a_start     the map after loading
  b_selected  the command center clicked
  c_left .. f_bottom   the cursor held on each edge of the map view (scrolls)
  g_minimap   a click on the minimap
  h_menu      the game menu (the HUD's Menu button)
  i_score     after Quit Current Game and Yes: the score screen (800x600)
  j_menus     its Main Menu button: back on the main menu (800x600)

It reads the log for the SetDisplayMode calls and prints them: the mode must
be SIZE in the skirmish and 800x600 on the menus.

ingame: a skirmish at 800x600, then the game menu's Options switches the
Screen Size in the game: to 1024x768, to the third size (1280x1024, or WxH
with --res), back to 800x600. After each switch the HUD's Menu button opens
the game menu, which must be centred on the new screen (it once stopped
answering after a switch to 1024x768); then Quit Current Game, the score
screen and Main Menu, as above. Shots: a_start, NN_<size>, NN_<size>_menu,
i_score, j_menus.

popups: a skirmish at --size (wide: the third size, WxH with --res), then
every window it opens over the map, each shot before and after: the game menu,
Objectives, Chat, Diplomacy (dialogs: centred on the screen), the tech tree
(the whole screen), Save Game with a name typed and OK, Load (Yes, then the
Saved Game screen and Cancel) and About. At WxH the Save and Load screens are
the largest of the game's sizes that fits, centred, black around them
(widescreen.c, "full screens shown in a game").

Each check prints PASS or FAIL lines; the exit code is 1 on any FAIL. Shots
land in DIR as BMP and PNG (ffmpeg). The game runs with --headless --multi, so
it has its own registry options (see host.c): a Screen Size it sets stays for
this --game folder only.
"""
import argparse, os, re, subprocess, sys

SIZES = {800: (800, 600), 1024: (1024, 768), 1280: (1280, 1024)}
RADIO = {800: (453, 383), 1024: (453, 413), 1280: (453, 443)}   # on the 800x600 Options menu
# Where things sit in game pixels, by size: the command center after loading, the
# minimap's middle and the half-width of its diamond.
CC = {800: (400, 170), 1024: (500, 300), 1280: (610, 340)}
MINIMAP = {800: (604, 550, 120), 1024: (772, 700, 150), 1280: (965, 920, 200)}
MENU = {800: (765, 480), 1024: (980, 624), 1280: (1230, 850)}   # the HUD's Menu button
# The HUD's buttons on the right, top to bottom: Menu, Objectives, Chat, Diplomacy, Tech Tree.
HUD_X = {800: 768, 1024: 983, 1280: 1230}
HUD_Y = {800: (480, 502, 526, 552, 577), 1024: (625, 653, 678, 705, 732), 1280: (852, 880, 908, 937, 966)}
# The game menu's buttons, from the screen's middle: Quit Current Game, Save, Load, Options, About.
GM_QUIT, GM_SAVE, GM_LOAD, GM_OPTIONS, GM_ABOUT = -122, -82, -49, 17, 50
# The in-game Options dialog at 800x600 (centred on bigger screens): the Screen Size
# radio buttons and OK.
IG_RADIO_X, IG_RADIO_Y, IG_OK = 133, {800: 318, 1024: 353, 1280: 388}, (305, 455)
STANDARD = {'base': (600, 168), 'cc': (603, 228)}   # Single Player -> Standard Game
SAFE = (400, 300)   # inside every screen size, away from its edges (edge scrolling)
# What fails a run (tools/test.py's EVENTS): a fault, an unresolved call, a stub.
EVENTS = re.compile(r'ICALL: unresolved|ITAIL: unresolved|=== fault|\[not-lifted\]|=== unresolved|'
                    r'\[coverage\]|\[eh32\] funclet .* not lifted|STUB:')


def fit(w, h):
    """The largest of the game's own sizes within w x h."""
    return (1280, 1024) if w >= 1280 and h >= 1024 else (1024, 768) if w >= 1024 and h >= 768 else (800, 600)


def hud_base(w, h):
    """The size the HUD is laid out at on a w x h screen (widescreen.c: g_base_w)."""
    bw = 1280 if w >= 1280 and h >= 720 else 1024 if w >= 1024 and h >= 700 else 800
    return bw, SIZES[bw][1]


def hud_button(w, h, i):
    """The HUD's button i (0 Menu .. 4 Tech Tree) on a w x h screen: the base layout
    at the bottom, centred (widescreen.c)."""
    bw, bh = hud_base(w, h)
    return HUD_X[bw] + (w - bw) // 2, HUD_Y[bw][i] + h - bh


class Script:
    """A --keys script: (seconds, step) pairs; a click is a move, then the click."""
    def __init__(self, out):
        self.s, self.out = [], out

    def add(self, t, step):
        self.s.append((t, step))

    def click(self, t, x, y):
        self.s += [(t - 0.5, 'move:%d:%d' % (x, y)), (t, 'click:%d:%d' % (x, y))]

    def move(self, t, x, y):
        self.s.append((t, 'move:%d:%d' % (x, y)))

    def shot(self, t, name):
        self.s.append((t, 'shot=%s' % os.path.join(self.out, name + '.bmp')))

    def text(self):
        return ','.join('%g:%s' % s for s in sorted(self.s, key=lambda s: s[0]))


def start_skirmish(sc, radio, standard):
    """Options -> the Screen Size radio button RADIO (or none), Single Player, Standard
    Game, Start Game. Returns the time the map is up."""
    t = 20
    if radio:
        sc.click(13, 40, 490)
        sc.click(20, *radio)
        t = 25
    sc.click(t, 345, 250)
    sc.click(t + 8, *standard)
    sc.click(t + 17, 118, 568)
    return t + 64


def run_game(a, sc, watchdog, extra=()):
    log = os.path.join(a.out, 'run.log')
    cmd = [a.run, '--run', '--multi', '--headless', '--exe', a.exe, '--game', a.game,
           '--watchdog', str(int(watchdog)), '--keys', sc.text()] + list(extra)
    with open(log, 'w') as f:
        rc = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT).returncode
    text = open(log, errors='replace').read()
    for f in sorted(os.listdir(a.out)):
        if f.endswith('.bmp'):
            subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', os.path.join(a.out, f),
                            os.path.join(a.out, f[:-4] + '.png')])
    return rc, text


def modes_of(text):
    return [(int(x), int(y)) for x, y, _ in re.findall(r"SetDisplayMode\((\d+)(?:, |x)(\d+)(?:, |x)(\d+)\)", text)]


# ---- reading the shots ------------------------------------------------------------------------
def grey(a, name):
    p = os.path.join(a.out, name + '.bmp')
    if not os.path.exists(p):
        return None
    import numpy as np
    from PIL import Image
    return np.asarray(Image.open(p).convert('L'), dtype=np.int16)


def popup_box(before, after, frac=0.35):
    """The box of what a window put over the screen: the columns, then the rows within
    them, where many pixels changed (a unit walking or the button's highlight changes
    few). None when nearly nothing changed."""
    import numpy as np
    if before is None or after is None or before.shape != after.shape:
        return None
    m = np.abs(after - before) > 40
    cols = m.sum(0)
    if cols.max() < 40:
        return None
    xs = np.nonzero(cols >= cols.max() * frac)[0]
    x0, x1 = int(xs.min()), int(xs.max())
    rows = m[:, x0:x1 + 1].sum(1)
    ys = np.nonzero(rows >= rows.max() * frac)[0]
    return x0, int(ys.min()), x1, int(ys.max())


class Report:
    def __init__(self):
        self.fails = 0

    def ok(self, cond, what):
        print('%s: %s' % ('PASS' if cond else 'FAIL', what))
        self.fails += not cond
        return cond


def check_centred(rep, before, after, what):
    """A dialog centred on the screen: its box's middle within 4% of the screen's."""
    if after is None:
        return rep.ok(False, '%s: no shot' % what)
    h, w = after.shape
    b = popup_box(before, after)
    if not b:
        return rep.ok(False, '%s: nothing opened' % what)
    cx, cy = (b[0] + b[2]) / 2, (b[1] + b[3]) / 2
    return rep.ok(abs(cx - w / 2) <= 0.04 * w and abs(cy - h / 2) <= 0.06 * h,
                  '%s centred on %dx%d: box %d,%d..%d,%d, middle %d,%d' % (what, w, h, b[0], b[1], b[2], b[3], cx, cy))


def check_full_screen(rep, img, what):
    """A screen of its own: the largest of the game's sizes within the screen, centred,
    black around it."""
    if img is None:
        return rep.ok(False, '%s: no shot' % what)
    h, w = img.shape
    fw, fh = fit(w, h)
    x0, y0 = (w - fw) // 2, (h - fh) // 2
    inner = img[y0 + 8:y0 + fh - 8, x0 + 8:x0 + fw - 8].mean()
    around = []   # the margins there are (none at the game's own sizes)
    if x0 > 4:
        around += [img[:, :x0 - 2], img[:, x0 + fw + 2:]]
    if y0 > 4:
        around += [img[:y0 - 2, :], img[y0 + fh + 2:, :]]
    around = [r.mean() for r in around if r.size]
    edge = max(around) if around else 0.0
    return rep.ok(inner > 15 and edge < 6, '%s: %dx%d at %d,%d of %dx%d (inside grey %.0f, around %.0f)'
                  % (what, fw, fh, x0, y0, w, h, inner, edge))


def check_game(rep, img, size, what):
    """A game frame of SIZE: the shot's size, a HUD at the bottom."""
    if img is None:
        return rep.ok(False, '%s: no shot' % what)
    h, w = img.shape
    return rep.ok((w, h) == size and img[h - 120:, :].mean() > 15,
                  '%s: %dx%d (want %dx%d), HUD grey %.0f' % (what, w, h, size[0], size[1], img[h - 120:, :].mean()))


# ---- the game's own sizes (main menu Options) -------------------------------------------------
def run_size(a):
    size = int(a.what)
    w, h = SIZES[size]
    sc = Script(a.out)
    out = a.out
    cx, cy = CC[size]
    mx, my, mr = MINIMAP[size]
    t = start_skirmish(sc, None if a.no_options else RADIO[size], STANDARD[a.kind])
    sc.shot(t, 'a_start')
    t += 2
    sc.click(t + 1, cx, cy)
    sc.shot(t + 4, 'b_selected')
    t += 6
    for name, (ex, ey) in (("c_left", (0, h // 2)), ("d_right", (w - 1, h // 2)),
                           ("e_top", (w // 2, 0)), ("f_bottom", (w // 2, h - 1))):
        sc.move(t, ex, ey)
        sc.shot(t + 9, name)
        t += 10
    sc.move(t, w // 2, h // 2)
    sc.click(t + 2, mx - mr // 2, my)
    sc.shot(t + 5, 'g_minimap')
    t += 7
    t = quit_to_menus(sc, t, w, h, *MENU[size])
    rc, text = run_game(a, sc, a.watchdog)
    modes = modes_of(text)
    print("SetDisplayMode calls:", ", ".join("%dx%d" % m for m in modes) or "none")
    for m in re.findall(r"\[shot\] .*", text):
        print(m)
    print("menus restored to 800x600 after Quit:", "yes" if modes and modes[-1] == (800, 600) and len(modes) > 2 else "no")
    ok = (w, h) in modes
    print("PASS" if ok else "FAIL", "%dx%d reached" % (w, h))
    return 0 if ok else 1


def quit_to_menus(sc, t, w, h, bx, by):
    """The HUD's Menu button, Quit Current Game, Yes: the score screen; its Main Menu."""
    sc.click(t + 1, bx, by)
    sc.shot(t + 4, 'h_menu')
    sc.click(t + 6, w // 2, h // 2 + GM_QUIT)
    sc.click(t + 10, w // 2 - 95, h // 2 + 28)
    sc.shot(t + 18, 'i_score')
    sc.click(t + 20, 709, 514)
    sc.shot(t + 28, 'j_menus')
    return t + 30


# ---- switching in the game -----------------------------------------------------------------------
def run_ingame(a):
    wide = a.res
    third = wide or (1280, 1024)
    order = [(1024, (1024, 768)), (1280, third), (800, (800, 600))]
    sc = Script(a.out)
    t = start_skirmish(sc, RADIO[800], STANDARD[a.kind])
    sc.shot(t, 'a_start')
    cur = (800, 600)
    shots = []
    for k, (radio, size) in enumerate(order, 1):
        w, h = cur
        bx, by = hud_button(w, h, 0)
        sc.click(t + 2, bx, by)
        sc.click(t + 5, w // 2, h // 2 + GM_OPTIONS)
        dx, dy = (w - 800) // 2, (h - 600) // 2
        sc.click(t + 8, IG_RADIO_X + dx, IG_RADIO_Y[radio] + dy)
        sc.click(t + 9.5, IG_OK[0] + dx, IG_OK[1] + dy)
        sc.move(t + 10, *SAFE)   # off the old OK: on a smaller screen it would be past an edge
        sc.move(t + 12, *SAFE)
        name = '%02d_%dx%d' % (k, size[0], size[1])
        t += 16
        sc.shot(t, name)
        w, h = cur = size
        bx, by = hud_button(w, h, 0)
        sc.move(t + 1, bx, by)
        sc.shot(t + 2.5, name + '_before')
        sc.click(t + 3, bx, by)
        sc.shot(t + 6, name + '_menu')
        sc.add(t + 7, 'esc')
        shots.append((name, size))
        t += 9
    t = quit_to_menus(sc, t, cur[0], cur[1], *hud_button(cur[0], cur[1], 0))
    extra = ['--res', '%dx%d' % wide] if wide else []
    rc, text = run_game(a, sc, t + 6, extra)
    rep = Report()
    modes = modes_of(text)
    print('SetDisplayMode calls:', ', '.join('%dx%d' % m for m in modes) or 'none')
    want = [(800, 600)] + [s for _, s in order]   # already 800x600: Quit sets no mode
    seen = [m for k, m in enumerate(modes) if k == 0 or m != modes[k - 1]]
    rep.ok(seen == want, 'the modes are %s' % ' '.join('%dx%d' % m for m in want))
    for name, size in shots:
        check_game(rep, grey(a, name), size, name)
        check_centred(rep, grey(a, name + '_before'), grey(a, name + '_menu'), name + ': the game menu (the HUD\'s Menu button)')
    sm = grey(a, 'i_score')
    rep.ok(sm is not None and sm.shape == (600, 800), 'the score screen at 800x600')
    rep.ok(grey(a, 'j_menus') is not None, 'Main Menu: the menus')
    rep.ok(rc == 4 and not EVENTS.search(text), 'no fault, no unresolved call: exit %d (4: the watchdog)' % rc)
    return 1 if rep.fails else 0


# ---- windows over the map ------------------------------------------------------------------------
def run_popups(a):
    if a.size == 'wide':
        if not a.res:
            sys.exit('--size wide needs --res WxH')
        w, h = a.res
        radio = RADIO[1280]
    else:
        w, h = SIZES[int(a.size)]
        radio = RADIO[int(a.size)]
    fw, fh = fit(w, h)
    ox, oy = (w - fw) // 2, (h - fh) // 2

    def on_screen(x, y):   # a place on the 1280x1024 Save and Load screens
        return ox + x * fw // 1280, oy + y * fh // 1024

    sc = Script(a.out)
    t = start_skirmish(sc, radio, STANDARD[a.kind])
    sc.shot(t, 'a_map')
    dialogs = []
    for i, name in enumerate(('menu', 'objectives', 'chat', 'diplomacy', 'techtree')):
        bx, by = hud_button(w, h, i)
        sc.move(t + 1, bx, by)
        sc.shot(t + 2.5, 'b_%s_before' % name)
        sc.click(t + 3, bx, by)
        sc.shot(t + 6, 'b_%s' % name)
        sc.add(t + 7, 'esc')
        dialogs.append(name)
        t += 9
    mb = hud_button(w, h, 0)
    # Save Game: a name, OK, back in the game
    sc.click(t + 1, *mb)
    sc.click(t + 3, w // 2, h // 2 + GM_SAVE)
    sc.shot(t + 7, 'c_save')
    sc.click(t + 8, *on_screen(645, 130))
    for k, c in enumerate('rescheck'):
        sc.add(t + 9 + k * 0.3, c)
    sc.shot(t + 12, 'c_save_typed')
    sc.click(t + 13, *on_screen(284, 969))
    sc.move(t + 14, *SAFE)
    sc.shot(t + 20, 'c_saved')
    t += 22
    # Load: Yes, the Saved Game screen, Cancel
    sc.click(t + 1, *mb)
    sc.click(t + 3, w // 2, h // 2 + GM_LOAD)
    sc.click(t + 5, w // 2 - 95, h // 2 + 28)
    sc.shot(t + 10, 'd_load')
    sc.click(t + 11, *on_screen(1009, 969))
    sc.move(t + 12, *SAFE)
    sc.shot(t + 17, 'd_back')
    t += 19
    # About
    sc.click(t + 1, *mb)
    sc.shot(t + 3.5, 'e_about_before')
    sc.click(t + 4, w // 2, h // 2 + GM_ABOUT)
    sc.shot(t + 7, 'e_about')
    sc.add(t + 8, 'esc')
    t += 10
    extra = ['--res', '%dx%d' % a.res] if a.res else []
    saves = os.path.join(a.game, 'SaveGame')
    for f in os.listdir(saves) if os.path.isdir(saves) else []:
        if f.lower().startswith('rescheck'):
            os.remove(os.path.join(saves, f))
    rc, text = run_game(a, sc, t + 4, extra)
    rep = Report()
    modes = modes_of(text)
    print('SetDisplayMode calls:', ', '.join('%dx%d' % m for m in modes) or 'none')
    rep.ok((w, h) in modes, 'the skirmish at %dx%d' % (w, h))
    check_game(rep, grey(a, 'a_map'), (w, h), 'the map')
    for name in dialogs:
        if name == 'techtree':   # the whole screen, laid out at its size (left full size at WxH)
            b = popup_box(grey(a, 'b_techtree_before'), grey(a, 'b_techtree'))
            rep.ok(bool(b) and b[2] - b[0] >= 0.8 * w, 'the tech tree across the screen: box %s' % (b,))
        else:
            check_centred(rep, grey(a, 'b_%s_before' % name), grey(a, 'b_%s' % name), name)
    check_full_screen(rep, grey(a, 'c_save'), 'Save Game')
    check_game(rep, grey(a, 'c_saved'), (w, h), 'saved: back in the game')
    rep.ok(os.path.isdir(saves) and any(f.lower().startswith('rescheck') for f in os.listdir(saves)),
           'the save rescheck is in SaveGame')
    check_full_screen(rep, grey(a, 'd_load'), 'Load: the Saved Game screen')
    check_game(rep, grey(a, 'd_back'), (w, h), 'Cancel: back in the game')
    check_centred(rep, grey(a, 'e_about_before'), grey(a, 'e_about'), 'about')
    rep.ok(rc == 4 and not EVENTS.search(text), 'no fault, no unresolved call: exit %d (4: the watchdog)' % rc)
    return 1 if rep.fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=['800', '1024', '1280', 'ingame', 'popups'])
    ap.add_argument("--out", default="res_shots")
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if os.name == "nt":
        ap.add_argument("--game", default=r"A:\Games\star\games\resolutions")
        ap.add_argument("--exe", default=r"A:\Games\star\swgb-recomp\work\Battlegrounds.exe")
        ap.add_argument("--run", default=r"A:\Games\star\builds\resolutions\swgbrecomp.exe")
    else:   # Linux and macOS: this checkout's game folder, lift and build
        ap.add_argument("--game", default=os.path.join(root, "game"))
        ap.add_argument("--exe", default=os.path.join(root, "work", "Battlegrounds.exe"))
        ap.add_argument("--run", default=os.path.join(root, "build-macos" if sys.platform == "darwin" else "build-linux", "swgbrecomp"))
    ap.add_argument("--no-options", action="store_true",
                    help="keep the Screen Size already stored (checks that it persists)")
    ap.add_argument("--res", help="WxH: the third Screen Size (the host's --res)")
    ap.add_argument("--size", default=None, help="popups: 800, 1024, 1280 or wide (default: wide with --res, else 1024)")
    ap.add_argument("--cc", action="store_true", help="Clone Campaigns (its Standard Game button); pass its --run and --exe")
    ap.add_argument("--watchdog", type=int, default=240)
    a = ap.parse_args()
    a.kind = 'cc' if a.cc else 'base'
    if a.res:
        a.res = tuple(int(v) for v in a.res.lower().split('x'))
        a.res = (a.res[0] & ~3, a.res[1])   # the host aligns the width to 4
    if a.size is None:
        a.size = 'wide' if a.res else '1024'
    os.makedirs(a.out, exist_ok=True)
    a.out = os.path.abspath(a.out)
    if a.what == 'ingame':
        return run_ingame(a)
    if a.what == 'popups':
        return run_popups(a)
    return run_size(a)


if __name__ == "__main__":
    sys.exit(main())
