#!/usr/bin/env python3
"""Function entries the catalog missed, found from the pointers that name them.

disasm32 finds most entries by descent and by scanning data for code
pointers, but a pointer whose target lies inside a body it has already
decoded is dropped as "a label in that body". When that body is a false one
(bytes of a switch's index table decoded as `or al, 0x0C`, running on past
the table into the next function), the real function after it is lost, and
the game meets it as `ICALL: unresolved VA` the first time it calls the
vtable slot (0x005648A0, docs/lifting.md).

This scans every non-code section, and the immediates of
`mov r/m32, imm32` and `push imm32` in code, for addresses in the code
range that are not catalog entries, and classifies each:

    vtable     in a run of 2+ code pointers in data (an RTTI COL before the
               run is noted), and decodes as a function
    pointer    a lone pointer in data, or an immediate stored or pushed by
               code, that decodes as a function and starts on a boundary a
               function would (after ret / jmp / padding / a table)
    seh        a filter or handler of an _except_handler3 scope table
               (eh32 resumes there as a label: never an entry)
    cxx_eh     a C++ catch handler or unwind action from a FuncInfo
    switch     an arm of a `jmp [reg*4 + table]` switch
    label      a branch target inside the body that holds it
    noise      none of the above: a float, part of a string, a value
               that only looks like an address

Only `vtable` and `pointer` become seeds. Writes work/code_ptr_seeds.json
(run_lift.py loads it) and prints the table.

    py -3 tools/code_ptr_seeds.py [--exe work/Battlegrounds.exe]
        [--catalog work/functions.json] [--out work/code_ptr_seeds.json] [-v]
"""
import argparse
import json
import os
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_GRP_INVALID

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Instructions a compiler never emits in a game's user-mode code: a decode
# that meets one is reading data.
NEVER = {'in', 'out', 'ins', 'insb', 'insd', 'insw', 'outs', 'outsb', 'outsd', 'outsw', 'hlt',
         'cli', 'sti', 'arpl', 'bound', 'into', 'aaa', 'aas', 'aad', 'aam', 'daa', 'das',
         'les', 'lds', 'salc', 'lock', 'iretd', 'retf', 'sahf', 'int1', 'icebp', 'wait', 'lahf',
         'xlatb', 'cmc', 'sldt', 'lgdt', 'lidt', 'ljmp', 'lcall', 'enter', 'pushal', 'popal',
         'pushfd', 'popfd', 'stc', 'clc'}


class Image:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        nt = struct.unpack_from('<I', raw, 0x3C)[0]
        nsec = struct.unpack_from('<H', raw, nt + 6)[0]
        optsz = struct.unpack_from('<H', raw, nt + 20)[0]
        self.base = struct.unpack_from('<I', raw, nt + 24 + 28)[0]
        self.sections = []
        for i in range(nsec):
            o = nt + 24 + optsz + 40 * i
            name = raw[o:o + 8].rstrip(b'\0').decode('latin-1')
            vsize, rva, rsize, roff = struct.unpack_from('<IIII', raw, o + 8)
            chars = struct.unpack_from('<I', raw, o + 36)[0]
            self.sections.append((name, self.base + rva, max(vsize, rsize), raw[roff:roff + rsize],
                                  bool(chars & 0x20000000)))

    def read(self, va, n):
        for _, lo, size, data, _ in self.sections:
            if lo <= va and va + n <= lo + len(data):
                return data[va - lo:va - lo + n]
            if lo <= va < lo + size:
                return bytes(n)          # virtual tail: zeros
        return None

    def dword(self, va):
        b = self.read(va, 4)
        return struct.unpack('<I', b)[0] if b else None

    def section_of(self, va):
        for name, lo, size, _, is_code in self.sections:
            if lo <= va < lo + size:
                return name, is_code
        return None, False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.path.join(ROOT, 'work', 'Battlegrounds.exe'))
    ap.add_argument('--catalog', default=os.path.join(ROOT, 'work', 'functions.json'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'work', 'code_ptr_seeds.json'))
    ap.add_argument('-v', action='store_true', help='print every candidate')
    args = ap.parse_args()

    img = Image(args.exe)
    code = [(lo, lo + size, data) for name, lo, size, data, c in img.sections if c]
    cs, ce = min(c[0] for c in code), max(c[1] for c in code)
    cat = json.load(open(args.catalog))['functions']
    entries = {f['address'] for f in cat}
    bodies = sorted((f['address'], f['end']) for f in cat if f.get('entry_kind') == 'start')

    def in_code(va):
        return any(lo <= va < hi for lo, hi, _ in code)

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    # ---- what code says about the image: switch tables, SEH, C++ EH ------
    switch_tables, index_tables, scope_tables, funcinfos = set(), set(), set(), set()
    stores = {}                                 # immediate code pointer -> where code names it
    for lo, hi, data in code:
        for i in range(len(data) - 7):
            b0, b1 = data[i], data[i + 1]
            # jmp dword ptr [reg*4 + table]: FF 24 85/8D/95/9D/AD/B5/BD imm32
            if b0 == 0xFF and b1 == 0x24 and data[i + 2] & 0xC7 == 0x85:
                switch_tables.add(struct.unpack_from('<I', data, i + 3)[0])
            # a switch's byte index table: mov r8, [reg + table] (8A 80+) or
            # movzx r32, byte [reg + table] (0F B6 80+), the table in code
            elif (b0 == 0x8A and b1 & 0xC0 == 0x80 and b1 & 7 != 4) or \
                    (b0 == 0x0F and b1 == 0xB6 and data[i + 2] & 0xC0 == 0x80 and data[i + 2] & 7 != 4):
                o = i + 2 if b0 == 0x8A else i + 3
                t = struct.unpack_from('<I', data, o)[0]
                if cs <= t < ce:
                    index_tables.add(t)
            # push -1; push scopetable; push _except_handler3
            elif b0 == 0x6A and b1 == 0xFF and data[i + 2] == 0x68 and data[i + 7:i + 8] == b'\x68':
                scope_tables.add(struct.unpack_from('<I', data, i + 3)[0])
            # mov eax, FuncInfo; jmp __CxxFrameHandler
            elif b0 == 0xB8 and data[i + 5:i + 6] == b'\xE9':
                fi = struct.unpack_from('<I', data, i + 1)[0]
                if img.dword(fi) == 0x19930520:
                    funcinfos.add(fi)
            # push imm32 (68) / mov r/m32, imm32 (C7 /0, modrm with a memory operand)
            if b0 == 0x68:
                stores.setdefault(struct.unpack_from('<I', data, i + 1)[0], []).append(lo + i)
            elif b0 == 0xC7 and (b1 & 0x38) == 0 and (b1 >> 6) != 3:
                ins = next(md.disasm(data[i:i + 15], lo + i, 1), None)
                if ins and ins.mnemonic == 'mov' and ins.size >= 6:
                    imm = struct.unpack_from('<I', data, i + ins.size - 4)[0]
                    stores.setdefault(imm, []).append(lo + i)

    # Data inside the code sections: each switch's jump table, and its byte
    # index table (up to the next thing code names). A function can start
    # right after one, with no ret or padding before it (0x005648A0).
    switch_arms, data_in_code = set(), []
    for t in switch_tables:
        k = t
        while True:
            v = img.dword(k)
            if v is None or not in_code(v):
                break
            switch_arms.add(v)
            k += 4
        if k > t and in_code(t):
            data_in_code.append((t, k))
    for t in index_tables:
        k = t
        while k < t + 4096 and k not in switch_tables and k not in entries and k not in switch_arms \
                and (img.read(k, 1) or b'\0')[0] < 0x40:
            k += 1
        if k > t:
            data_in_code.append((t, k))

    seh = set()
    for st in scope_tables:
        for n in range(64):
            prev, filt, hand = (img.dword(st + 12 * n + 4 * j) for j in range(3))
            if prev is None or not (prev == 0xFFFFFFFF or prev < n) or not in_code(hand) \
                    or not (filt == 0 or in_code(filt)):
                break
            seh |= {hand} | ({filt} if filt else set())

    cxx = set()
    for fi in funcinfos:
        nunwind, unwind, ntry, trymap = (img.dword(fi + 4 * j) for j in range(1, 5))
        for s in range(min(nunwind or 0, 4096)):
            a = img.dword(unwind + 8 * s + 4)
            if a and in_code(a):
                cxx.add(a)
        for t in range(min(ntry or 0, 1024)):
            ncatch, harr = img.dword(trymap + 20 * t + 12), img.dword(trymap + 20 * t + 16)
            for c in range(min(ncatch or 0, 64)):
                a = img.dword(harr + 16 * c + 12)
                if a and in_code(a):
                    cxx.add(a)

    # ---- pointers in data --------------------------------------------------
    found = {}                                  # va -> list of (where, kind)
    for name, lo, size, data, is_code in img.sections:
        if is_code:
            continue
        for off in range(0, len(data) - 3, 4):
            v = struct.unpack_from('<I', data, off)[0]
            if in_code(v) and v not in entries:
                at = lo + off
                run = sum(1 for d in (-4, 4) if (lambda w: w is not None and in_code(w))(img.dword(at + d)))
                found.setdefault(v, []).append((at, 'run' if run else 'lone', name))
    for v, where in stores.items():
        if in_code(v) and v not in entries:
            found.setdefault(v, []).extend((w, 'imm', 'code') for w in where)

    def col_before(at):
        """The RTTI Complete Object Locator just before a vtable slot run, as a name."""
        k = at
        while (lambda w: w is not None and in_code(w))(img.dword(k - 4)):
            k -= 4
        col = img.dword(k - 4)
        if col is None or img.section_of(col)[0] != '.rdata' or img.dword(col) != 0:
            return None
        td = img.dword(col + 12)
        nm = img.read(td + 8, 64) if td else None
        if not nm or not nm.startswith(b'.?A'):
            return None
        return nm.split(b'\0')[0].decode('latin-1')

    def container(va):
        lo, hi = 0, len(bodies)
        while lo < hi:
            m = (lo + hi) // 2
            if bodies[m][0] <= va:
                lo = m + 1
            else:
                hi = m
        for a, e in reversed(bodies[max(0, lo - 3):lo]):
            if a <= va < e:
                return a
        return None

    def is_label(va, body):
        """va is an instruction of `body`'s own decode that the body branches to."""
        if body is None:
            return False
        end = next(e for a, e in bodies if a == body)
        starts, targets = set(), set()
        for ins in md.disasm(img.read(body, end - body) or b'', body):
            starts.add(ins.address)
            if (ins.mnemonic.startswith('j') or ins.mnemonic.startswith('loop')) and ins.op_str.startswith('0x'):
                targets.add(int(ins.op_str, 16))
        return va in starts and va in targets

    def first_mnemonic(va):
        ins = next(md.disasm(img.read(va, 16) or b'', va, 1), None)
        return ins.mnemonic if ins else None

    def decodes_as_function(va):
        """A decode from va reaches ret/jmp with nothing a compiler never emits."""
        b = img.read(va, 1024)
        if not b:
            return False, 'unreadable'
        n = 0
        for ins in md.disasm(b, va):
            n += 1
            if ins.mnemonic in NEVER or ins.id == 0 or ins.group(CS_GRP_INVALID):
                return False, '%s at 0x%08X' % (ins.mnemonic, ins.address)
            if ins.mnemonic.startswith('ret') or (ins.mnemonic == 'jmp' and n > 1) or ins.mnemonic == 'int3':
                return True, '%d insns' % n
            # 00 00 is `add [eax], al`: a run of them is zero data
            if ins.bytes[:2] == b'\x00\x00':
                return False, 'zero bytes at 0x%08X' % ins.address
            if n > 400:
                return True, 'long'
        return False, 'ran off'

    def after_boundary(va):
        """What precedes va: a function starts after ret, jmp, padding or data."""
        p1 = img.read(va - 1, 1)
        p3 = img.read(va - 3, 3)
        p5 = img.read(va - 5, 5)
        if p1 in (b'\xC3', b'\xCC', b'\x90') or (p3 and p3[0] == 0xC2) or (p5 and p5[0] == 0xE9):
            return True
        if any(lo < va <= hi for lo, hi in data_in_code):
            return True
        p2 = img.read(va - 2, 2)
        return bool(p2 and p2[0] == 0xEB) or bool(p2 and p2[0] == 0xFF and p2[1] in (0xE0, 0xE1, 0xE2, 0xE3, 0xE6, 0xE7))

    out, counts = [], {}
    for va in sorted(found):
        where = found[va]
        kinds = {k for _, k, _ in where}
        ok, why = decodes_as_function(va)
        col = next((col_before(a) for a, k, _ in where if k == 'run'), None)
        if va in seh:
            cls = 'seh'
        elif va in cxx:
            cls = 'cxx_eh'
        elif va in switch_arms:
            cls = 'switch'
        elif not ok or first_mnemonic(va) == 'nop' or (first_mnemonic(va) == 'int3' and 'run' not in kinds):
            cls = 'noise'               # padding, or bytes that are no code
        elif is_label(va, container(va)):
            cls = 'label'               # a branch target inside a real body
        elif 'run' in kinds and (after_boundary(va) or col):
            cls = 'vtable'
        elif ('lone' in kinds or 'imm' in kinds or 'run' in kinds) and after_boundary(va):
            cls = 'pointer'
        else:
            cls = 'noise'
        counts[cls] = counts.get(cls, 0) + 1
        rec = {'address': va, 'class': cls, 'inside': container(va), 'decode': why, 'rtti': col,
               'refs': ['0x%08X %s %s' % w for w in where[:4]]}
        out.append(rec)
        if args.v or cls in ('vtable', 'pointer'):
            print('0x%08X %-7s inside %-12s %-10s %s %s' % (
                va, cls, 'sub_%08X' % rec['inside'] if rec['inside'] else '-', why,
                col or '', ' '.join(rec['refs'][:2])))

    seeds = [r for r in out if r['class'] in ('vtable', 'pointer')]
    print('candidates %d: %s' % (len(out), ', '.join('%s %d' % kv for kv in sorted(counts.items()))))
    print('switch tables %d (+%d index tables), scope tables %d, FuncInfos %d'
          % (len(switch_tables), len(index_tables), len(scope_tables), len(funcinfos)))
    with open(args.out, 'w', newline='\n') as f:
        json.dump({'seeds': [{'address': r['address'], 'class': r['class'], 'rtti': r['rtti'],
                              'inside': r['inside'], 'refs': r['refs']} for r in seeds],
                   'rejected': [{'address': r['address'], 'class': r['class'], 'decode': r['decode'],
                                 'refs': r['refs']} for r in out if r not in seeds]}, f, indent=1)
    print('%d seeds -> %s' % (len(seeds), args.out))


if __name__ == '__main__':
    sys.exit(main())
