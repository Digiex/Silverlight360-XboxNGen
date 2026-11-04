"""Disassembles the bodies of one method in a native image (a memory image from xex.py -b).

    python nishow.py image.pe base methods.txt Type::Method

methods.txt is methodnames.py's listing for the assembly; bodies are found by MethodDef row as nicompare.py pairs
them (a generic method's instantiations each have a body of the same row). Needs capstone, as ppcdis.py does.
"""
import sys

import capstone

from nicompare import Image


def main():
    path, base, names_path, wanted = sys.argv[1], int(sys.argv[2], 16), sys.argv[3], sys.argv[4]
    rows = set()
    for line in open(names_path, encoding='utf-8-sig'):
        parts = line.split()
        if len(parts) >= 2 and parts[1] == wanted:
            rows.add(int(parts[0], 16) & 0xFFFFFF)
    if not rows:
        sys.exit('no method %s in %s' % (wanted, names_path))
    img = Image(path, base)
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_64 | capstone.CS_MODE_BIG_ENDIAN)
    for main_fn, funclets in img.methods():
        if img.method_rid(main_fn[0]) not in rows:
            continue
        for rva, words, prolog, is_funclet, _ in [main_fn] + funclets:
            print('%s %08x: %d words, prolog %d%s' % (wanted, base + rva, words, prolog, ' (funclet)' if is_funclet else ''))
            for i in range(words):
                w = img.d[rva + 4 * i:rva + 4 * i + 4]
                got = list(md.disasm(w, base + rva + 4 * i))
                print('  %08x: %s  %s' % (base + rva + 4 * i, w.hex(),
                      (got[0].mnemonic + ' ' + got[0].op_str) if got else '.long 0x' + w.hex()))


if __name__ == '__main__':
    main()
