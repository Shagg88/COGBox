#!/usr/bin/env python3
"""xex_addr_map.py - find where code addresses moved between two builds of the same Xbox 360 game
(for example the disc XEX and the same XEX after a title update's default.xexp is applied).

Game-neutral. Shared by Gears 3 and Gears 2 (2026-10-22).

Inputs
  base image      raw dump of the loaded image of the OLD build (rexglue REX_DUMP_IMAGE / --gow2_dump_image;
                  a dump that starts with an 'MZ' PE header is accepted too)
  patched image   the same for the NEW build
  addresses       one or more text files (a rexglue manifest .toml, C++ source, or a plain list). Every hex number
                  0x........ inside the image range is taken, except in comments (# ... and // ...).
                  Or --addr 0x82AF98EC (repeatable).

Method
  Each address is compared as a window of instructions around it (default 8 before, 24 after). Instruction words
  are "masked" before comparing so that things that move with the code do not count as differences:
    - b/bl target, bc target               (always masked)
    - lis/addis immediate                   (high half of an address)
    - the 16-bit immediate of addi/ori/loads/stores whose base register was set by a lis in the window
      (low half of the same address)
  Candidates come from an index of 6-instruction runs in the new image (fully masked), so the search is fast.
  Each candidate is scored by how many window words match. A result is:
    exact      every masked word in the window matches, and only one place in the new image does that
    shifted    best score is not perfect or not unique, but the winner uses the same shift as nearby exact results
    ambiguous  several equally good places, none confirmed by nearby shifts (all listed)
    not found  nothing scores at least --min-score of the window

Output: TSV with old address, new address, shift, status, score, the old and new instruction (hex + disassembly
when capstone is installed), and the source line it came from.

Usage
  python xex_addr_map.py base.bin patched.bin gow3_manifest.toml src/game_patches.cpp > moved.tsv
  python xex_addr_map.py base.bin patched.bin --addr 0x82969614 --addr 0x827B2BD0
Options: --base 0x82000000 (image base), --before 8 --after 24, --min-score 0.75, --check-funcs old.json new.json
"""
import argparse, bisect, re, struct, sys
import numpy as np

K = 6  # instructions per index key


def load_image(path):
    data = open(path, 'rb').read()
    if data[:2] == b'MZ':  # PE header in front: use SizeOfHeaders-free layout = image as loaded (offset 0 = base)
        pass  # rexglue dumps with a header are still laid out from the image base, so nothing to strip
    n = len(data) // 4
    return np.frombuffer(data[:n * 4], dtype='>u4').astype(np.uint32)


def mask_full(w):
    """Vectorised mask used for the index: branch targets and every 16-bit immediate of D-form instructions."""
    op = w >> 26
    out = w.copy()
    out = np.where(op == 18, w & 0xFC000003, out)                 # b / bl
    out = np.where(op == 16, w & 0xFFFF0003, out)                 # bc
    dform = (op == 14) | (op == 15) | (op == 24) | (op == 25) | ((op >= 32) & (op <= 55)) | (op == 58) | (op == 62)
    out = np.where(dform, w & 0xFFFF0000, out)
    return out


def mask_smart(words):
    """Per-window mask: branch targets, lis immediates, and the low half of addresses built from a lis."""
    out = []
    lis_regs = set()
    for w in words:
        w = int(w)
        op = w >> 26
        rt = (w >> 21) & 31
        ra = (w >> 16) & 31
        if op == 18:
            m = w & 0xFC000003
        elif op == 16:
            m = w & 0xFFFF0003
        elif op == 15 and ra == 0:          # lis rT, hi
            m = w & 0xFFFF0000
            lis_regs.add(rt)
        elif op == 15:                      # addis rT, rA, hi (address arithmetic)
            m = w & 0xFFFF0000
            lis_regs.add(rt)
        elif op in (14, 24) and ra in lis_regs:   # addi / ori from a lis register
            m = w & 0xFFFF0000
            lis_regs.add(rt) if op == 14 else lis_regs.add(ra)
        elif (32 <= op <= 55 or op in (58, 62)) and ra in lis_regs:   # load/store with lis base
            m = w & 0xFFFF0000
        else:
            m = w
            # a register overwritten by anything else is no longer an address
            if op not in (36, 37, 38, 39, 44, 45, 52, 53, 54, 55, 62) and rt in lis_regs and op not in (16, 18):
                lis_regs.discard(rt)
        out.append(m)
    return out


def build_index(masked):
    """Hash of every K-word run, sorted, for fast lookup."""
    n = len(masked) - K + 1
    h = np.zeros(n, dtype=np.uint64)
    for i in range(K):
        h = h * np.uint64(1000003) ^ masked[i:i + n].astype(np.uint64)
    order = np.argsort(h, kind='stable')
    return h[order], order


def lookup(index, key):
    hs, order = index
    lo = np.searchsorted(hs, key, 'left')
    hi = np.searchsorted(hs, key, 'right')
    return order[lo:hi]


def key_of(masked, i):
    h = np.uint64(0)
    np.seterr(over='ignore')
    for k in range(K):
        h = h * np.uint64(1000003) ^ np.uint64(masked[i + k])
    return h


def disasm(word, addr):
    try:
        import capstone
        md = disasm.md if hasattr(disasm, 'md') else None
        if md is None:
            md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
            disasm.md = md
        for ins in md.disasm(struct.pack('>I', word), addr):
            return f'{ins.mnemonic} {ins.op_str}'.strip()
    except Exception:
        pass
    return ''


def collect_addresses(paths, extra, base, size):
    found = []  # (addr, source)
    for p in paths:
        for ln, line in enumerate(open(p, errors='replace'), 1):
            code = line.split('//')[0]
            code = code.split('#')[0] if not p.endswith(('.cpp', '.h', '.c')) else code
            for m in re.finditer(r'0x([0-9A-Fa-f]{8})(?![0-9A-Fa-f])', code):
                a = int(m.group(1), 16)
                if base <= a < base + size:
                    found.append((a, f'{p.split("/")[-1]}:{ln}'))
    for a in extra:
        found.append((int(a, 16), 'cmdline'))
    seen, out = set(), []
    for a, s in found:
        if a not in seen:
            seen.add(a)
            out.append((a, s))
    return out


def main():
    np.seterr(over='ignore')
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('base_image')
    ap.add_argument('patched_image')
    ap.add_argument('files', nargs='*')
    ap.add_argument('--addr', action='append', default=[])
    ap.add_argument('--base', default='0x82000000')
    ap.add_argument('--before', type=int, default=8)
    ap.add_argument('--after', type=int, default=24)
    ap.add_argument('--min-score', type=float, default=0.75)
    ap.add_argument('--ref-step', type=lambda x: int(x, 0), default=0x8000,
                    help='spacing of extra reference points used to learn the local shift (bytes)')
    ap.add_argument('--check-funcs', nargs=2, metavar=('OLD_JSON', 'NEW_JSON'),
                    help='self-test: map every old function start and check the result is a new function start')
    a = ap.parse_args()
    base = int(a.base, 16)
    old = load_image(a.base_image)
    new = load_image(a.patched_image)
    new_full = mask_full(new)
    old_full = mask_full(old)
    index = build_index(new_full)
    B, A = a.before, a.after

    def window(img, i):
        lo, hi = max(0, i - B), min(len(img), i + A)
        return lo, img[lo:hi]

    def map_one(addr):
        i = (addr - base) // 4
        if not (0 <= i < len(old)):
            return None, 'outside image', 0, []
        lo, w_old = window(old, i)
        m_old = mask_smart(w_old)
        cands = set()
        # anchors: the run starting at the address, the run ending at it, and the window's start
        for start in (i, i - K + 1, lo, i + K):
            if 0 <= start <= len(old) - K:
                for c in lookup(index, key_of(old_full, start)):
                    cands.add(int(c) + (i - start))
        # Fast pre-score of every candidate with the coarse (index) mask, vectorised; then the exact per-window
        # mask only on the best few dozen.
        off = i - lo
        cl = np.array(sorted(j for j in cands if 0 <= j - off and j - off + len(w_old) <= len(new)), dtype=np.int64)
        if len(cl) == 0:
            return None, 'not found', 0.0, []
        ref = old_full[lo:lo + len(w_old)]
        idx = (cl - off)[:, None] + np.arange(len(w_old))[None, :]
        pre = (new_full[idx] == ref[None, :]).sum(axis=1)
        keep = cl[pre >= pre.max() - 2]
        if len(keep) > 64:
            keep = cl[pre == pre.max()]
        if len(keep) > 64:   # many identical places: report them as ambiguous without the slow pass
            frac = float(pre.max()) / len(w_old)
            return None, ('ambiguous' if frac >= a.min_score else 'not found'), frac, [int(x) for x in keep]
        scored = []
        for j in keep:
            j = int(j)
            lo2 = j - off
            m_new = mask_smart(new[lo2:lo2 + len(w_old)])
            s = sum(1 for x, y in zip(m_old, m_new) if x == y)
            scored.append((s, j))
        if not scored:
            return None, 'not found', 0.0, []
        scored.sort(reverse=True)
        best = scored[0][0]
        tops = [j for s, j in scored if s == best]
        frac = best / len(w_old)
        if frac < a.min_score:
            return None, 'not found', frac, tops[:5]
        if len(tops) == 1:
            return tops[0], ('exact' if best == len(w_old) else 'close'), frac, tops
        return None, 'ambiguous', frac, tops

    def resolve(addrs):
        results = []
        for addr, src in addrs:
            j, st, frac, tops = map_one(addr)
            results.append([addr, j, st, frac, tops, src])
        # Second pass: settle ambiguous ones by the shift of the nearest exact neighbours. Neighbours come from the
        # results themselves plus a set of reference points sampled across the whole image (every --ref-step bytes).
        exact = {r[0]: base + r[1] * 4 - r[0] for r in results if r[2] == 'exact'}
        for ra in range(base, base + len(old) * 4, a.ref_step):
            if ra not in exact:
                j, st, _, _ = map_one(ra)
                if st == 'exact':
                    exact[ra] = base + j * 4 - ra
        ex = sorted(exact.items())
        ex_addrs = [e[0] for e in ex]
        for r in results:
            if r[2] == 'ambiguous' and ex:
                k = bisect.bisect_left(ex_addrs, r[0])
                near = [ex[x][1] for x in (k - 2, k - 1, k, k + 1) if 0 <= x < len(ex)]
                hits = [t for t in r[4] if (base + t * 4 - r[0]) in near]
                if len(hits) == 1:
                    r[1], r[2] = hits[0], 'shifted'
        # Confidence for 'close' / 'shifted': the instruction at the address must be the same (masked), and the shift
        # must equal the shift of a nearby exact match. Anything else is marked CHECK for a hand look.
        for r in results:
            if r[1] is None or r[2] == 'exact':
                continue
            i = (r[0] - base) // 4
            same = mask_smart([old[i]])[0] == mask_smart([new[r[1]]])[0]
            k = bisect.bisect_left(ex_addrs, r[0])
            near = [ex[x][1] for x in (k - 2, k - 1, k, k + 1) if 0 <= x < len(ex)]
            agrees = (base + r[1] * 4 - r[0]) in near
            if not (same and agrees):
                r[2] = 'CHECK ' + r[2] + ('' if same else ' (instruction differs)') + ('' if agrees else ' (shift unlike neighbours)')
        return results

    if a.check_funcs:
        import json
        oldf = json.load(open(a.check_funcs[0]))
        newf = set(json.load(open(a.check_funcs[1])))
        step = max(1, len(oldf) // 3000)
        stats = {}
        for addr, j, st, frac, tops, src in resolve([(f, '') for f in oldf[::step]]):
            ok = j is not None and (base + j * 4) in newf
            key = st + (' -> function start' if ok else (' -> NOT a function start' if j is not None else ''))
            stats[key] = stats.get(key, 0) + 1
        for k, v in sorted(stats.items(), key=lambda kv: -kv[1]):
            print(f'{v:6d}  {k}')
        return

    results = resolve(collect_addresses(a.files, a.addr, base, len(old) * 4))
    print('old\tnew\tshift\tstatus\tscore\told_instr\tnew_instr\tsource')
    for addr, j, st, frac, tops, src in results:
        i = (addr - base) // 4
        ow = int(old[i])
        if j is not None:
            na = base + j * 4
            nw = int(new[j])
            print(f'0x{addr:08X}\t0x{na:08X}\t{na - addr:+#x}\t{st}\t{frac:.2f}\t{ow:08X} {disasm(ow, addr)}\t'
                  f'{nw:08X} {disasm(nw, na)}\t{src}')
        else:
            alts = ','.join(f'0x{base + t * 4:08X}' for t in tops[:5])
            print(f'0x{addr:08X}\t-\t-\t{st}\t{frac:.2f}\t{ow:08X} {disasm(ow, addr)}\t{alts}\t{src}')


if __name__ == '__main__':
    main()
