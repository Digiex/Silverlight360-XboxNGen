#!/usr/bin/env python3
"""Reads an XEX2: its memory image and its headers, in place of xextool's -b and -l.

usage: xex.py -b <out.pe> <in.xex>   writes the memory image (the PE as it lies in memory from the load address:
                                      what xextool -b calls a basefile)
       xex.py -l <in.xex>             prints the headers

Every file format the console loads is read: not encrypted or encrypted (AES-128-CBC with the image's own key, under
the retail key or, failing that, the devkit one) and uncompressed, basic (data blocks and zero runs) or normal (LZX)
compressed (xenia:src/xenia/cpu/xex_module.cc:59-88, 530-560, 625-830). memory_image() gives the same to other tools.
Needs pycryptodome for an encrypted image.
"""
import hashlib
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
import lzx  # noqa: E402

# xenia:src/xenia/cpu/xex_module.cc:66-71.
RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
DEVKIT_KEY = bytes(16)

HEADER_ENTRY_POINT = 0x00010100
HEADER_IMAGE_BASE = 0x00010201
HEADER_IMPORT_LIBRARIES = 0x000103FF
HEADER_DEFAULT_STACK_SIZE = 0x00020200
HEADER_FILE_FORMAT = 0x000003FF
HEADER_EXECUTION_INFO = 0x00040006
HEADER_NAMES = {
    0x000002FF: "resource info", HEADER_FILE_FORMAT: "file format", 0x00000405: "base reference",
    0x000005FF: "delta patch", 0x000080FF: "bounding path", 0x00010001: "original base address",
    HEADER_ENTRY_POINT: "entry point", HEADER_IMAGE_BASE: "image base", HEADER_IMPORT_LIBRARIES: "import libraries",
    0x00018002: "checksum and timestamp", 0x00018102: "enabled for callcap", 0x00018200: "enabled for fastcap",
    0x000183FF: "original PE name", 0x000200FF: "static libraries", 0x00020104: "TLS info",
    HEADER_DEFAULT_STACK_SIZE: "default stack size", 0x00020301: "filesystem cache size",
    0x00020401: "default heap size", 0x00028002: "page heap size and flags", 0x00030000: "system flags",
    0x00030100: "system flags 32", 0x00030200: "system flags 64", 0x00035EFF: "title-workspace size",
    0x00040006: "execution info", 0x00040201: "title workspace size", 0x00040310: "game ratings",
    0x00040404: "LAN key", 0x000405FF: "Xbox 360 logo", 0x000406FF: "multidisc media IDs",
    0x000407FF: "alternate title IDs", 0x00040801: "additional title memory", 0x00E10402: "exports by name",
}
MODULE_FLAGS = {0x1: "title", 0x2: "exports to title", 0x4: "system debugger", 0x8: "DLL", 0x10: "module patch",
                0x20: "full patch", 0x40: "delta patch", 0x80: "user mode"}
ENCRYPTIONS = {0: "none", 1: "normal"}
COMPRESSIONS = {0: "none", 1: "basic", 2: "normal (LZX)", 3: "delta"}


class XexError(Exception):
    pass


class Xex:
    def __init__(self, data):
        if data[:4] != b"XEX2":
            raise XexError("not an XEX2")
        self.data = data
        (self.module_flags, self.header_size, _, self.security_offset,
         count) = struct.unpack_from(">IIIII", data, 4)
        self.headers = dict(struct.unpack_from(">II", data, 0x18 + 8 * i) for i in range(count))
        sec = self.security_offset
        self.image_size = struct.unpack_from(">I", data, sec + 4)[0]
        self.load_address = struct.unpack_from(">I", data, sec + 0x110)[0]
        self.image_key = data[sec + 0x150:sec + 0x160]
        self.export_table = struct.unpack_from(">I", data, sec + 0x160)[0]
        self.region, self.media = struct.unpack_from(">II", data, sec + 0x178)
        self.page_count = struct.unpack_from(">I", data, sec + 0x180)[0]
        fmt = self.headers.get(HEADER_FILE_FORMAT)
        if fmt is None:
            raise XexError("no file format header")
        self.format_size, self.encryption, self.compression = struct.unpack_from(">IHH", data, fmt)
        self.format_offset = fmt

    def header_value(self, key):
        return self.headers.get(key)

    @property
    def base(self):
        return self.headers.get(HEADER_IMAGE_BASE, self.load_address)

    def file_data(self, key):
        data = self.data[self.header_size:]
        if self.encryption == 0:
            return data
        if self.encryption != 1:
            raise XexError(f"encryption {self.encryption}")
        try:
            from Crypto.Cipher import AES
        except ImportError:
            raise XexError("an encrypted image needs pycryptodome (pip install pycryptodome)")
        session = AES.new(key, AES.MODE_ECB).decrypt(self.image_key)
        usable = len(data) & ~15
        return AES.new(session, AES.MODE_CBC, bytes(16)).decrypt(data[:usable]) + data[usable:]

    def image_with(self, key):
        data = self.file_data(key)
        fmt = self.format_offset
        if self.compression == 0:
            image = bytes(data[:self.image_size])
        elif self.compression == 1:
            image = bytearray()
            offset = 0
            for i in range((self.format_size - 8) // 8):
                data_size, zero_size = struct.unpack_from(">II", self.data, fmt + 8 + 8 * i)
                image += data[offset:offset + data_size]
                image += bytes(zero_size)
                offset += data_size
        elif self.compression == 2:
            window_size, block_size = struct.unpack_from(">II", self.data, fmt + 8)
            block_hash = self.data[fmt + 16:fmt + 36]
            stream = bytearray()
            p = 0
            while block_size:
                block = data[p:p + block_size]
                if hashlib.sha1(block).digest() != block_hash:
                    raise XexError("a compressed block's digest doesn't match (the wrong key?)")
                next_size = struct.unpack_from(">I", block, 0)[0]
                next_hash = block[4:24]
                q = 24
                while True:
                    chunk = block[q] << 8 | block[q + 1]
                    q += 2
                    if not chunk:
                        break
                    stream += block[q:q + chunk]
                    q += chunk
                p += block_size
                block_size, block_hash = next_size, next_hash
            image = lzx.decompress(bytes(stream), self.image_size, window_size)
        else:
            raise XexError(f"compression {COMPRESSIONS.get(self.compression, self.compression)}")
        image = bytes(image[:self.image_size])
        return image + bytes(self.image_size - len(image))

    def memory_image(self):
        """The image as it lies in memory from its load address; for an encrypted one, under the key it decodes with."""
        if self.encryption == 0:
            return self.image_with(None)
        failure = None
        for key in (RETAIL_KEY, DEVKIT_KEY):
            try:
                image = self.image_with(key)
            except (XexError, lzx.LzxError) as error:
                failure = error
                continue
            if image[:2] == b"MZ":
                return image
        raise XexError(f"the image decodes under neither key ({failure})")


def memory_image(data):
    """(load address, memory image) of an XEX file's bytes."""
    xex = Xex(data)
    return xex.base, xex.memory_image()


def version_text(value):
    """major.minor.build.qfe, from 4, 4, 16 and 8 bits."""
    return f"v{value >> 28}.{value >> 24 & 15}.{value >> 8 & 0xFFFF}.{value & 0xFF}"


def describe(xex):
    flags = ", ".join(name for bit, name in MODULE_FLAGS.items() if xex.module_flags & bit) or "none"
    lines = [
        f"module flags     {xex.module_flags:#010x} ({flags})",
        f"header size      {xex.header_size:#x}",
        f"image base       {xex.base:#010x}",
        f"image size       {xex.image_size:#x}",
        f"file format      encryption {ENCRYPTIONS.get(xex.encryption, xex.encryption)}, "
        f"compression {COMPRESSIONS.get(xex.compression, xex.compression)}",
        f"page descriptors {xex.page_count}",
        f"export table     {xex.export_table:#010x}",
        f"region, media    {xex.region:#010x}, {xex.media:#010x}",
        "optional headers:",
    ]
    for key, value in sorted(xex.headers.items()):
        lines.append(f"  {key:#010x} {HEADER_NAMES.get(key, '?'):26} {value:#010x}")
    entry = xex.headers.get(HEADER_ENTRY_POINT)
    if entry is not None:
        lines.append(f"entry point      {entry:#010x}")
    info = xex.headers.get(HEADER_EXECUTION_INFO)
    if info is not None:
        media_id, version, base_version, title_id = struct.unpack_from(">IIII", xex.data, info)
        lines.append(f"execution info   title {title_id:08X}, media {media_id:08X}, version {version_text(version)}, "
                     f"base version {version_text(base_version)}")
    imports = xex.headers.get(HEADER_IMPORT_LIBRARIES)
    if imports is not None:
        # The header's size, the library names' bytes and their count (one per library), the names, then each
        # library: its size, the next one's digest, an ID, its version and minimum version (0x1C, 0x20), its name's
        # index and its record count (0x24, 0x26) (xenia:src/xenia/kernel/util/xex2_info.h:794-803).
        _, strings_size, count = struct.unpack_from(">III", xex.data, imports)
        names = xex.data[imports + 12:imports + 12 + strings_size].split(b"\0")
        names = [n.decode("ascii", "replace") for n in names if n]
        p = imports + 12 + strings_size
        lines.append("import libraries:")
        for _ in range(count):
            lib_size = struct.unpack_from(">I", xex.data, p)[0]
            version, min_version = struct.unpack_from(">II", xex.data, p + 0x1C)
            name_index, record_count = struct.unpack_from(">HH", xex.data, p + 0x24)
            name = names[name_index & 0xFF] if (name_index & 0xFF) < len(names) else "?"
            lines.append(f"  {name:20} {version_text(version)} (min {version_text(min_version)}), {record_count} records")
            p += lib_size
    return "\n".join(lines)


def main():
    args = sys.argv[1:]
    if len(args) == 3 and args[0] == "-b":
        base, image = memory_image(open(args[2], "rb").read())
        open(args[1], "wb").write(image)
        print(f"{base:08X} {len(image):#x} bytes")
    elif len(args) == 2 and args[0] == "-l":
        print(describe(Xex(open(args[1], "rb").read())))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    try:
        main()
    except (XexError, lzx.LzxError) as error:
        sys.exit(f"xex.py: {error}")
