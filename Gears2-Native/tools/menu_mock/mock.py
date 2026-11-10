import sys
from PIL import Image
raw,w,h,bg,out=sys.argv[1],int(sys.argv[2]),int(sys.argv[3]),sys.argv[4],sys.argv[5]
o=Image.frombytes('RGBA',(w,h),open(raw,'rb').read())
b=Image.open(bg).convert('RGBA'); s=max(w/b.width,h/b.height); b=b.resize((int(b.width*s+1),int(b.height*s+1)),Image.LANCZOS)
b=b.crop(((b.width-w)//2,(b.height-h)//2,(b.width-w)//2+w,(b.height-h)//2+h)); Image.alpha_composite(b,o).convert('RGB').save(out)
