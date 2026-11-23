"""Unwrap SteamStub C0DEC0DE/0xD0 (SWGB Steam Saga) from Battlegrounds.exe or battlegrounds_x1.exe.

    py -3 tools/steamstub_swgb.py <Steam>/Game/battlegrounds_x1.exe work/battlegrounds_x1.exe

The wrapper's header (XOR-chained before the entry point) holds the original
entry point and the AES key for the code section: the section is decrypted,
and pcrecomp's tools/drm/steamstub.py `finish` points the entry back at it
and drops the .bind section.
Needs `pefile` and `cryptography`."""
import struct, sys, os
sys.path.insert(0, os.path.join(os.environ.get('PCRECOMP', os.path.join(os.path.dirname(__file__), '..', '..', 'pcrecomp')), 'tools', 'drm'))
import pefile
from steamstub import xor_chain, V3_COPY, V3_PROLOGUE, finish
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

def unwrap_swgb(raw: bytes) -> bytes:
    pe = pefile.PE(data=raw, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    ep = pe.OPTIONAL_HEADER.AddressOfEntryPoint
    entry = raw[pe.get_offset_from_rva(ep):pe.get_offset_from_rva(ep)+0x100]
    if not entry.startswith(V3_PROLOGUE):
        raise SystemExit('not a SteamStub 3.x prologue')
    m = V3_COPY.search(entry)
    size = struct.unpack('<I', m.group(1))[0]
    off = pe.get_offset_from_rva(ep - size)
    seed = struct.unpack_from('<I', raw, off)[0]
    hdr = struct.pack('<I', seed) + xor_chain(raw[off+4:off+size], seed)
    if struct.unpack_from('<I', hdr, 4)[0] != 0xC0DEC0DE:
        raise SystemExit('expected signature 0xC0DEC0DE')
    oep_rva = struct.unpack_from('<I', hdr, 0x1C)[0]
    code_rva = struct.unpack_from('<I', hdr, 0x40)[0]
    code = next(s for s in pe.sections if s.VirtualAddress == code_rva)
    code_size = code.SizeOfRawData
    key, enc_iv, fb = hdr[0x48:0x68], hdr[0x68:0x78], hdr[0x78:0x88]
    enc = raw[code.PointerToRawData:code.PointerToRawData+code_size]
    iv = Cipher(algorithms.AES(key), modes.ECB()).decryptor().update(enc_iv)
    plain = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor().update(fb + enc)
    pad = plain[-1]
    if not (1 <= pad <= 16 and plain[-pad:] == bytes([pad])*pad and len(plain)-pad == code_size):
        raise SystemExit('AES decrypt failed (bad padding)')
    out = bytearray(raw)
    out[code.PointerToRawData:code.PointerToRawData+code_size] = plain[:-pad]
    finish(out, pe, base + oep_rva)
    return bytes(out)

if __name__ == '__main__':
    inp, outp = sys.argv[1], sys.argv[2]
    open(outp, 'wb').write(unwrap_swgb(open(inp, 'rb').read()))
    print('wrote', outp)
