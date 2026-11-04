"""Lists an assembly's methods as `token Type::name clauses` lines, one per MethodDef row, for nicompare.py; clauses is
the method body's number of exception handling clauses (0 without a body).

    python methodnames.py assembly.dll > methods.txt

Reads the #~ metadata tables directly (ECMA-335 II.22, II.24): only Module, TypeRef, TypeDef, Field and MethodDef
are decoded, which is enough to name every method with its declaring type.
"""
import struct
import sys

from pe import PE


def read_metadata(path):
    pe = PE(path)
    cli = pe.dirs[14]
    cli_off = pe.off(cli[0])
    md_rva, md_size = struct.unpack_from('<II', pe.d, cli_off + 8)
    root = pe.off(md_rva)
    d = pe.d
    version_length = struct.unpack_from('<I', d, root + 12)[0]
    p = root + 16 + version_length
    stream_count = struct.unpack_from('<H', d, p + 2)[0]
    p += 4
    streams = {}
    for _ in range(stream_count):
        offset, size = struct.unpack_from('<II', d, p)
        p += 8
        end = d.index(b'\0', p)
        name = d[p:end].decode()
        p = (end + 4) & ~3
        streams[name] = (root + offset, size)
    return d, streams


def clause_count(pe, rva):
    """The exception clauses of a method body (ECMA-335 II.25.4)."""
    if not rva:
        return 0
    d = pe.d
    o = pe.off(rva)
    if d[o] & 3 != 3:
        return 0  # a tiny header has no sections
    flags = struct.unpack_from('<H', d, o)[0]
    if not flags & 8:
        return 0
    code_size = struct.unpack_from('<I', d, o + 4)[0]
    p = (o + (flags >> 12) * 4 + code_size + 3) & ~3
    count = 0
    while True:
        kind = d[p]
        if kind & 0x40:
            size = struct.unpack_from('<I', d, p)[0] >> 8
            if kind & 1:
                count += (size - 4) // 24
        else:
            size = d[p + 1]
            if kind & 1:
                count += (size - 4) // 12
        if not kind & 0x80:
            return count
        p += size


def main():
    pe = PE(sys.argv[1])
    d, streams = read_metadata(sys.argv[1])
    strings_base = streams['#Strings'][0]
    tables = streams.get('#~') or streams['#-']
    t = tables[0]
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

    def string_at(offset):
        end = d.index(b'\0', strings_base + offset)
        return d[strings_base + offset:end].decode('utf-8', 'replace')

    # Row sizes of the tables up to MethodDef (FieldPtr and MethodPtr only appear in uncompressed metadata).
    sizes = {
        0: 2 + strings + 3 * guid,
        1: coded(2, 0, 0x1A, 0x23, 1) + 2 * strings,
        2: 4 + 2 * strings + coded(2, 2, 1, 0x1B) + index(4) + index(6),
        3: index(4),
        4: 2 + strings + blob,
        5: index(6),
        6: 4 + 2 + 2 + strings + blob + index(8),
    }
    starts = {}
    for i in range(7):
        starts[i] = p
        p += rows.get(i, 0) * sizes[i]
    if 3 in rows or 5 in rows:
        sys.exit('uncompressed metadata (FieldPtr/MethodPtr tables) is not handled')

    types = []
    for r in range(rows.get(2, 0)):
        q = starts[2] + r * sizes[2] + 4
        name = struct.unpack_from('<I' if strings == 4 else '<H', d, q)[0]
        q += 2 * strings + coded(2, 2, 1, 0x1B) + index(4)
        method_list = struct.unpack_from('<I' if index(6) == 4 else '<H', d, q)[0]
        types.append((string_at(name), method_list))
    method_count = rows.get(6, 0)
    for i, (name, first) in enumerate(types):
        last = types[i + 1][1] if i + 1 < len(types) else method_count + 1
        for m in range(first, last):
            q = starts[6] + (m - 1) * sizes[6]
            rva = struct.unpack_from('<I', d, q)[0]
            method_name = struct.unpack_from('<I' if strings == 4 else '<H', d, q + 8)[0]
            print('0x%08x %s::%s %d' % (0x06000000 | m, name, string_at(method_name), clause_count(pe, rva)))


if __name__ == '__main__':
    main()
