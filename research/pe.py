import struct,sys
class PE:
    def __init__(s,path):
        s.d=d=open(path,'rb').read()
        e=struct.unpack_from('<I',d,0x3c)[0]
        s.mach,s.nsec,s.ts,_,_,s.optsz,s.chars=struct.unpack_from('<HHIIIHH',d,e+4)
        o=e+24; s.opt=o
        s.magic=struct.unpack_from('<H',d,o)[0]
        s.entry=struct.unpack_from('<I',d,o+16)[0]
        s.base=struct.unpack_from('<I',d,o+28)[0]
        s.salign,s.falign=struct.unpack_from('<II',d,o+32)
        s.imgsize=struct.unpack_from('<I',d,o+56)[0]
        nd=struct.unpack_from('<I',d,o+92)[0]
        s.dirs=[struct.unpack_from('<II',d,o+96+8*i) for i in range(nd)]
        so=o+s.optsz; s.secs=[]
        for i in range(s.nsec):
            n,vs,va,rs,rp=struct.unpack_from('<8sIIII',d,so+40*i)
            ch=struct.unpack_from('<I',d,so+40*i+36)[0]
            s.secs.append((n.rstrip(b'\0').decode(),va,vs,rp,rs,ch))
    def off(s,rva):
        for n,va,vs,rp,rs,ch in s.secs:
            if va<=rva<va+max(vs,rs): return rva-va+rp
        return rva  # flat image
    def u32(s,rva,le=True): return struct.unpack_from('<I' if le else '>I',s.d,s.off(rva))[0]
    def dump(s):
        print('machine %x nsec %d chars %x magic %x base %08x entry %x imgsize %x salign %x falign %x'%(s.mach,s.nsec,s.chars,s.magic,s.base,s.entry,s.imgsize,s.salign,s.falign))
        for i,(a,b) in enumerate(s.dirs):
            if a or b: print(' dir%-2d %08x %x'%(i,a,b))
        for x in s.secs: print(' sec %-8s va %08x vs %08x raw %08x rs %08x ch %08x'%x)
if __name__=='__main__':
    for p in sys.argv[1:]: print(p); PE(p).dump()
