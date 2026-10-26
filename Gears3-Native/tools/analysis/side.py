# Print an aligned view of a Gears 2 function and its matched Gears 3 function around a site.
import sys,difflib
sys.path.insert(0,'/root/g3tools')
from match import dis,func_of,norm
a2=int(sys.argv[1],16); f3=int(sys.argv[2],16); w=int(sys.argv[3]) if len(sys.argv)>3 else 12
s2,e2=func_of('g2',a2); s3,e3=func_of('g3',f3)
d2=dis('g2',s2,e2); d3=dis('g3',s3,e3)
t2=[norm(m,o) for _,m,o in d2]; t3=[norm(m,o) for _,m,o in d3]
sm=difflib.SequenceMatcher(None,t2,t3,autojunk=False)
idx=(a2-s2)//4
for tag,i1,i2,j1,j2 in sm.get_opcodes():
    if i2<idx-w or i1>idx+w: continue
    for k in range(max(i2-i1,j2-j1)):
        L=d2[i1+k] if i1+k<i2 else None; R=d3[j1+k] if j1+k<j2 else None
        if L and not (idx-w<=i1+k<=idx+w): continue
        ls=f"{L[0]:08X} {L[1]} {L[2]}" if L else ''; rs=f"{R[0]:08X} {R[1]} {R[2]}" if R else ''
        mark='>>' if L and L[0]==a2 else '  '
        print(f"{mark}{tag[:3]} {ls:<44} | {rs}")
