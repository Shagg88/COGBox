"""Gears2-Native: propose recompiler function hints from a failed codegen/build.

Reads out/codegen.log, out/build.log and generated/default/*.cpp, writes proposals to stdout.
Usage: find_hints.py [root] [tu6]   (tu6: out/codegen-tu6.log, out/build-tu6.log, generated/tu6 - notes/63)
Three patterns, each reported with its evidence; a human (Claude) checks them before they go into the manifest:
  1. "Unresolved b target T from S": T is a tail chunk of the function containing S -> T = { parent = P }.
  2. "use of undeclared label 'loc_X'": the enclosing function F runs past a no-return call into the next
     catch funclet. The next funclet starts at the first 'addi r31,r12,...' after that call:
     F = { end = N }, N = {}.
"""
import bisect, glob, os, re, sys

root = sys.argv[1] if len(sys.argv) > 1 else '.'
ver = sys.argv[2] if len(sys.argv) > 2 else 'base'
sfx = '-tu6' if ver == 'tu6' else ''
gen_dir = os.path.join(root, 'generated', 'tu6' if ver == 'tu6' else 'default')

def read(p):
    for enc in ('utf-8-sig', 'utf-16'):
        try:
            with open(p, encoding=enc) as f:
                return f.read()
        except (UnicodeError, FileNotFoundError):
            continue
    return ''

func_re = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)', re.M)
files = sorted(glob.glob(os.path.join(gen_dir, '*.cpp')))
starts, texts = [], {}
for f in files:
    t = read(f)
    texts[f] = t
    starts += [int(m.group(1), 16) for m in func_re.finditer(t)]
starts = sorted(set(starts))
print(f'functions in generated code: {len(starts)} in {len(files)} files')

def containing(addr):
    i = bisect.bisect_right(starts, addr) - 1
    return starts[i] if i >= 0 else None

seen = set()
print('\n# 1. tail chunks (codegen: Unresolved b target)')
for m in re.finditer(r'Unresolved b target 0x([0-9A-F]{8}) from 0x([0-9A-F]{8})', read(os.path.join(root, 'out', 'codegen%s.log' % sfx))):
    t, s = int(m.group(1), 16), int(m.group(2), 16)
    if t in seen:
        continue
    seen.add(t)
    p = containing(s)
    print(f'0x{t:08X} = {{ parent = 0x{p:08X} }}   # b from 0x{s:08X}, inside sub_{p:08X}')

print('\n# 2. functions that run into the next funclet (build: undeclared label)')
inst_re = re.compile(r'^\t// (\S+) ?(.*)$', re.M)
done = set()
for m in re.finditer(r"([A-Za-z]:[^:\n]*?gow2_recomp\.\d+\.cpp):(\d+):\d+: error: use of undeclared label 'loc_([0-9A-F]{8})'", read(os.path.join(root, 'out', 'build%s.log' % sfx))):
    path, line, label = m.group(1), int(m.group(2)), m.group(3)
    f = os.path.join(gen_dir, os.path.basename(path))
    t = texts.get(f) or read(f)
    lines = t.split('\n')
    k = line - 1
    while k >= 0 and not lines[k].startswith('DEFINE_REX_FUNC('):
        k -= 1
    fstart = int(lines[k][len('DEFINE_REX_FUNC(sub_'):][:8], 16)
    if fstart in done:
        continue
    done.add(fstart)
    end = k
    while end < len(lines) and lines[end] != '}':
        end += 1
    body = '\n'.join(lines[k:end])
    found = None
    # The function's own prologue is the first 'addi r31,r12,'; the second one starts the next funclet.
    pro = [im for im in inst_re.finditer(body) if im.group(1) == 'addi' and im.group(2).startswith('r31,r12,')]
    if len(pro) >= 2:
        cut = pro[1].start()
        calls = list(re.finditer(r'ctx\.lr = 0x([0-9A-F]{8});\n\tsub_([0-9A-F]{8})\(ctx, base\);', body[:cut]))
        if calls:
            c = calls[-1]
            n = len(inst_re.findall(body[c.end():cut]))
            found = (int(c.group(1), 16) + 4 * n, c.group(2), n)
    if found:
        nxt, callee, junk = found
        print(f'0x{fstart:08X} = {{ end = 0x{nxt:08X} }}   # runs past no-return call to sub_{callee} ({junk} data words), label loc_{label}')
        print(f'0x{nxt:08X} = {{}}   # next catch funclet (addi r31,r12,...)')
    else:
        print(f'# sub_{fstart:08X}: label loc_{label} undeclared, pattern not recognised - needs a manual look')

print('\n# 3. functions the running game called but discovery missed (latest out/runs/*/gow2.log)')
runs = sorted(glob.glob(os.path.join(root, 'out', 'runs', '*', 'gow2.log')))
if runs:
    for m in sorted(set(re.findall(r'unregistered function at guest address 0x([0-9A-Fa-f]{8})', read(runs[-1])))):
        a = int(m, 16)
        p = containing(a)
        where = f'inside sub_{p:08X} (+0x{a - p:X})' if p is not None else 'before the first function'
        print(f'0x{a:08X} = {{}}   # {os.path.basename(os.path.dirname(runs[-1]))}: {where}')
