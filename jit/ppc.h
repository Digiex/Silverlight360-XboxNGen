// PowerPC instruction encoders. Xenon is a 64-bit PowerPC; managed code here keeps pointers and int32 in the low 32
// bits of 64-bit registers and int64 in a whole register, as Microsoft's compiled code does.
#ifndef XNGEN_PPC_H
#define XNGEN_PPC_H

#include <stdint.h>

#define PPC_D(op, rt, ra, d) ((uint32_t)(op) << 26 | (uint32_t)(rt) << 21 | (uint32_t)(ra) << 16 | ((uint32_t)(d) & 0xFFFF))
#define PPC_X(op, rt, ra, rb, xo, rc) \
	((uint32_t)(op) << 26 | (uint32_t)(rt) << 21 | (uint32_t)(ra) << 16 | (uint32_t)(rb) << 11 | (uint32_t)(xo) << 1 | (rc))
#define PPC_XO(rt, ra, rb, oe, xo) \
	(31u << 26 | (uint32_t)(rt) << 21 | (uint32_t)(ra) << 16 | (uint32_t)(rb) << 11 | (uint32_t)(oe) << 10 | (uint32_t)(xo) << 1)
#define PPC_A(op, frt, fra, frb, frc, xo) \
	((uint32_t)(op) << 26 | (uint32_t)(frt) << 21 | (uint32_t)(fra) << 16 | (uint32_t)(frb) << 11 | (uint32_t)(frc) << 6 | (uint32_t)(xo) << 1)

// Loads and stores (D-form; ld/std are DS-form, offsets a multiple of 4).
static inline uint32_t ppc_lwz(int rt, int d, int ra) { return PPC_D(32, rt, ra, d); }
static inline uint32_t ppc_lbz(int rt, int d, int ra) { return PPC_D(34, rt, ra, d); }
static inline uint32_t ppc_lhz(int rt, int d, int ra) { return PPC_D(40, rt, ra, d); }
static inline uint32_t ppc_lha(int rt, int d, int ra) { return PPC_D(42, rt, ra, d); }
static inline uint32_t ppc_lfs(int ft, int d, int ra) { return PPC_D(48, ft, ra, d); }
static inline uint32_t ppc_lfd(int ft, int d, int ra) { return PPC_D(50, ft, ra, d); }
static inline uint32_t ppc_stw(int rs, int d, int ra) { return PPC_D(36, rs, ra, d); }
static inline uint32_t ppc_stb(int rs, int d, int ra) { return PPC_D(38, rs, ra, d); }
static inline uint32_t ppc_sth(int rs, int d, int ra) { return PPC_D(44, rs, ra, d); }
static inline uint32_t ppc_stfs(int fs, int d, int ra) { return PPC_D(52, fs, ra, d); }
static inline uint32_t ppc_stfd(int fs, int d, int ra) { return PPC_D(54, fs, ra, d); }
static inline uint32_t ppc_ld(int rt, int d, int ra) { return PPC_D(58, rt, ra, d & ~3); }
static inline uint32_t ppc_std(int rs, int d, int ra) { return PPC_D(62, rs, ra, d & ~3); }
static inline uint32_t ppc_stwu(int rs, int d, int ra) { return PPC_D(37, rs, ra, d); }

// Immediates.
static inline uint32_t ppc_addi(int rt, int ra, int si) { return PPC_D(14, rt, ra, si); }
static inline uint32_t ppc_addis(int rt, int ra, int si) { return PPC_D(15, rt, ra, si); }
static inline uint32_t ppc_mulli(int rt, int ra, int si) { return PPC_D(7, rt, ra, si); }
static inline uint32_t ppc_li(int rt, int si) { return ppc_addi(rt, 0, si); }
static inline uint32_t ppc_lis(int rt, int si) { return ppc_addis(rt, 0, si); }
static inline uint32_t ppc_ori(int ra, int rs, int ui) { return PPC_D(24, rs, ra, ui); }
static inline uint32_t ppc_oris(int ra, int rs, int ui) { return PPC_D(25, rs, ra, ui); }
static inline uint32_t ppc_cmpwi(int cr, int ra, int si) { return PPC_D(11, cr << 2, ra, si); }
static inline uint32_t ppc_cmpdi(int cr, int ra, int si) { return PPC_D(11, cr << 2 | 1, ra, si); }
static inline uint32_t ppc_cmplwi(int cr, int ra, int ui) { return PPC_D(10, cr << 2, ra, ui); }

// Register arithmetic.
static inline uint32_t ppc_add(int rt, int ra, int rb) { return PPC_XO(rt, ra, rb, 0, 266); }
static inline uint32_t ppc_subf(int rt, int ra, int rb) { return PPC_XO(rt, ra, rb, 0, 40); } // rt = rb - ra
static inline uint32_t ppc_mullw(int rt, int ra, int rb) { return PPC_XO(rt, ra, rb, 0, 235); }
static inline uint32_t ppc_mulld(int rt, int ra, int rb) { return PPC_XO(rt, ra, rb, 0, 233); }
static inline uint32_t ppc_mulhd(int rt, int ra, int rb) { return PPC_XO(rt, ra, rb, 0, 73); }
static inline uint32_t ppc_mulhdu(int rt, int ra, int rb) { return PPC_XO(rt, ra, rb, 0, 9); }
static inline uint32_t ppc_divw(int rt, int ra, int rb) { return PPC_XO(rt, ra, rb, 0, 491); }
static inline uint32_t ppc_divwu(int rt, int ra, int rb) { return PPC_XO(rt, ra, rb, 0, 459); }
static inline uint32_t ppc_divd(int rt, int ra, int rb) { return PPC_XO(rt, ra, rb, 0, 489); }
static inline uint32_t ppc_divdu(int rt, int ra, int rb) { return PPC_XO(rt, ra, rb, 0, 457); }
static inline uint32_t ppc_neg(int rt, int ra) { return PPC_XO(rt, ra, 0, 0, 104); }
static inline uint32_t ppc_and(int ra, int rs, int rb) { return PPC_X(31, rs, ra, rb, 28, 0); }
static inline uint32_t ppc_or(int ra, int rs, int rb) { return PPC_X(31, rs, ra, rb, 444, 0); }
static inline uint32_t ppc_xor(int ra, int rs, int rb) { return PPC_X(31, rs, ra, rb, 316, 0); }
static inline uint32_t ppc_nor(int ra, int rs, int rb) { return PPC_X(31, rs, ra, rb, 124, 0); }
static inline uint32_t ppc_mr(int ra, int rs) { return ppc_or(ra, rs, rs); }
static inline uint32_t ppc_slw(int ra, int rs, int rb) { return PPC_X(31, rs, ra, rb, 24, 0); }
static inline uint32_t ppc_srw(int ra, int rs, int rb) { return PPC_X(31, rs, ra, rb, 536, 0); }
static inline uint32_t ppc_sraw(int ra, int rs, int rb) { return PPC_X(31, rs, ra, rb, 792, 0); }
static inline uint32_t ppc_sld(int ra, int rs, int rb) { return PPC_X(31, rs, ra, rb, 27, 0); }
static inline uint32_t ppc_srd(int ra, int rs, int rb) { return PPC_X(31, rs, ra, rb, 539, 0); }
static inline uint32_t ppc_srad(int ra, int rs, int rb) { return PPC_X(31, rs, ra, rb, 794, 0); }
static inline uint32_t ppc_extsb(int ra, int rs) { return PPC_X(31, rs, ra, 0, 954, 0); }
static inline uint32_t ppc_extsh(int ra, int rs) { return PPC_X(31, rs, ra, 0, 922, 0); }
static inline uint32_t ppc_extsw(int ra, int rs) { return PPC_X(31, rs, ra, 0, 986, 0); }
static inline uint32_t ppc_cmpw(int cr, int ra, int rb) { return PPC_X(31, cr << 2, ra, rb, 0, 0); }
static inline uint32_t ppc_cmplw(int cr, int ra, int rb) { return PPC_X(31, cr << 2, ra, rb, 32, 0); }
static inline uint32_t ppc_cmpd(int cr, int ra, int rb) { return PPC_X(31, cr << 2 | 1, ra, rb, 0, 0); }
static inline uint32_t ppc_cmpld(int cr, int ra, int rb) { return PPC_X(31, cr << 2 | 1, ra, rb, 32, 0); }
// rlwinm ra, rs, sh, mb, me
static inline uint32_t ppc_rlwinm(int ra, int rs, int sh, int mb, int me)
{
	return 21u << 26 | (uint32_t)rs << 21 | (uint32_t)ra << 16 | (uint32_t)sh << 11 | (uint32_t)mb << 6 | (uint32_t)me << 1;
}
static inline uint32_t ppc_clrlwi(int ra, int rs, int n) { return ppc_rlwinm(ra, rs, 0, n, 31); }
// rldicl ra, rs, 0, 32: zero-extends the low word.
static inline uint32_t ppc_clrldi32(int ra, int rs) { return 30u << 26 | (uint32_t)rs << 21 | (uint32_t)ra << 16 | 1u << 5; }
// sldi ra,rs,32 (rldicr ra,rs,32,31) and rotldi ra,rs,32 (rldicl ra,rs,32,0).
static inline uint32_t ppc_sldi32(int ra, int rs) { return 30u << 26 | (uint32_t)rs << 21 | (uint32_t)ra << 16 | 62u << 5 | 1u << 2 | 1u << 1; }
// rldicl ra, rs, sh, mb: rotates left by sh and clears the bits left of mb (keeps the low 64 - mb).
static inline uint32_t ppc_rldicl(int ra, int rs, int sh, int mb)
{
	return 30u << 26 | (uint32_t)rs << 21 | (uint32_t)ra << 16 | (uint32_t)(sh & 31) << 11 |
		(uint32_t)((mb & 31) << 1 | mb >> 5) << 5 | (uint32_t)(sh >> 5) << 1;
}
static inline uint32_t ppc_rotldi32(int ra, int rs) { return 30u << 26 | (uint32_t)rs << 21 | (uint32_t)ra << 16 | 1u << 1; }

// Floating point.
static inline uint32_t ppc_fadd(int t, int a, int b) { return PPC_A(63, t, a, b, 0, 21); }
static inline uint32_t ppc_fsub(int t, int a, int b) { return PPC_A(63, t, a, b, 0, 20); }
static inline uint32_t ppc_fmul(int t, int a, int c) { return PPC_A(63, t, a, 0, c, 25); }
static inline uint32_t ppc_fdiv(int t, int a, int b) { return PPC_A(63, t, a, b, 0, 18); }
static inline uint32_t ppc_fneg(int t, int b) { return PPC_X(63, t, 0, b, 40, 0); }
static inline uint32_t ppc_fabs(int t, int b) { return PPC_X(63, t, 0, b, 264, 0); }
static inline uint32_t ppc_fsel(int t, int a, int b, int c) { return PPC_A(63, t, a, b, c, 23); }  // a >= 0 ? c : b
static inline uint32_t ppc_fsqrt(int t, int b) { return PPC_A(63, t, 0, b, 0, 22); }
static inline uint32_t ppc_fsqrts(int t, int b) { return PPC_A(59, t, 0, b, 0, 22); }
static inline uint32_t ppc_fres(int t, int b) { return PPC_A(59, t, 0, b, 0, 24); }
static inline uint32_t ppc_frsqrte(int t, int b) { return PPC_A(63, t, 0, b, 0, 26); }
static inline uint32_t ppc_fctiw(int t, int b) { return PPC_X(63, t, 0, b, 14, 0); }
static inline uint32_t ppc_fctid(int t, int b) { return PPC_X(63, t, 0, b, 814, 0); }
static inline uint32_t ppc_frin(int t, int b) { return PPC_X(63, t, 0, b, 392, 0); }
static inline uint32_t ppc_frsp(int t, int b) { return PPC_X(63, t, 0, b, 12, 0); }
static inline uint32_t ppc_fmr(int t, int b) { return PPC_X(63, t, 0, b, 72, 0); }
static inline uint32_t ppc_fcmpu(int cr, int a, int b) { return PPC_X(63, cr << 2, a, b, 0, 0); }
static inline uint32_t ppc_fctiwz(int t, int b) { return PPC_X(63, t, 0, b, 15, 0); }
static inline uint32_t ppc_fctidz(int t, int b) { return PPC_X(63, t, 0, b, 815, 0); }
static inline uint32_t ppc_fcfid(int t, int b) { return PPC_X(63, t, 0, b, 846, 0); }

// Branches.
static inline uint32_t ppc_b(int32_t offset) { return 18u << 26 | ((uint32_t)offset & 0x03FFFFFC); }
static inline uint32_t ppc_bl(int32_t offset) { return ppc_b(offset) | 1; }
static inline uint32_t ppc_bc(int bo, int bi, int32_t offset) { return 16u << 26 | (uint32_t)bo << 21 | (uint32_t)bi << 16 | ((uint32_t)offset & 0xFFFC); }
static inline uint32_t ppc_cror(int bt, int ba, int bb) { return PPC_X(19, bt, ba, bb, 449, 0); }
static inline uint32_t ppc_mtctr(int rs) { return 31u << 26 | (uint32_t)rs << 21 | 9u << 16 | 467u << 1; }
static inline uint32_t ppc_mflr(int rt) { return 31u << 26 | (uint32_t)rt << 21 | 8u << 16 | 339u << 1; }
static inline uint32_t ppc_mtlr(int rs) { return 31u << 26 | (uint32_t)rs << 21 | 8u << 16 | 467u << 1; }

#define PPC_BLR 0x4E800020u
#define PPC_LWSYNC 0x7C2004ACu
static inline uint32_t ppc_srawi(int ra, int rs, int sh) { return PPC_X(31, rs, ra, sh, 824, 0); }
static inline uint32_t ppc_stwux(int rs, int ra, int rb) { return PPC_X(31, rs, ra, rb, 183, 0); }
static inline uint32_t ppc_lwzu(int rt, int d, int ra) { return PPC_D(33, rt, ra, d); }
static inline uint32_t ppc_bdnz(int32_t offset) { return ppc_bc(16, 0, offset); }
#define PPC_BCTRL 0x4E800421u
#define PPC_TRAP 0x7FE00008u  // tw 31,0,0
#define PPC_BCTR 0x4E800420u
#define PPC_NOP 0x60000000u

// Condition register bits within a field.
enum { CR_LT = 0, CR_GT = 1, CR_EQ = 2, CR_SO = 3 };
// BO values: branch if the bit is set / clear.
enum { BO_TRUE = 12, BO_FALSE = 4 };

#endif
