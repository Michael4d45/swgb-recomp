"""Read a --profile file and print where the game thread's time went.

    py -3 tools/profile_report.py FILE [--top N] [--mode M] [--all-samples]
                                       [--callers NAME] [--folded OUT]

The host (src/runtime/profile.c) writes raw host addresses; this names them
with DbgHelp: the host executable's PDB (lifted functions are sub_<VA>, so
the guest address is in the name) and the system DLLs' exports.

Tables:
  self       the function EIP was in (where the CPU actually is)
  inclusive  every function on the sampled stack, counted once a sample
             (the stack is scanned: a few stale frames come along)
  modules    self time by module; the host executable is split into lifted
             code (sub_*) and the host's own runtime
  native     the Windows call in progress when EIP was outside the executable
  lifted     self and inclusive, lifted functions only

By default only samples where the game thread ran count (the thread's cycle
count moved since the last sample); --all-samples counts waits too.
--mode M keeps samples taken while the game's mode was M (2 menus, 4 game).
--callers NAME prints the functions just above NAME on the sampled stacks.
--folded OUT writes collapsed stacks (root;...;leaf count) for a flame graph.
"""
import argparse
import collections
import ctypes
import ctypes.wintypes as wt
import os
import sys


class SymbolInfo(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
        ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
        ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
        ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
        ("Name", ctypes.c_char * 512),
    ]


class Symbols:
    """Address -> (module, function) through DbgHelp, for modules listed in the file."""

    def __init__(self, modules):
        self.modules = sorted(modules)  # (base, size, path)
        self.dbg = ctypes.WinDLL("dbghelp.dll")
        self.dbg.SymInitialize.argtypes = [wt.HANDLE, ctypes.c_char_p, wt.BOOL]
        self.dbg.SymLoadModuleEx.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p,
                                             ctypes.c_uint64, wt.DWORD, ctypes.c_void_p, wt.DWORD]
        self.dbg.SymLoadModuleEx.restype = ctypes.c_uint64
        self.dbg.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
                                         ctypes.POINTER(SymbolInfo)]
        self.h = wt.HANDLE(0x5157)  # any unique value: no live process behind it
        self.dbg.SymSetOptions(0x2 | 0x4)  # SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS
        if not self.dbg.SymInitialize(self.h, None, False):
            raise OSError("SymInitialize failed")
        for base, size, path in self.modules:
            self.dbg.SymLoadModuleEx(self.h, None, path.encode("mbcs"), None, base, size, None, 0)
        self.cache = {}
        self.info = SymbolInfo()

    def module(self, a):
        for base, size, path in self.modules:
            if base <= a < base + size:
                return os.path.basename(path)
        return "?"

    def name(self, a):
        r = self.cache.get(a)
        if r:
            return r
        mod = self.module(a)
        self.info.SizeOfStruct = 88  # sizeof(SYMBOL_INFO): the header, Name[1], padding
        self.info.MaxNameLen = 511
        disp = ctypes.c_uint64()
        if self.dbg.SymFromAddr(self.h, a, ctypes.byref(disp), ctypes.byref(self.info)):
            fn = self.info.Name.decode("mbcs", "replace")
        else:
            fn = "%s+%X" % (mod, a)
        r = (mod, fn)
        self.cache[a] = r
        return r


def load(path):
    head, modules, samples = {}, [], []
    with open(path) as f:
        for line in f:
            p = line.split()
            if not p or line.startswith("#"):
                continue
            if p[0] == "module":
                modules.append((int(p[1], 16), int(p[2], 16), line.split(None, 3)[3].strip()))
            elif p[0] == "s":
                # s count mode R|W import eip frames...
                samples.append((int(p[1]), int(p[2]), p[3] == "R", p[4], [int(x, 16) for x in p[5:]]))
            else:
                head[p[0]] = float(p[1])
    return head, modules, samples


def kind(mod, fn, exe):
    if mod.lower() == exe:
        return "lifted" if fn.startswith("sub_") else "host"
    return mod


def table(title, counter, total, top, width=60):
    print("\n%s" % title)
    for name, n in counter.most_common(top):
        print("  %6.2f%%  %7d  %s" % (100.0 * n / total if total else 0, n, name[:width] if isinstance(name, str) else name))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--mode", type=int)
    ap.add_argument("--all-samples", action="store_true")
    ap.add_argument("--callers")
    ap.add_argument("--folded")
    a = ap.parse_args()

    head, modules, samples = load(a.file)
    exe = next((os.path.basename(p).lower() for b, s, p in modules if p.lower().endswith(".exe")), "swgbrecomp.exe")
    sym = Symbols(modules)

    wall = head.get("wall_s", 0)
    print("%s: %.1f s, %d samples (%d while the game thread ran), %d lost" % (
        a.file, wall, head.get("samples", 0), head.get("running", 0), head.get("lost", 0)))
    if wall:
        print("cpu: game thread %.0f%% of a core, process %.0f%%" % (
            100 * head.get("cpu_thread_s", 0) / wall, 100 * head.get("cpu_process_s", 0) / wall))
    modes = collections.Counter()
    for n, mode, run, imp, fr in samples:
        modes["mode %d %s" % (mode, "ran" if run else "waited")] += n
    print("samples by game mode: " + ", ".join("%s %d" % kv for kv in sorted(modes.items())))

    self_fn, incl_fn, mods, native = (collections.Counter() for _ in range(4))
    lself, lincl, callers, folded = (collections.Counter() for _ in range(4))
    total = 0
    for n, mode, run, imp, fr in samples:
        if a.mode is not None and mode != a.mode:
            continue
        if not run and not a.all_samples:
            continue
        total += n
        names = [sym.name(x) for x in fr]
        mod, fn = names[0]
        k = kind(mod, fn, exe)
        self_fn["%s  [%s]" % (fn, k)] += n
        mods[k] += n
        if k == "lifted":
            lself[fn] += n
        if imp != "-":
            native[imp] += n
        seen = set()
        chain = []
        for m, f in names:
            if f in seen:
                continue
            seen.add(f)
            chain.append(f)
            incl_fn["%s  [%s]" % (f, kind(m, f, exe))] += n
            if f.startswith("sub_") and m.lower() == exe:
                lincl[f] += n
        if a.callers:
            for i, f in enumerate(chain[:-1]):
                if f == a.callers:
                    callers[chain[i + 1]] += n
        if a.folded:
            folded[";".join(reversed(chain))] += n

    print("samples counted: %d%s" % (total, "" if a.mode is None else " (mode %d)" % a.mode))
    table("self, by function", self_fn, total, a.top)
    table("inclusive, by function", incl_fn, total, a.top)
    table("self, by module (the executable split into lifted and host code)", mods, total, a.top)
    table("native call in progress (EIP outside the executable)", native, total, a.top)
    table("lifted functions, self", lself, total, a.top)
    table("lifted functions, inclusive", lincl, total, a.top)
    if a.callers:
        table("callers of %s" % a.callers, callers, total, a.top)
    if a.folded:
        with open(a.folded, "w") as o:
            for k, n in folded.most_common():
                o.write("%s %d\n" % (k, n))
        print("\nfolded stacks -> %s" % a.folded)


if __name__ == "__main__":
    sys.exit(main())
