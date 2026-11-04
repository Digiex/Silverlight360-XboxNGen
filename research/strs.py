import sys,re
d=open(sys.argv[1],'rb').read()
pat=re.compile(sys.argv[2],re.I) if len(sys.argv)>2 else None
out=[]
for m in re.finditer(rb'[\x20-\x7e]{5,}',d): out.append((m.start(),'a',m.group().decode()))
for m in re.finditer(rb'(?:[\x20-\x7e]\x00){5,}',d): out.append((m.start(),'u',m.group().decode('utf-16le')))
for m in re.finditer(rb'(?:\x00[\x20-\x7e]){5,}',d): out.append((m.start(),'U',m.group().decode('utf-16be')))
out.sort()
for o,k,s in out:
    if pat is None or pat.search(s): print('%08x %s %s'%(o,k,s))
