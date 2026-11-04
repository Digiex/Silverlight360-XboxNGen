import struct,sys,bisect
img=open(sys.argv[1],'rb').read(); syms=[l.split(None,2) for l in open(sys.argv[2])]
addrs=[int(a,16) for a,_,_ in syms]; names=[n.strip() for _,_,n in syms]
e=struct.unpack_from('<I',img,0x3c)[0]; nsec=struct.unpack_from('<H',img,e+6)[0]; optsz=struct.unpack_from('<H',img,e+20)[0]
base=struct.unpack_from('<I',img,e+24+28)[0]
secs=[struct.unpack_from('<IIII',img,e+24+optsz+40*i+8) for i in range(nsec)]
def off(va):
    r=va-base
    for vs,v,rs,rp in secs:
        if v<=r<v+vs: return r-v+rp
vt=int(sys.argv[3],16); n=int(sys.argv[4]) if len(sys.argv)>4 else 400
for i in range(n):
    p=struct.unpack_from('<I',img,off(vt+4*i))[0]
    j=bisect.bisect_right(addrs,p)-1
    nm=names[j] if j>=0 and addrs[j]==p else ('?? %08x'%p)
    if i>0 and addrs[bisect.bisect_left(addrs,vt+4*i)]==vt+4*i and names[bisect.bisect_left(addrs,vt+4*i)].startswith('??_7'): break
    if not (base<=p<base+0x400000): break
    print('%3d 0x%03x %s'%(i,4*i,nm))
