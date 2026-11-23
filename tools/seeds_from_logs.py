#!/usr/bin/env python3
"""A crash's unresolved address, made a seed in one step.

Scans run logs for the lines the host and pcrecomp print when lifted code
reaches an address with no lifted function:

    ICALL: unresolved VA 0x005648A0 from 0x00406E20
    ITAIL: unresolved VA 0x00646465 from 0x0064F3C0

and prints each address with where it is (the catalog function whose extent
holds it, the image bytes there), skipping those already in RUN_SEEDS, the
seed file or the catalog. --merge adds the new ones to work/run_seeds.json,
which run_lift.py reads; then re-lift and rebuild (docs/lifting.md).

    py -3 tools/seeds_from_logs.py                 # the default logs
    py -3 tools/seeds_from_logs.py --merge
    py -3 tools/seeds_from_logs.py LOG [LOG...]    # these logs only
    py -3 tools/seeds_from_logs.py --cc LOG ...    # Clone Campaigns (run_lift.py --build cc)

The default logs: A:/Games/star/logs/play-*.log (the launcher's), work/*.log
(conformance), and work/soak/**/*.log (tools/soak.py). Logs written by
PowerShell's Tee are UTF-16; both encodings are read.
"""
import argparse
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

LINE = re.compile(r'(ICALL|ITAIL): unresolved VA (0x[0-9A-Fa-f]{1,8})(?: from (0x[0-9A-Fa-f]{1,8}))?')
CODE = (0x00401000, 0x006509B9)       # .text and THIS_COD
CC_CODE = (0x00401000, 0x006539B9)    # battlegrounds_x1.exe's


def read_log(path):
    data = open(path, 'rb').read()
    if data[:2] in (b'\xff\xfe', b'\xfe\xff'):
        return data.decode('utf-16', errors='replace')
    if data.count(b'\0') > len(data) // 8:     # UTF-16 without (or after) a BOM, or mixed
        data = data.replace(b'\0', b'')
    return data.decode('utf-8', errors='replace')


def default_logs():
    pats = [r'A:\Games\star\logs\play-*.log',
            os.path.join(ROOT, 'work', '*.log'),
            os.path.join(ROOT, 'work', 'soak', '**', '*.log')]
    out = []
    for p in pats:
        out += glob.glob(p, recursive=True)
    return sorted(set(out))


def known_seeds(seed_file, names=('RUN_SEEDS', 'HLE')):
    """RUN_SEEDS and HLE (CC_SEEDS and CC_HLE) from run_lift.py (read, not
    imported: no pcrecomp needed), and the seed file."""
    import ast
    seeds = set()
    tree = ast.parse(open(os.path.join(ROOT, 'run_lift.py'), encoding='utf-8').read())
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(getattr(t, 'id', '') in names for t in node.targets):
            seeds |= set(ast.literal_eval(node.value))
    if os.path.exists(seed_file):
        seeds |= {e['address'] if isinstance(e, dict) else e for e in json.load(open(seed_file))}
    return seeds


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='*', help='logs to scan (default: the launcher, conformance and soak logs)')
    ap.add_argument('--cc', action='store_true',
                    help="Clone Campaigns: battlegrounds_x1.exe's catalog, seeds and code range")
    ap.add_argument('--catalog', default=None, help='default work/functions.json (--cc: functions_x1.json)')
    ap.add_argument('--seeds', default=None, help='default work/run_seeds.json (--cc: run_seeds_cc.json)')
    ap.add_argument('--merge', action='store_true', help='add the new addresses to --seeds')
    args = ap.parse_args()
    args.catalog = args.catalog or os.path.join(ROOT, 'work', 'functions_x1.json' if args.cc else 'functions.json')
    args.seeds = args.seeds or os.path.join(ROOT, 'work', 'run_seeds_cc.json' if args.cc else 'run_seeds.json')
    code = CC_CODE if args.cc else CODE

    logs = args.logs or default_logs()
    hits = {}                                    # va -> [(kind, from, log)]
    for path in logs:
        try:
            text = read_log(path)
        except OSError as e:
            print('skip %s: %s' % (path, e), file=sys.stderr)
            continue
        for m in LINE.finditer(text):
            va = int(m.group(2), 16)
            hits.setdefault(va, []).append((m.group(1), int(m.group(3), 16) if m.group(3) else None, path))
    print('%d logs, %d unresolved addresses' % (len(logs), len(hits)))
    if not hits:
        return 0

    cat, bodies = set(), []
    if os.path.exists(args.catalog):
        fs = json.load(open(args.catalog))['functions']
        cat = {f['address'] for f in fs}
        bodies = sorted((f['address'], f['end']) for f in fs if f.get('entry_kind') == 'start')
    seeds = known_seeds(args.seeds, ('CC_SEEDS', 'CC_HLE') if args.cc else ('RUN_SEEDS', 'HLE'))

    new = []
    for va in sorted(hits):
        where = hits[va]
        froms = sorted({'0x%08X' % f for _, f, _ in where if f is not None})
        holder = [a for a, e in bodies if a <= va < e]
        note = []
        if not (code[0] <= va < code[1]):
            note.append('not in the code sections: a bad pointer, not a missing entry -- not seeded')
        elif va in seeds:
            note.append('already a seed: is this build older than the lift?')
        elif va in cat:
            note.append('already a catalog entry: is this build older than the lift?')
        else:
            if holder:
                note.append('inside sub_%08X\'s extent (a body the catalog ran over it)' % holder[-1])
            new.append(va)
        print('0x%08X  %s x%d from %s  %s  [%s]' % (va, '/'.join(sorted({k for k, _, _ in where})), len(where),
                                                  ','.join(froms[:4]) or '?', '; '.join(note),
                                                  os.path.basename(where[0][2])))
    if not new:
        print('nothing new to seed')
        return 0
    print('%d new: %s' % (len(new), ', '.join('0x%08X' % v for v in new)))
    if args.merge:
        cur = json.load(open(args.seeds)) if os.path.exists(args.seeds) else []
        have = {e['address'] if isinstance(e, dict) else e for e in cur}
        for va in new:
            if va not in have:
                cur.append({'address': va, 'source': 'seeds_from_logs',
                            'from': sorted({'0x%08X' % f for _, f, _ in hits[va] if f is not None})})
        os.makedirs(os.path.dirname(args.seeds), exist_ok=True)
        with open(args.seeds, 'w', newline='\n') as f:
            json.dump(cur, f, indent=1)
        print('merged into %s; now re-lift: py -3 run_lift.py %s--all, and rebuild'
              % (args.seeds, '--build cc ' if args.cc else ''))
    else:
        print('--merge adds them to %s (run_lift.py reads it)' % args.seeds)
    return 0


if __name__ == '__main__':
    sys.exit(main())
