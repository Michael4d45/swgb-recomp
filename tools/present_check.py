"""The presenter, checked headless: letterboxed mouse, edge scrolling, fullscreen, pacing, cursor.

  py -3 tools/present_check.py [--only A,B,C,D] [--run EXE] [--game DIR] [--out DIR]

Four scripted skirmishes (--headless --multi, nothing on your screen), each a case group:

  A  windowed: the game's own size, a wide window, a tall window, a window resized
     several times, then Alt+Enter fullscreen. With --refresh 10 (presents exceed it).
     Also the cursor coming back into the window (WM_SETCURSOR).
  B  Alt+Enter on the menus, then Options -> 1024 x 768 and a game (SetDisplayMode),
     and Quit to the menus (800x600 again): fullscreen must survive all of it.
  C  --fullscreen at launch: the same edge checks, in the monitor's rectangle.
  D  --res 1920x1080 in a 16:9 window (no bars), then fullscreen.
  E  a window the player sized (1100x620) through a mode change (Options -> 1024 x 768
     and a game), Quit to the menus and a second game: it must keep its size (the game
     sizes its own window at every mode change; that shrank it a little each time).

In every window shape, edge scrolling is checked on all four sides of the window, in
each letterbox bar, and that nothing scrolls with the cursor well inside the picture:
the cursor is put there by the scripted (virtual) cursor, which the host reads back
through the same path as the real one (screen -> client -> letterbox -> the game's
pixels), then the map view is compared before and after (the game's frame, shot=).

It prints PASS or FAIL per case and exits 1 on any FAIL. About 21 minutes.
"""
import argparse, os, re, subprocess, sys, time

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from swgb import runner, screens  # noqa: E402

LAY = {size: screens.layout('base', size) for size in (800, 1024)}
LOAD = 47           # seconds from Start Game to the map
SCROLLED = 0.10     # share of the frame that changes when the view scrolls (about half; a cursor sprite is under 0.3%)
STILL = 0.03        # ... and that may change with nothing scrolling (animations, a tooltip, the cursor)
PACE_HZ = 600       # --refresh for group A: a headless game presents ~700-1,000 times a second, so presents are 1.2-1.6x the refresh
HOLD = 0.8          # seconds the cursor is held at a place: the view scrolls several hundred pixels (the map is small: much longer and it is all black)


def load(path):
    return np.asarray(Image.open(path).convert("L"), dtype=np.int16)


def changed(a, b):
    if a.shape != b.shape:
        return 1.0
    return float((np.abs(a - b) > 24).mean())


class Script(runner.Script):
    """A --keys script written to a file: times in seconds, and the probes in it."""

    def __init__(self, out):
        super().__init__()
        self.out = out
        self.t = 0.0
        self.moves = []      # [label, ...] one per wmove step, in order: the host logs each
        self.probes = []     # dicts: see probe()
        self.cfgs = []

    def at(self, t, what):
        super().at(t, what)
        self.t = max(self.t, t)

    def wmove(self, t, x, y, label=""):
        self.at(t, f"wmove:{x}:{y}")
        self.moves.append(label)

    def snap(self, t, name):
        return self.shot(t, os.path.join(self.out, name + ".bmp"))

    def wshot(self, t, name):
        path = os.path.join(self.out, name + ".bmp")
        self.at(t, f"wshot={path}")
        return path

    def skirmish(self, t, size=None):
        """[Options -> Screen Size SIZE ->] Single Player -> Standard Game (1 computer, Easiest)
        -> Start Game. Returns the time the map is up."""
        t = screens.to_setup(self, LAY[800], t + 3, size)
        t = screens.pick(self, t, screens.DD_OTHERS, 0)         # a fresh folder's saved 7-Hardest
        t = screens.pick(self, t, screens.DD_DIFFICULTY, 0)     # setup can kill the idle player mid-run
        self.click(t + 1, screens.SETUP_START)
        return t + 1 + LOAD

    def quit_to_menus(self, t, prefix):
        """At 1024x768: the HUD's Menu, Quit Current Game, Yes; the score screen; Main Menu.
        Returns the time of the last shot, on the menus."""
        yes = screens.quit_game(self, LAY[1024], t + 1)
        self.wshot(yes + 8, prefix + "_score")
        self.click(yes + 10, screens.SCORE_MENU)
        self.wshot(yes + 18, prefix + "_menus_again")
        return yes + 18

    def battery(self, t, cfg, bars=True, overshoot=False):
        """The probes at one window shape. cfg names the shape; the picture's rectangle (to know
        which bars exist) comes from the wshot taken first."""
        stem = re.sub(r"[^A-Za-z0-9]+", "_", cfg).strip("_")
        wp = self.wshot(t, f"{stem}_window")
        self.cfgs.append(dict(name=cfg, wshot=wp))
        t += 0.5
        plan = [("still: well inside the left edge", "PL+60", "C", "still"),
                ("still: well inside the top edge", "C", "PT+60", "still"),
                ("still: the middle", "C", "C", "still"),
                ("right edge", "R", "C", "scroll", "right"),
                ("left edge", "L", "C", "scroll", "left"),
                ("bottom edge", "C", "B", "scroll", "bottom"),
                ("top edge", "C", "T", "scroll", "top")]
        if overshoot:
            plan.append(("past the right of the screen (confined)", "R+9999", "C", "scroll", "right"))
            plan.append(("past the left of the screen (confined)", "L-9999", "C", "scroll", "left"))
        if bars:
            plan += [("right bar", "BR", "C", "scroll", "right", "barR"), ("left bar", "BL", "C", "scroll", "left", "barL"),
                     ("bottom bar", "C", "BB", "scroll", "bottom", "barB"), ("top bar", "C", "BT", "scroll", "top", "barT")]
        for item in plan:
            name, x, y, expect = item[:4]
            side = item[4] if len(item) > 4 else None
            need = item[5] if len(item) > 5 else None
            # every probe starts from the same view: H centres the game on the command center
            self.at(t, "h")
            self.wmove(t + 0.7, "C", "C", "centre")
            before = self.snap(t + 1.4, f"{stem}_{len(self.probes)}a")
            self.wmove(t + 1.7, x, y, f"{cfg}:{name}")
            self.wmove(t + 1.7 + HOLD, "C", "C", "centre")
            after = self.snap(t + 1.7 + HOLD + 1.0, f"{stem}_{len(self.probes)}b")
            self.probes.append(dict(cfg=cfg, name=name, expect=expect, side=side, need=need, before=before, after=after,
                                    move_index=len(self.moves) - 2))
            t += 1.7 + HOLD + 1.6
        return t

    def write(self, name):
        return super().write(os.path.join(self.out, name + ".keys"))


def run(a, name, script, extra, results):
    keys = script.write(name)
    log = os.path.join(a.out, name + ".log")
    cmd = runner.host_cmd(a.run, a.exe, a.game, script.t + 25, keys, "--present-log", *extra)
    t0 = time.time()
    print(f"[{name}] running ({script.t + 25:.0f} s at most): {' '.join(extra)}", flush=True)
    with open(log, "w") as f:
        subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT)
    print(f"[{name}] done in {time.time() - t0:.0f} s, log {log}", flush=True)
    return open(log, errors="replace").read()


def check(results, name, ok, detail=""):
    results.append((name, ok))
    print(f"{'PASS' if ok else 'FAIL'}: {name}" + (f"  ({detail})" if detail else ""), flush=True)


def pictures(text, script):
    """cfg name -> (cw, ch, px, py, pw, ph) from the [wshot] lines."""
    out = {}
    for c in script.cfgs:
        base = os.path.basename(c["wshot"])
        m = re.search(r"\[wshot\] window (\d+)x(\d+), picture (-?\d+),(-?\d+) (\d+)x(\d+).*" + re.escape(base), text)
        if m:
            out[c["name"]] = tuple(int(v) for v in m.groups())
    return out


def game_mode(text):
    modes = re.findall(r"SetDisplayMode\((\d+)(?:, |x)(\d+)(?:, |x)\d+\)", text)
    return [(int(x), int(y)) for x, y in modes]


def judge_edges(results, group, text, script, mode_hint=None):
    pics = pictures(text, script)
    keys_lines = re.findall(r"\[keys\] window (-?\d+),(-?\d+) -> game (-?\d+),(-?\d+)", text)
    modes = game_mode(text)
    gw, gh = modes[-1] if modes else (800, 600)
    if mode_hint:
        gw, gh = mode_hint
    for pr in script.probes:
        cfg = pr["cfg"]
        label = f"{group}/{cfg}: {pr['name']}"
        if cfg not in pics:
            check(results, label, False, "the window shot is missing: the run did not get here")
            continue
        cw, ch, px, py, pw, ph = pics[cfg]
        if pr["need"] == "barR" and cw - (px + pw) < 8: continue
        if pr["need"] == "barL" and px < 8: continue
        if pr["need"] == "barB" and ch - (py + ph) < 8: continue
        if pr["need"] == "barT" and py < 8: continue
        if not (os.path.exists(pr["before"]) and os.path.exists(pr["after"])):
            check(results, label, False, "no shot")
            continue
        d = changed(load(pr["before"]), load(pr["after"]))
        if pr["expect"] == "scroll":
            ok = d >= SCROLLED
            detail = f"the view changed {d * 100:.1f}% ({'scrolled' if ok else 'did NOT scroll'})"
            # the edge's own game pixel: the cursor must reach 0 / W-1 / H-1
            i = pr["move_index"]
            if i < len(keys_lines):
                gx, gy = int(keys_lines[i][2]), int(keys_lines[i][3])
                want = {"right": gx == gw - 1, "left": gx == 0, "bottom": gy == gh - 1, "top": gy == 0}[pr["side"]]
                detail += f"; the game saw the cursor at {gx},{gy}"
                ok = ok and want
        else:
            ok = d < STILL
            detail = f"the view changed {d * 100:.2f}% ({'still' if ok else 'SCROLLED with the cursor inside the picture'})"
        check(results, label, ok, detail)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="A,B,C,D,E")
    ap.add_argument("--run", help="the host (default: the build of tools/swgb/runner.paths())")
    ap.add_argument("--game", help="the game folder (default: GAMES/present-check-base, made on first use)")
    ap.add_argument("--exe", help="the game's exe (default: work/ of the main checkout)")
    ap.add_argument("--out", default=os.path.join(os.environ.get("TEMP", "."), "present_check"))
    a = ap.parse_args()
    P = runner.paths()
    a.run = a.run or P.host("base")
    a.exe = a.exe or P.exe("base")
    a.game = a.game or P.game("present-check", "base")
    os.makedirs(a.out, exist_ok=True)
    only = set(a.only.upper().split(","))
    results = []

    if "A" in only:
        s = Script(a.out)
        s.at(14, "setcursor")                       # on the menus
        t = s.skirmish(20, 800)
        s.at(t - 3, "move:400:300")
        s.at(t - 2, "setcursor")                    # in the game, the cursor (back) in the window
        t = s.battery(t, "own size")
        s.at(t, "size:1500:700"); t = s.battery(t + 1.5, "wide window")
        s.at(t, "size:700:900"); t = s.battery(t + 1.5, "tall window")
        for w, h in ((900, 600), (1300, 500), (640, 800), (1100, 620)):
            s.at(t, f"size:{w}:{h}"); t += 1.2
        t = s.battery(t + 0.3, "resized several times")
        s.at(t, "alt+enter"); t = s.battery(t + 1.5, "Alt+Enter fullscreen", overshoot=True)
        # --fps off: the game's own rate, so presents exceed the refresh (the default caps them at it)
        text = run(a, "A", s, ["--refresh", str(PACE_HZ), "--fps", "off"], results)
        judge_edges(results, "A", text, s)
        # the cursor coming back
        cur = re.findall(r"\[cursor\] re-entry: the game's cursor (\w+), after WM_SETCURSOR (\w+) \(the arrow is (\w+)\): (\w+)", text)
        for i, label in enumerate(("on the menus", "in a game")):
            if i < len(cur):
                before, after, arrow, verdict = cur[i]
                vac = " (vacuous: the game's cursor is the arrow)" if before == arrow else ""
                check(results, f"A/cursor re-entry {label}: the game's cursor stays", verdict == "kept" and (before != arrow or True),
                      f"game {before}, after {after}, arrow {arrow}{vac}")
            else:
                check(results, f"A/cursor re-entry {label}: the game's cursor stays", False, "no [cursor] line")
        # pacing: when presents come only a little faster than the refresh, frames are still shown at about the
        # refresh (an "interval since the last one" rule showed every other present: about half)
        lines = [(float(el), int(p), int(sh), int(hz)) for el, p, sh, hz in
                 re.findall(r"\[present\] ([\d.]+) s: (\d+) presents, (\d+) shown \((\d+) Hz", text)]
        busy = [(el, p, sh, hz) for el, p, sh, hz in lines if hz == PACE_HZ and 1.1 * hz * el < p < 2.2 * hz * el]
        label = f"A/pacing: presents a little above the refresh ({PACE_HZ} Hz) show at about the refresh"
        if len(busy) >= 8:
            rate = sum(sh for _, _, sh, _ in busy) / sum(el for el, _, _, _ in busy)
            pr = sum(p for _, p, _, _ in busy) / sum(el for el, _, _, _ in busy)
            check(results, label, 0.85 * PACE_HZ <= rate <= 1.1 * PACE_HZ,
                  f"{pr:.0f} presents/s, {rate:.0f} shown/s over {len(busy)} s")
        else:
            check(results, label, False, f"only {len(busy)} s with presents 1.1-2.2x the refresh")

    if "B" in only:
        s = Script(a.out)
        s.at(6, "alt+enter")
        s.wshot(8, "B_menus_fullscreen")
        t = s.skirmish(10, 1024)                         # Options: 1024 x 768, then a game
        s.wshot(t - 4, "B_game_1024")
        t = s.battery(t, "1024x768 game in fullscreen", overshoot=True)
        s.quit_to_menus(t, "B")
        text = run(a, "B", s, [], results)
        judge_edges(results, "B", text, s, mode_hint=(1024, 768))
        sizes = {}
        for tag in ("B_menus_fullscreen", "B_game_1024", "B_score", "B_menus_again"):
            m = re.search(r"\[wshot\] window (\d+)x(\d+), picture .*?" + tag, text)
            sizes[tag] = (int(m.group(1)), int(m.group(2))) if m else None
        mon = sizes["B_menus_fullscreen"]
        modes = game_mode(text)
        check(results, "B/the game changed its mode (1024x768 in the game, 800x600 back on the menus)",
              (1024, 768) in modes and modes[-1] == (800, 600), "SetDisplayMode: " + ", ".join(f"{x}x{y}" for x, y in modes))
        for tag, what in (("B_menus_fullscreen", "Alt+Enter on the menus"), ("B_game_1024", "after the 1024x768 mode change"),
                          ("B_score", "on the score screen"), ("B_menus_again", "back on the 800x600 menus")):
            ok = sizes[tag] is not None and mon is not None and sizes[tag] == mon and mon[0] >= 1024
            check(results, f"B/still fullscreen {what}", ok, f"window {sizes[tag]}, the monitor {mon}")
        check(results, "B/fullscreen never left for a window", "[present] windowed:" not in text)

    if "C" in only:
        s = Script(a.out)
        s.wshot(8, "C_menus")
        t = s.skirmish(10, 800)
        t = s.battery(t, "--fullscreen at launch", overshoot=True)
        text = run(a, "C", s, ["--fullscreen"], results)
        judge_edges(results, "C", text, s)
        m = re.search(r"\[wshot\] window (\d+)x(\d+), picture .*?C_menus", text)
        check(results, "C/--fullscreen: the menus are fullscreen from the start", bool(m) and int(m.group(1)) >= 1024,
              f"window {m.group(1) + 'x' + m.group(2) if m else None}")

    if "D" in only:
        s = Script(a.out)
        t = s.skirmish(10, 1280)
        s.at(t - 3, "size:1600:900")
        t = s.battery(t, "--res 1920x1080 in a 16:9 window")
        s.at(t, "alt+enter"); t = s.battery(t + 1.5, "--res 1920x1080 fullscreen", overshoot=True)
        text = run(a, "D", s, ["--res", "1920x1080"], results)
        judge_edges(results, "D", text, s)
        pics = pictures(text, s)
        p = pics.get("--res 1920x1080 in a 16:9 window")
        check(results, "D/a 16:9 game in a 16:9 window has no bars", p is not None and p[2] == 0 and p[3] == 0 and p[4] == p[0] and p[5] == p[1],
              f"window and picture {p}")

    if "E" in only:
        s = Script(a.out)
        s.at(5, "size:1100:620")                          # the player sizes the window on the menus
        s.wshot(7, "E_menus_sized")
        t = s.skirmish(8, 1024)                           # Options: 1024 x 768, then a game
        s.wshot(t - 3, "E_game_1024")
        t = s.battery(t, "a sized window after a mode change")
        t = s.quit_to_menus(t, "E")
        t = s.skirmish(t)                                 # a second game: the mode changes again
        s.wshot(t - 3, "E_game_again")
        s.t = t
        text = run(a, "E", s, [], results)
        judge_edges(results, "E", text, s, mode_hint=(1024, 768))
        modes = game_mode(text)
        check(results, "E/the mode changed (1024x768, 800x600 on the menus, 1024x768 again)",
              modes.count((1024, 768)) >= 2 and (800, 600) in modes[1:], "SetDisplayMode: " + ", ".join(f"{x}x{y}" for x, y in modes))
        for tag, what in (("E_menus_sized", "on the menus, as sized"), ("E_game_1024", "after the change to 1024x768"),
                          ("E_score", "on the score screen (800x600)"), ("E_menus_again", "back on the menus"),
                          ("E_game_again", "in the second game")):
            m = re.search(r"\[wshot\] window (\d+)x(\d+), picture .*?" + tag, text)
            size = (int(m.group(1)), int(m.group(2))) if m else None
            check(results, f"E/the window keeps the player's size {what}", size == (1100, 620), f"client {size}, sized 1100x620")

    bad = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(bad)}/{len(results)} passed" + (f"; FAILED: {len(bad)}" if bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
