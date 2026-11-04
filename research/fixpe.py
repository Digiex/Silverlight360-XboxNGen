# Make a memory image (xex.py -b) a valid PE: raw offsets = RVAs.
import struct,sys
src,dst=sys.argv[1],sys.argv[2]
d=bytearray(open(src,'rb').read())
e=struct.unpack_from('<I',d,0x3c)[0]
nsec=struct.unpack_from('<H',d,e+6)[0]; optsz=struct.unpack_from('<H',d,e+20)[0]
imgsize=struct.unpack_from('<I',d,e+24+56)[0]
if len(d)<imgsize: d+=bytes(imgsize-len(d))
struct.pack_into('<I',d,e+24+36,0x200)
so=e+24+optsz
for i in range(nsec):
    o=so+40*i; vs,va,rs,rp=struct.unpack_from('<IIII',d,o+8)
    size=max(vs,rs); size=min(size,len(d)-va)
    struct.pack_into('<II',d,o+16,size,va)
open(dst,'wb').write(d)
