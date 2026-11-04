"""LZX decompression, as the Xbox 360 compresses an XEX's image ("normal" compression).

decompress(data, out_size, window_size) gives out_size bytes. The stream is 16-bit little-endian words read most
significant bit first, in 32 KB frames of verbatim, aligned-offset or uncompressed blocks, without resets
(the LZX format of Microsoft's CAB files, as xenia decodes an XEX with libmspack's lzxd:
xenia:src/xenia/cpu/lzx.cc:97-140).
"""

FRAME = 0x8000
MIN_MATCH = 2
NUM_CHARS = 256
PRIMARY_LENGTHS = 7
SECONDARY_LENGTHS = 249
PRETREE_SYMBOLS = 20
ALIGNED_SYMBOLS = 8
VERBATIM, ALIGNED, UNCOMPRESSED = 1, 2, 3

# Position slots per window size, from 2^15 to 2^25 bytes.
POSITION_SLOTS = {15: 30, 16: 32, 17: 34, 18: 36, 19: 38, 20: 42, 21: 50, 22: 66, 23: 98, 24: 162, 25: 290}
EXTRA_BITS = [0, 0, 0, 0] + [i // 2 - 1 for i in range(4, 36)] + [17] * (290 - 36)
POSITION_BASE = [0]
for _slot in range(1, 290):
    POSITION_BASE.append(POSITION_BASE[-1] + (1 << EXTRA_BITS[_slot - 1]))


class LzxError(Exception):
    pass


class Bits:
    """The bit stream: whole 16-bit words taken into a buffer as reads need them."""

    def __init__(self, data):
        self.data = data
        self.pos = 0
        self.buffer = 0
        self.left = 0

    def ensure(self, n):
        while self.left < n:
            word = self.data[self.pos] | self.data[self.pos + 1] << 8 if self.pos + 1 < len(self.data) else 0
            self.pos += 2
            self.buffer = (self.buffer << 16 | word) & 0xFFFFFFFFFFFF
            self.left += 16

    def peek(self, n):
        self.ensure(n)
        return self.buffer >> (self.left - n) & ((1 << n) - 1)

    def read(self, n):
        if n == 0:
            return 0
        value = self.peek(n)
        self.left -= n
        return value

    def align_word(self):
        """Drops what is left of the current 16-bit word (at a frame's end)."""
        if self.left > 0:
            self.ensure(16)
        self.left -= self.left & 15

    def to_bytes(self):
        """Leaves the bit stream for raw bytes (an uncompressed block): 1 to 16 bits are skipped to a word boundary."""
        if self.left == 0:
            self.ensure(16)
        self.left = 0
        self.buffer = 0

    def raw(self, n):
        out = self.data[self.pos:self.pos + n]
        self.pos += n
        return out


class Huffman:
    """A canonical code: codes in order of length, then of symbol, read most significant bit first."""

    def __init__(self, lengths):
        self.max = max(lengths) if lengths else 0
        self.table = None
        if not self.max:
            return
        bits = self.max
        table = [None] * (1 << bits)
        code = 0
        for length in range(1, bits + 1):
            for symbol, symbol_length in enumerate(lengths):
                if symbol_length != length:
                    continue
                entry = (symbol, length)
                shift = bits - length
                start = code << shift
                if start + (1 << shift) > len(table):
                    raise LzxError("over-subscribed code")
                for i in range(start, start + (1 << shift)):
                    table[i] = entry
                code += 1
            code <<= 1
        self.table = table

    def decode(self, bits):
        if self.table is None:
            raise LzxError("symbol from an empty code")
        entry = self.table[bits.peek(self.max)]
        if entry is None:
            raise LzxError("invalid code")
        bits.left -= entry[1]
        return entry[0]


def read_lengths(bits, lengths, first, last):
    """Updates lengths[first:last] from a pretree-coded delta list."""
    pretree = Huffman([bits.read(4) for _ in range(PRETREE_SYMBOLS)])
    x = first
    while x < last:
        z = pretree.decode(bits)
        if z == 17 or z == 18:
            run = bits.read(4) + 4 if z == 17 else bits.read(5) + 20
            value = 0
        elif z == 19:
            run = bits.read(1) + 4
            value = (lengths[x] - pretree.decode(bits)) % 17
        else:
            run, value = 1, (lengths[x] - z) % 17
        end = min(x + run, last)  # a run past the range is cut there
        lengths[x:end] = [value] * (end - x)
        x = end


def decompress(data, out_size, window_size):
    window_bits = window_size.bit_length() - 1
    if 1 << window_bits != window_size or window_bits not in POSITION_SLOTS:
        raise LzxError(f"window size {window_size:#x}")
    main_symbols = NUM_CHARS + POSITION_SLOTS[window_bits] * 8
    bits = Bits(data)
    out = bytearray()
    main_lengths = [0] * main_symbols
    length_lengths = [0] * SECONDARY_LENGTHS
    r0 = r1 = r2 = 1
    block_type = 0
    block_length = block_remaining = 0
    main = length = aligned = None
    intel_size = bits.read(32) if bits.read(1) else 0
    frames = []

    while len(out) < out_size:
        frame_start = len(out)
        frame_end = min(frame_start + FRAME, out_size)
        while len(out) < frame_end:
            if block_remaining == 0:
                if block_type == UNCOMPRESSED and block_length & 1:
                    bits.pos += 1  # an odd uncompressed block is padded to a word
                block_type = bits.read(3)
                block_length = block_remaining = bits.read(16) << 8 | bits.read(8)
                if block_type == ALIGNED:
                    aligned = Huffman([bits.read(3) for _ in range(ALIGNED_SYMBOLS)])
                if block_type in (VERBATIM, ALIGNED):
                    read_lengths(bits, main_lengths, 0, NUM_CHARS)
                    read_lengths(bits, main_lengths, NUM_CHARS, main_symbols)
                    main = Huffman(main_lengths)
                    read_lengths(bits, length_lengths, 0, SECONDARY_LENGTHS)
                    length = Huffman(length_lengths)
                elif block_type == UNCOMPRESSED:
                    bits.to_bytes()
                    stored = bits.raw(12)
                    r0, r1, r2 = (int.from_bytes(stored[i:i + 4], "little") for i in (0, 4, 8))
                else:
                    raise LzxError(f"block type {block_type}")
            run = min(block_remaining, frame_end - len(out))
            start = len(out)
            if block_type == UNCOMPRESSED:
                out += bits.raw(run)
            else:
                while len(out) - start < run:
                    element = main.decode(bits)
                    if element < NUM_CHARS:
                        out.append(element)
                        continue
                    element -= NUM_CHARS
                    match_length = element & PRIMARY_LENGTHS
                    if match_length == PRIMARY_LENGTHS:
                        match_length += length.decode(bits)
                    match_length += MIN_MATCH
                    slot = element >> 3
                    if slot == 0:
                        offset = r0
                    elif slot == 1:
                        offset, r1, r0 = r1, r0, r1
                    elif slot == 2:
                        offset, r2, r0 = r2, r0, r2
                    else:
                        extra = EXTRA_BITS[slot]
                        offset = POSITION_BASE[slot] - 2
                        if block_type == ALIGNED and extra >= 3:
                            if extra > 3:
                                offset += bits.read(extra - 3) << 3
                            offset += aligned.decode(bits)
                        else:
                            offset += bits.read(extra)
                        r2, r1, r0 = r1, r0, offset
                    if offset > len(out):
                        raise LzxError("match before the start of the stream")
                    source = len(out) - offset
                    if offset >= match_length:
                        out += out[source:source + match_length]
                    else:  # overlapping: the bytes repeat
                        for i in range(match_length):
                            out.append(out[source + i])
            done = len(out) - start
            if done > block_remaining:
                raise LzxError("match past the end of a block")
            block_remaining -= done
        if len(out) != frame_end:
            raise LzxError("frame overrun")
        bits.align_word()
        frames.append((frame_start, frame_end))
    if intel_size:
        # Translated in a copy: the window the matches copy from keeps the bytes as coded.
        out = bytearray(out)
        for start, end in frames:
            if end - start > 10:
                undo_e8(out, start, end, intel_size)
    return bytes(out[:out_size])


def undo_e8(out, start, end, size):
    """Undoes the x86 call translation of a frame (never seen in an XEX; kept for the format's sake)."""
    i = start
    while i < end - 10:
        if out[i] != 0xE8:
            i += 1
            continue
        absolute = int.from_bytes(out[i + 1:i + 5], "little", signed=True)
        if -i <= absolute < size:
            relative = absolute - i if absolute >= 0 else absolute + size
            out[i + 1:i + 5] = (relative & 0xFFFFFFFF).to_bytes(4, "little")
        i += 5
