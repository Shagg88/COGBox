#!/usr/bin/env python3
"""xref: find a string in a loaded image and the code that builds its address. Ours, MIT.

  xref.py image.bin "PoolSize" ["MaxSmoothedFrameRate" ...]   [--disasm N]

Each string is searched as ASCII and as UTF-16 big-endian (how UE3 stores TEXT() literals on the 360).
For every hit, code that builds the address with lis + addi/ori (or lis + a load/store displacement) is
listed with its containing .pdata function. --disasm N prints N instructions around each reference
(needs capstone).
"""
import argparse, bisect, struct, sys
sys.path.insert(0, __import__("os").path.dirname(__file__))
from funcscan import Image, BASE  # noqa: E402

DISP_OPS = {14, 24, 32, 34, 36, 38, 40, 44, 48, 50, 52, 54}


def builders(img, target):
    """Yield (address of the instruction that completes the address, address of its lis)."""
    hi_ha = ((target + 0x8000) >> 16) & 0xFFFF
    hi = (target >> 16) & 0xFFFF
    lo = target & 0xFFFF
    for a, b in img.code:
        for addr in range(a, b - 3, 4):
            w = img.word(addr)
            if w >> 26 != 15 or (w >> 16) & 31 != 0 or (w & 0xFFFF) not in (hi, hi_ha):
                continue
            rd = (w >> 21) & 31
            for k in range(1, 24):
                x = img.word(addr + 4 * k)
                op = x >> 26
                if op in DISP_OPS and (x >> 16) & 31 == rd and (x & 0xFFFF) == lo:
                    if op == 24 and (w & 0xFFFF) != hi:
                        continue
                    if op != 24 and (w & 0xFFFF) != hi_ha:
                        continue
                    yield addr + 4 * k, addr
                    break
                if (x >> 21) & 31 == rd and op not in (36, 38, 44, 52, 54) and op != 15:
                    break   # rd overwritten


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("strings", nargs="+")
    ap.add_argument("--disasm", type=int, default=0)
    a = ap.parse_args()
    img = Image(a.image)
    pd = sorted(img.pdata())
    ps = [s for s, _ in pd]

    def func(v):
        i = bisect.bisect_right(ps, v) - 1
        return pd[i][0] if i >= 0 and v < pd[i][0] + max(pd[i][1], 4) else None

    md = None
    if a.disasm:
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    for s in a.strings:
        for enc, pat in (("ascii", s.encode() + b"\0"), ("utf16be", s.encode("utf-16-be") + b"\0\0")):
            p = img.raw.find(pat)
            while p != -1:
                addr = BASE + p
                ok_start = (p == 0 or img.raw[p - 1] == 0) if enc == "ascii" else True
                if p % (2 if enc == "utf16be" else 1) == 0 and ok_start:
                    refs = list(builders(img, addr))
                    print(f'"{s}" {enc} at 0x{addr:08X} ({img.section_of(addr)}): {len(refs)} code reference(s)')
                    for at, lis in refs:
                        f = func(at)
                        print(f"    0x{at:08X} (lis 0x{lis:08X}) in {'0x%08X' % f if f else '?'}")
                        if md:
                            start = at - 4 * (a.disasm // 2)
                            for ins in md.disasm(img.raw[start - BASE:start - BASE + 4 * a.disasm], start):
                                print(f"        {'>' if ins.address == at else ' '} {ins.address:08X} {ins.mnemonic} {ins.op_str}")
                p = img.raw.find(pat, p + 1)


if __name__ == "__main__":
    main()
