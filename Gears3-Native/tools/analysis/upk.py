# Minimal reader for Gears 3's decompressed UE3 packages (version 828, big-endian): names, imports, exports.
import struct, sys
class Upk:
    def __init__(s, path):
        d = s.d = open(path, 'rb').read()
        o = 12; n, = struct.unpack('>i', d[o:o+4]); o += 4 + (n if n > 0 else -2*n)
        (s.pflags, s.nname, s.oname, s.nexp, s.oexp, s.nimp, s.oimp) = struct.unpack('>7I', d[o:o+28])
        s.names = []; o = s.oname
        for _ in range(s.nname):
            n, = struct.unpack('>i', d[o:o+4]); o += 4
            if n >= 0: t = d[o:o+n-1].decode('latin1'); o += n
            else: t = d[o:o-2*n-2].decode('utf-16-be'); o += -2*n
            s.names.append(t); o += 8
        s.imps = []; o = s.oimp
        for _ in range(s.nimp):
            pk, pn, cl, cn, outer, nm, num = struct.unpack('>iiiiiii', d[o:o+28]); o += 28
            s.imps.append((s.names[cn], outer, s.names[nm]))
        s.exps = []; o = s.oexp
        for _ in range(s.nexp):
            cls, sup, outer, nm, num, arch = struct.unpack('>iiiiii', d[o:o+24]); o += 24
            oflags, = struct.unpack('>Q', d[o:o+8]); o += 8
            size, off, eflags, nnet = struct.unpack('>iiIi', d[o:o+16]); o += 16 + 4*nnet + 16 + 4
            s.exps.append(dict(cls=cls, sup=sup, outer=outer, name=s.names[nm], num=num, size=size, off=off))
    def clsname(s, i):
        if i == 0: return 'Class'
        return s.exps[i-1]['name'] if i > 0 else s.imps[-i-1][2]
    def objname(s, i):
        if i == 0: return 'None'
        return s.exps[i-1]['name'] if i > 0 else s.imps[-i-1][2]
    def path(s, i):
        parts = []
        while i:
            if i > 0: e = s.exps[i-1]; parts.append(e['name']); i = e['outer']
            else: im = s.imps[-i-1]; parts.append(im[2]); i = im[1]
        return '.'.join(reversed(parts))
