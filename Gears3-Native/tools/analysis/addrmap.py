# Build an address -> PPC instruction map from rexglue's generated code (every instruction is emitted as a
# "\t// <asm>" comment; loc_ labels and function starts re-anchor the address). Output: addr<TAB>asm<TAB>func.
import re,sys,glob,os
out=open(sys.argv[2],'w'); mism=0
for fn in sorted(glob.glob(os.path.join(sys.argv[1],'gow*_recomp.*.cpp'))):
    addr=None; func=None
    for line in open(fn,errors='replace'):
        m=re.match(r'DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)',line)
        if m: addr=int(m.group(1),16); func=m.group(1); continue
        m=re.match(r'loc_([0-9A-F]{8}):',line)
        if m:
            a=int(m.group(1),16)
            if addr is not None and a!=addr: mism+=1
            addr=a; continue
        if addr is None: continue
        m=re.match(r'\t// (\S.*)$',line)
        if m and not m.group(1).startswith('...'):
            out.write(f"{addr:08X}\t{m.group(1)}\t{func}\n"); addr+=4
print('label mismatches',mism)
