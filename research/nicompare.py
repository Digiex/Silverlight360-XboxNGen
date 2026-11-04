"""Compares two native images of the same assembly: Microsoft's and ours (memory images from xex.py -b).

    python nicompare.py ms.pe ms_base ours.pe ours_base [jit.log assembly methods.txt] [-v]

Reports the PE sections, the .pdata functions (main bodies and funclets), unwind word and GC info header tallies,
then pairs the methods by MethodDef row (each body's record leads to its MethodDesc), named from the assembly's
metadata (methods.txt: `token Type::name` lines) with the declines from xclrjit's log. Per pair it compares the code size, the prolog, the funclets,
the calls and the GC info, and tallies the differences by category; -v lists the methods behind each category.
"""
import collections
import re
import struct
import sys

import gcinfo
from pe import PE

FUNCLET_ENTRY = bytes.fromhex('fbe1fff880630004')  # std r31,-8(r1); lwz r3,4(r3)
SAVE_GPRLR = 0xF9C1FF68  # std r14,-0x98(r1): __savegprlr_14, which Microsoft's prologs call
REST_GPRLR = 0xE9C1FF68  # ld r14,-0x98(r1): __restgprlr_14
BL = 0x48000001
BCTRL = 0x4E800421
BLR = 0x4E800020


class Image:
    def __init__(self, path, base):
        self.pe = PE(path)
        self.d = self.pe.d
        self.base = base
        pd = self.pe.dirs[3]
        self.pdata = [struct.unpack_from('>II', self.d, pd[0] + 8 * i) for i in range(pd[1] // 8)]
        self.functions = []  # (begin rva, words, prolog, is_funclet, unwind word)
        for begin, word in self.pdata:
            rva = begin - base if begin >= base else begin
            self.functions.append((rva, (word >> 8) & 0x3FFFFF, word & 0xFF, self.d[rva:rva + 8] == FUNCLET_ENTRY, word))
        self.mains = [f for f in self.functions if not f[3]]

    def u32(self, rva):
        return struct.unpack_from('>I', self.d, rva)[0]

    def gc_header(self, rva):
        try:
            rel = self.u32(rva - 12)
            rec = rva - 16 + rel
            g = rec + 8 + struct.unpack_from('>i', self.d, rec + 8)[0]
            h = gcinfo.header(self.d, g)
            h.pop('bitpos', None)
            try:
                h['slots'] = len(gcinfo.full(self.d, g)[3])
            except Exception:  # noqa: BLE001
                h['slots'] = -1
            return h
        except Exception as e:  # noqa: BLE001 - a malformed record is itself a finding
            return {'error': str(e)}

    def code(self, rva, words):
        return [self.u32(rva + 4 * i) for i in range(words)]

    def methods(self):
        """Main bodies with their funclets, in address order: (main, [funclets])."""
        out = []
        for f in sorted(self.functions):
            if f[3] and out:
                out[-1][1].append(f)
            elif not f[3]:
                out.append((f, []))
        return out

    def method_rid(self, rva):
        """The MethodDef row of a method: the method's record points at its MethodDesc, whose first halfword is the
        token remainder (the whole row in xcoreclr: PseudoCustomAttribute::GetCustomAttributes, row 0x1667, reads
        0x1667, where CLR 4's 12-bit remainder would give 0x667). None when the record points outside
        the image (the register save/restore routines, the first method's fixup)."""
        try:
            # The record found from the pre-header (16 bytes: MethodDesc, 0, GC info, RUNTIME_FUNCTION, all
            # relative) has this body's function table entry in its fourth word, but the MethodDesc of the method
            # before it in the zapper's list (checked by the code sizes xclrjit's log gives: the body of 1396 bytes
            # is PseudoCustomAttribute::GetCustomAttributes, the next record's MethodDesc, not IsDefined, this
            # record's); this body's MethodDesc is the next record's.
            rec = rva - 16 + self.u32(rva - 12)
            for candidate in (rec, rec - 16, rec + 16):
                entry = candidate + 12 + struct.unpack_from('>i', self.d, candidate + 12)[0]
                if 0 <= entry < len(self.d) - 4 and self.u32(entry) == self.base + rva:
                    rec = candidate
                    break
            rec += 16
            md = rec + struct.unpack_from('>i', self.d, rec)[0]
            if not 0 <= md < len(self.d) - 8:
                return None
            return struct.unpack_from('>H', self.d, md)[0] or None
        except Exception:  # noqa: BLE001
            return None


def results_from_log(path, assembly):
    """xclrjit's results for `assembly`, in order: (name, compiled, reason)."""
    results, active = [], False
    for line in open(path, encoding='utf-8', errors='replace'):
        if 'ngenhost: compiling' in line:
            active = assembly in line
        elif active and ('xclrjit: compiled ' in line or 'xclrjit: declined ' in line):
            text = line.split('xclrjit: ', 1)[1].rstrip()
            compiled = text.startswith('compiled ')
            name = text.split(' ', 1)[1].split(' (')[0]
            # "... -> 0 bytes at 00000000: reason" (older logs have no "at" field).
            reason = re.split(r'bytes(?: at [0-9A-F]{8})?: ', text, 1)[1] if not compiled and re.search(r'bytes(?: at [0-9A-F]{8})?: ', text) else ''
            results.append((name, compiled, reason))
    return results


def names_from_metadata(path):
    """MethodDef row -> Type::name, from a listing of `token Type::name` lines (research/methodnames.py)."""
    names, clauses = {}, {}
    for line in open(path, encoding='utf-8-sig'):
        parts = line.split()
        if len(parts) < 2:
            continue
        rid = int(parts[0], 16) & 0xFFFFFF
        names[rid] = parts[1]
        clauses[rid] = int(parts[2]) if len(parts) > 2 else -1
    names['clauses'] = clauses
    return names


def describe(img, main, funclets):
    rva, words, prolog, _, _ = main
    code = img.code(rva, words)
    h = img.gc_header(rva)
    return {
        'words': words, 'prolog': prolog, 'funclets': len(funclets),
        'bl': sum(1 for w in code if (w & 0xFC000003) == BL), 'bctrl': code.count(BCTRL),
        'gc': h,
    }


def compare(ms, ours, results, names, verbose):
    ms_methods = [m for m in ms.methods() if ms.u32(m[0][0]) not in (SAVE_GPRLR, REST_GPRLR)]
    our_methods = ours.methods()
    compiled = [r for r in results if r[1]]
    print('pairing: ms %d methods (less the register save/restore routines), ours %d in the image, %d results in the log '
          '(%d compiled)' % (len(ms_methods), len(our_methods), len(results), len(compiled)))
    # Neither image lays methods out in compile order, but each method's record leads to its MethodDesc and so to
    # its MethodDef row: that names it (from the assembly's metadata) and pairs it (the k-th body of a row with the
    # k-th: a generic method's instantiations share one row).
    by_rid = collections.defaultdict(list)
    for m in ms_methods:
        by_rid[ms.method_rid(m[0][0])].append(m)
    compiled_names = {r[0] for r in results if r[1]}
    our_by_rid = collections.defaultdict(list)
    for m in our_methods:
        our_by_rid[ours.method_rid(m[0][0])].append(m)
    cats = collections.defaultdict(list)
    for name, _, reason in results:
        if reason:
            cats['declined: ' + reason].append(name)
    taken = collections.Counter()
    for main, funclets in our_methods:
        rid = ours.method_rid(main[0])
        name = names.get(rid, 'row %s' % rid)
        k = taken[rid]
        taken[rid] += 1
        if rid is None or k >= len(by_rid.get(rid, [])):
            cats["unpaired: ours has a body Microsoft's image lacks (row %s)" % rid].append(name)
            continue
        m = describe(ms, *by_rid[rid][k])
        o = describe(ours, main, funclets)
        mg, og = m['gc'], o['gc']
        if 'error' in og:
            cats['ERROR: our GC info unreadable'].append(name)
        # A body without funclets for a method with clauses is an error unless another body of ours for the same
        # row has them: a method's record can be paired to a stub's body near it (pairing by the next record).
        if names['clauses'].get(rid, -1) > 0 and o['funclets'] == 0 and not any(len(x[1]) for x in our_by_rid[rid])                 and name in compiled_names:
            cats['ERROR: IL has exception clauses, our body has no funclets'].append(name)
        if o['bl'] + o['bctrl'] > 0 and not og.get('safepoints') and not og.get('ranges'):
            cats['ERROR: our body calls but reports no safe points'].append(name)
        if og.get('codelen', 0) < o['words'] * 4:
            cats['ERROR: our GC code length short of .pdata'].append(name)
        if m['funclets'] != o['funclets']:
            cats['funclets differ: ms %d ours %d' % (m['funclets'], o['funclets'])].append(name)
        if m['prolog'] == 0 and o['prolog'] != 0:
            cats['frameless in ms, framed in ours'].append(name)
        if mg.get('ranges') and og.get('safepoints'):
            cats['ms fully interruptible (ranges), ours safe points'].append(name)
        elif mode(mg) != mode(og):
            cats['GC reporting mode differs: ms %s ours %s' % (mode(mg), mode(og))].append(name)
        if (mg.get('flags', 0) & ~0x40) != (og.get('flags', 0) & ~0x40):
            cats['GC header flags differ: ms %#x ours %#x' % (mg.get('flags', 0), og.get('flags', 0))].append(name)
        if (mg.get('outgoing', 0) == 0) != (og.get('outgoing', 0) == 0):
            cats['outgoing area: ms %d ours %d' % (mg.get('outgoing', 0), og.get('outgoing', 0))].append(name)
        if m['bl'] + m['bctrl'] == 0 and o['bl'] + o['bctrl'] > 0:
            cats['calls where ms has none (helper calls for checks ms inlines)'].append(name)
        if o['bl'] + o['bctrl'] < m['bl'] + m['bctrl']:
            cats['fewer calls than ms'].append(name)
        ratio = o['words'] / max(m['words'], 1)
        bucket = '<1' if ratio < 1 else '1-2' if ratio < 2 else '2-4' if ratio < 4 else '4+'
        cats['code size ratio ours/ms ' + bucket].append(name)
    unpaired_ms = sum(len(v) - taken[r] for r, v in by_rid.items() if len(v) > taken[r])
    print('ms bodies without a counterpart of ours: %d (declined methods and their instantiations)' % unpaired_ms)
    if verbose:
        for r, v in sorted(by_rid.items(), key=lambda kv: kv[0] or 0):
            if len(v) > taken[r]:
                print('           %s: %d of ours, %d of ms' % (names.get(r, 'row %s' % r), taken[r], len(v)))
    print('categories (count: category)')
    for k, v in sorted(cats.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        print('  %5d: %s' % (len(v), k))
        if verbose:
            for name in v[:40]:
                print('           %s' % name)
            if len(v) > 40:
                print('           ... %d more' % (len(v) - 40))


def mode(h):
    return 'ranges' if h.get('ranges') else 'safepoints' if h.get('safepoints') else 'none'


def main():
    args = [a for a in sys.argv[1:] if a != '-v']
    verbose = '-v' in sys.argv
    ms = Image(args[0], int(args[1], 16))
    ours = Image(args[2], int(args[3], 16))
    print('sections:')
    for a, b in zip(ms.pe.secs, ours.pe.secs):
        print('  %-8s ms va %08x size %06x   ours va %08x size %06x' % (a[0], a[1], a[2], b[1], b[2]))
    print('functions: ms %d (%d main, %d funclets)   ours %d (%d main, %d funclets)' % (
        len(ms.functions), len(ms.mains), len(ms.functions) - len(ms.mains),
        len(ours.functions), len(ours.mains), len(ours.functions) - len(ours.mains)))
    flags = collections.Counter()
    for side, img in (('ms', ms), ('ours', ours)):
        for rva, words, prolog, funclet, word in img.functions:
            flags[(side, 'funclet' if funclet else 'main', word >> 30, prolog)] += 1
    print('unwind words (side, kind, top bits, prolog length): count')
    for k, v in sorted(flags.items()):
        print('  %s %d' % (k, v))
    gc = collections.Counter()
    for side, img in (('ms', ms), ('ours', ours)):
        for rva, words, prolog, funclet, word in img.mains:
            h = img.gc_header(rva)
            gc[(side, h.get('flags'), h.get('outgoing'), 'safepoints' if h.get('safepoints') else '',
                'ranges' if h.get('ranges') else '', 'codelen ok' if h.get('codelen') == words * 4 or h.get('codelen', 0) > words * 4 else 'codelen SHORT')] += 1
    print('GC info headers (side, flags, outgoing, safe points, ranges, code length vs .pdata): count')
    for k, v in sorted(gc.items(), key=str):
        print('  %s %d' % (k, v))
    if len(args) >= 7:
        compare(ms, ours, results_from_log(args[4], args[5]), names_from_metadata(args[6]), verbose)


if __name__ == '__main__':
    main()
