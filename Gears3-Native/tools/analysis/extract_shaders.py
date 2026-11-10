# Extract Xbox 360 compiled shaders (D3D shader containers, flags 0x102A1100 pixel / 0x102A1101 vertex) from any file,
# e.g. UE3's GlobalShaderCache-Xbox360.bin or decompressed packages. Container layout per XenosRecomp shader.h (MIT):
# header {flags, virtualSize, physicalSize, fieldC, constantTableOffset, definitionTableOffset, shaderOffset, ...};
# the Shader record at +shaderOffset gives {physicalOffset, size}; microcode = container + virtualSize + physicalOffset.
# Output (same naming as Gears 2's --gow2_shader_dump_dir): <out>/<vs|ps>-<XXH3-64 of big-endian bytes>.ucode with
# host-order (little-endian) dwords, so tools/gn_disasm_check and gn_hlsl_check can read them. Game-derived output:
# keep it under out\ and never commit it.
import sys,struct,os,xxhash
def extract(data,out,stats):
    pos=0
    while True:
        i=data.find(b'\x10\x2a\x11',pos)
        if i<0: break
        pos=i+1
        if i+0x24>len(data) or data[i+3] not in (0,1): continue
        flags,vsz,psz,_,cto,dto,so=struct.unpack('>7I',data[i:i+28])
        if not (0x24<=so<vsz<=0x100000 and 0<psz<=0x100000 and i+vsz+psz<=len(data)): stats['bad']+=1; continue
        poff,size=struct.unpack('>II',data[i+so:i+so+8])
        if size==0 or size%4 or poff+size>psz: stats['bad']+=1; continue
        be=data[i+vsz+poff:i+vsz+poff+size]
        h=xxhash.xxh3_64_intdigest(be)
        kind='vs' if data[i+3]==1 else 'ps'
        name=f"{kind}-{h:016X}"
        p=os.path.join(out,name+'.ucode')
        if os.path.exists(p): stats['dup']+=1; continue
        le=b''.join(struct.pack('<I',w) for w in struct.unpack(f'>{size//4}I',be))
        open(p,'wb').write(le); stats[kind]+=1
if __name__=='__main__':
    out=sys.argv[1]; os.makedirs(out,exist_ok=True)
    stats={'vs':0,'ps':0,'dup':0,'bad':0}
    for f in sys.argv[2:]: extract(open(f,'rb').read(),out,stats)
    print(stats)
