# Find code that references a global address (lis rX,hi + d(rX)/addi rX,lo within a short window).
import sys,json
sys.path.insert(0,'/root/g3tools')
from match import G,dis,func_of,BASE
from capstone import Cs,CS_ARCH_PPC,CS_MODE_32,CS_MODE_BIG_ENDIAN
import re
def xrefs(g,target,limit=200):
    img=G[g][0]; fs=G[g][1]; hi=(target+0x8000)>>16; lo=target-(hi<<16)
    out=[]
    # scan text for lis with hi
    import struct
    for i in range(0,len(img)-4,4):
        w=struct.unpack('>I',img[i:i+4])[0]
        if (w>>26)==15 and (w&0xFFFF)==(hi&0xFFFF) and ((w>>16)&31)==0:  # lis rD,hi (addis rD,0,hi)
            rd=(w>>21)&31
            for k in range(1,12):
                if i+4*k+4>len(img): break
                w2=struct.unpack('>I',img[i+4*k:i+4*k+4])[0]; op=w2>>26; ra=(w2>>16)&31; d=w2&0xFFFF
                if d>=0x8000: d-=0x10000
                if ra==rd and op in (14,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55) and d==lo:
                    a=BASE+i+4*k; out.append(a); break
    return out
if __name__=='__main__':
    g=sys.argv[1]; t=int(sys.argv[2],16)
    for a in xrefs(g,t):
        f=func_of(g,a)[0]; ins=dis(g,a,a+4)[0]
        print(f"{a:08X} in {f:08X}: {ins[1]} {ins[2]}")
