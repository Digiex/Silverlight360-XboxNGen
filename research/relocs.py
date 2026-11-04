import struct,sys,collections
d=open(sys.argv[1],'rb').read()
e=struct.unpack_from('<I',d,0x3c)[0]; nsec=struct.unpack_from('<H',d,e+6)[0]; optsz=struct.unpack_from('<H',d,e+20)[0]
secs=[struct.unpack_from('<8sIIII',d,e+24+optsz+40*i) for i in range(nsec)]
def off(r):
    for n,vs,va,rs,rp in secs:
        if va<=r<va+max(vs,rs): return r-va+rp
rva,size=struct.unpack_from('<II',d,e+24+96+5*8)
o=off(rva); end=o+size; c=collections.Counter(); ex={}
while o<end:
    page,bs=struct.unpack_from('<II',d,o)
    if bs==0: break
    i=0; n=(bs-8)//2
    while i<n:
        w=struct.unpack_from('<H',d,o+8+2*i)[0]; t=w>>12; c[t]+=1; ex.setdefault(t,[]).append(page+(w&0xfff))
        if t==4: i+=1  # HIGHADJ: the next entry holds the low 16 bits
        i+=1
    o+=bs
print(dict(c))
for t,l in ex.items(): print(t,[hex(x) for x in l[:6]])
