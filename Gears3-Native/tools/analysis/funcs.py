# Function list (start, end) from rexglue's generated code: starts from DEFINE_REX_FUNC; end = next start (approx).
import re,glob,os,sys,json
def load(gen_dir):
    starts=set()
    for fn in glob.glob(os.path.join(gen_dir,'gow*_recomp.*.cpp')):
        for m in re.finditer(r'DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)',open(fn,errors='replace').read()):
            starts.add(int(m.group(1),16))
    s=sorted(starts); return s
if __name__=='__main__':
    s=load(sys.argv[1]); json.dump(s,open(sys.argv[2],'w')); print(len(s))
