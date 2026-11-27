#!/usr/bin/env python3
"""How smoothly objects move on screen, from the host's draw log (--smooth, docs/framerate.md).

    py -3 tools/smooth_check.py DRAW.LOG [--top N] [--json FILE]

SWGB_SMOOTH_LOG=FILE makes the host (with or without --smooth) write a line for every object
the map view draws, every frame:

    ms step step_ms object id x y sx sy ox oy

ms the wall clock (timeGetTime), step the world's step count and step_ms its clock, the
object's address and id, its position in tiles (x, y), where the game draws it (sx, sy: the
tile's place plus the object's offset in the tile, screen pixels) and what --smooth adds to
that (ox, oy; 0 without it). So one log of a --smooth run gives both pictures: the game's own
(sx, sy) and the one shown (sx + ox, sy + oy).

For each object that moved, between its first and last move:

  frames     frames drawn
  steps      world steps in which it moved
  plain      the game's own draw: the largest change from one frame to the next (pixels),
             and the share of frames in which it did not move at all
  smooth     the same, for what --smooth shows (the 95th percentile change too)

The view must not scroll while it is measured: a scroll moves everything on screen.
"""
import argparse
import collections
import json
import sys


def read(path):
    """{(object, id): [(ms, step, x, y, sx, sy, ox, oy)]}, one entry a frame (the last draw of it)."""
    rows = collections.defaultdict(dict)
    for line in open(path, encoding='ascii', errors='replace'):
        p = line.split()
        if len(p) != 11:
            continue
        try:
            ms, step = int(p[0]), int(p[1])
            x, y = float(p[5]), float(p[6])
            sx, sy, ox, oy = (int(v) for v in p[7:11])
        except ValueError:
            continue
        rows[(p[3], p[4])][ms] = (ms, step, x, y, sx, sy, ox, oy)
    return {k: [v[t] for t in sorted(v)] for k, v in rows.items()}


def pct(values, q):
    if not values:
        return 0.0
    v = sorted(values)
    return v[min(len(v) - 1, int(q * len(v)))]


def measure(frames):
    """The figures for one object, or None if it never moved."""
    moved = [i for i in range(1, len(frames)) if frames[i][2:4] != frames[i - 1][2:4]]
    if len(moved) < 2:
        return None
    a, b = moved[0], moved[-1]
    seg = frames[a - 1:b + 1]
    plain, smooth = [], []
    for f0, f1 in zip(seg, seg[1:]):
        plain.append(((f1[4] - f0[4]) ** 2 + (f1[5] - f0[5]) ** 2) ** .5)
        smooth.append(((f1[4] + f1[6] - f0[4] - f0[6]) ** 2 + (f1[5] + f1[7] - f0[5] - f0[7]) ** 2) ** .5)
    n = len(plain)
    return {
        'frames': n, 'steps': len(moved), 'seconds': (seg[-1][0] - seg[0][0]) / 1000.0,
        'smoothed': any(f[6] or f[7] for f in seg),
        'plain_max': round(max(plain), 1), 'plain_still': round(sum(1 for d in plain if d == 0) / n, 3),
        'plain_mean_moving': round(sum(plain) / max(1, sum(1 for d in plain if d)), 1),
        'smooth_max': round(max(smooth), 1), 'smooth_p95': round(pct(smooth, 0.95), 1),
        'smooth_still': round(sum(1 for d in smooth if d == 0) / n, 3),
    }


def analyse(path):
    """Every object that moved, the one that moved in the most steps first."""
    out = []
    for (obj, oid), frames in read(path).items():
        m = measure(frames)
        if m:
            m['object'], m['id'] = obj, int(oid)
            out.append(m)
    out.sort(key=lambda m: -m['steps'])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('--top', type=int, default=10)
    ap.add_argument('--json', help='write the figures here too')
    a = ap.parse_args()
    res = analyse(a.log)
    if not res:
        print('no object moved in %s' % a.log)
        return 1
    print('%-10s %5s %6s %6s | %-27s | %s' % ('object', 'id', 'frames', 'steps', 'plain: max px, still', 'smooth: max px, p95, still'))
    for m in res[:a.top]:
        print('%-10s %5d %6d %6d | %5.1f %4.0f%%  (a move %4.1f px) | %5.1f %5.1f %4.0f%%%s' % (
            m['object'], m['id'], m['frames'], m['steps'], m['plain_max'], 100 * m['plain_still'], m['plain_mean_moving'],
            m['smooth_max'], m['smooth_p95'], 100 * m['smooth_still'], '' if m['smoothed'] else '  (not smoothed: no --smooth)'))
    if a.json:
        json.dump(res, open(a.json, 'w'), indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
