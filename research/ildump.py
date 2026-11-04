"""Disassembles a method's IL and lists its exception clauses, to see what the compiler declined.

    python ildump.py assembly.dll Type::Method

Every MethodDef whose `Type::name` (as methodnames.py prints it) matches is dumped. Operands are printed raw
(tokens in hex, branch targets as IL offsets); the body comes from the method's RVA (ECMA-335 II.25.4).
"""
import io
import struct
import sys
from contextlib import redirect_stdout

import methodnames
from pe import PE

# name, operand: '' none, 'i1' 'i4' 'i8' 'r4' 'r8' 'tok' 'br1' 'br4' 'u1' 'u2' 'sw'
ONE = {
    0x00: ('nop', ''), 0x01: ('break', ''), 0x02: ('ldarg.0', ''), 0x03: ('ldarg.1', ''), 0x04: ('ldarg.2', ''),
    0x05: ('ldarg.3', ''), 0x06: ('ldloc.0', ''), 0x07: ('ldloc.1', ''), 0x08: ('ldloc.2', ''), 0x09: ('ldloc.3', ''),
    0x0A: ('stloc.0', ''), 0x0B: ('stloc.1', ''), 0x0C: ('stloc.2', ''), 0x0D: ('stloc.3', ''),
    0x0E: ('ldarg.s', 'u1'), 0x0F: ('ldarga.s', 'u1'), 0x10: ('starg.s', 'u1'), 0x11: ('ldloc.s', 'u1'),
    0x12: ('ldloca.s', 'u1'), 0x13: ('stloc.s', 'u1'), 0x14: ('ldnull', ''), 0x15: ('ldc.i4.m1', ''),
    0x1F: ('ldc.i4.s', 'i1'), 0x20: ('ldc.i4', 'i4'), 0x21: ('ldc.i8', 'i8'), 0x22: ('ldc.r4', 'r4'),
    0x23: ('ldc.r8', 'r8'), 0x25: ('dup', ''), 0x26: ('pop', ''), 0x27: ('jmp', 'tok'), 0x28: ('call', 'tok'),
    0x29: ('calli', 'tok'), 0x2A: ('ret', ''), 0x45: ('switch', 'sw'), 0x6F: ('callvirt', 'tok'),
    0x70: ('cpobj', 'tok'), 0x71: ('ldobj', 'tok'), 0x72: ('ldstr', 'tok'), 0x73: ('newobj', 'tok'),
    0x74: ('castclass', 'tok'), 0x75: ('isinst', 'tok'), 0x79: ('unbox', 'tok'), 0x7A: ('throw', ''),
    0x7B: ('ldfld', 'tok'), 0x7C: ('ldflda', 'tok'), 0x7D: ('stfld', 'tok'), 0x7E: ('ldsfld', 'tok'),
    0x7F: ('ldsflda', 'tok'), 0x80: ('stsfld', 'tok'), 0x81: ('stobj', 'tok'), 0x8C: ('box', 'tok'),
    0x8D: ('newarr', 'tok'), 0x8E: ('ldlen', ''), 0x8F: ('ldelema', 'tok'), 0xA3: ('ldelem', 'tok'),
    0xA4: ('stelem', 'tok'), 0xA5: ('unbox.any', 'tok'), 0xC2: ('refanyval', 'tok'), 0xC3: ('ckfinite', ''),
    0xC6: ('mkrefany', 'tok'), 0xD0: ('ldtoken', 'tok'), 0xDC: ('endfinally', ''), 0xDD: ('leave', 'br4'),
    0xDE: ('leave.s', 'br1'), 0xDF: ('stind.i', ''), 0xE0: ('conv.u', ''),
}
for i in range(9):
    ONE[0x16 + i] = ('ldc.i4.%d' % i, '')
for i, n in enumerate(['br', 'brfalse', 'brtrue', 'beq', 'bge', 'bgt', 'ble', 'blt', 'bne.un', 'bge.un', 'bgt.un',
                       'ble.un', 'blt.un']):
    ONE[0x2B + i] = (n + '.s', 'br1')
    ONE[0x38 + i] = (n, 'br4')
for i, n in enumerate(['ldind.i1', 'ldind.u1', 'ldind.i2', 'ldind.u2', 'ldind.i4', 'ldind.u4', 'ldind.i8', 'ldind.i',
                       'ldind.r4', 'ldind.r8', 'ldind.ref', 'stind.ref', 'stind.i1', 'stind.i2', 'stind.i4',
                       'stind.i8', 'stind.r4', 'stind.r8', 'add', 'sub', 'mul', 'div', 'div.un', 'rem', 'rem.un',
                       'and', 'or', 'xor', 'shl', 'shr', 'shr.un', 'neg', 'not', 'conv.i1', 'conv.i2', 'conv.i4',
                       'conv.i8', 'conv.r4', 'conv.r8', 'conv.u4', 'conv.u8']):
    ONE[0x46 + i] = (n, '')
ONE[0x76] = ('conv.r.un', '')
for i, n in enumerate(['conv.ovf.i1.un', 'conv.ovf.i2.un', 'conv.ovf.i4.un', 'conv.ovf.i8.un', 'conv.ovf.u1.un',
                       'conv.ovf.u2.un', 'conv.ovf.u4.un', 'conv.ovf.u8.un', 'conv.ovf.i.un', 'conv.ovf.u.un']):
    ONE[0x82 + i] = (n, '')
for i, n in enumerate(['ldelem.i1', 'ldelem.u1', 'ldelem.i2', 'ldelem.u2', 'ldelem.i4', 'ldelem.u4', 'ldelem.i8',
                       'ldelem.i', 'ldelem.r4', 'ldelem.r8', 'ldelem.ref', 'stelem.i', 'stelem.i1', 'stelem.i2',
                       'stelem.i4', 'stelem.i8', 'stelem.r4', 'stelem.r8', 'stelem.ref']):
    ONE[0x90 + i] = (n, '')
for i, n in enumerate(['conv.ovf.i1', 'conv.ovf.u1', 'conv.ovf.i2', 'conv.ovf.u2', 'conv.ovf.i4', 'conv.ovf.u4',
                       'conv.ovf.i8', 'conv.ovf.u8']):
    ONE[0xB3 + i] = (n, '')
for i, n in enumerate(['conv.u2', 'conv.u1', 'conv.i', 'conv.ovf.i', 'conv.ovf.u', 'add.ovf', 'add.ovf.un',
                       'mul.ovf', 'mul.ovf.un', 'sub.ovf', 'sub.ovf.un']):
    ONE[0xD1 + i] = (n, '')
TWO = {
    0x00: ('arglist', ''), 0x01: ('ceq', ''), 0x02: ('cgt', ''), 0x03: ('cgt.un', ''), 0x04: ('clt', ''),
    0x05: ('clt.un', ''), 0x06: ('ldftn', 'tok'), 0x07: ('ldvirtftn', 'tok'), 0x09: ('ldarg', 'u2'),
    0x0A: ('ldarga', 'u2'), 0x0B: ('starg', 'u2'), 0x0C: ('ldloc', 'u2'), 0x0D: ('ldloca', 'u2'),
    0x0E: ('stloc', 'u2'), 0x0F: ('localloc', ''), 0x11: ('endfilter', ''), 0x12: ('unaligned.', 'u1'),
    0x13: ('volatile.', ''), 0x14: ('tail.', ''), 0x15: ('initobj', 'tok'), 0x16: ('constrained.', 'tok'),
    0x17: ('cpblk', ''), 0x18: ('initblk', ''), 0x19: ('no.', 'u1'), 0x1A: ('rethrow', ''), 0x1C: ('sizeof', 'tok'),
    0x1D: ('refanytype', ''), 0x1E: ('readonly.', ''),
}
SIZES = {'': 0, 'i1': 1, 'u1': 1, 'br1': 1, 'u2': 2, 'i4': 4, 'tok': 4, 'br4': 4, 'r4': 4, 'i8': 8, 'r8': 8}
CLAUSE_KINDS = {0: 'catch', 1: 'filter', 2: 'finally', 4: 'fault'}


def body(pe, rva):
    """IL bytes and clauses (flags, try start, try length, handler start, handler length, token or filter)."""
    d = pe.d
    o = pe.off(rva)
    if d[o] & 3 == 2:
        return d[o + 1:o + 1 + (d[o] >> 2)], [], 8
    flags, max_stack, code_size = struct.unpack_from('<HHI', d, o)
    start = o + (flags >> 12) * 4
    il = d[start:start + code_size]
    clauses = []
    if flags & 8:
        p = (start + code_size + 3) & ~3
        while True:
            kind = d[p]
            fat = kind & 0x40
            size = struct.unpack_from('<I', d, p)[0] >> 8 if fat else d[p + 1]
            q = p + 4
            while q + (24 if fat else 12) <= p + size:
                clauses.append(struct.unpack_from('<6I', d, q) if fat else struct.unpack_from('<HHBHBI', d, q))
                q += 24 if fat else 12
            if not kind & 0x80:
                break
            p += size
    return il, clauses, max_stack


def disassemble(il):
    pc = 0
    while pc < len(il):
        at = pc
        op = il[pc]
        pc += 1
        if op == 0xFE:
            op2 = il[pc]
            pc += 1
            name, kind = TWO.get(op2, ('?fe%02x' % op2, ''))
        else:
            name, kind = ONE.get(op, ('?%02x' % op, ''))
        if kind == 'sw':
            n = struct.unpack_from('<I', il, pc)[0]
            pc += 4
            targets = [pc + 4 * n + struct.unpack_from('<i', il, pc + 4 * i)[0] for i in range(n)]
            pc += 4 * n
            text = '(' + ', '.join('IL_%04x' % t for t in targets) + ')'
        else:
            raw = il[pc:pc + SIZES[kind]]
            pc += SIZES[kind]
            if kind == 'br1':
                text = 'IL_%04x' % (pc + struct.unpack('<b', raw)[0])
            elif kind == 'br4':
                text = 'IL_%04x' % (pc + struct.unpack('<i', raw)[0])
            elif kind == 'tok':
                text = '0x%08x' % struct.unpack('<I', raw)[0]
            elif kind in ('i1', 'i4', 'i8'):
                text = str(struct.unpack({'i1': '<b', 'i4': '<i', 'i8': '<q'}[kind], raw)[0])
            elif kind in ('r4', 'r8'):
                text = repr(struct.unpack('<f' if kind == 'r4' else '<d', raw)[0])
            elif kind in ('u1', 'u2'):
                text = str(int.from_bytes(raw, 'little'))
            else:
                text = ''
        yield at, name, text


def main():
    path, wanted = sys.argv[1], sys.argv[2]
    listing = io.StringIO()
    sys.argv = [sys.argv[0], path]
    with redirect_stdout(listing):
        methodnames.main()
    pe = PE(path)
    d, streams = methodnames.read_metadata(path)
    rvas = method_rvas(pe, d, streams)
    for line in listing.getvalue().splitlines():
        token, name, _ = line.split(' ')
        if name != wanted:
            continue
        rva = rvas[int(token, 16) & 0xFFFFFF]
        if not rva:
            print('%s %s: no body' % (token, name))
            continue
        il, clauses, max_stack = body(pe, rva)
        print('%s %s: %d bytes of IL, max stack %d' % (token, name, len(il), max_stack))
        for c in clauses:
            print('  %-7s try IL_%04x-IL_%04x handler IL_%04x-IL_%04x %s' % (CLAUSE_KINDS.get(c[0], c[0]), c[1],
                  c[1] + c[2], c[3], c[3] + c[4], ('filter IL_%04x' % c[5]) if c[0] == 1 else '0x%08x' % c[5]))
        for at, op, text in disassemble(il):
            print('  IL_%04x  %-14s %s' % (at, op, text))


def method_rvas(pe, d, streams):
    """MethodDef row -> RVA, from the same table walk as methodnames.main."""
    t = (streams.get('#~') or streams['#-'])[0]
    heap_sizes = d[t + 6]
    valid = struct.unpack_from('<Q', d, t + 8)[0]
    rows = {}
    p = t + 24
    for i in range(64):
        if valid >> i & 1:
            rows[i] = struct.unpack_from('<I', d, p)[0]
            p += 4
    strings = 4 if heap_sizes & 1 else 2
    guid = 4 if heap_sizes & 2 else 2
    blob = 4 if heap_sizes & 4 else 2

    def index(table):
        return 4 if rows.get(table, 0) >= 0x10000 else 2

    def coded(bits, *tabs):
        return 4 if any(rows.get(x, 0) >= 1 << (16 - bits) for x in tabs) else 2

    sizes = [2 + strings + 3 * guid, coded(2, 0, 0x1A, 0x23, 1) + 2 * strings,
             4 + 2 * strings + coded(2, 2, 1, 0x1B) + index(4) + index(6), index(4), 2 + strings + blob, index(6),
             4 + 2 + 2 + strings + blob + index(8)]
    for i in range(6):
        p += rows.get(i, 0) * sizes[i]
    return {m: struct.unpack_from('<I', d, p + (m - 1) * sizes[6])[0] for m in range(1, rows.get(6, 0) + 1)}


if __name__ == '__main__':
    main()
