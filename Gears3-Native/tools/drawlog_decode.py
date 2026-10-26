#!/usr/bin/env python3
"""drawlog_decode.py - read a --gow2_draw_log_frames file (with --gow2_draw_log_full=N) and print, for chosen draws,
the texture fetch constants (address, format, size, filters, swizzle, signedness) and chosen pixel/vertex shader float
constants, in plain words. Game-neutral (Xenos register layout).

Usage: python drawlog_decode.py drawlog-000150.txt --draws 140-146 [--slots 0,1] [--ps 1,2,3,7,8,9,10,11,255] [--vs ...]
Draw numbers count DRAW lines only (copies are skipped), starting at 0 - the renderer's "draw N" numbering
(check against the summary's draw ranges per copy).
Fetch constant layout (Xenos xe_gpu_texture_fetch_t, 6 dwords per slot at 0x4800 + 6*slot):
  dw0: type[1:0] sign_x/y/z/w[2-9] clamp_x/y/z[10-18] signed_rf_mode[19-20] dim_tbd[21] pitch[22-30] tiled[31]
  dw1: format[5:0] endianness[7:6] request_size[9:8] stacked[10] nearest_clamp_policy[11] base_address[31:12] (<<12)
  dw2 (2D): width-1 [12:0] height-1 [25:13]
  dw3: num_format[0] swizzle[12:1] exp_adjust[18:13] mag_filter[20:19] min_filter[22:21] mip_filter[24:23]
       aniso[27:25] arbitrary_filter[30:28] border_size[31]
  dw4: vol_mag/min, mip_min/max level, mag/min aniso walk, lod_bias, grad_exp_adjust ...
  dw5: border_color[1:0] force_bc_w_to_max[2] tri_clamp[4:3] aniso_bias[8:5] dimension[10:9] packed_mips[11]
       mip_address[31:12]
"""
import argparse, re, struct

FORMATS = {6: 'k_8_8_8_8', 7: 'k_2_10_10_10', 18: 'DXT1', 19: 'DXT2_3', 20: 'DXT4_5', 22: 'k_24_8 (depth)',
           23: 'k_24_8_FLOAT (depth 20e4)', 24: 'k_16', 25: 'k_16_16', 26: 'k_16_16_16_16', 30: 'k_16_FLOAT',
           31: 'k_16_16_FLOAT', 32: 'k_16_16_16_16_FLOAT', 36: 'k_32_FLOAT', 49: 'DXN', 2: 'k_8', 0: 'k_1_REVERSE'}
FILTER = {0: 'point', 1: 'linear', 2: 'basemap', 3: 'fetch-const'}


def parse(path):
    draws = []  # (line_index, kind, header, {reg: value})
    cur = None
    for line in open(path, errors='replace'):
        m = re.match(r'\s*(\d+) (DRAW|COPY) (.*)', line)
        if m:
            cur = [int(m.group(1)), m.group(2), m.group(3).strip(), {}]
            draws.append(cur)
            continue
        m = re.match(r'\s+R([0-9A-F]{4}) ([0-9A-F]{8})', line)
        if m and cur is not None:
            cur[3][int(m.group(1), 16)] = int(m.group(2), 16)
    return draws


def f32(v):
    return struct.unpack('<f', struct.pack('<I', v))[0]


def fetch(regs, slot):
    base = 0x4800 + 6 * slot
    dw = [regs.get(base + i) for i in range(6)]
    if None in dw:
        return f'slot {slot}: not in the dump'
    d0, d1, d2, d3, d4, d5 = dw
    typ = d0 & 3
    fmt = d1 & 0x3F
    addr = (d1 >> 12) << 12
    w = (d2 & 0x1FFF) + 1
    h = ((d2 >> 13) & 0x1FFF) + 1
    pitch = ((d0 >> 22) & 0x1FF) * 32
    tiled = (d0 >> 31) & 1
    signs = [(d0 >> (2 + 2 * i)) & 3 for i in range(4)]
    swz = (d3 >> 1) & 0xFFF
    sw = ''.join('xyzw01??'[(swz >> (3 * i)) & 7] for i in range(4))
    expadj = ((d3 >> 13) & 0x3F)
    if expadj & 0x20: expadj -= 0x40
    mag, minf, mip = (d3 >> 19) & 3, (d3 >> 21) & 3, (d3 >> 23) & 3
    dim = (d5 >> 9) & 3
    endian = (d1 >> 6) & 3
    return (f'slot {slot}: type {typ} format {fmt} {FORMATS.get(fmt, "?")} at 0x{addr:08X} {w}x{h} pitch {pitch} '
            f'tiled {tiled} endian {endian} dim {dim} | swizzle {sw} signs {signs} num_format {d3 & 1} exp_adjust '
            f'{expadj} | filter mag {FILTER[mag]} min {FILTER[minf]} mip {mip} | raw {" ".join(f"{x:08X}" for x in dw)}')


def consts(regs, base, idxs, label):
    out = []
    for c in idxs:
        r = base + 4 * c
        vals = [regs.get(r + i) for i in range(4)]
        if None in vals:
            out.append(f'{label}c{c}: not in the dump')
        else:
            out.append(f'{label}c{c}: ' + ', '.join(f'{f32(v):.6g}' for v in vals))
    return out


def rng(s):
    out = []
    for part in s.split(','):
        if '-' in part:
            a, b = part.split('-'); out += list(range(int(a), int(b) + 1))
        elif part:
            out.append(int(part))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--draws', required=True)
    ap.add_argument('--slots', default='0,1,2,3')
    ap.add_argument('--ps', default='')
    ap.add_argument('--vs', default='')
    a = ap.parse_args()
    draws = [d for d in parse(a.log) if d[1] == 'DRAW']
    for n in rng(a.draws):
        if n >= len(draws):
            print(f'draw {n}: beyond the log ({len(draws)} draws)'); continue
        line, kind, hdr, regs = draws[n]
        print(f'== draw {n} (log line {line}): {hdr}')
        if not regs:
            print('   (no full register dump for this draw - raise --gow2_draw_log_full)'); continue
        for s in rng(a.slots):
            print('   ' + fetch(regs, s))
        for t in consts(regs, 0x4400, rng(a.ps), 'ps '):  # pixel shader floats: 0x4400 + 4*c (c 0..255)
            print('   ' + t)
        for t in consts(regs, 0x4000, rng(a.vs), 'vs '):
            print('   ' + t)


if __name__ == '__main__':
    main()
