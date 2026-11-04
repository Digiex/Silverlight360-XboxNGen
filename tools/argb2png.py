"""Makes a PNG of an app's own screenshot (the SDK's samples\\Common\\Screenshot.cs, Silverdash360's ui.screenshot):
rows of four bytes a pixel, alpha, red, green and blue, premultiplied, the size given separately.

    python argb2png.py screenshot.argb 1280 720 out.png

The alpha is dropped (an app's root is opaque, so the colours are as drawn).
"""
import struct
import sys
import zlib


def png(path, width, height, rgb):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)
    stride = width * 3
    rows = b''.join(b'\0' + rgb[y * stride:(y + 1) * stride] for y in range(height))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(rows, 6)))
        f.write(chunk(b'IEND', b''))


def main():
    source, width, height, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    argb = open(source, 'rb').read()
    if len(argb) != width * height * 4:
        sys.exit('%s: %d bytes, not %dx%d pixels' % (source, len(argb), width, height))
    rgb = bytearray(width * height * 3)
    rgb[0::3] = argb[1::4]
    rgb[1::3] = argb[2::4]
    rgb[2::3] = argb[3::4]
    png(out, width, height, bytes(rgb))


if __name__ == '__main__':
    main()
