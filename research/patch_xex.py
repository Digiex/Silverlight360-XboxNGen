#!/usr/bin/env python3
"""Patch words into an uncompressed or basic-compressed, unencrypted XEX at virtual addresses (for diagnostics).

usage: patch_xex.py <in.xex> <out.xex> <address>=<word>[,<word>...] ...
       patch_xex.py <in.xex> <out.xex> --jump <address>=<target>   (lis r12/ori r12/mtctr r12/bctr)
Only an unencrypted image can be patched this way; the page digests are not updated (xenia doesn't check them).
"""
import struct
import sys


def image_offsets(xex):
    header_size = struct.unpack_from(">I", xex, 8)[0]
    count = struct.unpack_from(">I", xex, 0x14)[0]
    base = fmt = None
    for i in range(count):
        key, value = struct.unpack_from(">II", xex, 0x18 + 8 * i)
        if key == 0x00010201:
            base = value
        elif key == 0x000003FF:
            fmt = value
    info_size, encryption, compression = struct.unpack_from(">IHH", xex, fmt)
    if encryption:
        sys.exit("encrypted XEX")
    if compression == 0:
        return base, [(0, header_size, None)]
    if compression != 1:
        sys.exit("only uncompressed and basic-compressed XEX files")
    # Basic compression: [data bytes, zero bytes] blocks; map image offsets of data runs to file offsets.
    runs = []
    image = 0
    file = header_size
    for i in range((info_size - 8) // 8):
        data, zeros = struct.unpack_from(">II", xex, fmt + 8 + 8 * i)
        runs.append((image, file, data))
        image += data + zeros
        file += data
    return base, runs


def file_offset(base, runs, address):
    rva = address - base
    for image, file, size in runs:
        if size is None:
            return file + rva
        if image <= rva < image + size:
            return file + rva - image
    sys.exit(f"{address:#x} is in a zero run or outside the image")


def main():
    data = bytearray(open(sys.argv[1], "rb").read())
    base, runs = image_offsets(data)
    args = sys.argv[3:]
    jump = False
    for arg in args:
        if arg == "--jump":
            jump = True
            continue
        address, _, value = arg.partition("=")
        address = int(address, 16)
        if jump:
            target = int(value, 16)
            words = [0x3D800000 | target >> 16, 0x618C0000 | (target & 0xFFFF), 0x7D8903A6, 0x4E800420]
            jump = False
        else:
            words = [int(w, 16) for w in value.split(",")]
        for i, word in enumerate(words):
            struct.pack_into(">I", data, file_offset(base, runs, address + 4 * i), word)
    open(sys.argv[2], "wb").write(data)


if __name__ == "__main__":
    main()
