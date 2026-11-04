"""trace_names.py <package> <xenia.log> <out.txt>: name every 'Defined guest function' address of a trace that
lies in one of the package's .ni.dll images (unwrapped into <package>\\unwrapped), one 'image method' line each in
trace order; addresses outside the images are written as '? address'."""
import bisect
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from nicompare import Image  # noqa: E402
from xex import memory_image  # noqa: E402

package, log, out = sys.argv[1:4]
out_dir = os.path.join(package, 'unwrapped')
os.makedirs(out_dir, exist_ok=True)
images = []
for ni in glob.glob(os.path.join(package, '*.ni.dll')):
    name = os.path.basename(ni)[:-7]
    base, mem = memory_image(open(ni, 'rb').read())
    pe = os.path.join(out_dir, name + '.ni.pe')
    open(pe, 'wb').write(mem)
    il = os.path.join(package, name + '.dll')
    aliases = {'SW.Controls.Navigation': 'System.Windows.Controls.Navigation', 'MWM.SmoothStreaming': 'Microsoft.Web.Media.SmoothStreaming'}
    if not os.path.exists(il):
        il = os.path.join(package, aliases.get(name, name) + '.dll')
    names = {}
    if os.path.exists(il):
        for line in subprocess.run([sys.executable, os.path.join(HERE, 'methodnames.py'), il],
                                   capture_output=True, text=True).stdout.splitlines():
            parts = line.split()
            if len(parts) >= 2:
                names[int(parts[0], 16) & 0xFFFFFF] = parts[1]
    img = Image(pe, base)
    mains = sorted((m[0], m[1], f) for m, f in img.methods())
    images.append((base, base + len(mem), name, img, names, mains, [m[0] for m in mains]))

rx = re.compile(r'Defined guest function ([0-9A-F]{8})')
with open(out, 'w', encoding='utf-8') as o:
    for line in open(log, encoding='utf-8', errors='replace'):
        m = rx.search(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        for base, end, name, img, names, mains, starts in images:
            if base <= addr < end:
                rva = addr - base
                i = bisect.bisect_right(starts, rva) - 1
                start, words, funclets = mains[i]
                inside = start <= rva < start + words * 4
                try:
                    rid = img.method_rid(start)
                except Exception:  # noqa: BLE001
                    rid = None
                o.write('%s %s%s\n' % (name, names.get(rid) if inside and rid else 'stub', '' if inside else '@%x' % rva))
                break
        else:
            o.write('? %08X\n' % addr)
print('ok')
