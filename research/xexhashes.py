"""Checks the page digest chain of an unencrypted XEX against its memory image, as the console's hypervisor does.

    python xexhashes.py <file.xex> <memory image from xex.py -b>

The security info holds the image digest and the page descriptors (a count at +0x180, then 24 bytes each from
+0x184: count << 4 | kind, then a SHA-1). The image digest is SHA-1 over the first run's pages and its descriptor,
each descriptor's digest the same over the next run's; the last descriptor's digest is zero. xenia checks none of it.
"""
import hashlib
import struct
import sys

xex, image = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
security = struct.unpack_from('>I', xex, 0x10)[0]
image_size = struct.unpack_from('>I', xex, security + 4)[0]
info_size, flags, base = struct.unpack_from('>III', xex, security + 0x108)
image_digest = xex[security + 0x114:security + 0x128]
count = struct.unpack_from('>I', xex, security + 0x180)[0]
page = 0x1000 if flags & 0x10000000 else 0x10000
print('image size %x, flags %08x, base %08x, %d descriptors, %s pages' % (image_size, flags, base, count,
                                                                         '4 KB' if page == 0x1000 else '64 KB'))
descriptors = [xex[security + 0x184 + 24 * i:security + 0x184 + 24 * (i + 1)] for i in range(count)]
expected = image_digest
offset = 0
bad = 0
for i, d in enumerate(descriptors):
    info = struct.unpack_from('>I', d)[0]
    pages = info >> 4
    data = image[offset:offset + pages * page]
    actual = hashlib.sha1(data + d).digest()
    if actual != expected:
        bad += 1
        if bad <= 5:
            print('descriptor %d (offset %x, %d pages, kind %x): digest mismatch' % (i, offset, pages, info & 15))
    expected = d[4:]
    offset += pages * page
print('covered %x of %x bytes; %d of %d descriptors fail' % (offset, len(image), bad, count))
