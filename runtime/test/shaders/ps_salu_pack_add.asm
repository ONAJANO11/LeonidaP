; Scalar ops the translator handles that only CPU tests touched: s_pack_ll/lh/hh_b32_b16, s_sext_i32_i8/i16,
; s_lshl1..4_add_u32 (SCC), s_absdiff_i32 (SCC), s_mul_hi_i32, s_cmov_b32, s_cmovk_i32. Derived from the RDNA2 ISA
; pseudocode; s40 = 0 sets SCC with s_cmp_eq_u32 s40, 0 (SCC 1) or s_cmp_lg_u32 s40, 0 (SCC 0), always the opposite
; of what the next op must write, so an op that leaves SCC alone shows up.
;
; word 0 (packs and cmov). s_pack_xy: D = {S1 half y, S0 half x}, l = bits 15:0, h = bits 31:16.
;   S0 = 0x00020001, S1 = 0x00080004 (every half a different bit):
;   ll = 0x00040001, lh = 0x00080001, hh = 0x00080002.
;   s_cmov_b32 (D = SCC ? S0 : D): s20 = 1, SCC 1, cmov 2 -> 2; s21 = 1, SCC 0, cmov 2 -> 1.
;   s_cmovk_i32 s22 = 9, SCC 0, cmovk 3 -> 9.
;   word0 = ll | lh << 4 | hh << 8 | (s20 | s21 << 2) << 12 | s22 << 28
;         = 0x00040001 | 0x00800010 | 0x08000200 | 0x6000 | 0x90000000 = 0x98846211
;   (cmov ignoring SCC: nibble 3 = 0xa or 5; cmovk moving on SCC 0: top nibble 3.)
;
; word 1 (sign extension; D = S0 bits 7:0 or 15:0 sign-extended):
;   a = s_sext_i32_i8(0x000001f6)  = 0xfffffff6   (bit 8 set must be dropped)
;   b = s_sext_i32_i8(0xffffff7b)  = 0x0000007b   (upper ones must be dropped)
;   c = s_sext_i32_i16(0xffff7ff3) = 0x00007ff3
;   d = s_sext_i32_i16(0x00018005) = 0xffff8005
;   word1 = s_pack_lh(a, d) ^ s_pack_lh(b, c) = 0xfffffff6 ^ 0x0000007b = 0xffffff8d
;
; word 2 (shift-add; D = (S0 << N) + S1, SCC = carry out of bit 31; no operand shifts out a set bit, so a 32- or
; 64-bit shift gives the same answer). byte = D | SCC << 7:
;   lshl1(3, 16)          = 22   = 0x16, SCC 0 -> 0x16
;   lshl2(0x3fffffff, 5)  = 0xfffffffc + 5 = 0x1_00000001 -> 1, SCC 1 -> 0x81
;   lshl3(5, 10)          = 50   = 0x32, SCC 0 -> 0x32
;   lshl4(0x0fffffff, 19) = 0xfffffff0 + 0x13 = 0x1_00000003 -> 3, SCC 1 -> 0x83
;   word2 = 0x83328116
;
; word 3 (s_absdiff_i32: D = S0 - S1 wrapped to 32 bits, negated if negative as a signed value; SCC = D != 0.
;   s_mul_hi_i32: bits 63:32 of the signed product. s_cmovk_i32: the 16-bit immediate sign-extended.)
;   absdiff(-3, 5)                   = |0xfffffff8| = 8, SCC 1   -> nibble 0 = 8, bit 16
;   absdiff(7, 7)                    = 0, SCC 0                  -> nibble 1 = 0, bit 17 clear
;   absdiff(0x7fffffff, 0x80000000)  = |0xffffffff| = 1, SCC 1   -> nibble 2 = 1, bit 18
;   absdiff(9, -2)                   = 11, SCC 1                 -> nibble 3 = 0xb, bit 19
;   mul_hi_i32(0xc0000000, -12) = hi(-2^30 * -12 = 3 * 2^32) = 3          -> bits 23:20 = 3 (unsigned: 0xbffffff7)
;   mul_hi_i32(0xc0000000, 12)  = hi(-3 * 2^32) = 0xfffffffd, low nibble d -> bits 27:24 = 0xd (unsigned: 9)
;   s33 = 5, SCC 1, cmovk 0xfff8 -> 0xfffffff8, bits 31:28 = 0xf            -> bits 31:28 = 0xf
;   word3 = 0xf0000000 | 0x0d000000 | 0x00300000 | 0x000d0000 | 0xb108 = 0xfd3db108
; Expected mrt0 bits: 0x98846211, 0xffffff8d, 0x83328116, 0xfd3db108
s_mov_b32 s40, 0
s_mov_b32 s0, 0x20001
s_mov_b32 s1, 0x80004
s_pack_ll_b32_b16 s2, s0, s1
s_pack_lh_b32_b16 s3, s0, s1
s_pack_hh_b32_b16 s4, s0, s1
s_mov_b32 s20, 1
s_mov_b32 s21, 1
s_mov_b32 s22, 9
s_cmp_eq_u32 s40, 0
s_cmov_b32 s20, 2
s_cmp_lg_u32 s40, 0
s_cmov_b32 s21, 2
s_cmovk_i32 s22, 3
s_lshl_b32 s3, s3, 4
s_lshl_b32 s4, s4, 8
s_lshl_b32 s21, s21, 2
s_or_b32 s20, s20, s21
s_lshl_b32 s20, s20, 12
s_lshl_b32 s22, s22, 28
s_or_b32 s2, s2, s3
s_or_b32 s2, s2, s4
s_or_b32 s2, s2, s20
s_or_b32 s2, s2, s22
s_sext_i32_i8 s5, 0x1f6
s_sext_i32_i8 s6, 0xffffff7b
s_sext_i32_i16 s7, 0xffff7ff3
s_sext_i32_i16 s8, 0x18005
s_pack_lh_b32_b16 s9, s5, s8
s_pack_lh_b32_b16 s10, s6, s7
s_xor_b32 s9, s9, s10
s_cmp_eq_u32 s40, 0
s_lshl1_add_u32 s11, 3, 16
s_cselect_b32 s12, 0x80, 0
s_mov_b32 s13, 0x3fffffff
s_cmp_lg_u32 s40, 0
s_lshl2_add_u32 s13, s13, 5
s_cselect_b32 s14, 0x80, 0
s_cmp_eq_u32 s40, 0
s_lshl3_add_u32 s15, 5, 10
s_cselect_b32 s16, 0x80, 0
s_mov_b32 s17, 0xfffffff
s_cmp_lg_u32 s40, 0
s_lshl4_add_u32 s17, s17, 19
s_cselect_b32 s18, 0x80, 0
s_or_b32 s11, s11, s12
s_or_b32 s13, s13, s14
s_or_b32 s15, s15, s16
s_or_b32 s17, s17, s18
s_lshl_b32 s13, s13, 8
s_lshl_b32 s15, s15, 16
s_lshl_b32 s17, s17, 24
s_or_b32 s11, s11, s13
s_or_b32 s11, s11, s15
s_or_b32 s11, s11, s17
s_cmp_lg_u32 s40, 0
s_absdiff_i32 s23, -3, 5
s_cselect_b32 s24, 1, 0
s_cmp_eq_u32 s40, 0
s_absdiff_i32 s25, 7, 7
s_cselect_b32 s26, 2, 0
s_mov_b32 s27, 0x7fffffff
s_cmp_lg_u32 s40, 0
s_absdiff_i32 s27, s27, 0x80000000
s_cselect_b32 s28, 4, 0
s_cmp_lg_u32 s40, 0
s_absdiff_i32 s29, 9, -2
s_cselect_b32 s30, 8, 0
s_mul_hi_i32 s31, 0xc0000000, -12
s_mul_hi_i32 s32, 0xc0000000, 12
s_mov_b32 s33, 5
s_cmp_eq_u32 s40, 0
s_cmovk_i32 s33, 0xfff8
s_lshl_b32 s25, s25, 4
s_lshl_b32 s27, s27, 8
s_lshl_b32 s29, s29, 12
s_or_b32 s24, s24, s26
s_or_b32 s24, s24, s28
s_or_b32 s24, s24, s30
s_lshl_b32 s24, s24, 16
s_and_b32 s31, s31, 15
s_lshl_b32 s31, s31, 20
s_and_b32 s32, s32, 15
s_lshl_b32 s32, s32, 24
s_and_b32 s33, s33, 0xf0000000
s_or_b32 s23, s23, s25
s_or_b32 s23, s23, s27
s_or_b32 s23, s23, s29
s_or_b32 s23, s23, s24
s_or_b32 s23, s23, s31
s_or_b32 s23, s23, s32
s_or_b32 s23, s23, s33
v_mov_b32_e32 v0, s2
v_mov_b32_e32 v1, s9
v_mov_b32_e32 v2, s11
v_mov_b32_e32 v3, s23
exp mrt0 v0, v1, v2, v3 done vm
s_endpgm
