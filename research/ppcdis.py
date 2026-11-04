import sys,capstone
d=open(sys.argv[1],'rb').read(); base=int(sys.argv[4],16) if len(sys.argv)>4 else 0
o=int(sys.argv[2],16); n=int(sys.argv[3],16)
md=capstone.Cs(capstone.CS_ARCH_PPC,capstone.CS_MODE_64|capstone.CS_MODE_BIG_ENDIAN)
i=o
while i<o+n:
    w=d[i:i+4]; got=list(md.disasm(w,base+i))
    print('%08x: %s  %s'%(base+i,w.hex(),(got[0].mnemonic+' '+got[0].op_str) if got else '.long 0x'+w.hex()))
    i+=4
