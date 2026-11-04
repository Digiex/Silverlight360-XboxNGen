import sys,re
src=open(sys.argv[1],encoding='utf8',errors='ignore').read().split('\n')
cur=None; res={}
for l in src:
    if l.startswith('//==== '): cur=l.split()[1]; continue
    for t in sys.argv[2:]:
        if ('FUN_'+t+'(' in l or '_'+t in l or '0x'+t in l or 'ffffffff'+t in l) and cur!=t: res.setdefault(t,set()).add(cur)
for t in sys.argv[2:]: print(t,'<-',sorted(res.get(t,[])))
