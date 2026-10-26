#!/usr/bin/env python3
"""Gears2-Native: find each game-specific address of the base image again in a patched image (title update).
Our code (MIT). Images are the --gow2_dump_image dumps (guest 0x82000000, big-endian PowerPC code).

Code addresses: the instructions around the address (a window, default -12..+12 words) with relative branch targets
and lis/addi-style immediates masked must match exactly once in the patched image's code; the window grows (up to
+-64 words) until the match is unique. Reports the new address, or NOT FOUND / AMBIGUOUS (with candidates).
Data addresses (.rdata / .data): found through the code that references them (lis + addi/lwz/lfs/stw pairs within
a few instructions); each referencing site is matched as code and the address it builds in the patched image is read.

Usage: tu_remap.py base.bin patched.bin inventory.txt [--out remap.txt]
inventory.txt: lines "kind | 0xADDRESS | where | note" (notes/63-tu6-addresses.txt); kind 'data' forces the data
method; anything else is tried as code first, then as data if the word there is not an instruction we can anchor on.
"""
import struct, sys, collections, argparse

BASE = 0x82000000


def words(img):
    n = len(img) // 4
    return struct.unpack('>%dI' % n, img[:n * 4])


def mask(w):
    op = w >> 26
    if op == 18:   # b / bl / ba: LI
        return w & 0xFC000003
    if op == 16:   # bc: BD
        return w & 0xFFFF0003
    if op == 15:   # addis / lis: the immediate is an address half when rA = 0 (lis) - mask always (data moves)
        return w & 0xFFFF0000
    if op in (14, 32, 34, 36, 38, 40, 44, 48, 50, 52, 54, 58, 62):  # addi, lwz, lbz, stw, stb, lhz, sth, lfs, lfd, stfs, stfd, ld, std
        ra = (w >> 16) & 31
        if ra != 1:  # stack offsets stay (they pin the frame layout); other displacements may be data halves
            return w & 0xFFFF0000
    return w


class Image:
    def __init__(self, path):
        self.raw = open(path, 'rb').read()
        self.w = words(self.raw)
        self.m = [mask(x) for x in self.w]
        self.index = collections.defaultdict(list)
        for i, x in enumerate(self.m):
            self.index[x].append(i)

    def word(self, addr):
        i = (addr - BASE) // 4
        return self.w[i] if 0 <= i < len(self.w) else None


def match_code(base, pat, addr, lo=12, hi=64):
    i = (addr - BASE) // 4
    if not (0 <= i < len(base.m)):
        return 'OUT OF IMAGE', []
    win = lo
    cands = pat.index.get(base.m[i], [])
    if not cands:
        return 'NOT FOUND', []
    while True:
        keep = []
        for c in cands:
            ok = True
            for k in range(-win, win + 1):
                a, b = i + k, c + k
                if not (0 <= a < len(base.m) and 0 <= b < len(pat.m)) or base.m[a] != pat.m[b]:
                    ok = False
                    break
            if ok:
                keep.append(c)
        if len(keep) == 1:
            exact = all(base.w[i + k] == pat.w[keep[0] + k] for k in range(-2, 3) if 0 <= i + k < len(base.w))
            return ('OK' if exact else 'OK (masked fields differ near it)'), [BASE + keep[0] * 4]
        if not keep:
            return ('NOT FOUND' if win == lo else 'NOT FOUND at +-%d (ambiguous below)' % win), [BASE + c * 4 for c in cands[:8]]
        if win >= hi:
            return 'AMBIGUOUS', [BASE + c * 4 for c in keep[:8]]
        cands = keep
        win = min(hi, win * 2)


def built_address(img, i):
    """If word i starts a lis rX,hi and a later instruction (within 8) uses rX with a 16-bit low half, return the
    address they build."""
    w = img.w[i]
    if w >> 26 != 15 or (w >> 16) & 31 != 0:
        return None
    rd = (w >> 21) & 31
    hi = w & 0xFFFF
    for k in range(1, 9):
        if i + k >= len(img.w):
            break
        x = img.w[i + k]
        op = x >> 26
        if op in (14, 32, 34, 36, 38, 40, 44, 48, 50, 52, 54) and (x >> 16) & 31 == rd:
            lo = x & 0xFFFF
            if lo >= 0x8000:
                lo -= 0x10000
            return ((hi << 16) + lo) & 0xFFFFFFFF
    return None


def data_refs(img, addr, limit=40):
    out = []
    for i, w in enumerate(img.w):
        if w >> 26 == 15 and (w >> 16) & 31 == 0 and (w & 0xFFFF) in ((addr >> 16) & 0xFFFF, ((addr + 0x8000) >> 16) & 0xFFFF):
            if built_address(img, i) == addr:
                out.append(i)
                if len(out) >= limit:
                    break
    return out


def match_data(base, pat, addr):
    refs = data_refs(base, addr)
    if not refs:
        return 'NO CODE REFERENCE', []
    votes = collections.Counter()
    for i in refs:
        st, new = match_code(base, pat, BASE + i * 4, lo=8, hi=32)
        if new:
            b = built_address(pat, (new[0] - BASE) // 4)
            if b is not None:
                votes[b] += 1
    if not votes:
        return 'REFERENCES NOT FOUND (%d refs)' % len(refs), []
    best, n = votes.most_common(1)[0]
    agree = '%d of %d references agree' % (n, len(refs))
    return ('OK data (%s)' % agree if len(votes) == 1 else 'DISAGREE data (%s)' % agree), [a for a, _ in votes.most_common(4)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('base'); ap.add_argument('patched'); ap.add_argument('inventory'); ap.add_argument('--out')
    a = ap.parse_args()
    base, pat = Image(a.base), Image(a.patched)
    lines, seen = [], {}
    for raw in open(a.inventory):
        if raw.startswith('#') or '|' not in raw:
            continue
        kind, addr, where, note = [x.strip() for x in (raw.split('|', 3) + [''] * 4)[:4]]
        addr_i = int(addr, 16)
        key = (kind == 'data', addr_i)
        if key not in seen:
            in_code = base.word(addr_i)
            is_data = kind == 'data' or addr_i >= 0x83000000 or (kind == 'code' and in_code is not None and (in_code >> 26) == 0)
            if kind in ('hook', 'hook-target', 'hint') or not is_data:
                st, new = match_code(base, pat, addr_i)
                if st.startswith('NOT FOUND') and kind == 'code':
                    st2, new2 = match_data(base, pat, addr_i)
                    if new2:
                        st, new = st2, new2
            else:
                st, new = match_data(base, pat, addr_i)
            seen[key] = (st, new)
        st, new = seen[key]
        newtxt = ', '.join('0x%08X' % x for x in new) if new else '-'
        delta = ('%+d' % (new[0] - addr_i)) if len(new) == 1 or st.startswith('OK') else ''
        lines.append('%-11s %s -> %-24s %-8s %-40s %s | %s' % (kind, addr, newtxt, delta, st, where, note[:80]))
    text = '\n'.join(lines) + '\n'
    summary = collections.Counter(l.split()[4] if len(l.split()) > 4 else '?' for l in lines)
    if a.out:
        open(a.out, 'w').write(text)
    sys.stdout.write(text)
    ok = sum(1 for k, (s, n) in seen.items() if s.startswith('OK'))
    print('# %d distinct addresses: %d OK, %d need a look' % (len(seen), ok, len(seen) - ok))


if __name__ == '__main__':
    main()
