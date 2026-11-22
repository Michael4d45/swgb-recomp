#!/usr/bin/env python3
"""Galactic Battlegrounds lift driver: Battlegrounds.exe -> src/recomp/gen/.

Battlegrounds.exe (v1.1) is the whole game (MSVC6 C++, DirectDraw, base
0x00400000, no relocations), catalogued by disasm32 into work/functions.json.
The driver is Crazy Taxi's, plus MSVC exception frames: every function that
installs a C++ try or a __try gets a landing pad (pcrecomp tools/lift/eh32.py,
docs/exceptions.md).

`--build cc` lifts the Clone Campaigns expansion instead: Steam's
battlegrounds_x1.exe, unwrapped (tools/steamstub_swgb.py), from
work/functions_x1.json into src/recomp/gen_cc/ (docs/lifting.md).

    py -3 run_lift.py --all
    py -3 run_lift.py --roots 0x00632881
    py -3 run_lift.py --build cc --all
"""
import argparse
import hashlib
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
# PCRECOMP picks the toolkit checkout, the same knob CMakeLists.txt has: the
# lifter and the runtime header must come from the same tree.
_TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(_HERE, '..', 'pcrecomp')), 'tools')
sys.path.insert(0, os.path.join(_TOOLS, 'lift'))
sys.path.insert(0, os.path.join(_TOOLS, 'pe'))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32          # noqa: E402
from generate import (EXTENT_REACH, closure, find_splits, true_extent,  # noqa: E402
                      linear_disassemble_function, lift_function_linear, write_chunk)
from lift32 import Lifter                                  # noqa: E402
import eh32                                                # noqa: E402
from pe_analyze import analyze_pe, build_iat_map           # noqa: E402

_WORK = os.path.join(_HERE, 'work')

# Entries the catalog does not find: 0x00427090 (called through a pointer
# from 0x00424425), and the THIS_COD blitter's jump-table targets (tables at
# 0x0064C300-0x0064E780, read from the image; a skirmish map's first frame
# jumped to 0x00646465, not a function entry).
RUN_SEEDS = [
    0x00427090,
    # Entries named only by a pointer in data or an immediate in code, that the
    # catalog lost (tools/code_ptr_seeds.py classifies every such pointer;
    # docs/lifting.md, "Pointers the catalog dropped"). Vtable slots:
    # 0x0066100C, folded into sub_0056480C, a false body decoded from
    # sub_005641F0's switch tables: called from 0x00406E20 a minute or two into
    # a skirmish, an unresolved ICALL and a null read in sub_005C45C0.
    0x005648A0,
    0x005C6D90,     # 0x00664550, behind sub_005C63B0's switch table (false sub_005C6D61, sub_005C6D6F)
    0x004C39B0,     # 0x00659C18 and 0x006649C4: `int3; mov al, 1; ret 4`
    # zlib's default zalloc/zfree and friends, stored into a z_stream by code
    # the catalog does not reach (0x00623A84..0x006248A4: mov [ebx+18h], imm).
    0x00625E80, 0x00625E90, 0x00625EA0, 0x00625EC0, 0x00625EE0, 0x00625EF0, 0x00625F00,
    0x00626060, 0x00626070, 0x00626080, 0x006260B0, 0x006260D0, 0x006260F0, 0x00626110,
    0x00626130, 0x00626140,
    0x00646465,
    0x0064EE80,
    0x0064EEA0,
    0x0064EEA5,
    0x0064EF40,
    0x0064EF51,
    0x0064EF80,
    0x0064F020,
    0x0064F0C0,
    0x0064F160,
    0x0064F3E0,
    0x0064F400,
    0x0064F420,
    0x0064F460,
    0x0064F46B,
    0x0064F480,
    0x0064F4C0,
    0x0064F500,
    0x0064F540,
    0x0064F580,
    0x0064F596,
    0x0064F5C0,
    0x0064F5D6,
    0x0064F640,
    0x0064F660,
    0x0064F680,
    0x0064F6A0,
    0x00650080,
    0x00650111,
    0x00650117,
    0x0065011B,
    0x00650124,
    0x0065012B,
    0x00650136,
    0x00650140,
    0x00650149,
    0x00650155,
    0x0065015B,
    0x00650162,
    0x00650168,
    0x0065016E,
    0x00650172,
    0x0065017B,
]

# Clone Campaigns, battlegrounds_x1.exe. Its THIS_COD is the base game's byte
# for byte but for absolute addresses, moved by +0x3000 (code) and +0x2000
# (data): the jump tables are at 0x0064F300-0x00651780, and their targets are
# the base game's plus 0x3000 (each checked against x1's tables). 0x00427920
# is the base game's 0x00427090, found by its bytes with addresses and branch
# offsets masked (one match).
CC_SEEDS = [0x00427920] + [va + 0x3000 for va in RUN_SEEDS[1:]]

DATA_IN_CODE = []


def is_data(va):
    return any(lo <= va < hi for lo, hi in DATA_IN_CODE)

# Fixes to the game itself: C run after the instruction at the address.
PATCHES = {}

# Functions the host does instead (src/runtime/*.c says why for each).
HLE = {
    0x0062343B: 'swgb_inflate',   # the deflate decoder, a stack-copying coroutine (inflate.c)
}
CC_HLE = {
    0x006271AB: 'swgb_inflate',   # the same decoder, byte for byte but for its addresses
}

# What --build picks: its modules (file, catalog, PE timestamp of the one
# build supported, what it is, a digest of its code or None), where its lift
# goes, and its own addresses. A different build lifts, but the patches and
# seeds are at this build's addresses: it would crash in the game, so it is
# refused (README).
BUILDS = {
    'base': {
        'modules': [(os.path.join(_WORK, 'Battlegrounds.exe'), os.path.join(_WORK, 'functions.json'),
                     0x3C03FD69, 'Battlegrounds.exe v1.1, from LucasArts BGUpdate1_1.exe (2001-11-28)', None)],
        'out': os.path.join(_HERE, 'src', 'recomp', 'gen'),
        'stats': os.path.join(_WORK, 'lift_stats.json'),
        'seeds': os.path.join(_WORK, 'run_seeds.json'),
        'run_seeds': RUN_SEEDS, 'hle': HLE, 'name': 'Battlegrounds.exe',
    },
    # Steam's battlegrounds_cc.exe has the same timestamp: it is this build
    # with a community widescreen mod applied, other code, refused by digest.
    'cc': {
        'modules': [(os.path.join(_WORK, 'battlegrounds_x1.exe'), os.path.join(_WORK, 'functions_x1.json'),
                     0x3CF57142, "battlegrounds_x1.exe, Clone Campaigns (2002-05-29), Steam's, unwrapped",
                     '66c989ce5835d122')],
        'out': os.path.join(_HERE, 'src', 'recomp', 'gen_cc'),
        'stats': os.path.join(_WORK, 'lift_stats_cc.json'),
        'seeds': os.path.join(_WORK, 'run_seeds_cc.json'),
        'run_seeds': CC_SEEDS, 'hle': CC_HLE, 'name': 'battlegrounds_x1.exe',
    },
}


def apply_patches(body, patches):
    """Add each patch's C after the lines that lift its instruction.

    Some instructions lift to a block over several lines (idiv's `{ ... }`):
    the patch goes after the block closes, not inside it. From the Tiberium
    Wars driver (tiberiumwars-recomp run_lift.py)."""
    for va, c in patches.items():
        tag = '/* 0x%08X:' % va
        k = body.find(tag)
        if k != -1:
            s = body.rfind('\n', 0, k) + 1
            e = body.index('\n', k)
            depth = body[s:e].count('{') - body[s:e].count('}')
            while depth > 0:
                s, e = e + 1, body.index('\n', e + 1)
                depth += body[s:e].count('{') - body[s:e].count('}')
            body = body[:e + 1] + '    ' + c + '\n' + body[e + 1:]
    return body


def load_seeds(path):
    """Addresses from a seed file: a list of {'address': va} or of plain VAs."""
    if not path or not os.path.exists(path):
        return set()
    return {e['address'] if isinstance(e, dict) else e for e in json.load(open(path))}


def exe_stamp(path):
    """The PE header's TimeDateStamp: which build of a module this is."""
    with open(path, 'rb') as f:
        head = f.read(4096)
    nt = int.from_bytes(head[0x3C:0x40], 'little')
    return int.from_bytes(head[nt + 8:nt + 12], 'little')


def exe_sections(path):
    """{name: raw bytes, up to the virtual size} of a PE's sections."""
    raw = open(path, 'rb').read()
    nt = int.from_bytes(raw[0x3C:0x40], 'little')
    n = int.from_bytes(raw[nt + 6:nt + 8], 'little')
    sh = nt + 24 + int.from_bytes(raw[nt + 20:nt + 22], 'little')
    out = {}
    for k in range(n):
        h = raw[sh + 40 * k:sh + 40 * k + 40]
        vsize, rsize, roff = (int.from_bytes(h[o:o + 4], 'little') for o in (8, 16, 20))
        out[h[:8].rstrip(b'\0').decode('latin-1')] = raw[roff:roff + min(vsize, rsize)]
    return out


def code_digest(path):
    """The first 16 hex digits of SHA-256 over .text and THIS_COD: the code itself."""
    sec = exe_sections(path)
    return hashlib.sha256(sec.get('.text', b'') + sec.get('THIS_COD', b'')).hexdigest()[:16]


class Out:
    """The generated tree: chunks numbered across both modules."""

    def __init__(self, path, split):
        self.path, self.split = path, split
        self.chunk, self.idx, self.entries = [], 0, []

    def add(self, body, addr, name):
        self.chunk.append((body, addr, name))
        self.entries.append((addr, name))
        if len(self.chunk) >= self.split:
            self.flush()

    def flush(self):
        if self.chunk:
            write_chunk(self.path, self.idx, self.chunk)
            self.idx += 1
            self.chunk = []


def lift_module(path, catalog, args, out, extra_roots, build):
    """Lift one module into `out`. Returns (entry VA, stats)."""
    info = analyze_pe(path)
    iat = build_iat_map(info)
    cs, ce = info.code_start, info.code_end
    print('[*] %s: base=0x%08X code=0x%08X-0x%08X IAT=%d'
          % (os.path.basename(path), info.image_base, cs, ce, len(iat)))

    # The code section, by address.
    cat = json.load(open(catalog))
    byaddr = {f['address']: f for f in cat['functions'] if cs <= f['address'] < ce and not is_data(f['address'])}
    print('[*]   catalog: %d functions inside the code section' % len(byaddr))

    raw = open(path, 'rb').read()
    # The code range spans sections here (THIS_COD after .text: matrix and
    # blit routines), so every section is laid out at its address. Read from
    # .text alone, a function in the second section lifted to an empty body
    # that never popped its return address.
    code = bytearray(ce - cs)
    for sec in info.sections:
        lo = info.image_base + sec.virtual_address
        a, b = max(lo, cs), min(lo + sec.raw_size, ce)
        if a < b:
            code[a - cs:b - cs] = raw[sec.raw_offset + a - lo:sec.raw_offset + b - lo]
    code = bytes(code)

    def read(va, n):
        """n bytes of the image at va (FuncInfo and scope tables live in .rdata)."""
        for sec in info.sections:
            lo = info.image_base + sec.virtual_address
            if lo <= va and va + n <= lo + sec.raw_size:
                return raw[sec.raw_offset + va - lo:sec.raw_offset + va - lo + n]
        return None

    # Bound each injected entry by the next known one: an entry landing
    # mid-code handed `ce` makes the extent walk descend the whole section.
    want = {a for a in set(build['run_seeds']) | load_seeds(args.seeds) if cs <= a < ce and a not in byaddr}
    known = sorted(set(byaddr) | want)
    nxt = {a: (known[i + 1] if i + 1 < len(known) else ce) for i, a in enumerate(known)}
    for a in want:
        byaddr[a] = {'address': a, 'end': min(nxt[a], a + 0x100), 'calls_to': [], 'entry_kind': 'start'}

    entry = info.image_base + info.entry_point_rva
    roots = [entry] + [info.image_base + e.rva for e in getattr(info, 'exports', [])
                       if getattr(e, 'rva', 0)] + [r for r in extra_roots if cs <= r < ce]
    chosen = set(byaddr) if args.all else set(closure(byaddr, roots, args.max))
    print('[*]   lifting %d of %d functions (%s)'
          % (len(chosen), len(byaddr), 'all' if args.all else 'closure, cap %d' % args.max))

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    # MSVC exception frames (eh32.frame). Where a handler continues inside the
    # function that installed the frame (an __except block, a catch's
    # continuation) is a label in that body, never an entry of its own:
    # disasm32 takes a scope table's __except pointers for functions, and
    # dispatched as one an __except block would run as a call and "return"
    # into _except_handler3.
    frames = {}
    for a in sorted(byaddr):
        head = list(md.disasm(code[a - cs:a - cs + 64], a, 12))
        if not any(i.op_str == 'dword ptr fs:[0], esp' for i in head):
            continue
        reached = set()
        end, _ = true_extent(md, code, cs, a, min(a + EXTENT_REACH, ce), set(byaddr), reached=reached)
        insns, _ = linear_disassemble_function(md, code, cs, a, end, reached=reached)
        fr = eh32.frame(md, read, insns)
        if fr and fr[0]:
            frames[a] = fr
    resume = set().union(*(r for r, _ in frames.values())) if frames else set()
    # A seed that is a handler's continuation would run as a call and "return"
    # into _except_handler3: code_ptr_seeds.py files those as `seh`, never seeds.
    bad = resume & set(RUN_SEEDS)
    if bad:
        sys.exit('seeds that are exception-handler continuations: %s'
                 % ', '.join('0x%08X' % a for a in sorted(bad)))
    dropped = resume & set(byaddr)
    for a in dropped:
        del byaddr[a]
    chosen -= dropped
    print('[*]   %d functions install an exception frame with somewhere to continue '
          '(%d frame-pointer-omitted); %d resume points, %d were catalog entries'
          % (len(frames), sum(1 for _, o in frames.values() if o == 0), len(resume), len(dropped)))

    # precise carry: adc/sbb take their carry from the flag state that set it.
    # The default reads a `_cf` that add/sub/cmp never write, which breaks every
    # 64-bit add and subtract (The Movies lost its menu video to it).
    lifter = Lifter(iat_map=iat, lifted=set(byaddr), precise_carry=True, precise_sbb=True)
    ordered = sorted(byaddr)
    starts = {a for a in ordered if byaddr[a].get('entry_kind') != 'alias'}
    splits = find_splits(md, code, cs, ce, starts)
    starts -= splits          # still dispatchable; just not a wall inside its parent
    errors = dirty = 0

    def lift_one(addr, name, end, reached):
        nonlocal errors
        try:
            lo = min(reached) if reached else addr   # a chunk can sit below the entry
            insns, leaders = (linear_disassemble_function(md, code, cs, lo, end, reached=reached)
                              if end > addr else ([], None))
            if leaders is not None:
                leaders.add(addr)
            resume, off = frames.get(addr, (None, None))
            if addr in build['hle']:
                out.add('void %s(void) { %s(); }\n' % (name, build['hle'][addr]), addr, name)
                return
            if not insns:
                raise ValueError('no instructions at 0x%08X' % addr)
            body = lift_function_linear(lifter, name, insns, leaders, addr, eh_resume=resume, eh_off=off)
            body = apply_patches(body, {va: c for va, c in PATCHES.items() if reached and va in reached})
        except Exception as e:                      # noqa: BLE001 -- counted, not hidden
            body = '/* ERROR %s: %s */\nvoid %s(void) { }\n' % (name, e, name)
            errors += 1
        out.add(body, addr, name)

    # A direct branch or call to an address nothing catalogued is a tail call
    # or a jump into shared code the catalog missed. The target has to be
    # dispatchable or the RECOMP_ITAIL cannot resolve, so each round's outside
    # targets are lifted in the next round, until none are new.
    todo, added = sorted(chosen), 0
    while todo:
        outside = set()
        for addr in todo:
            reached, behind, called = set(), set(), set()
            end, clean = true_extent(md, code, cs, addr, min(addr + EXTENT_REACH, ce), starts,
                                     reached=reached, behind=behind, called=called,
                                     seeds=frames.get(addr, ((),))[0])
            outside |= {t for t in behind if t not in reached} | called
            dirty += not clean
            lift_one(addr, 'sub_%08X' % addr, end, reached)
        todo = sorted(t for t in outside if cs <= t < ce and t not in byaddr and not is_data(t))
        for t in todo:
            byaddr[t] = {'address': t, 'end': ce, 'calls_to': [], 'entry_kind': 'start'}
            chosen.add(t)
        added += len(todo)

    stubs = [a for a in ordered if a not in chosen]
    for a in stubs:
        name = 'sub_%08X' % a
        out.add('void %s(void) { RECOMP_NOT_LIFTED(0x%08Xu); }\n' % (name, a), a, name)
    print('[*]   lifted %d, %d outside targets added, %d stubs, %d errors, %d with no terminator'
          % (len(chosen), added, len(stubs), errors, dirty))
    return entry, {'lifted': len(chosen), 'stubs': len(stubs), 'errors': errors, 'no_terminator': dirty}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', choices=sorted(BUILDS), default='base',
                    help='base: Battlegrounds.exe v1.1; cc: Clone Campaigns, battlegrounds_x1.exe')
    ap.add_argument('--out', default=None, help="the generated tree (default: the build's own)")
    ap.add_argument('--roots', default='', help='comma-separated extra root VAs')
    ap.add_argument('--max', type=int, default=3000, help='closure size cap')
    ap.add_argument('--all', action='store_true', help='lift every function')
    ap.add_argument('--split', type=int, default=400, help='functions per .c file')
    ap.add_argument('--seeds', default=None,
                    help="seed_from_log.py JSON of unresolved targets from runs (default: the build's own)")
    args = ap.parse_args()
    build = BUILDS[args.build]
    args.out = args.out or build['out']
    args.seeds = args.seeds or build['seeds']
    for path, catalog, stamp, what, digest in build['modules']:
        if not os.path.exists(path):
            sys.exit('no %s -- README, Step by step' % path)
        if not os.path.exists(catalog):
            sys.exit('no catalog at %s -- run disasm32.py first (README, Step by step)' % catalog)
        if exe_stamp(path) != stamp:
            sys.exit('%s is not the build this project supports: its PE timestamp is 0x%08X, '
                     'expected 0x%08X: %s (README).' % (path, exe_stamp(path), stamp, what))
        if '.bind' in exe_sections(path):
            sys.exit('%s is SteamStub-wrapped (it has a .bind section): unwrap it with '
                     'tools/steamstub_swgb.py first (docs/lifting.md)' % path)
        if digest and code_digest(path) != digest:
            sys.exit('%s has the right timestamp but other code (digest %s, expected %s): a patched '
                     'copy? Lift %s (docs/lifting.md).' % (path, code_digest(path), digest, what))

    os.makedirs(args.out, exist_ok=True)
    for fn in os.listdir(args.out):                 # a smaller lift must not leave stale chunks
        if fn.startswith('recomp_') and fn.endswith('.c'):
            os.remove(os.path.join(args.out, fn))
    t0 = time.time()
    out = Out(args.out, args.split)
    roots = [int(x, 0) for x in args.roots.split(',') if x.strip()]
    entries_va, stats = [], {'lifted': 0, 'stubs': 0, 'errors': 0, 'no_terminator': 0}
    for path, catalog, _, _, _ in build['modules']:
        entry, s = lift_module(path, catalog, args, out, roots, build)
        entries_va.append(entry)
        for k in stats:
            stats[k] += s[k]
    out.flush()

    patch_decls = ''.join('void %s(void);\n' % f for f in build['hle'].values())
    with open(os.path.join(args.out, 'recomp_funcs.h'), 'w', newline='\n') as f:
        f.write('/* %s - AUTO-GENERATED by run_lift.py */\n#pragma once\n'
                '#include <stdint.h>\n\n'
                'void recomp_not_lifted(uint32_t va);\n'
                '#define RECOMP_NOT_LIFTED(va) recomp_not_lifted(va)\n\n' % build['name'] + patch_decls + '\n')
        for a, n in out.entries:
            f.write('void %s(void);\n' % n)
    with open(os.path.join(args.out, 'recomp_dispatch.c'), 'w', newline='\n') as f:
        f.write('/* %s - AUTO-GENERATED by run_lift.py */\n'
                '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'
                'const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n' % build['name'])
        for a, n in sorted(out.entries):
            f.write('    { 0x%08Xu, %s },\n' % (a, n))
        f.write('};\nconst uint32_t recomp_dispatch_count = %d;\n'
                'const uint32_t swgb_entry_va = 0x%08Xu;\n'
                % (len(out.entries), entries_va[0]))

    # A patch that found no instruction to follow is a fix that silently isn't there.
    gen = ''.join(open(os.path.join(args.out, fn), encoding='utf-8', errors='replace').read()
                  for fn in os.listdir(args.out) if fn.endswith('.c'))
    for va, c in PATCHES.items():
        if c not in gen:
            sys.exit('patch at 0x%08X (%s) was not applied: is 0x%08X lifted?' % (va, c, va))
    print('[*] patches applied: %d' % len(PATCHES))

    lines = sum(sum(1 for _ in open(os.path.join(args.out, fn), encoding='utf-8', errors='replace'))
                for fn in os.listdir(args.out))
    stats.update({'files': out.idx, 'lines': lines})
    json.dump(stats, open(build['stats'], 'w'), indent=1)
    print('=' * 60)
    print('  lifted %(lifted)d   not-lifted stubs %(stubs)d   errors %(errors)d   '
          'no terminator %(no_terminator)d' % stats)
    print('  %s lines of C in %d files, %.1fs' % (format(lines, ','), out.idx, time.time() - t0))
    print('=' * 60)


if __name__ == '__main__':
    main()
