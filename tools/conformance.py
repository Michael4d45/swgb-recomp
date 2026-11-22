#!/usr/bin/env python3
"""Conformance harness: milestones the recompiled game must still reach.

Runs build\\swgbrecomp.exe headless (never a window: REPO_RULES section 13)
with scripted input, reads its log, and reports PASS/FAIL per milestone and
N/M at the end. Exit 1 on any FAIL. Without the game data (game\\,
work\\Battlegrounds.exe) every milestone is SKIP and the exit code is 0.

    py -3 tools/conformance.py [path\\to\\swgbrecomp.exe] [--game DIR] [--exe Battlegrounds.exe]

--game and --exe point a worktree's build at its own game folder and the
shared executable (docs/parallel-work.md). The run is always --multi, so it
never ends another run's game.

Milestones, in the order a run reaches them:
  boot       the image maps, binds and enters the game
  genie      data\\genie.dat is read and inflated (the host's decoder)
  directdraw the 800x600x8 mode is set and 100 frames are presented
  menu       the main menu's child windows exist (its text-entry control)
  exit       a click on the menu's Exit button ends the game with code 0
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHECKS = [
    ('boot', r'entering 0x00632881'),
    ('genie', r'open data\\genie\.dat -> ok'),
    ('directdraw', r'(?s)SetDisplayMode\(800, 600, 8\).*frame 100 presented'),
    ('menu', r'CreateWindowExA "InputDisabledWindow"'),
    ('exit', r'ExitProcess\(0\)'),
]


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('run', nargs='?', default=os.path.join(ROOT, 'build', 'swgbrecomp.exe'))
    ap.add_argument('--game', default=os.path.join(ROOT, 'game'))
    ap.add_argument('--exe', default=os.path.join(ROOT, 'work', 'Battlegrounds.exe'))
    a = ap.parse_args()
    exe = os.path.abspath(a.run)
    data = [a.game, a.exe]
    if not all(os.path.exists(p) for p in data):
        for name, _ in CHECKS:
            print('SKIP: %s (no game data in game\\ and work\\)' % name)
        print('0/%d (skipped)' % len(CHECKS))
        return 0
    if not os.path.exists(exe):
        sys.exit('no %s: build it first (README, Building from source)' % exe)
    # The menu is up within ten seconds; Exit is bottom left (40, 565).
    cmd = [exe, '--run', '--headless', '--multi', '--files', '--watchdog', '60',
           '--game', os.path.abspath(a.game), '--exe', os.path.abspath(a.exe),
           '--keys', '20:move:40:565,21:click:40:565']
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors='replace', timeout=120)
    log = r.stdout + r.stderr
    open(os.path.join(ROOT, 'work', 'conformance.log'), 'w', encoding='utf-8').write(log)
    passed = 0
    for name, pat in CHECKS:
        ok = re.search(pat, log) is not None and (name != 'exit' or r.returncode == 0)
        passed += ok
        print('%s: %s' % ('PASS' if ok else 'FAIL', name))
    print('%d/%d' % (passed, len(CHECKS)))
    return 0 if passed == len(CHECKS) else 1


if __name__ == '__main__':
    sys.exit(main())
