#!/usr/bin/env python3
"""Lift regression: after a re-lift, is the lifted C what it was?

    py -3 tools/lift_check.py [--cc] [--gen DIR] [--stats FILE] [--baseline FILE] [--save] [--json FILE]

Reads every generated file in DIR (src/recomp/gen, or gen_cc with --cc) and
reports:

  functions  the dispatch table's entries (recomp_dispatch.c), and the
             function bodies found in the recomp_*.c files
  errors     run_lift.py's lift errors and not-lifted stubs (work/lift_stats.json,
             lift_stats_cc.json): both must be 0
  identical  against a baseline (a SHA-1 of each function's C, by address):
             functions whose C is byte-identical, changed, new and gone

It fails (exit 1) on any lift error or stub, on a function count below the
baseline's, on a function in the dispatch table without a body (or the other
way round), and on any changed or vanished function. New functions alone pass:
a seed adds them. --save writes the baseline (do it once the lift is
accepted). With no baseline yet, it writes one and passes.

The baseline holds hashes only, never the C: it is derived from the game's
executable, so it stays on this machine (by default beside the lift, in the
checkout's work\\ folder, which git ignores).
"""
import argparse
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FUNC = re.compile(rb'^void (sub_[0-9A-F]{8})\(void\) \{', re.M)   # also the host's one-liners (run_lift.py HLE)
DISPATCH = re.compile(rb'\{ 0x([0-9A-F]{8})u, (sub_[0-9A-F]{8}) \}')


def scan(gen):
    """{name: sha1} of every lifted function's C, and the dispatch table's names."""
    funcs, dispatch, files = {}, [], 0
    for fn in sorted(os.listdir(gen)):
        if not fn.endswith('.c'):
            continue
        data = open(os.path.join(gen, fn), 'rb').read()
        if fn == 'recomp_dispatch.c':
            dispatch = [m.group(2).decode() for m in DISPATCH.finditer(data)]
            continue
        if not fn.startswith('recomp_'):
            continue
        files += 1
        starts = [(m.start(), m.group(1).decode()) for m in FUNC.finditer(data)]
        for k, (pos, name) in enumerate(starts):
            end = starts[k + 1][0] if k + 1 < len(starts) else len(data)
            funcs[name] = hashlib.sha1(data[pos:end].rstrip()).hexdigest()[:16]
    return funcs, dispatch, files


def check(gen, stats_path, baseline_path, save=False):
    """The report as a dict: ok, lines (what to print), and the numbers."""
    rep = {'gen': gen, 'ok': True, 'lines': [], 'problems': []}

    def bad(msg):
        rep['ok'] = False
        rep['problems'].append(msg)

    if not os.path.isdir(gen):
        rep.update(ok=None, skipped='no lift at %s' % gen)
        return rep
    funcs, dispatch, files = scan(gen)
    rep.update(functions=len(dispatch), bodies=len(funcs), files=files)
    rep['lines'].append('%d functions in the dispatch table, %d bodies in %d files' % (len(dispatch), len(funcs), files))
    missing = sorted(set(dispatch) - set(funcs))
    extra = sorted(set(funcs) - set(dispatch))
    if missing:
        bad('%d in the dispatch table without a body: %s' % (len(missing), ' '.join(missing[:8])))
    if extra:
        bad('%d bodies not in the dispatch table: %s' % (len(extra), ' '.join(extra[:8])))
    if stats_path and os.path.exists(stats_path):
        st = json.load(open(stats_path))
        rep['stats'] = st
        rep['lines'].append('run_lift.py: lifted %s, errors %s, stubs %s, no terminator %s'
                            % (st.get('lifted'), st.get('errors'), st.get('stubs'), st.get('no_terminator')))
        if st.get('errors'):
            bad('%d lift errors' % st['errors'])
        if st.get('stubs'):
            bad('%d not-lifted stubs' % st['stubs'])
    else:
        rep['lines'].append('no %s: errors and stubs not checked' % stats_path)
    if baseline_path and os.path.exists(baseline_path) and not save:
        base = json.load(open(baseline_path))
        old = base['functions']
        same = sum(1 for k, v in old.items() if funcs.get(k) == v)
        changed = sorted(k for k, v in old.items() if k in funcs and funcs[k] != v)
        gone = sorted(k for k in old if k not in funcs)
        new = sorted(k for k in funcs if k not in old)
        rep.update(identical=same, changed=len(changed), gone=len(gone), new=len(new),
                   changed_names=changed[:200], gone_names=gone[:200], new_names=new[:200],
                   baseline=baseline_path, baseline_functions=len(old))
        rep['lines'].append('against the baseline (%d functions, %s): %d identical, %d changed, %d gone, %d new'
                            % (len(old), base.get('saved', '?'), same, len(changed), len(gone), len(new)))
        if changed:
            rep['lines'].append('  changed: ' + ' '.join(changed[:12]) + (' ...' if len(changed) > 12 else ''))
            bad('%d functions whose C changed' % len(changed))
        if gone:
            rep['lines'].append('  gone: ' + ' '.join(gone[:12]) + (' ...' if len(gone) > 12 else ''))
            bad('%d functions gone' % len(gone))
        if new:
            rep['lines'].append('  new: ' + ' '.join(new[:12]) + (' ...' if len(new) > 12 else ''))
        if len(dispatch) < base.get('dispatch', 0):
            bad('the function count fell from %d to %d' % (base['dispatch'], len(dispatch)))
    elif baseline_path:
        import datetime
        os.makedirs(os.path.dirname(os.path.abspath(baseline_path)), exist_ok=True)
        json.dump({'saved': datetime.datetime.now().isoformat(timespec='seconds'), 'gen': gen,
                   'dispatch': len(dispatch), 'functions': funcs}, open(baseline_path, 'w'))
        rep['baseline_saved'] = baseline_path
        rep['lines'].append('baseline written: %s' % baseline_path)
    return rep


def defaults(cc, gen=None):
    gen = gen or os.path.join(ROOT, 'src', 'recomp', 'gen_cc' if cc else 'gen')
    checkout = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(gen))))
    stats = os.path.join(checkout, 'work', 'lift_stats_cc.json' if cc else 'lift_stats.json')
    baseline = os.path.join(checkout, 'work', 'lift_baseline_cc.json' if cc else 'lift_baseline.json')
    return gen, stats, baseline


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cc', action='store_true', help='Clone Campaigns: src/recomp/gen_cc, lift_stats_cc.json')
    ap.add_argument('--gen')
    ap.add_argument('--stats')
    ap.add_argument('--baseline')
    ap.add_argument('--save', action='store_true', help='write the baseline from this lift')
    ap.add_argument('--json', help='write the report here')
    a = ap.parse_args()
    gen, stats, baseline = defaults(a.cc, a.gen)
    rep = check(gen, a.stats or stats, a.baseline or baseline, a.save)
    if rep.get('skipped'):
        print('SKIP: ' + rep['skipped'])
        return 0
    for line in rep['lines']:
        print(line)
    for p in rep['problems']:
        print('FAIL: ' + p)
    print('PASS' if rep['ok'] else 'FAIL')
    if a.json:
        json.dump(rep, open(a.json, 'w'), indent=1)
    return 0 if rep['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
