#!/usr/bin/env python3
"""Wrap a native image PE (zapper output) into the XEX2 DLL module xcoreclr loads as <name>.ni.dll.

The layout follows the Plex runtime's .ni.dll files (PlexResources.ni.dll, for one):
module flags title|DLL, no imports or exports, basic compression, 4 KB pages for images at or
above 0x90000000, image flags PAGE_SIZE_4KB|REVOCATION_CHECK_REQUIRED, the execution info of the
app's title, and the image ending before the PE's trailing .reloc section. Nothing is signed;
xenia doesn't check the RSA signature (a retail console would reject the module).
"""

import argparse
import hashlib
import struct
from pathlib import Path

# xenia:src/xenia/kernel/util/xex2_info.h
HEADER_FILE_FORMAT_INFO = 0x000003FF
HEADER_IMAGE_BASE_ADDRESS = 0x00010201
HEADER_CHECKSUM_TIMESTAMP = 0x00018002
HEADER_ORIGINAL_PE_NAME = 0x000183FF
HEADER_SYSTEM_FLAGS = 0x00030000
HEADER_SYSTEM_FLAGS_32 = 0x00030100
HEADER_EXECUTION_INFO = 0x00040006
HEADER_LAN_KEY = 0x00040404
MODULE_TITLE = 0x00000001
MODULE_DLL = 0x00000008
IMAGE_PAGE_SIZE_4KB = 0x10000000
IMAGE_REVOCATION_CHECK_REQUIRED = 0x80000000
SECTION_CODE, SECTION_DATA, SECTION_READONLY = 1, 2, 3
SECURITY_INFO_BYTES = 0x184
IMAGE_INFO_BYTES = 0x174
COMPRESSION_BASIC = 1
# The values the Plex .ni.dll files carry for these headers.
SYSTEM_FLAGS = 0x00000524
SYSTEM_FLAGS_32 = 0x00003210


def align(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def sha1(*parts):
    digest = hashlib.sha1()
    for part in parts:
        digest.update(part)
    return digest.digest()


def load_pe(data):
    """Returns (image bytes, base, sections, timestamp, checksum) with sections mapped at their RVAs."""
    e = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e:e + 4] != b"PE\0\0":
        raise SystemExit("not a PE file")
    machine, nsec, timestamp, _, _, optsz, _ = struct.unpack_from("<HHIIIHH", data, e + 4)
    if machine != 0x1F2:
        raise SystemExit(f"machine {machine:#x} is not PowerPC big-endian")
    opt = e + 24
    base = struct.unpack_from("<I", data, opt + 28)[0]
    image_size, headers_size, checksum = struct.unpack_from("<III", data, opt + 56)
    sections = []
    for i in range(nsec):
        name, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", data, opt + optsz + 40 * i)
        flags = struct.unpack_from("<I", data, opt + optsz + 40 * i + 36)[0]
        sections.append({"name": name.rstrip(b"\0").decode(), "va": va, "vsize": max(vsize, rsize) if not vsize else vsize,
                         "raw": data[rptr:rptr + min(rsize, vsize or rsize)], "flags": flags})
    image = bytearray(image_size)
    image[:headers_size] = data[:headers_size]
    for s in sections:
        image[s["va"]:s["va"] + len(s["raw"])] = s["raw"]
    return image, base, sections, timestamp, checksum


def rebase(image, old_base, new_base):
    """Applies the PE's base relocations for a move from old_base to new_base, as Microsoft's pipeline must have:
    xcoreclr's zapper writes images based at 0 with a .reloc section (observed: its output for PlexResources.dll),
    and the shipped images are based at their load address. The kinds it emits are HIGHLOW (a big-endian word),
    HIGHADJ (the immediate of a lis, followed by an entry holding the low half it pairs with), LOW (the immediate of
    the paired instruction) and ABSOLUTE padding.

    The zapper also relocates addresses that aren't in the image: a method's precode is `lis/addi r12` of xcoreclr's
    PrecodeFixupThunk (0x84F06D18) or its other fixup thunks, as the runtime writes precodes, and the image's .reloc
    lists those words too. Moving them with the image (0x84F06D18 + 0x95000000 = 0x19F06D18, nothing there at run
    time: "No guest code at 19F06D18 for a call") broke every method with a precode; Microsoft's images keep the
    runtime's address. Only words whose value lies inside the image move."""
    delta = (new_base - old_base) & 0xFFFFFFFF
    e = struct.unpack_from("<I", image, 0x3C)[0]
    size_of_image = struct.unpack_from("<I", image, e + 24 + 56)[0]

    def inside(value):
        return old_base <= value < old_base + size_of_image

    def low_half(low):
        return low - 0x10000 if low & 0x8000 else low
    reloc_rva, reloc_size = struct.unpack_from("<II", image, e + 24 + 96 + 5 * 8)
    offset, end = reloc_rva, reloc_rva + reloc_size
    counts = {}
    high_inside = []  # the HIGHADJ decisions not yet claimed by a LOW, in order: two lis may precede their two loads
    while offset < end:
        page, block = struct.unpack_from("<II", image, offset)
        if not block:
            break
        entries = (block - 8) // 2
        i = 0
        while i < entries:
            entry = struct.unpack_from("<H", image, offset + 8 + 2 * i)[0]
            kind, at = entry >> 12, page + (entry & 0xFFF)
            counts[kind] = counts.get(kind, 0) + 1
            if kind == 3:
                value = struct.unpack_from(">I", image, at)[0]
                if inside(value):
                    struct.pack_into(">I", image, at, (value + delta) & 0xFFFFFFFF)
                else:
                    counts["kept"] = counts.get("kept", 0) + 1
            elif kind == 4:
                # The zapper writes the low half's entry big-endian, like the instruction it belongs to (997 of
                # 1000 HIGHADJ entries of one image match their paired instruction's immediate that way, the other
                # three read the same both ways).
                low = struct.unpack_from(">H", image, offset + 8 + 2 * (i + 1))[0]
                high = struct.unpack_from(">H", image, at + 2)[0]
                value = ((high << 16) + low_half(low)) & 0xFFFFFFFF
                high_inside.append(inside(value))
                if high_inside[-1]:
                    value = (value + delta) & 0xFFFFFFFF
                    struct.pack_into(">H", image, at + 2, ((value + 0x8000) >> 16) & 0xFFFF)
                else:
                    counts["kept"] = counts.get("kept", 0) + 1
                i += 1
            elif kind == 2:
                # The LOWs follow their HIGHADJs in the table in the same order (padding between them at a page
                # boundary; `lis r11; lis r12; ld r11; ld r12` gives two of each): a pair moves or stays together,
                # and the high half isn't read from the word before.
                if not high_inside:
                    raise SystemExit(f"LOW relocation at {at:#x} without a HIGHADJ before it")
                if high_inside.pop(0):
                    low = struct.unpack_from(">H", image, at + 2)[0]
                    struct.pack_into(">H", image, at + 2, (low + delta) & 0xFFFF)
                else:
                    counts["kept"] = counts.get("kept", 0) + 1
            elif kind != 0:
                raise SystemExit(f"relocation kind {kind} at {at:#x} is not handled")
            i += 1
        offset += block
    struct.pack_into("<I", image, e + 24 + 28, new_base)
    # The exception directory's RUNTIME_FUNCTIONs begin with image RVAs in the zapper's output and have no
    # relocations, but the shipped images carry absolute addresses there, which xcoreclr's unwinder needs: without
    # them it can't find a compiled method's entry and calls DbgBreakPoint during stack walks.
    pdata_rva, pdata_size = struct.unpack_from("<II", image, e + 24 + 96 + 3 * 8)
    for at in range(pdata_rva, pdata_rva + pdata_size, 8):
        begin = struct.unpack_from(">I", image, at)[0]
        if begin and begin < len(image):
            struct.pack_into(">I", image, at, begin + new_base)
    # CORCOMPILE_HEADER.ImageBase (offset 0x88, big-endian) has no relocation; the zapper leaves it 0 and the shipped
    # images carry their load address there.
    cor20 = struct.unpack_from("<I", image, e + 24 + 96 + 14 * 8)[0]
    native_header = struct.unpack_from("<I", image, cor20 + 0x40)[0]
    if native_header:
        struct.pack_into(">I", image, native_header + 0x88, new_base)
        # The reverse P/Invoke thunk records (0x7C bytes each; the runtime walks them when the image loads, making
        # a marshalling thunk for each: FUN_84BD2408, size read at +0xA8) hold RVAs in the zapper's output and no
        # relocations: the MethodDesc at +4, and two more pointers at +0x74 and +0x78. The shipped images carry
        # addresses there, and the loader uses them as pointers (a null MethodDesc faulted in 0x84BB8A9C).
        records, size = struct.unpack_from("<II", image, native_header + 0xA4)
        for at in range(records, records + size, 0x7C):
            for field in (4, 0x74, 0x78):
                value = struct.unpack_from(">I", image, at + field)[0]
                if value and value < len(image):
                    struct.pack_into(">I", image, at + field, value + new_base)
            counts["thunks"] = counts.get("thunks", 0) + 1
    return counts


def section_kind(flags):
    if flags & 0x20000000:
        return SECTION_CODE
    if flags & 0x80000000:
        return SECTION_DATA
    return SECTION_READONLY


def page_runs(image_bytes, sections, page):
    kinds = [SECTION_READONLY] + [None] * (image_bytes // page - 1)
    for s in sections:
        first, last = s["va"] // page, (s["va"] + s["vsize"] - 1) // page
        for p in range(first, min(last, len(kinds) - 1) + 1):
            k = section_kind(s["flags"])
            # A page holding code and data is code, as the zapper keeps them on separate pages anyway.
            kinds[p] = k if kinds[p] in (None, SECTION_READONLY) or k == SECTION_CODE else kinds[p]
    # A descriptor covers at most 64 KB and never crosses a 64 KB boundary: the console's hypervisor checks both
    # (HvxLoadImageData, syscall 0x14, HV 0x2A120 in 2.0.17559) and the loader fails the image with
    # STATUS_IMAGE_CHECKSUM_MISMATCH otherwise. xenia checks neither.
    runs = []
    for index, kind in enumerate(kinds):
        kind = kind or SECTION_READONLY
        if runs and runs[-1][0] == kind and (index * page) % 0x10000:
            runs[-1][1] += 1
        else:
            runs.append([kind, 1])
    return runs


def page_descriptors(image, runs, page):
    next_digest = bytes(20)
    out = []
    offset = len(image)
    for kind, count in reversed(runs):
        offset -= count * page
        descriptor = struct.pack(">I", count << 4 | kind) + next_digest
        out.append(descriptor)
        next_digest = sha1(image[offset:offset + count * page], descriptor)
    out.reverse()
    return b"".join(out), next_digest, len(runs)


def compression_blocks(image):
    blocks, zero = [], bytes(0x1000)
    for offset in range(0, len(image), 0x1000):
        if image[offset:offset + 0x1000] == zero and blocks:
            blocks[-1][1] += 0x1000
        elif blocks and not blocks[-1][1]:
            blocks[-1][0] += 0x1000
        else:
            blocks.append([0x1000, 0])
    blocks[-1][1] = 0
    return blocks


def build(pe_bytes, pe_name, title_id, media_id, version, base_version, new_base=None):
    image, base, sections, timestamp, checksum = load_pe(pe_bytes)
    if new_base is not None and new_base != base:
        if new_base & 0xFFFF:
            raise SystemExit("the base must be a multiple of 64 KB")
        rebase(image, base, new_base)
        base = new_base
    # The XEX image stops where a trailing .reloc section starts, as in the platform's .ni.dll files.
    if sections and sections[-1]["name"] == ".reloc":
        image = image[:sections[-1]["va"]]
        sections = sections[:-1]
    page = 0x1000 if base >= 0x90000000 else 0x10000
    image = image + bytes(align(len(image), page) - len(image))
    runs = page_runs(len(image), sections, page)
    descriptors, image_digest, ndesc = page_descriptors(image, runs, page)
    blocks = compression_blocks(image)
    file_data = bytearray()
    offset = 0
    for data_bytes, zero_bytes in blocks:
        file_data += image[offset:offset + data_bytes]
        offset += data_bytes + zero_bytes

    name = pe_name.encode() + b"\0"
    name_block = struct.pack(">I", 4 + align(len(name), 4)) + name + bytes(align(len(name), 4) - len(name))
    inline = {HEADER_IMAGE_BASE_ADDRESS: base, HEADER_SYSTEM_FLAGS: SYSTEM_FLAGS, HEADER_SYSTEM_FLAGS_32: SYSTEM_FLAGS_32}
    data_blocks = {
        HEADER_FILE_FORMAT_INFO: struct.pack(">IHH", 8 + 8 * len(blocks), 0, COMPRESSION_BASIC)
            + b"".join(struct.pack(">II", *b) for b in blocks),
        HEADER_CHECKSUM_TIMESTAMP: struct.pack(">II", checksum, timestamp),
        HEADER_ORIGINAL_PE_NAME: name_block,
        HEADER_EXECUTION_INFO: struct.pack(">IIIIBBBBI", media_id, version, base_version, title_id, 0, 0, 0, 0, 0),
        HEADER_LAN_KEY: bytes(16),
    }
    count = len(inline) + len(data_blocks)
    security_offset = align(0x18 + 8 * count, 8)
    security_bytes = SECURITY_INFO_BYTES + len(descriptors)
    data_offset = align(security_offset + security_bytes, 8)
    table, data = b"", b""
    for key in sorted(list(inline) + list(data_blocks)):
        if key in inline:
            table += struct.pack(">II", key, inline[key])
        else:
            table += struct.pack(">II", key, data_offset + len(data))
            block = data_blocks[key]
            data += block + bytes(align(len(block), 8) - len(block))
    header_size = align(data_offset + len(data), 0x1000)

    security = bytearray(SECURITY_INFO_BYTES)
    struct.pack_into(">II", security, 0, security_bytes, len(image))
    flags = IMAGE_REVOCATION_CHECK_REQUIRED | (IMAGE_PAGE_SIZE_4KB if page == 0x1000 else 0)
    struct.pack_into(">III20sI20s", security, 0x108, IMAGE_INFO_BYTES, flags, base, image_digest, 0, bytes(20))
    struct.pack_into(">III", security, 0x178, 0xFFFFFFFF, 0xFFFFFFFF, ndesc)

    header = bytearray(header_size)
    struct.pack_into(">4sIIIII", header, 0, b"XEX2", MODULE_TITLE | MODULE_DLL, header_size, 0, security_offset, count)
    header[0x18:0x18 + len(table)] = table
    header[security_offset:security_offset + SECURITY_INFO_BYTES] = security
    header[security_offset + SECURITY_INFO_BYTES:security_offset + security_bytes] = descriptors
    header[data_offset:data_offset + len(data)] = data
    digest = sha1(header[security_offset + 0x17C:], header[:security_offset + 8])
    header[security_offset + 0x164:security_offset + 0x178] = digest
    return bytes(header) + bytes(file_data)


def read_execution_info(xex):
    count = struct.unpack_from(">I", xex, 0x14)[0]
    for i in range(count):
        key, value = struct.unpack_from(">II", xex, 0x18 + 8 * i)
        if key == HEADER_EXECUTION_INFO:
            media_id, version, base_version, title_id = struct.unpack_from(">IIII", xex, value)
            return media_id, version, base_version, title_id
    # Silverlight360 runtime profiles take the donor's title out of default.xex (its docs/title.md); the module
    # then carries none either.
    return 0, 0, 0, 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pe", type=Path)
    parser.add_argument("xex", type=Path)
    parser.add_argument("--template", type=Path, help="an XEX of the app (default.xex or a .ni.dll) to take the title from")
    parser.add_argument("--title-id", type=lambda t: int(t, 16))
    parser.add_argument("--name", help="original PE name (default: output file name)")
    parser.add_argument("--base", type=lambda t: int(t, 16), help="rebase the image to this load address (hex)")
    args = parser.parse_args()
    media_id, version, base_version, title_id = 0, 0, 0, 0
    if args.template:
        media_id, version, base_version, title_id = read_execution_info(args.template.read_bytes())
    if args.title_id is not None:
        title_id = args.title_id
    args.xex.write_bytes(build(args.pe.read_bytes(), args.name or args.xex.name, title_id, media_id, version,
                               base_version, args.base))


if __name__ == "__main__":
    main()
