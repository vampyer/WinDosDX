#!/usr/bin/env python3
"""Which DLLs and functions do Windows programs need that WinDosDX lacks?

Reads the import tables (normal and delay-load) of every .exe and .dll
under the given folders, resolves apiset names the way the WinDosDX
loader does (sdk/lib/apisets/apisets.table.c), and checks each imported
function against the export tables of the DLLs a WinDosDX build puts in
system32. DLLs that ship next to the program count as the program's own:
their imports are checked too, their exports satisfy the program.

    python tools/compat/check-imports.py --build output-VS-amd64 "C:/Program Files/7-Zip" ...

64-bit programs are checked against an amd64 build, 32-bit programs
against an i386 build (--build32). The summary ranks the missing
functions by how many programs need them: the order to implement them.
"""

import argparse
import collections
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))


# ---------------------------------------------------------------- PE files

class PE:
    """Just enough of a PE reader: machine, imports, delay imports, exports."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError("not an MZ file")
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        self.machine, nsect = struct.unpack_from("<HH", d, pe + 4)
        opt_size = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", d, opt)[0]
        self.is64 = magic == 0x20B
        dirs = opt + (112 if self.is64 else 96)
        ndirs = struct.unpack_from("<I", d, dirs - 4)[0]
        self.dirs = [struct.unpack_from("<II", d, dirs + 8 * i) for i in range(min(ndirs, 16))]
        self.sections = []
        for i in range(nsect):
            s = opt + opt_size + 40 * i
            vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", d, s + 8)
            self.sections.append((va, max(vsize, rawsize), rawptr, rawsize))
        # .NET assemblies have a CLR header (directory 14).
        self.managed = len(self.dirs) > 14 and self.dirs[14][0] != 0

    def off(self, rva):
        for va, size, raw, rawsize in self.sections:
            if va <= rva < va + size:
                o = rva - va
                return raw + o if o < rawsize else None
        return rva if rva < len(self.data) else None

    def cstr(self, rva):
        o = self.off(rva) if rva else None
        if o is None:
            return ""
        end = self.data.find(b"\0", o)
        return self.data[o:end].decode("latin-1")

    def _thunks(self, rva):
        size, fmt = (8, "<Q") if self.is64 else (4, "<I")
        flag = 1 << (63 if self.is64 else 31)
        o = self.off(rva) if rva else None
        while o is not None and o + size <= len(self.data):
            v = struct.unpack_from(fmt, self.data, o)[0]
            if not v:
                break
            if v & flag:
                yield "#%d" % (v & 0xFFFF)
            else:
                yield self.cstr((v & 0x7FFFFFFF) + 2)
            o += size

    def imports(self, delayed=False):
        """{dll (lower case): set of names ('#n' for ordinals)}; the static
        imports, or with delayed=True the delay-load ones."""
        out = collections.defaultdict(set)
        if delayed:
            return self._delay_imports(out)
        rva, size = self.dirs[1] if len(self.dirs) > 1 else (0, 0)
        o = self.off(rva) if rva else None
        while o is not None and o + 20 <= len(self.data):
            ilt, _, _, name, iat = struct.unpack_from("<IIIII", self.data, o)
            if not name:
                break
            out[self.cstr(name).lower()].update(self._thunks(ilt or iat))
            o += 20
        return out

    def _delay_imports(self, out):
        rva, size = self.dirs[13] if len(self.dirs) > 13 else (0, 0)
        o = self.off(rva) if rva else None
        while o is not None and o + 32 <= len(self.data):
            # Attributes, DllName, ModuleHandle, IAT, INT, BoundIAT, UnloadIAT, TimeStamp
            attrs, name, _, iat, int_rva = struct.unpack_from("<IIIII", self.data, o)
            if not name:
                break
            if not int_rva:
                o += 32
                continue
            # Old-style delay descriptors hold VAs instead of RVAs.
            fix = 0 if attrs & 1 else -self.image_base()
            out[self.cstr(name + fix).lower()].update(self._thunks(int_rva + fix))
            o += 32
        return out

    def image_base(self):
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        opt = pe + 24
        return struct.unpack_from("<Q" if self.is64 else "<I", self.data, opt + (24 if self.is64 else 28))[0]

    def exports(self):
        """set of exported names plus '#n' for every ordinal"""
        out = set()
        rva, size = self.dirs[0] if self.dirs else (0, 0)
        o = self.off(rva) if rva else None
        if o is None:
            return out
        base, nfuncs, nnames, funcs, names, ords = struct.unpack_from("<IIIIII", self.data, o + 16)
        for i in range(nfuncs):
            out.add("#%d" % (base + i))
        no, oo = self.off(names), self.off(ords)
        for i in range(nnames if no is not None and oo is not None else 0):
            out.add(self.cstr(struct.unpack_from("<I", self.data, no + 4 * i)[0]))
        return out


# ---------------------------------------------------------------- WinDosDX side

def load_apisets():
    text = open(os.path.join(ROOT, "sdk", "lib", "apisets", "apisets.table.c"), encoding="utf-8").read()
    table = {}
    for line in text.splitlines():
        if line.lstrip().startswith("//"):
            continue
        m = re.search(r'L"([^"]+)"\), RTL_CONSTANT_STRING\(L"([^"]*)"\)', line)
        if m and m.group(2):
            table[m.group(1).lower()] = m.group(2).lower()
    return table


def load_system(build):
    """{dll name: path} for everything the build puts in system32."""
    lst = os.path.join(build, "boot", "bootcd.Debug.lst")
    dlls = {}
    for line in open(lst, encoding="utf-8", errors="replace"):
        line = line.strip()
        if "=" not in line:
            continue
        dest, src = line.split("=", 1)
        dest = dest.lower()
        if dest.startswith("reactos/system32/") and dest.count("/") == 2:
            dlls[dest.rsplit("/", 1)[1]] = src
    return dlls


class System:
    def __init__(self, build, apisets):
        self.dlls = load_system(build)
        self.apisets = apisets
        self._exports = {}
        self._pe = {}
        self._closure = {}

    def resolve(self, dll):
        name = dll[:-4] if dll.endswith(".dll") else dll
        if name.startswith(("api-", "ext-")):
            return self.apisets.get(name)
        return dll

    def pe(self, dll):
        if dll not in self._pe:
            path = self.dlls.get(dll)
            try:
                self._pe[dll] = PE(path) if path else None
            except (OSError, ValueError, struct.error):
                self._pe[dll] = None
        return self._pe[dll]

    def exports(self, dll):
        if dll not in self._exports:
            pe = self.pe(dll)
            self._exports[dll] = pe.exports() if pe else None
        return self._exports[dll]

    def closure(self, dll):
        """What loading this system DLL needs and the system lacks, through
        its own static imports: ({dll}, {(dll, func)}), each tagged with the
        DLL that asked for it."""
        if dll in self._closure:
            return self._closure[dll]
        self._closure[dll] = (set(), set())       # cycles end here
        mdll, mfn = set(), set()
        pe = self.pe(dll)
        if pe:
            for imp, funcs in pe.imports().items():
                host = self.resolve(imp)
                if host is None:
                    if not imp.startswith("ext-"):
                        mdll.add("%s (needed by %s)" % (imp, dll))
                    continue
                exports = self.exports(host)
                if exports is None:
                    mdll.add("%s (needed by %s)" % (host, dll))
                    continue
                for fn in funcs:
                    if fn not in exports:
                        mfn.add((host, "%s (needed by %s)" % (fn, dll)))
                sub_dll, sub_fn = self.closure(host)
                mdll |= sub_dll
                mfn |= sub_fn
        self._closure[dll] = (mdll, mfn)
        return mdll, mfn


# ---------------------------------------------------------------- checking

def scan(folder):
    for base, _, files in os.walk(folder):
        for f in files:
            if f.lower().endswith((".exe", ".dll")):
                yield os.path.join(base, f)


def check_import(system, local, pe, dll, funcs, mdll, mfn):
    if dll in local and local[dll].is64 == pe.is64:
        return                                # the program's own DLL
    host = system.resolve(dll)
    if host is None:
        if not dll.startswith("ext-"):        # extension apisets are optional
            mdll.add(dll)
        return
    exports = system.exports(host)
    if exports is None:
        if host not in local:
            mdll.add(dll if host == dll else "%s (-> %s)" % (dll, host))
        return
    for fn in funcs:
        if fn not in exports:
            mfn.add((host, fn))
    # What the system DLL itself needs to load.
    sub_dll, sub_fn = system.closure(host)
    mdll |= sub_dll
    mfn |= sub_fn


def check_program(folder, systems):
    """What one program folder needs and the system lacks: static DLLs and
    functions (the program cannot start), delay-loaded ones (a feature fails
    when used), and how many native files were checked."""
    local = {}
    pes = {}
    for path in scan(folder):
        try:
            pe = PE(path)
        except (OSError, ValueError, struct.error):
            continue
        if pe.machine not in (0x14C, 0x8664) or pe.managed:
            continue
        pes[path] = pe
        local.setdefault(os.path.basename(path).lower(), pe)

    missing_dll, missing_fn = set(), set()
    late_dll, late_fn = set(), set()
    checked = 0
    for path, pe in pes.items():
        system = systems.get(pe.is64)
        if not system:
            continue
        checked += 1
        for delayed in (False, True):
            mdll, mfn = (late_dll, late_fn) if delayed else (missing_dll, missing_fn)
            for dll, funcs in pe.imports(delayed).items():
                check_import(system, local, pe, dll, funcs, mdll, mfn)
    return missing_dll, missing_fn, late_dll - missing_dll, late_fn - missing_fn, checked


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folders", nargs="+", help="program folders (one program each)")
    ap.add_argument("--build", default=os.path.join(ROOT, "output-VS-amd64"), help="amd64 build folder")
    ap.add_argument("--build32", default=os.path.join(ROOT, "output-VS-i386"), help="i386 build folder")
    ap.add_argument("--top", type=int, default=60, help="how many missing functions to list")
    ap.add_argument("--details", action="store_true", help="list every missing function per program")
    args = ap.parse_args()

    apisets = load_apisets()
    systems = {}
    for is64, build in ((True, args.build), (False, args.build32)):
        if os.path.exists(os.path.join(build, "boot", "bootcd.Debug.lst")):
            systems[is64] = System(build, apisets)

    need = {"dll": collections.Counter(), "fn": collections.Counter(),
            "late_dll": collections.Counter(), "late_fn": collections.Counter()}
    ready = total = 0
    for folder in args.folders:
        mdll, mfn, ldll, lfn, checked = check_program(folder, systems)
        name = os.path.basename(os.path.normpath(folder))
        if not checked:
            print("%-28s no native programs checked" % name)
            continue
        total += 1
        if not mdll and not mfn:
            ready += 1
            state = "can start"
        else:
            state = "BLOCKED: %d DLLs, %d functions" % (len(mdll), len(mfn))
        if ldll or lfn:
            state += " (+ %d DLLs, %d functions delay-loaded)" % (len(ldll), len(lfn))
        print("%-28s %s" % (name, state))
        if args.details:
            for d in sorted(mdll):
                print("      dll  %s" % d)
            for d, f in sorted(mfn):
                print("      fn   %s!%s" % (d, f))
        need["dll"].update(mdll)
        need["fn"].update(mfn)
        need["late_dll"].update(ldll)
        need["late_fn"].update(lfn)

    print("\n%d of %d programs have every startup import available." % (ready, total))
    for key, title in (("dll", "Missing DLLs that stop programs starting"),
                       ("fn", "Missing functions that stop programs starting"),
                       ("late_dll", "Missing delay-loaded DLLs"),
                       ("late_fn", "Missing delay-loaded functions")):
        if not need[key]:
            continue
        print("\n%s (programs needing each):" % title)
        for item, n in need[key].most_common(args.top):
            print("  %3d  %s" % (n, item if key.endswith("dll") else "%s!%s" % item))
    fns = need["fn"] + need["late_fn"]
    if fns:
        by_dll = collections.Counter(d for d, _ in fns)
        print("\nMissing functions per DLL: " + ", ".join("%s %d" % x for x in by_dll.most_common()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
