#!/usr/bin/env python3
# Gears2-Native (MIT): find code addresses that the game builds in code (lis rX,hi / addi rY,rX,lo) and then passes to
# a call or stores, where no generated function covers that address. Those are callbacks the recompiler never saw as
# function entries: calling one ends in "[FATAL] Call to invalid or unregistered function". Idea from the Gears 3 port.
#   python scan_lis_addi.py <generated dir> <loaded image .bin, based at 0x82000000>
# Needs capstone (pip install capstone) to show the first instructions at each address.
import bisect, collections, glob, re, sys

gen, image = sys.argv[1], sys.argv[2]
starts = sorted({int(m, 16) for m in re.findall(r'\{ 0x([0-9A-F]{8}), sub_', open(gen + '/gow2_init.cpp').read())})
S = set(starts)
lo_c, hi_c = starts[0], starts[-1]
fre = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')
ire = re.compile(r'^\t// (\w+\.?) (.*)$')
size = {}                                  # function start -> instruction count
found = collections.defaultdict(lambda: [None, None])   # address -> [use, built in]


def use_of(reg, after):
    for op, a in after:
        if op.startswith('st') and a and a[0] == reg: return 'stored'
        if op in ('lwzx', 'lbzx', 'lhzx', 'lwax', 'add') and reg in a[1:]: return 'table'
        if op == 'mtctr' and a[0] == reg: return 'mtctr'
        if op.startswith(('lwz', 'lbz', 'lhz', 'lfs', 'lfd', 'ld', 'lvx')) and '(%s)' % reg in ','.join(a): return 'load-from'
        if op == 'bl': return 'arg-to-call' if reg in ('r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10') else '?'
        if a and a[0] == reg: return 'overwritten'
    return '?'


def scan(fn, ins):
    size[fn] = len(ins)
    lis = {}
    for i, (op, a) in enumerate(ins):
        if op == 'lis' and len(a) == 2:
            try: lis[a[0]] = ((int(a[1], 0) & 0xFFFF) << 16, i)
            except ValueError: pass
            continue
        if op in ('addi', 'ori') and len(a) == 3 and a[1] in lis:
            hi, at = lis[a[1]]
            try: v = int(a[2], 0)
            except ValueError: continue
            addr = (hi + v) & 0xFFFFFFFF if op == 'addi' else hi | (v & 0xFFFF)
            if lo_c <= addr <= hi_c and addr not in S and i - at < 60:
                found[addr] = [use_of(a[0], ins[i + 1:i + 25]), 'sub_%08X' % fn]
        if a and a[0] in lis and op != 'lis': lis.pop(a[0], None)


for f in sorted(glob.glob(gen + '/gow2_recomp.*.cpp')):
    fn, ins = None, []
    for line in open(f, errors='replace'):
        m = fre.match(line)
        if m:
            if fn is not None: scan(fn, ins)
            fn, ins = int(m.group(1), 16), []
            continue
        m = ire.match(line)
        if m: ins.append((m.group(1), [x.strip() for x in m.group(2).split(',')]))
    if fn is not None: scan(fn, ins)

try:
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    img = open(image, 'rb').read()
    def first(a): return '; '.join('%s %s' % (x.mnemonic, x.op_str) for x in list(md.disasm(img[a - 0x82000000:a - 0x81FFFFF0], a))[:3]) or '(not code)'
except ImportError:
    def first(a): return '(capstone missing)'

counts = collections.Counter()
for addr in sorted(found):
    use, built = found[addr]
    s = starts[bisect.bisect(starts, addr) - 1]
    gap = addr >= s + 4 * size.get(s, 0)
    counts[(use, 'gap' if gap else 'inside')] += 1
    # Candidates: in a gap (no code generated), passed on or stored, and decoding as code.
    if gap and use in ('stored', 'arg-to-call', 'overwritten', '?'):
        txt = first(addr)
        if txt != '(not code)' and not txt.startswith(('b 0x', 'lwz r22')):   # skip jump-table / scope-table words
            print('0x%08X = {}   # after sub_%08X, %s, built in %s; first: %s' % (addr, s, use, built, txt))
print('# summary:', dict(counts), file=sys.stderr)
