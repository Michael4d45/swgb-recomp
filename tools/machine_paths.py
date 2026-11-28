#!/usr/bin/env python3
"""Fails on a Windows drive path (X:\\...) in the code: a default that only works on one machine.

    python tools/machine_paths.py        (.github/workflows/unit.yml runs it)

Looks at the files git tracks in tools/, src/ (not the lifted C) and cmake/, and every .sh,
.ps1, .cmd and .bat. Defaults belong to tools/swgb/runner.py's paths() (SWGB_STAR,
tools/local.json, else the checkout). ALLOW lists what is not a machine path: the game's
own drives inside the recompiled game (C: is its folder, D: its disc) and a prompt's
example. Exit 1 with each line found.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVE = re.compile(r'(?<![\w%])[A-Za-z]:\\')
SCOPE = ['tools', 'src', 'cmake', '*.sh', '*.ps1', '*.cmd', '*.bat', ':!src/recomp/gen*',
         ':!tools/machine_paths.py']     # this file: its ALLOW list is drive paths
# (file, text on the line): what the guest sees, not where this machine keeps things
ALLOW = [
    ('src/linux/main.c', '"D:\\\\"'),                      # the disc's drive in the game's registry
    ('src/linux/main.c', "game's folder is C:\\"),
    ('src/linux/main.c', '"C:\\\\" GUEST_EXE'),
    ('src/linux/main.c', '"\\"C:\\\\" GUEST_EXE'),
    ('src/linux/shims.c', 'C:\\WINDOWS\\TEMP\\'),
    ('src/linux/shims.c', '"C:\\\\WINDOWS\\\\TEMP\\\\"'),
    ('src/runtime/cdaudio.c', '"C:\\\\Music\\\\%s"'),
    ('tools/setup.ps1', 'Its drive (E:\\)'),
]


def find(files, read=lambda f: open(os.path.join(ROOT, f), encoding='utf-8', errors='replace')):
    """[(file, line number, line)] with a drive path that ALLOW does not name."""
    out = []
    for f in files:
        with read(f) as fh:
            for n, line in enumerate(fh, 1):
                if DRIVE.search(line) and not any(f == af and text in line for af, text in ALLOW):
                    out.append((f, n, line.rstrip()))
    return out


def main():
    files = subprocess.run(['git', '-C', ROOT, 'ls-files', '--'] + SCOPE, capture_output=True, text=True,
                           check=True).stdout.split()
    found = find(files)
    for f, n, line in found:
        print('%s:%d: %s' % (f, n, line.strip()))
    if found:
        print('%d machine path(s): take the default from tools/swgb/runner.py\'s paths() instead '
              '(or, for a path the game itself sees, add it to ALLOW in tools/machine_paths.py)' % len(found))
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main())
