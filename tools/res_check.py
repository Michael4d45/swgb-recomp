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
land in DIR as PNG. The game runs with --headless --multi, so it has its own
registry options (see host.c): a Screen Size it sets stays for this --game
folder only.
"""
import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from swgb import runner, screens  # noqa: E402
from swgb.screens import fit  # noqa: E402

SAFE = (400, 300)   # inside every screen size, away from its edges (edge scrolling)
LOAD = 47           # seconds from Start Game to the map


class Shots(runner.Script):
    """A --keys script whose shots are named, in the run's folder."""

    def __init__(self, out):
        super().__init__()
        self.out = out

    def snap(self, t, name):
        self.shot(t, os.path.join(self.out, name + '.bmp'))


def run_game(a, sc, watchdog, extra=()):
    log = os.path.join(a.out, 'run.log')
    keys = sc.write(os.path.join(a.out, 'keys.txt'))
    with open(log, 'w') as f:
        rc = subprocess.run(runner.host_cmd(a.run, a.exe, a.game, watchdog, keys, *extra),
                            stdout=f, stderr=subprocess.STDOUT).returncode
    text = open(log, errors='replace').read()
    runner.to_png(a.out)
    return rc, text


# ---- reading the shots ------------------------------------------------------------------------
def grey(a, name):
    p = os.path.join(a.out, name + '.png')
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
    def __init__(self, quiet=False):
        self.fails, self.quiet = 0, quiet

    def ok(self, cond, what):
        if not self.quiet:
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


def quit_to_menus(sc, lay, t):
    """The HUD's Menu button (h_menu), Quit Current Game, Yes: the score screen (i_score); its
    Main Menu (j_menus). Returns when the menus are up."""
    yes = screens.quit_game(sc, lay, t + 1)
    sc.snap(t + 4, 'h_menu')
    sc.snap(yes + 8, 'i_score')
    sc.click(yes + 10, screens.SCORE_MENU)
    sc.snap(yes + 18, 'j_menus')
    return yes + 20


# ---- the game's own sizes (main menu Options) -------------------------------------------------
def run_size(a):
    size = int(a.what)
    lay = screens.layout(a.kind, size)
    w, h = lay.w, lay.h
    sc = Shots(a.out)
    if a.no_options:
        t = screens.to_skirmish(sc, lay, 20) + LOAD
    else:
        t = screens.to_skirmish(sc, lay, 13, size) + LOAD
    sc.snap(t, 'a_start')
    t += 2
    sc.click(t + 1, lay.command_center)
    sc.snap(t + 4, 'b_selected')
    t += 6
    for name, edge in (('c_left', (0, h // 2)), ('d_right', (w - 1, h // 2)),
                       ('e_top', (w // 2, 0)), ('f_bottom', (w // 2, h - 1))):
        sc.move(t, edge)
        sc.snap(t + 9, name)
        t += 10
    sc.move(t, (w // 2, h // 2))
    mx, my, reach = lay.minimap
    sc.click(t + 2, (mx - reach // 2, my))
    sc.snap(t + 5, 'g_minimap')
    t = quit_to_menus(sc, lay, t + 7)
    rc, text = run_game(a, sc, a.watchdog)
    modes = runner.modes(text)
    print("SetDisplayMode calls:", ", ".join("%dx%d" % m for m in modes) or "none")
    for m in re.findall(r"\[shot\] .*", text):
        print(m)
    print("menus restored to 800x600 after Quit:", "yes" if modes and modes[-1] == (800, 600) and len(modes) > 2 else "no")
    ok = (w, h) in modes
    print("PASS" if ok else "FAIL", "%dx%d reached" % (w, h))
    return 0 if ok else 1


# ---- switching in the game -----------------------------------------------------------------------
def run_ingame(a):
    wide = a.res
    third = wide or (1280, 1024)
    order = [(1024, (1024, 768)), (1280, third), (800, (800, 600))]
    sc = Shots(a.out)
    lay = screens.layout(a.kind, 800)
    t = screens.to_skirmish(sc, lay, 13, 800) + LOAD
    sc.snap(t, 'a_start')
    shots = []
    for k, (radio, size) in enumerate(order, 1):
        sc.click(t + 2, lay.hud['menu'])
        sc.click(t + 5, lay.game_menu['options'])
        sc.click(t + 8, lay.options_radio[radio])
        sc.click(t + 9.5, lay.options_ok)
        sc.move(t + 10, SAFE)   # off the old OK: on a smaller screen it would be past an edge
        sc.move(t + 12, SAFE)
        name = '%02d_%dx%d' % (k, size[0], size[1])
        t += 16
        sc.snap(t, name)
        lay = screens.layout(a.kind, size)
        sc.move(t + 1, lay.hud['menu'])
        sc.snap(t + 2.5, name + '_before')
        sc.click(t + 3, lay.hud['menu'])
        sc.snap(t + 6, name + '_menu')
        sc.at(t + 7, 'esc')
        shots.append((name, size))
        t += 9
    t = quit_to_menus(sc, lay, t)
    extra = ['--res', '%dx%d' % wide] if wide else []
    rc, text = run_game(a, sc, t + 6, extra)
    rep = Report()
    modes = runner.modes(text)
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
    rep.ok(rc == 4 and not runner.events(text), 'no fault, no unresolved call: exit %d (4: the watchdog)' % rc)
    return 1 if rep.fails else 0


# ---- windows over the map ------------------------------------------------------------------------
def run_popups(a):
    if a.size == 'wide':
        if not a.res:
            sys.exit('--size wide needs --res WxH')
        lay = screens.layout(a.kind, a.res)
        radio = 1280
    else:
        lay = screens.layout(a.kind, int(a.size))
        radio = int(a.size)
    w, h = lay.w, lay.h
    sc = Shots(a.out)
    t = screens.to_skirmish(sc, lay, 13, radio) + LOAD
    sc.snap(t, 'a_map')
    for name in screens.HUD_BUTTONS:
        sc.move(t + 1, lay.hud[name])
        sc.snap(t + 2.5, 'b_%s_before' % name)
        sc.click(t + 3, lay.hud[name])
        sc.snap(t + 6, 'b_%s' % name)
        sc.at(t + 7, 'esc')
        t += 9
    # Save Game: a name, OK, back in the game
    sc.click(t + 1, lay.hud['menu'])
    sc.click(t + 3, lay.game_menu['save'])
    sc.snap(t + 7, 'c_save')
    sc.click(t + 8, lay.save_name)
    for k, c in enumerate('rescheck'):
        sc.at(t + 9 + k * 0.3, c)
    sc.snap(t + 12, 'c_save_typed')
    sc.click(t + 13, lay.save_ok)
    sc.move(t + 14, SAFE)
    sc.snap(t + 20, 'c_saved')
    t += 22
    # Load: Yes, the Saved Game screen, Cancel
    sc.click(t + 1, lay.hud['menu'])
    sc.click(t + 3, lay.game_menu['load'])
    sc.click(t + 5, lay.yes)
    sc.snap(t + 10, 'd_load')
    sc.click(t + 11, lay.load_cancel)
    sc.move(t + 12, SAFE)
    sc.snap(t + 17, 'd_back')
    t += 19
    # About
    sc.click(t + 1, lay.hud['menu'])
    sc.snap(t + 3.5, 'e_about_before')
    sc.click(t + 4, lay.game_menu['about'])
    sc.snap(t + 7, 'e_about')
    sc.at(t + 8, 'esc')
    t += 10
    extra = ['--res', '%dx%d' % a.res] if a.res else []
    saves = os.path.join(a.game, 'SaveGame')
    for f in os.listdir(saves) if os.path.isdir(saves) else []:
        if f.lower().startswith('rescheck'):
            os.remove(os.path.join(saves, f))
    rc, text = run_game(a, sc, t + 4, extra)
    rep = Report()
    modes = runner.modes(text)
    print('SetDisplayMode calls:', ', '.join('%dx%d' % m for m in modes) or 'none')
    rep.ok((w, h) in modes, 'the skirmish at %dx%d' % (w, h))
    check_game(rep, grey(a, 'a_map'), (w, h), 'the map')
    for name in screens.HUD_BUTTONS:
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
    rep.ok(rc == 4 and not runner.events(text), 'no fault, no unresolved call: exit %d (4: the watchdog)' % rc)
    return 1 if rep.fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=['800', '1024', '1280', 'ingame', 'popups'])
    ap.add_argument("--out", default="res_shots")
    ap.add_argument("--game", help="the game folder (default: GAMES/res-check-base or -cc, made on first use)")
    ap.add_argument("--exe", help="the game's exe (default: work/ of the main checkout)")
    ap.add_argument("--run", help="the host (default: the build of tools/swgb/runner.paths())")
    ap.add_argument("--no-options", action="store_true",
                    help="keep the Screen Size already stored (checks that it persists)")
    ap.add_argument("--res", help="WxH: the third Screen Size (the host's --res)")
    ap.add_argument("--size", default=None, help="popups: 800, 1024, 1280 or wide (default: wide with --res, else 1024)")
    ap.add_argument("--cc", action="store_true", help="Clone Campaigns: its host, exe and Standard Game button")
    ap.add_argument("--watchdog", type=int, default=240)
    a = ap.parse_args()
    a.kind = 'cc' if a.cc else 'base'
    P = runner.paths()
    a.run = a.run or P.host(a.kind)
    a.exe = a.exe or P.exe(a.kind)
    a.game = a.game or P.game('res-check', a.kind)
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
