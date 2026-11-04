# Decode the header of an xcoreclr (Xbox 360) GC info blob, per GcInfoDecoder at 0x84EFB348.
import struct,sys
class R:
    def __init__(s,d,o): s.d=d; s.o=o*8
    def bit(s):
        b=(s.d[s.o>>3]>>(s.o&7))&1; s.o+=1; return b
    def read(s,n):
        v=0
        for i in range(n): v|=s.bit()<<i
        return v
    def vu(s,base):
        v=0; sh=0
        while True:
            c=s.read(base+1); v|=(c&((1<<base)-1))<<sh
            if not c>>base&1: return v
            sh+=base
    def vs(s,base):
        v=0; sh=0
        while True:
            c=s.read(base+1); v|=(c&((1<<base)-1))<<sh; sh+=base
            if not c>>base&1:
                if v>>(sh-1)&1: v-=1<<sh
                return v
def header(d,o):
    r=R(d,o); h={}
    f=r.read(7); h['flags']=f
    cl=r.vu(6); h['codelen']=cl<<2
    if f&6:
        h['prolog']=(r.vu(3)+1)*2; h['epilog_norm']=r.vu(3)
    if f&2: h['secobj']=r.vs(4)<<2
    if f&4: h['gscookie']=r.vs(4)<<2
    if f&8: h['pspsym']=r.vs(4)<<2
    if f&0x30: h['generics']=r.vs(4)<<2
    if f&0x40: h['stackbase']=r.vu(2)+0x1f
    h['outgoing']=r.vu(4)<<2
    h['safepoints']=r.vu(2); h['ranges']=r.vu(2); h['bitpos']=r.o-o*8
    return h
if __name__=='__main__':
    d=open(sys.argv[1],'rb').read()
    for a in sys.argv[2:]: print(a, header(d,int(a,16)))

def full(d,o):
    """Header, safe points (return-address offsets), ranges, slots [(kind, reg/base, offset, flags)], live bits."""
    r=R(d,o); h=header(d,o); r.o=o*8+h['bitpos']
    cl=h['codelen']; n=(cl//2-1).bit_length() if cl//2>1 else 0
    sp=[(r.read(n)+1)*2 for _ in range(h['safepoints'])]
    rg=[(r.vu(4),r.vu(4)) for _ in range(h['ranges'])]
    nreg=r.vu(4); nstk=r.vu(4); slots=[]; fl=None; reg=off=0
    for i in range(nreg):
        if i==0 or fl!=0: reg=r.vu(4); fl=r.read(2)
        else: reg=reg+r.vu(2)+1
        slots.append(('reg',reg,0,fl))
    for i in range(nstk):
        base=r.read(2)
        if i==0 or fl!=0: off=r.vs(4)<<2; fl=r.read(2)
        else: off=off+(r.vu(2)<<2)
        slots.append(('stk',base,off,fl))
    live=[[r.bit() for _ in slots] for _ in sp] if not rg else None
    return h,sp,rg,slots,live
