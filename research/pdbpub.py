# Dump public symbols (S_PUB32) of an MSF 7.00 PDB as "rva name", using the image's section table.
import struct,sys
pdb=open(sys.argv[1],'rb').read(); img=open(sys.argv[2],'rb').read()
bs,fpm,nblocks,dirsize,_,dirmap=struct.unpack_from('<IIIIII',pdb,32)
def blocks_data(blist,size): return b''.join(pdb[b*bs:(b+1)*bs] for b in blist)[:size]
ndirblocks=(dirsize+bs-1)//bs
dirblocks=struct.unpack_from('<%dI'%ndirblocks,pdb,dirmap*bs)
dir_=blocks_data(dirblocks,dirsize)
ns=struct.unpack_from('<I',dir_,0)[0]
sizes=struct.unpack_from('<%dI'%ns,dir_,4); o=4+4*ns; streams=[]
for s in sizes:
    if s==0xffffffff: s=0
    n=(s+bs-1)//bs; bl=struct.unpack_from('<%dI'%n,dir_,o); o+=4*n; streams.append((bl,s))
def stream(i): return blocks_data(*streams[i])
dbi=stream(3)
symrec=struct.unpack_from('<H',dbi,20)[0]
syms=stream(symrec)
e=struct.unpack_from('<I',img,0x3c)[0]; nsec=struct.unpack_from('<H',img,e+6)[0]; optsz=struct.unpack_from('<H',img,e+20)[0]
magic=struct.unpack_from('<H',img,e+24)[0]
base=struct.unpack_from('<Q',img,e+24+24)[0] if magic==0x20b else struct.unpack_from('<I',img,e+24+28)[0]
secs=[struct.unpack_from('<I',img,e+24+optsz+40*i+12)[0] for i in range(nsec)]
# Prefer the PDB's section headers (DBI optional debug header, entry 5): segments in public symbols index them.
modsz,secconsz,secmapsz,srcsz,tsmsz,_,dbgsz,ecsz=struct.unpack_from('<iiiiiiii',dbi,24)
dbgoff=64+modsz+secconsz+secmapsz+srcsz+tsmsz+ecsz
if dbgsz>=12:
    shs=struct.unpack_from('<H',dbi,dbgoff+10)[0]
    if shs!=0xffff:
        sh=stream(shs); secs=[struct.unpack_from('<I',sh,40*i+12)[0] for i in range(len(sh)//40)]
omap=None
if dbgsz>=10:
    oms=struct.unpack_from('<H',dbi,dbgoff+8)[0]   # OmapFromSrc
    if oms!=0xffff:
        om=stream(oms); omap=[struct.unpack_from('<II',om,8*i) for i in range(len(om)//8)]
        hs=struct.unpack_from('<H',dbi,dbgoff+20)[0]  # SectionHdrOrig: the pre-optimization sections
        if hs!=0xffff:
            sh=stream(hs); secs=[struct.unpack_from('<I',sh,40*i+12)[0] for i in range(len(sh)//40)]
import bisect
omsrc=[a for a,_ in omap] if omap else None
def mapr(r):
    if not omap: return r
    i=bisect.bisect_right(omsrc,r)-1
    if i<0 or omap[i][1]==0: return None
    return omap[i][1]+(r-omap[i][0])
o=0
while o+4<=len(syms):
    ln,typ=struct.unpack_from('<HH',syms,o)
    if typ==0x110e:
        fl,off,seg=struct.unpack_from('<IIH',syms,o+4)
        name=syms[o+14:syms.index(b'\0',o+14)].decode('latin1')
        if 1<=seg<=len(secs):
            r=mapr(secs[seg-1]+off)
            if r is not None: print('%08x %d %s'%(base+r,fl,name))
    o+=ln+2
