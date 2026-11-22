"""Scripted skirmish at one of the game's own resolutions, headless.

  py -3 tools/res_check.py 1024 [--out DIR] [--game DIR] [--keep-setting]
                                [--stay] [--no-options]

SIZE is 800, 1024 or 1280 (800x600, 1024x768, 1280x1024). The run picks the
Screen Size on the Options menu, starts a Standard Game and takes shots:

  a_start     the map after loading
  b_selected  the command center clicked
  c_left .. f_bottom   the cursor held on each edge of the map view (scrolls)
  g_minimap   a click on the minimap
  h_menu      the game menu (the HUD's Menu button)
  i_score     after Quit Current Game and Yes: the score screen (800x600)
  j_menus     its Main Menu button: back on the main menu (800x600)

It reads the log for the SetDisplayMode calls and prints them: the mode must
be SIZE in the skirmish and 800x600 on the menus. Shots land in DIR as BMP and
PNG (ffmpeg). The game runs with --headless --multi, so it has its own
registry options (see host.c): the Screen Size it sets stays for this --game
folder only.
"""
import argparse, os, re, subprocess, sys

SIZES = {800: (800, 600), 1024: (1024, 768), 1280: (1280, 1024)}
RADIO = {800: (453, 383), 1024: (453, 413), 1280: (453, 443)}   # on the 800x600 Options menu
# Where things sit in game pixels, by size: the command center after loading, the
# minimap's middle and the half-width of its diamond.
CC = {800: (400, 170), 1024: (500, 300), 1280: (610, 340)}
MINIMAP = {800: (604, 550, 120), 1024: (772, 700, 150), 1280: (965, 920, 200)}
MENU = {800: (765, 480), 1024: (980, 624), 1280: (1230, 850)}   # the HUD's Menu button


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("size", type=int, choices=sorted(SIZES))
    ap.add_argument("--out", default="res_shots")
    ap.add_argument("--game", default=r"A:\Games\star\games\resolutions")
    ap.add_argument("--exe", default=r"A:\Games\star\swgb-recomp\work\Battlegrounds.exe")
    ap.add_argument("--run", default=r"A:\Games\star\builds\resolutions\swgbrecomp.exe")
    ap.add_argument("--no-options", action="store_true",
                    help="keep the Screen Size already stored (checks that it persists)")
    ap.add_argument("--watchdog", type=int, default=240)
    a = ap.parse_args()
    w, h = SIZES[a.size]
    os.makedirs(a.out, exist_ok=True)
    out = os.path.abspath(a.out)
    cx, cy = CC[a.size]
    mx, my, mr = MINIMAP[a.size]
    s = []
    if not a.no_options:
        rx, ry = RADIO[a.size]
        s += [(12, "move:40:490"), (13, "click:40:490"), (19, f"move:{rx}:{ry}"), (20, f"click:{rx}:{ry}"),
              (24, "move:345:250"), (25, "click:345:250")]
    else:
        s += [(20, "move:345:250"), (21, "click:345:250")]
    t = s[-1][0] + 8
    s += [(t, "move:600:168"), (t + 1, "click:600:168"), (t + 9, "move:118:568"), (t + 10, "click:118:568")]
    t += 47   # the map, loaded
    s.append((t, f"shot={out}\\a_start.bmp"))
    t += 2
    s += [(t, f"move:{cx}:{cy}"), (t + 1, f"click:{cx}:{cy}"), (t + 4, f"shot={out}\\b_selected.bmp")]
    t += 6
    for name, (ex, ey) in (("c_left", (0, h // 2)), ("d_right", (w - 1, h // 2)),
                           ("e_top", (w // 2, 0)), ("f_bottom", (w // 2, h - 1))):
        s += [(t, f"move:{ex}:{ey}"), (t + 9, f"shot={out}\\{name}.bmp")]
        t += 10
    s += [(t, f"move:{w // 2}:{h // 2}"), (t + 1, f"move:{mx - mr // 2}:{my}"), (t + 2, f"click:{mx - mr // 2}:{my}"),
          (t + 5, f"shot={out}\\g_minimap.bmp")]
    t += 7
    bx, by = MENU[a.size]
    qx, qy, yx, yy = w // 2, h // 2 - 122, w // 2 - 95, h // 2 + 28   # Quit Current Game, then Yes
    s += [(t, f"move:{bx}:{by}"), (t + 1, f"click:{bx}:{by}"), (t + 4, f"shot={out}\\h_menu.bmp"),
          (t + 5, f"move:{qx}:{qy}"), (t + 6, f"click:{qx}:{qy}"),
          (t + 9, f"move:{yx}:{yy}"), (t + 10, f"click:{yx}:{yy}"), (t + 18, f"shot={out}\\i_score.bmp"),
          (t + 19, "move:709:514"), (t + 20, "click:709:514"), (t + 28, f"shot={out}\\j_menus.bmp")]
    keys = ",".join(f"{at}:{what}" for at, what in s)
    log = os.path.join(out, "run.log")
    cmd = [a.run, "--run", "--multi", "--headless", "--exe", a.exe, "--game", a.game,
           "--watchdog", str(a.watchdog), "--keys", keys]
    with open(log, "w") as f:
        subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT)
    text = open(log, errors="replace").read()
    modes = re.findall(r"SetDisplayMode\((\d+), (\d+), (\d+)\)", text)
    print("SetDisplayMode calls:", ", ".join(f"{x}x{y}x{b}" for x, y, b in modes) or "none")
    for m in re.findall(r"\[shot\] .*", text):
        print(m)
    seen = [(int(x), int(y)) for x, y, _ in modes]
    ok = (w, h) in seen
    print("menus restored to 800x600 after Quit:", "yes" if seen and seen[-1] == (800, 600) and len(seen) > 2 else "no")
    for f in sorted(os.listdir(out)):
        if f.endswith(".bmp"):
            subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", os.path.join(out, f),
                            os.path.join(out, f[:-4] + ".png")])
    print("PASS" if ok else "FAIL", f"{w}x{h} reached")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
