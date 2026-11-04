"""names_in_package.py [--package <dir>] <addr>...: name each guest address across a package's .ni.dll images
(NGenBench's by default), which it unwraps into <package>\\unwrapped."""
import bisect
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from nicompare import Image  # noqa: E402
from xex import memory_image  # noqa: E402

package = os.path.join(HERE, '..', 'test', 'NGenBench', 'bin', 'Release', 'xbox')
if sys.argv[1:] and sys.argv[1] == '--package':
    package = sys.argv[2]
    del sys.argv[1:3]
out_dir = os.path.join(package, 'unwrapped')
os.makedirs(out_dir, exist_ok=True)
images = []
for ni in glob.glob(os.path.join(package, '*.ni.dll')):
    name = os.path.basename(ni)[:-7]
    base, mem = memory_image(open(ni, 'rb').read())
    pe = os.path.join(out_dir, name + '.ni.pe')
    open(pe, 'wb').write(mem)
    il = os.path.join(package, name + '.dll')
    names = {}
    if os.path.exists(il):
        for line in subprocess.run([sys.executable, os.path.join(HERE, 'methodnames.py'), il],
                                   capture_output=True, text=True).stdout.splitlines():
            parts = line.split()
            if len(parts) >= 2:
                names[int(parts[0], 16) & 0xFFFFFF] = parts[1]
    img = Image(pe, base)
    mains = sorted((m[0], m[1], f) for m, f in img.methods())
    images.append((base, base + len(mem), name, img, names, mains, [m[0] for m in mains], pe))

for a in sys.argv[1:]:
    addr = int(a, 16)
    for base, end, name, img, names, mains, starts, pe in images:
        if base <= addr < end:
            rva = addr - base
            i = bisect.bisect_right(starts, rva) - 1
            start, words, funclets = mains[i]
            print('%s: %s %s body %08X +%x (%d words) [%s]' % (a, name, names.get(img.method_rid(start)), base + start, rva - start, words, pe))
            break
    else:
        print(a, '?')
