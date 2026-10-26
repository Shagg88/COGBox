#!/usr/bin/env python3
"""funcscan: list the function entry points of a loaded Xbox 360 image, and the ones only reachable indirectly.
Ours, MIT. Works on an image from tools/xextool (guest memory from 0x82000000, PE header at the start).

Sources of function starts, each kept separately so they can be compared:
  pdata   .pdata (the compiler's function table: start + length; covers every function that has unwind info)
  bl      targets of direct calls (bl) inside code sections
  ptr     aligned words in data sections that point into code (vtables, callback tables)
  lisaddi addresses built in code with lis rX,hi + addi/ori rY,rX,lo that land in code (callbacks passed at run time)

Indirect-only candidates (ptr, lisaddi) that are not pdata/bl starts are what a recompiler's call-graph walk can miss.
Pointer runs whose targets all fall inside one pdata function are treated as switch jump tables, reported apart.

  funcscan.py image.bin summary
  funcscan.py image.bin starts  > starts.txt          every start with its sources
  funcscan.py image.bin indirect                     indirect-only candidates with evidence
  funcscan.py image.bin check hints.txt              how a list of addresses (one 0x... per line) is covered
"""
import bisect, collections, re, struct, sys

BASE = 0x82000000


class Image:
    def __init__(self, path):
        self.raw = open(path, "rb").read()
        r = self.raw
        if r[:2] != b"MZ":
            raise SystemExit("image has no PE header (expected a full image from xextool)")
        pe = struct.unpack_from("<I", r, 0x3C)[0]
        nsec = struct.unpack_from("<H", r, pe + 6)[0]
        optsz = struct.unpack_from("<H", r, pe + 20)[0]
        self.secs = []
        for i in range(nsec):
            o = pe + 24 + optsz + i * 40
            name = r[o:o + 8].rstrip(b"\0").decode(errors="replace")
            vsize, va = struct.unpack_from("<II", r, o + 8)
            ch = struct.unpack_from("<I", r, o + 36)[0]
            self.secs.append((name, BASE + va, vsize, bool(ch & 0x20000000)))
        dd = pe + 24 + 96
        self.dirs = {k: struct.unpack_from("<II", r, dd + 8 * k) for k in range(16)}
        self.code = [(a, a + s) for _, a, s, x in self.secs if x]

    def word(self, addr):
        return struct.unpack_from(">I", self.raw, addr - BASE)[0]

    def in_code(self, v):
        return any(a <= v < b for a, b in self.code)

    def section_of(self, v):
        for n, a, s, x in self.secs:
            if a <= v < a + s:
                return n
        return None

    def pdata(self):
        va, size = self.dirs[3]
        out = []
        for o in range(BASE + va, BASE + va + size, 8):
            start = self.word(o)
            info = self.word(o + 4)
            if start == 0:
                continue
            flen = ((info >> 8) & 0x3FFFFF) * 4   # PowerPC .pdata: prolog len 8 bits, function length 22 bits (words)
            out.append((start, flen))
        return out

    def relocs(self):
        """Yield (type, address) for each base relocation."""
        va, size = self.dirs[5]
        p, end = va, va + size
        r = self.raw
        while p < end:
            page, bsize = struct.unpack_from(">II", r, p)
            if bsize < 8:
                break
            for k in range(p + 8, p + bsize, 2):
                e = struct.unpack_from(">H", r, k)[0]
                t, off = e >> 12, e & 0xFFF
                if t:
                    yield t, BASE + page + off
            p += bsize


def scan(img):
    starts = collections.defaultdict(set)
    pd = img.pdata()
    for s, ln in pd:
        starts[s].add("pdata")
    extents = sorted(pd)
    ext_starts = [s for s, _ in extents]

    def pdata_func(v):
        i = bisect.bisect_right(ext_starts, v) - 1
        if i >= 0 and extents[i][0] <= v < extents[i][0] + max(extents[i][1], 4):
            return extents[i][0]
        return None

    # Direct calls.
    for a, b in img.code:
        for addr in range(a, b - 3, 4):
            w = img.word(addr)
            if w >> 26 == 18 and (w & 3) == 1:   # bl (relative, link)
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                t = (addr + li) & 0xFFFFFFFF
                if img.in_code(t):
                    starts[t].add("bl")

    # Absolute pointers in data. The loaded image has no relocations (the ".reloc" range holds other data on
    # the 360 [V, Judgment]), so every aligned word of a data section that points into code counts.
    rtypes = collections.Counter()
    data_ptrs = {}
    for n, a, s, x in img.secs:
        if x or n in (".pdata", ".reloc"):
            continue
        for at in range(a, a + s - 3, 4):
            v = img.word(at)
            if v & 3 == 0 and img.in_code(v):
                data_ptrs[at] = v

    # Group consecutive pointers into runs (tables).
    runs, run, last = [], [], None
    for at in sorted(data_ptrs):
        if last is not None and at != last + 4:
            runs.append(run)
            run = []
        run.append(at)
        last = at
    if run:
        runs.append(run)
    jump_tables = []
    ptr_evidence = collections.defaultdict(list)
    for run in runs:
        targets = [data_ptrs[a] for a in run]
        owners = {pdata_func(v) for v in targets}
        if len(run) >= 2 and len(owners) == 1 and None not in owners and not any(v in starts for v in targets):
            jump_tables.append((run[0], len(run), owners.pop()))   # every entry inside one function: a switch table
            continue
        for a in run:
            v = data_ptrs[a]
            starts[v].add("ptr")
            ptr_evidence[v].append(a)

    # Addresses built in code (lis + addi/ori), landing in code.
    lis_evidence = collections.defaultdict(list)
    for a, b in img.code:
        lis = {}
        for addr in range(a, b - 3, 4):
            w = img.word(addr)
            op = w >> 26
            rd, ra = (w >> 21) & 31, (w >> 16) & 31
            if op == 15 and ra == 0:                  # lis rd, hi
                lis[rd] = ((w & 0xFFFF) << 16, addr)
                continue
            if op in (14, 24) and ra in lis:          # addi / ori rd, ra, lo
                hi, at = lis[ra]
                lo = w & 0xFFFF
                v = ((hi + (lo - 0x10000 if lo >= 0x8000 else lo)) if op == 14 else (hi | lo)) & 0xFFFFFFFF
                if addr - at <= 64 and img.in_code(v) and v % 4 == 0:
                    starts[v].add("lisaddi")
                    lis_evidence[v].append(addr)
            if w == 0x4E800020 or (op == 18 and (w & 3) == 0):   # blr / b: end of a straight run
                lis.clear()
            elif op in (14, 15, 24, 31, 32) and rd in lis and not (op == 15 and ra == 0):
                lis.pop(rd, None)
    return dict(starts=starts, extents=extents, pdata_func=pdata_func, jump_tables=jump_tables,
                ptr_evidence=ptr_evidence, lis_evidence=lis_evidence, rtypes=rtypes)


def indirect_only(res):
    return sorted(v for v, src in res["starts"].items() if not (src & {"pdata", "bl"}))


def main():
    img = Image(sys.argv[1])
    cmd = sys.argv[2] if len(sys.argv) > 2 else "summary"
    res = scan(img)
    st = res["starts"]
    if cmd == "summary":
        print("sections: " + ", ".join(f"{n} {a:08X}+{s:X}{' code' if x else ''}" for n, a, s, x in img.secs))
        c = collections.Counter(s for v in st.values() for s in v)
        print(f"starts: {len(st)} total; by source {dict(c)}")
        io = indirect_only(res)
        inside = sum(1 for v in io if res["pdata_func"](v) is not None)
        print(f"indirect-only candidates: {len(io)} ({inside} fall inside a pdata function, {len(io) - inside} outside any)")
        print(f"switch-style pointer tables (all entries in one function): {len(res['jump_tables'])}")
    elif cmd == "starts":
        for v in sorted(st):
            print(f"0x{v:08X} {','.join(sorted(st[v]))}")
    elif cmd == "indirect":
        for v in indirect_only(res):
            owner = res["pdata_func"](v)
            ev = []
            if res["ptr_evidence"].get(v):
                ev.append(f"ptr at 0x{res['ptr_evidence'][v][0]:08X} ({img.section_of(res['ptr_evidence'][v][0])})")
            if res["lis_evidence"].get(v):
                ev.append(f"built at 0x{res['lis_evidence'][v][0]:08X}")
            where = f"inside 0x{owner:08X} +0x{v - owner:X}" if owner else "outside pdata"
            print(f"0x{v:08X} {','.join(sorted(st[v]))}; {where}; {'; '.join(ev)}; word {img.word(v):08X}")
    elif cmd == "check":
        hints = [int(m, 16) for m in re.findall(r"0x([0-9A-Fa-f]{8})", open(sys.argv[3]).read())]
        cov = collections.Counter()
        for h in hints:
            src = st.get(h, set())
            key = ("direct(" + ",".join(sorted(src & {"pdata", "bl"})) + ")") if src & {"pdata", "bl"} else \
                  ("indirect(" + ",".join(sorted(src)) + ")") if src else \
                  ("inside-pdata" if res["pdata_func"](h) else "none")
            cov[key] += 1
        for k, n in cov.most_common():
            print(f"{n:5d}  {k}")
        print(f"{len(hints)} addresses")


if __name__ == "__main__":
    main()
