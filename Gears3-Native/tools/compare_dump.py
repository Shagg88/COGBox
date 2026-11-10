#!/usr/bin/env python3
"""Turn a --gow2_renderer_compare_dump_frames folder (patch 0013) into pictures and a last-writer report.

Usage: compare_dump.py <dump dir> <out dir> [stem ...] [--box x0,y0,x1,y1]
For each dumped copy (or the stems given): <stem>-ours.png, <stem>-ref.png, <stem>-diff.png (per-pixel max channel
difference, scaled), and in report.txt the draws that last wrote the differing pixels (all, and inside --box).
"""
import os
import re
import sys
from collections import Counter

import numpy as np
from PIL import Image


def half(u16):
    return u16.astype(np.uint16).view(np.float16).astype(np.float32)


def decode(words, w, h, nwords, fmt):
    """Words -> float RGB in [0,1] for display, plus raw channels for the difference."""
    a = np.asarray(words, dtype=np.uint32).reshape(h, w, nwords)
    if nwords == 1 and fmt in (7, 54):  # 2_10_10_10 (unorm view of 10-bit channels)
        v = a[..., 0]
        ch = np.stack([(v >> s) & 0x3FF for s in (0, 10, 20)], -1).astype(np.float32)
        return ch / 1023.0, ch
    if nwords == 1 and fmt in (31, 28):  # 16_16 float
        v = a[..., 0]
        ch = np.stack([half(v & 0xFFFF), half(v >> 16), np.zeros_like(v, np.float32)], -1)
        return np.clip(ch, 0, 1), ch * 1023.0
    if nwords == 1:  # 8_8_8_8 and anything else 32bpp: bytes
        v = a[..., 0]
        ch = np.stack([(v >> s) & 0xFF for s in (0, 8, 16)], -1).astype(np.float32)
        return ch / 255.0, ch * 4.0
    lo, hi = a[..., 0], a[..., 1]
    if fmt in (32, 29):  # 16_16_16_16 float
        ch = np.stack([half(lo & 0xFFFF), half(lo >> 16), half(hi & 0xFFFF)], -1)
        return np.clip(ch, 0, 1), ch * 1023.0
    ch = np.stack([lo & 0xFFFF, lo >> 16, hi & 0xFFFF], -1).astype(np.float32)  # 16_16_16_16 unorm
    return ch / 65535.0, ch / 64.0


def main():
    args = sys.argv[1:]
    box = None
    if "--box" in args:
        i = args.index("--box")
        box = tuple(int(v) for v in args[i + 1].split(","))
        del args[i:i + 2]
    src, out, stems = args[0], args[1], set(args[2:])
    os.makedirs(out, exist_ok=True)
    draws = {}
    for name in os.listdir(src):
        m = re.match(r"f(\d+)-draws\.txt$", name)
        if m:
            table = {}
            for line in open(os.path.join(src, name)):
                parts = line.split()
                table[int(parts[1])] = line.strip()
            draws[int(m.group(1))] = table
    rep = open(os.path.join(out, "report.txt"), "w")
    for line in open(os.path.join(src, "index.txt")):
        m = re.match(r"(f(\d+)-c\d+) w (\d+) h (\d+) words (\d+) dest_format (\d+)", line)
        if not m:
            continue
        stem, frame, w, h, nw, fmt = m.group(1), int(m.group(2)), *map(int, m.groups()[2:])
        if stems and stem not in stems:
            continue
        rd = lambda ext: np.fromfile(os.path.join(src, stem + ext), dtype="<u4")
        ours, ref, ids = rd(".ours.bin"), rd(".guest.bin"), rd(".id.bin").reshape(h, w)
        o_img, o_ch = decode(ours, w, h, nw, fmt)
        r_img, r_ch = decode(ref, w, h, nw, fmt)
        d = np.abs(o_ch - r_ch).max(-1)
        Image.fromarray((o_img * 255).astype(np.uint8)).save(os.path.join(out, stem + "-ours.png"))
        Image.fromarray((r_img * 255).astype(np.uint8)).save(os.path.join(out, stem + "-ref.png"))
        scale = 255.0 / max(1.0, float(np.percentile(d[d > 0], 99)) if (d > 0).any() else 1.0)
        Image.fromarray(np.clip(d * scale, 0, 255).astype(np.uint8)).save(os.path.join(out, stem + "-diff.png"))
        rep.write(line)
        lum_o, lum_r = o_img.mean(-1), r_img.mean(-1)
        for label, mask in (("all", d > 0), ("box", None)):
            if label == "box":
                if not box:
                    continue
                mask = np.zeros_like(d, bool)
                mask[box[1]:box[3], box[0]:box[2]] = True
                mask &= d > 0
            n = int(mask.sum())
            darker = int((mask & (lum_o < lum_r)).sum())
            rep.write(f"  {label}: {n} px differ, ours darker in {darker}\n")
            c = Counter(ids[mask].tolist())
            for i, k in c.most_common(8):
                rec = draws.get(frame, {}).get(i if i % 2 == 0 else i - 1, "")
                kind = {0: "not written this frame", 1: "transfer"}.get(i, "stand-in " if i % 2 else "")
                sel = mask & (ids == i)
                rep.write(f"    id {i} ({kind}) {k} px, mean lum ours {lum_o[sel].mean():.3f} ref {lum_r[sel].mean():.3f}"
                          f" | {rec}\n")
    rep.close()
    print(open(os.path.join(out, "report.txt")).read()[:4000])


if __name__ == "__main__":
    main()
