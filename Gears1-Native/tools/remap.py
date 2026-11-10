#!/usr/bin/env python3
"""remap: find base-disc code addresses again in the title-update image. Ours, MIT.

For each address, take the instruction words around it with every address-like field masked
(branch displacements, lis immediates, 16-bit displacements off registers other than r1) and look
for the one place in the other image where the same masked window appears. The window grows from
+-8 to +-128 words until the match is unique. When an address also has a known delta from its
neighbours, ties are broken by the nearest candidate to the address the neighbours predict.

  remap.py base.bin tu4.bin addresses.txt      (any text; every 0xXXXXXXXX in it is remapped, in order)
Output: one line per address: old -> new  delta  status
"""
import bisect, collections, re, struct, sys

BASE = 0x82000000
DISP_OPS = {14, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 58, 62}


def mask(w):
    op = w >> 26
    if op == 18:
        return w & 0xFC000003
    if op == 16:
        return w & 0xFFFF0003
    if op == 15:
        return w & 0xFFFF0000
    if op in DISP_OPS and ((w >> 16) & 31) != 1:
        return w & 0xFFFF0000
    return w


class Img:
    def __init__(self, path):
        raw = open(path, "rb").read()
        n = len(raw) // 4
        self.w = struct.unpack(f">{n}I", raw[:n * 4])
        self.m = [mask(x) for x in self.w]
        self.idx = collections.defaultdict(list)
        for i, x in enumerate(self.m):
            self.idx[x].append(i)


def find(a, b, addr, lo=8, hi=128):
    i = (addr - BASE) // 4
    if not 0 <= i < len(a.m):
        return "OUT", []
    cands = b.idx.get(a.m[i], [])
    win = lo
    while cands:
        keep = [c for c in cands if all(0 <= i + k < len(a.m) and 0 <= c + k < len(b.m) and a.m[i + k] == b.m[c + k]
                                        for k in range(-win, win + 1))]
        if len(keep) == 1:
            return "OK", [BASE + 4 * keep[0]]
        if not keep:
            return ("NOT FOUND" if win == lo else "AMBIGUOUS"), [BASE + 4 * c for c in cands[:6]]
        if win >= hi:
            return "AMBIGUOUS", [BASE + 4 * c for c in keep[:6]]
        cands, win = keep, min(hi, win * 2)
    return "NOT FOUND", []


def main():
    a, b = Img(sys.argv[1]), Img(sys.argv[2])
    addrs = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{8})", open(sys.argv[3]).read())]
    seen, results = set(), []
    for addr in addrs:
        if addr in seen:
            continue
        seen.add(addr)
        results.append((addr, *find(a, b, addr)))
    # Tie-break ambiguous ones by the delta of the nearest resolved neighbours.
    good = sorted((ad, new[0] - ad) for ad, st, new in results if st == "OK")
    keys = [g[0] for g in good]
    out = []
    for ad, st, new in results:
        if st == "AMBIGUOUS" and good:
            j = bisect.bisect_left(keys, ad)
            near = [good[k][1] for k in (j - 1, j) if 0 <= k < len(good)]
            pred = [ad + d for d in near]
            hit = [n for n in new if n in pred]
            if len(hit) == 1:
                st, new = "OK (by neighbour delta)", hit
        out.append((ad, st, new))
        nt = f"0x{new[0]:08X}" if len(new) == 1 else ",".join(f"0x{n:08X}" for n in new) or "-"
        d = f"{new[0] - ad:+#x}" if len(new) == 1 else ""
        print(f"0x{ad:08X} -> {nt:12s} {d:10s} {st}")
    c = collections.Counter(st.split(" (")[0] for _, st, _ in out)
    print(f"# {len(out)} addresses: {dict(c)}")


if __name__ == "__main__":
    main()
