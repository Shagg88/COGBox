import sys,struct,os,hashlib
d=open(sys.argv[1],'rb').read(); out=sys.argv[2]
hs=struct.unpack('>I',d[0x340:0x344])[0]; base=(hs+0xFFF)&~0xFFF
sep=d[0x37B]; ftc=struct.unpack('<H',d[0x37C:0x37E])[0]; ftb=int.from_bytes(d[0x37E:0x381],'little')
def b2o(block,male):
    base_=(block+0xAA)//0xAA
    if male: base_<<=1
    n=base_+block
    if block>=0xAA:
        b2=(block+0x70E4)//0x70E4
        if male: b2<<=1
        n+=b2
        if block>=0x70E4:
            b3=(block+0x4AF768)//0x4AF768
            if male: b3<<=1
            n+=b3
    return base+n*0x1000
print('header',hex(hs),'sep',sep,'ft blocks',ftc,'at',ftb)
for male in (0,1):
    ents=[]
    for i in range(ftc):
        o=b2o(ftb+i,male)
        for e in range(64):
            r=d[o+e*64:o+e*64+64]
            if len(r)<64 or r[0]==0: continue
            nl=r[0x28]&0x3F; fl=r[0x28]
            ents.append((r[:nl].decode('latin1'),fl,int.from_bytes(r[0x2F:0x32],'little'),struct.unpack('>h',r[0x32:0x34])[0],struct.unpack('>I',r[0x34:0x38])[0]))
    if ents and all(e[0].isprintable() for e in ents):
        print('male' if male else 'female')
        for i,e in enumerate(ents):
            print(i,e[0],'dir' if e[1]&0x80 else '', 'contig' if e[1]&0x40 else 'frag','start',e[2],'parent',e[3],'size',e[4])
            def path(k):
                p=[]
                while k!=-1 and k!=0xFFFF: p.insert(0,ents[k][0]); k=ents[k][3]
                return os.path.join(out,*p)
            if e[1]&0x80: os.makedirs(path(i),exist_ok=True)
            elif e[1]&0x40:
                data=b''.join(d[b2o(e[2]+k,male):b2o(e[2]+k,male)+0x1000] for k in range((e[4]+0xFFF)//0x1000))[:e[4]]
                os.makedirs(os.path.dirname(path(i)),exist_ok=True)
                open(path(i),'wb').write(data)
            else: print('  FRAGMENTED - not extracted:',e[0])
        break
