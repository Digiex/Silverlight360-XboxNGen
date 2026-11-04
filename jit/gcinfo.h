// GC info in the form xcoreclr's GcInfoDecoder reads (docs/RESEARCH.md, "What a JIT returns"; decoder 0x84EFB348,
// slot table 0x84EFBDF0, live slots 0x84EFB8B0). A bit stream taken from little-endian 32-bit words, least significant
// bit first; variable-length numbers are chunks of `base` bits each followed by a bit saying another chunk follows.
//
//   7 flag bits                      vararg, security object, GS cookie, PSPSym, generics context (2), stack base reg
//   code length / 4                  unsigned, 6-bit chunks
//   PSPSym offset / 4                signed, 4-bit chunks, from the caller's SP (flag 0x08)
//   stack base register - 31         unsigned, 2-bit chunks (flag 0x40)
//   outgoing argument area / 4       unsigned, 4-bit chunks (0x50 for a method with a frame)
//   safe point count                 unsigned, 2-bit chunks
//   interruptible range count        unsigned, 2-bit chunks
//   safe points                      each (return address offset / 2 - 1) in ceil(log2(code length / 2)) bits, sorted
//   register slot count              unsigned, 4-bit chunks
//   stack slot count                 unsigned, 4-bit chunks
//   register slots                   the register number (unsigned, 4-bit chunks) and 2 flag bits; after a slot
//                                    without flags: the increase in number - 1 (unsigned, 2-bit chunks), no flags
//   stack slots                      2-bit base (0: caller SP relative, 2: stack base register relative), then after
//                                    a slot with flags: the offset / 4
//                                    (signed, 4-bit chunks) and 2 flag bits (1 interior, 2 pinned); after a slot
//                                    without flags: the increase in offset / 4 (unsigned, 2-bit chunks), no flags
//   live slots                       one bit per slot at each safe point, the register slots first
//
// Interruptible ranges (fully interruptible code, which Microsoft's compiler makes of every method with exception
// handlers) follow as numbers of 2-byte units, then chunked liveness; this encoder makes only safe points.
#ifndef XNGEN_GCINFO_H
#define XNGEN_GCINFO_H

#include <stdint.h>
#include <string.h>

typedef struct {
	uint8_t *bytes;
	uint32_t bit;
	uint32_t capacity_bits;
} BitWriter;

static inline void bits_write(BitWriter *w, uint32_t value, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++, w->bit++)
		if (w->bit < w->capacity_bits && (value >> i & 1))
			w->bytes[w->bit >> 3] |= (uint8_t)(1u << (w->bit & 7));
}

static inline void bits_write_varlen(BitWriter *w, uint32_t value, uint32_t base)
{
	do {
		const uint32_t chunk = value & ((1u << base) - 1);
		value >>= base;
		bits_write(w, chunk | (value ? 1u << base : 0), base + 1);
	} while (value);
}

static inline void bits_write_signed(BitWriter *w, int32_t value, uint32_t base)
{
	// Chunks until the remaining value is all sign bits and the last chunk's top bit carries the sign.
	for (;;) {
		const uint32_t chunk = (uint32_t)value & ((1u << base) - 1);
		const int32_t rest = value >> base;
		const int last = (rest == 0 && !(chunk >> (base - 1) & 1)) || (rest == -1 && (chunk >> (base - 1) & 1));
		bits_write(w, chunk | (last ? 0 : 1u << base), base + 1);
		if (last)
			return;
		value = rest;
	}
}

typedef struct {
	int32_t offset;  // from the caller's stack pointer, or from the stack base register
	uint32_t flags;  // 1: interior pointer
} GcSlot;

enum { GC_SLOT_INTERIOR = 1 };

enum { GC_HAS_SECURITY_OBJECT = 0x02, GC_HAS_PSPSYM = 0x08, GC_HAS_STACK_BASE = 0x40 };
// The generics context (header bits 0x30): where shared generic code keeps its instantiation context. Microsoft's
// mscorlib.ni.dll: Dictionary`2::IsCompatibleKey flags 0x10, slot 20 (the MethodTable argument's home, r3's);
// String::Join<T> 0x20, slot 20 (the MethodDesc argument's); Dictionary`2::get_Values 0x30, slot -32 (a frame
// slot the prologue copies `this` to).
enum { GC_GENERICS_FROM_METHODTABLE = 0x10, GC_GENERICS_FROM_METHODDESC = 0x20, GC_GENERICS_FROM_THIS = 0x30 };

// Builds GC info into out (capacity bytes), returning its length. Slots are tracked: reg_slots (offset: the register
// number; Microsoft's methods list interior ones first, then the others, each ascending), then the stack slots;
// live_bits says which are live at each safe point (a row of live_stride bytes per safe point, bit i for slot i,
// counting the register slots first). safe_points are return address
// offsets, ascending. A nonzero stack_base (31) makes the slots relative to that register and declares a PSPSym at
// caller SP + 4, for a method with funclets. A nonzero security_object is the offset from the caller's SP of
// the frame's security object (the slot SecurityRuntime.Assert and the declarative checks use: Microsoft's
// AppDomain.CreateAppDomainManager, mscorlib.ni.dll 0x1D9330, has -416 with flag 2), valid from
// security_from to security_to, code offsets in bytes (GcInfoDecoder 0x84EFB348 reads (a + 1) * 2 and
// (words * 2 - b) * 2, both 3-bit chunks, before it).
static inline uint32_t gcinfo_build(uint8_t *out, uint32_t capacity, uint32_t code_bytes, uint32_t outgoing_bytes,
	const uint32_t *safe_points, const uint8_t *live_bits, uint32_t live_stride, uint32_t safe_point_count,
	const GcSlot *reg_slots, uint32_t reg_count, const GcSlot *slots, uint32_t slot_count, int stack_base, int32_t security_object, uint32_t security_from,
	uint32_t security_to, uint32_t generics_kind, int32_t generics_slot)
{
	// Only the bytes the stream can reach are cleared (the buffer is large; most methods' info is a few bytes).
	uint32_t bound = 64 + (safe_point_count * (32 + reg_count + slot_count) + (reg_count + slot_count) * 48) / 8 + 16;
	memset(out, 0, bound < capacity ? bound : capacity);
	BitWriter w = {out, 0, capacity * 8};
	bits_write(&w, (stack_base ? GC_HAS_PSPSYM | GC_HAS_STACK_BASE : 0) | (security_object ? GC_HAS_SECURITY_OBJECT : 0) |
		generics_kind, 7);
	bits_write_varlen(&w, code_bytes / 4, 6);
	if (security_object) {
		bits_write_varlen(&w, security_from / 2 - 1, 3);
		bits_write_varlen(&w, (code_bytes / 4) * 2 - security_to / 2, 3);
		bits_write_signed(&w, security_object / 4, 4);
	}
	if (stack_base)
		bits_write_signed(&w, 4 / 4, 4);
	if (generics_kind)
		bits_write_signed(&w, generics_slot / 4, 4);
	if (stack_base)
		bits_write_varlen(&w, (uint32_t)(stack_base - 31), 2);
	bits_write_varlen(&w, outgoing_bytes / 4, 4);
	bits_write_varlen(&w, safe_point_count, 2);
	bits_write_varlen(&w, 0, 2);
	uint32_t normalized = code_bytes / 2, safe_point_bits = 0;
	while ((1u << safe_point_bits) < normalized)
		safe_point_bits++;
	for (uint32_t i = 0; i < safe_point_count; i++)
		bits_write(&w, safe_points[i] / 2 - 1, safe_point_bits);
	bits_write_varlen(&w, reg_count, 4);
	bits_write_varlen(&w, slot_count, 4);
	uint32_t previous_flags = 1;
	for (uint32_t i = 0; i < reg_count; i++) {
		if (i && previous_flags == 0 && reg_slots[i].flags == 0 && reg_slots[i].offset > reg_slots[i - 1].offset) {
			bits_write_varlen(&w, (uint32_t)(reg_slots[i].offset - reg_slots[i - 1].offset - 1), 2);
		} else if (i && previous_flags == 0) {
			return 0; // out of the order above
		} else {
			bits_write_varlen(&w, (uint32_t)reg_slots[i].offset, 4);
			bits_write(&w, reg_slots[i].flags, 2);
			previous_flags = reg_slots[i].flags;
		}
	}
	previous_flags = 1;
	int32_t previous_offset = 0;
	for (uint32_t i = 0; i < slot_count; i++) {
		bits_write(&w, stack_base ? 2 : 0, 2);
		if (previous_flags == 0 && slots[i].flags == 0 && slots[i].offset >= previous_offset) {
			bits_write_varlen(&w, (uint32_t)(slots[i].offset - previous_offset) / 4, 2);
		} else if (previous_flags == 0) {
			// A slot after one without flags takes the delta form and keeps flags 0; callers order slots so this
			// doesn't happen (interior slots first, then the others in ascending order).
			return 0;
		} else {
			bits_write_signed(&w, slots[i].offset / 4, 4);
			bits_write(&w, slots[i].flags, 2);
			previous_flags = slots[i].flags;
		}
		previous_offset = slots[i].offset;
	}
	for (uint32_t s = 0; s < safe_point_count; s++)
		for (uint32_t i = 0; i < reg_count + slot_count; i++)
			bits_write(&w, live_bits[s * live_stride + i / 8] >> (i % 8) & 1, 1);
	// The shipped images end a leaf method's GC info with zeros; keep at least a few spare bits.
	w.bit += 15;
	const uint32_t bytes = (w.bit + 7) / 8;
	return bytes <= capacity ? bytes : 0;
}

#endif
