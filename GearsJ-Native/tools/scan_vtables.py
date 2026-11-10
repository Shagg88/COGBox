"""GearsJ-Native: copied from Gears2-Native tools/scan_vtables.py (Shagg's own project, D5); manifest name changed.

Input: an image dump (--gow2_dump_image or the codegen dump) and generated/default/*.cpp (known function starts) + gow2_manifest.toml.
Method: every aligned big-endian word in a non-code section that points into a code section is a code pointer.
Consecutive code pointers form a run. A run whose targets fall in two or more different functions looks like a vtable
or callback table (a switch jump table points into one function, so it's skipped). Entries in such runs that are not
known function starts are candidates. Each candidate is printed with its table, the containing function and the
first instruction word, for review before it goes into the manifest.
Usage: python tools/scan_vtables.py <root> <image.bin>
"""
import bisect, glob, os, re, struct, sys

root, image_path = sys.argv[1], sys.argv[2]
tu6 = '--tu6' in sys.argv[3:]   # notes/63: generated/tu6 + gow2_manifest_tu6.toml
ignore_manifest = '--ignore-manifest' in sys.argv[3:]   # self-test: would the scan have found the hints we added by running?
BASE = 0x82000000
img = open(image_path, 'rb').read()
if struct.unpack_from('<H', img, 0)[0] == 0x5A4D:   # 'MZ': image dump with its PE header
    pe = struct.unpack_from('<I', img, 0x3C)[0]
    nsec = struct.unpack_from('<H', img, pe + 6)[0]
    optsz = struct.unpack_from('<H', img, pe + 20)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        name = img[o:o + 8].rstrip(b'\0').decode(errors='replace')
        vsize, va = struct.unpack_from('<II', img, o + 8)
        ch = struct.unpack_from('<I', img, o + 36)[0]
        secs.append((name, BASE + va, vsize, bool(ch & 0x20000000)))
else:   # a dump with no header (as Gears 3's codegen dump): code range from the runtime log ("Function table initialized ... code=")
    cs, ce = [int(x, 16) for x in next(a for a in sys.argv[3:] if a.startswith('--code=')).split('=')[1].split('-')]
    secs = [('data-lo', BASE, cs - BASE, False), ('code', cs, ce - cs, True), ('data-hi', ce, BASE + len(img) - ce, False)]
code = [(a, a + s) for n, a, s, x in secs if x]
print('sections:', ', '.join(f'{n} {a:08X}+{s:X}{" code" if x else ""}' for n, a, s, x in secs))

def in_code(v):
    return any(a <= v < b for a, b in code)

starts = set()
init = next((a.split('=', 1)[1] for a in sys.argv[3:] if a.startswith('--init=')), None)   # a gow2_init.cpp: its function table
if init:
    starts.update(int(m, 16) for m in re.findall(r'\{ 0x([0-9A-F]{8}), sub_', open(init, encoding='utf-8', errors='replace').read()))
for f in ([] if init else glob.glob(os.path.join(root, 'generated', 'tu6' if tu6 else 'default', 'gowj_recomp.*.cpp'))):
    starts.update(int(m, 16) for m in re.findall(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)', open(f, encoding='utf-8', errors='replace').read(), re.M))
manifest_hints = {int(m, 16) for m in re.findall(r'^0x([0-9A-Fa-f]{8})\s*=', open(os.path.join(root, 'gowj_manifest.toml')).read(), re.M)}
if ignore_manifest:
    starts -= manifest_hints   # hinted functions also appear as DEFINE_REX_FUNC; drop them to test the scan blind
else:
    starts.update(manifest_hints)
order = sorted(starts)
print(f'known function starts: {len(order)}')

def containing(v):
    i = bisect.bisect_right(order, v) - 1
    return order[i] if i >= 0 else None

def ends_function(a):
    w = struct.unpack_from('>I', img, a - BASE)[0]
    return w in (0x4E800020, 0x4E800420) or (w >> 26 == 18 and w & 3 == 0)   # blr, bctr, b

cands = {}
tables = 0
for n, a, s, x in secs:
    if x:
        continue
    off0 = a - BASE
    run = []
    def flush():
        global tables
        if len(run) >= 2:
            funcs = {containing(v) for _, v in run}
            known = sum(1 for _, v in run if v in starts)
            if len(funcs) >= 2 and known * 2 >= len(run):
                tables += 1
                for at, v in run:
                    if v not in starts:
                        cands.setdefault(v, []).append(at)
            elif len(funcs) >= 2 and known >= 2:
                # Rule from the Gears 3 port: tables of tiny methods (8-byte 'li r3,N; blr' getters) that discovery merged into
                # one function have fewer than half known entries (0x82001040: 14 of 29; job 007 crashed on its entry
                # 0x82B21F30). Take an unknown entry here only when the word before it ends a function (blr, b, bctr).
                for at, v in run:
                    if v not in starts and ends_function(v - 4):
                        cands.setdefault(v, []).append(at)
    for off in range(off0, off0 + s - 3, 4):
        v = struct.unpack_from('>I', img, off)[0]
        if v % 4 == 0 and in_code(v):
            run.append((BASE + off, v))
        else:
            flush()
            run = []
    flush()
print(f'pointer tables that look like vtables/callback tables: {tables}')
print(f'candidate missing functions: {len(cands)}')
rejected = [v for v in cands if struct.unpack_from('>I', img, v - BASE)[0] >> 26 == 0]  # opcode 0: not an instruction (data)
# Rule from the Gears 3 port: exception scope tables point to resume points inside SEH functions; MSVC marks those regions with
# 'mr r8,r8' (0x7D084378). A split there breaks the function (codegen: unresolved branch 0x82CD7074 -> 0x82CD6FD0).
seh = [v for v in cands if v not in rejected and any(struct.unpack_from('>I', img, a - BASE)[0] == 0x7D084378 for a in range(v - 16, v + 16, 4))]
for v in seh:
    print(f'# rejected 0x{v:08X}: SEH resume point (mr r8,r8 marker next to it)')
rejected += seh
for v in rejected:
    print(f'# rejected 0x{v:08X}: first word is not an instruction (opcode 0)')
for v in sorted(set(cands) - set(rejected)):
    p = containing(v)
    w = struct.unpack_from('>I', img, v - BASE)[0]
    where = f'inside sub_{p:08X} (+0x{v - p:X})' if p is not None else 'no containing function'
    print(f'0x{v:08X} = {{}}   # {len(cands[v])} table ref(s), first at 0x{cands[v][0]:08X}; {where}; first word {w:08X}')
