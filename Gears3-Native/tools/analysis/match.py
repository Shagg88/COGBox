# Find the Gears 3 function that best matches a Gears 2 function (same UE3 engine code, new addresses).
# Usage: match.py <g2addr> [<g2addr> ...]  -> top candidates in Gears 3 with similarity, and the matching offset
# of the given address inside the function.
import json,bisect,sys,re,difflib
from capstone import Cs,CS_ARCH_PPC,CS_MODE_32,CS_MODE_BIG_ENDIAN
md=Cs(CS_ARCH_PPC,CS_MODE_32|CS_MODE_BIG_ENDIAN)
T='/root/g3tools/'
G={'g2':(open(T+'g2image.bin','rb').read(),json.load(open(T+'g2funcs.json'))),
   'g3':(open(T+'image.bin','rb').read(),json.load(open(T+'g3funcs.json')))}
BASE=0x82000000
def func_of(g,a):
    fs=G[g][1]; i=bisect.bisect_right(fs,a)-1; return fs[i], (fs[i+1] if i+1<len(fs) else fs[i]+0x1000)
def dis(g,start,end):
    img=G[g][0]; code=img[start-BASE:end-BASE]; out=[]
    for off in range(0,len(code),4):
        ins=list(md.disasm(code[off:off+4],start+off))
        if ins: out.append((start+off,ins[0].mnemonic,ins[0].op_str))
        else: out.append((start+off,'?',code[off:off+4].hex()))
    return out
def norm(m,ops):
    ops=re.sub(r'0x[0-9a-f]+|-?\d+','N',ops)
    return m+' '+ops
cache={}
def toks(g,s,e):
    k=(g,s)
    if k not in cache: cache[k]=[norm(m,o) for _,m,o in dis(g,s,e)]
    return cache[k]
def grams(t): return set(zip(t,t[1:],t[2:]))
def best(g2a, top=5):
    s,e=func_of('g2',g2a); t=toks('g2',s,e); n=len(t); gr=grams(t)
    cands=[]
    fs=G['g3'][1]
    for i,fs3 in enumerate(fs):
        e3=fs[i+1] if i+1<len(fs) else fs3+0x1000; n3=(e3-fs3)//4
        if n3<n*0.6 or n3>n*1.6 or n3>20000: continue
        t3=toks('g3',fs3,e3); j=len(gr & grams(t3))/max(1,len(gr|grams(t3)))
        if j>0.2: cands.append((j,fs3,e3))
    cands.sort(reverse=True); res=[]
    for j,fs3,e3 in cands[:top]:
        t3=toks('g3',fs3,e3); r=difflib.SequenceMatcher(None,t,t3,autojunk=False).ratio()
        # map the offset of g2a inside the function through the alignment
        idx=(g2a-s)//4; mapped=None
        for blk in difflib.SequenceMatcher(None,t,t3,autojunk=False).get_matching_blocks():
            if blk.a<=idx<blk.a+blk.size: mapped=fs3+4*(blk.b+idx-blk.a); break
        res.append((round(r,3),round(j,3),hex(fs3),n,(e3-fs3)//4,hex(mapped) if mapped else None))
    return (hex(s),n),sorted(res,reverse=True)
if __name__=='__main__':
    for a in sys.argv[1:]:
        a=int(a,16); f,res=best(a)
        ins=[x for x in dis('g2',a,a+4)][0]
        print(f"G2 {a:08X} ({ins[1]} {ins[2]}) in func {f[0]} ({f[1]} ins):")
        for r in res: print('   ratio',r[0],'jacc',r[1],'G3 func',r[2],f'({r[4]} ins)','-> site',r[5])
