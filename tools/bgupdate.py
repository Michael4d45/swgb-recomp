#!/usr/bin/env python3
"""Unpack LucasArts' v1.1 update (BGUpdate1_1.exe) without running it.

The update is a small self-extractor with a Microsoft cabinet behind its PE
image, lightly scrambled: "1CNT", a 32-bit key, then every byte b of the
cabinet stored as (b + hi) ^ lo, where (lo, hi) are the low and high bytes of
entry (offset & 0xFF) of a 256-entry table that the MSVC rand() generator
fills from the key (the stub's 0x00401255 and 0x0040119A). Undone, it is a
plain MSCF cabinet that Windows' own expand.exe unpacks: Eng\\Game\\
Battlegrounds.exe and Common\\Game\\Data\\GENIE.DAT are the two files that
change, plus the readmes.

    py -3 tools/bgupdate.py BGUpdate1_1.exe work/patch
    python3 tools/bgupdate.py BGUpdate1_1.exe work/patch    (Linux: needs cabextract)
"""
import os
import struct
import subprocess
import sys


def decode(exe):
    """The cabinet inside the update, unscrambled."""
    d = open(exe, 'rb').read()
    # The payload is the overlay: after the last section's raw data. (The
    # stub's own code holds the same four bytes, as the constant it checks.)
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    nsec, opt = struct.unpack_from('<HxxxxxxxxxxxxH', d, pe + 6)
    sec = pe + 24 + opt
    end = max(sum(struct.unpack_from('<II', d, sec + 40 * k + 16)) for k in range(nsec))
    o = d.find(b'1CNT', end)
    if o < 0:
        raise ValueError('%s: no "1CNT" payload; is it BGUpdate1_1.exe?' % exe)
    key = struct.unpack_from('<I', d, o + 4)[0]
    table, x = [], key
    for _ in range(256):
        x = (x * 0x343FD + 0x269EC3) & 0xFFFFFFFF
        table.append((x >> 16) & 0x7FFF)
    body = bytearray(d[o + 8:])
    for i, b in enumerate(body):
        w = table[i & 0xFF]
        body[i] = ((b ^ (w & 0xFF)) - (w >> 8)) & 0xFF
    if body[:4] != b'MSCF':
        raise ValueError('%s: payload did not decode to a cabinet' % exe)
    return bytes(body)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    exe, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    cab = os.path.join(out, 'BGUpdate1_1.cab')
    open(cab, 'wb').write(decode(exe))
    if os.name == 'nt':
        expand = os.path.join(os.environ['SystemRoot'], 'System32', 'expand.exe')
        cmd = [expand, '-F:*', cab, out]
    else:   # cabextract (its own package): the same folders, with / in them
        cmd = ['cabextract', '-q', '-d', out, cab]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('%s failed: %s' % (os.path.basename(cmd[0]), r.stdout + r.stderr))
    for rel in (('Eng', 'Game', 'Battlegrounds.exe'), ('Common', 'Game', 'Data', 'GENIE.DAT')):
        if not os.path.exists(os.path.join(out, *rel)):
            sys.exit('%s is not in the update' % '\\'.join(rel))
    print('unpacked the v1.1 update into %s' % out)


if __name__ == '__main__':
    main()
