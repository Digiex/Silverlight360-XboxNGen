import sys,struct,bisect,capstone
img=open(sys.argv[1],'rb').read(); syms=[l.split(None,2) for l in open(sys.argv[2])]
addrs=[int(a,16) for a,_,_ in syms]; names=[n.strip() for _,_,n in syms]
e=struct.unpack_from('<I',img,0x3c)[0]; nsec=struct.unpack_from('<H',img,e+6)[0]; optsz=struct.unpack_from('<H',img,e+20)[0]
base=struct.unpack_from('<I',img,e+24+28)[0]
secs=[struct.unpack_from('<IIII',img,e+24+optsz+40*i+8) for i in range(nsec)]
def off(va):
    r=va-base
    for vs,v,rs,rp in secs:
        if v<=r<v+vs: return r-v+rp
def name(a):
    j=bisect.bisect_left(addrs,a)
    return names[j] if j<len(addrs) and addrs[j]==a else None
start=int(sys.argv[3],16); n=int(sys.argv[4],16)
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
o=off(start)
for ins in md.disasm(img[o:o+n],start):
    s='%08x: %-8s %s'%(ins.address,ins.mnemonic,ins.op_str)
    import re
    for m in re.findall(r'0x[0-9a-f]{8}',ins.op_str):
        nm=name(int(m,16))
        if nm: s+='   ; '+nm
    print(s)
