import sys
d=open(sys.argv[1],'rb').read(); o=int(sys.argv[2],16); n=int(sys.argv[3],16)
for i in range(o,o+n,16):
    b=d[i:i+16]; print('%08x  %-48s %s'%(i,b.hex(' '),''.join(chr(c) if 32<=c<127 else '.' for c in b)))
