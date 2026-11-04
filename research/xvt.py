# Resolve xcoreclr ZapInfo vtable slots through adjustor thunks to their implementations.
import struct,sys
d=open('xcoreclr.dll.pe','rb').read(); B=0x84aa0000
def w(a): return struct.unpack_from('>I',d,a-B)[0]
def resolve(a):
    for _ in range(3):
        i0,i1,i2=w(a),w(a+4),w(a+8)
        if i0==0x8163fffc and i1==0x7c6b1850 and (i2>>26)==18:   # lwz r11,-4(r3); subf r3,r11,r3; b
            off=i2&0x3fffffc; off=off-0x4000000 if off&0x2000000 else off; a=a+8+off; continue
        if (i0>>16)==0x3863 and (i1>>26)==18:                    # addi r3,r3,imm; b
            off=i1&0x3fffffc; off=off-0x4000000 if off&0x2000000 else off; a=a+4+off; continue
        if (i0>>26)==18 and not (i0&1):
            off=i0&0x3fffffc; off=off-0x4000000 if off&0x2000000 else off; a=a+off; continue
        break
    return a
vts={'JitInfo':(0x84acb3a0,17),'MethodInfo':(0x84acb3e8,28),'ModuleInfo':(0x84acb458,13),'ClassInfo':(0x84acb490,42),
     'FieldInfo':(0x84acb538,7),'DebugInfo':(0x84acb554,6),'ArgInfo':(0x84acb56c,3),'ErrorInfo':(0x84acb578,6),
     'StaticInfo':(0x84acb590,1),'DynamicInfo':(0x84acb598,38)}
for n,(s,c) in vts.items():
    print(n,' '.join('%d:%08x'%(i,resolve(w(s+4*i))) for i in range(c)))
