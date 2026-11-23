#!/usr/bin/env python3
"""Find base-game addresses in Clone Campaigns' exe (battlegrounds_x1.exe).

The expansion is the base game's engine with code added, so most functions
are the base game's byte for byte but for absolute addresses and branch
displacements. A function is found by its first bytes with those masked
(one match, or it is reported as not found); a data address (a global, a
vtable, a string) by the functions that use it: each is found in x1, and the
same operand is read there. Every use votes, and a split vote is reported.

A masked match also means the function's struct offsets (displacements that
are not addresses) are the same, so code that reads the engine's objects at
fixed offsets can trust them where the functions that use them matched.

    py -3 tools/x1_addrs.py 0x005ED2D0 0x006A0FD4 ...
    py -3 tools/x1_addrs.py --base work/Battlegrounds.exe --x1 work/battlegrounds_x1.exe \\
        --catalog work/functions.json 0x0065CE24

Needs capstone (as run_lift.py does).
"""
import argparse
import bisect
import collections
import json
import os
import re
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_GRP_CALL, X86_GRP_JUMP, X86_OP_IMM, X86_OP_MEM

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LO, HI = 0x00401000, 0x00800000   # the images' code and data, both exes


class Image:
    """A PE laid out at its addresses (raw bytes of every section)."""

    def __init__(self, path):
        raw = open(path, 'rb').read()
        nt = struct.unpack_from('<I', raw, 0x3C)[0]
        n = struct.unpack_from('<H', raw, nt + 6)[0]
        opt = nt + 24
        self.base = struct.unpack_from('<I', raw, opt + 28)[0]
        size = struct.unpack_from('<I', raw, opt + 56)[0]
        sh = opt + struct.unpack_from('<H', raw, nt + 20)[0]
        self.mem = bytearray(size)
        self.code = []
        for k in range(n):
            name, vsize, va, rsize, roff, _, _, _, _, ch = struct.unpack_from('<8sIIIIIIHHI', raw, sh + 40 * k)
            take = min(vsize, rsize)
            self.mem[va:va + take] = raw[roff:roff + take]
            if ch & 0x20000000:   # IMAGE_SCN_MEM_EXECUTE
                self.code.append((self.base + va, self.base + va + vsize))
        self.mem = bytes(self.mem)

    def at(self, va, n):
        return self.mem[va - self.base:va - self.base + n]


md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True


def pattern(img, va, n):
    """(regex, [(offset, size)] of the masked fields) of n bytes of code at va."""
    rx, fields, off = [], [], 0
    for i in md.disasm(img.at(va, n), va):
        b = bytes(i.bytes)
        mask = [False] * len(b)
        rel = i.group(X86_GRP_JUMP) or i.group(X86_GRP_CALL)
        for op in i.operands:
            if op.type == X86_OP_IMM and i.imm_size and (rel or LO <= (op.imm & 0xFFFFFFFF) < HI):
                for k in range(i.imm_offset, i.imm_offset + i.imm_size):
                    mask[k] = True
                if i.imm_size == 4:
                    fields.append((off + i.imm_offset, 'rel' if rel else 'abs'))
            if op.type == X86_OP_MEM and i.disp_size == 4 and LO <= (op.mem.disp & 0xFFFFFFFF) < HI:
                for k in range(i.disp_offset, i.disp_offset + 4):
                    mask[k] = True
                fields.append((off + i.disp_offset, 'abs'))
        rx.append(b''.join(b'.' if m else re.escape(bytes([c])) for c, m in zip(b, mask)))
        off += len(b)
    return b''.join(rx), fields, off


def find_code(base, x1, va, sizes=(256, 128, 64)):
    """The x1 address of the code at base va, or None: the longest prefix with
    one masked match (no match: a shorter one; two: give up, shorter is worse)."""
    for n in sizes:
        rx, _, length = pattern(base, va, n)
        if length < 24:
            continue
        hits = []
        for lo, hi in x1.code:
            seg = x1.mem[lo - x1.base:hi - x1.base]
            hits += [lo + m.start() for m in re.finditer(rx, seg, re.S)]
            if len(hits) > 1:
                break
        if len(hits) == 1:
            return hits[0]
        if hits:
            return None
    return None


def find_data(base, x1, va, starts):
    """Votes {x1 address: uses} for a data address, from the functions that use it."""
    votes = collections.Counter()
    needle = struct.pack('<I', va)
    for lo, hi in base.code:
        seg = base.mem[lo - base.base:hi - base.base]
        for m in re.finditer(re.escape(needle), seg):
            site = lo + m.start()
            f = starts[bisect.bisect_right(starts, site) - 1] if starts else site
            if site - f > 0x600:
                continue
            g = find_code(base, x1, f, sizes=(max(64, site - f + 16), max(128, site - f + 64)))
            if g is None:
                continue
            _, fields, _ = pattern(base, f, site - f + 16)
            if any(o == site - f and k == 'abs' for o, k in fields):
                votes[struct.unpack_from('<I', x1.at(g + site - f, 4))[0]] += 1
    return votes


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('va', nargs='+')
    ap.add_argument('--base', default=os.path.join(ROOT, 'work', 'Battlegrounds.exe'))
    ap.add_argument('--x1', default=os.path.join(ROOT, 'work', 'battlegrounds_x1.exe'))
    ap.add_argument('--catalog', default=os.path.join(ROOT, 'work', 'functions.json'),
                    help="the base game's catalog: where a use's function starts")
    a = ap.parse_args()
    base, x1 = Image(a.base), Image(a.x1)
    starts = sorted(f['address'] for f in json.load(open(a.catalog))['functions']
                    if f.get('entry_kind') != 'alias') if os.path.exists(a.catalog) else []
    bad = 0
    for s in a.va:
        va = int(s, 0)
        if any(lo <= va < hi for lo, hi in base.code):
            g = find_code(base, x1, va)
            print('0x%08X  code  %s' % (va, '0x%08X' % g if g else 'NOT FOUND'))
            bad += g is None
        else:
            v = find_data(base, x1, va, starts)
            if not v:
                print('0x%08X  data  NOT FOUND (no use matched)' % va)
                bad += 1
                continue
            (best, n), *rest = v.most_common()
            print('0x%08X  data  0x%08X  (%d uses%s)' % (va, best, n,
                  '; also ' + ', '.join('0x%08X x%d' % r for r in rest) if rest else ''))
            bad += bool(rest)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
