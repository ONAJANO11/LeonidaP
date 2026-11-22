; s_bfe_u64 / s_bfe_i64 on an SGPR pair that holds 64-bit data (two s_mov_b32 halves, not a lane mask).
; RDNA2 ISA: D.u64 = (S0.u64 >> S1[5:0]) & ((1 << S1[22:16]) - 1); the i64 form sign-extends the field from its
; top bit. S0 = 0xf0e1d2c3_89abcdef (s1:s0).
; s_bfe_u64, S1 = 0x00280004 (offset 4, width 40): S0 >> 4 = 0x0f0e1d2c_389abcde, low 40 bits = 0x2c_389abcde,
;   so D = 0x0000002c_389abcde: lo 0x389abcde, hi 0x0000002c. The field crosses bit 32 of the source.
; s_bfe_i64, S1 = 0x00240014 (offset 20, width 36): S0 >> 20 = 0x00000f0e_1d2c389a, low 36 bits = 0xe_1d2c389a,
;   bit 35 (top of the field) = 1, so D = 0xfffffffe_1d2c389a: lo 0x1d2c389a, hi 0xfffffffe.
; A swapped pair gives other low words; a 32-bit extract leaves hi 0 (u64) or the low word's sign (i64).
; Expected mrt0 bits: 0x389abcde, 0x0000002c, 0x1d2c389a, 0xfffffffe
s_mov_b32 s0, 0x89abcdef
s_mov_b32 s1, 0xf0e1d2c3
s_bfe_u64 s[2:3], s[0:1], 0x280004
s_bfe_i64 s[4:5], s[0:1], 0x240014
v_mov_b32_e32 v0, s2
v_mov_b32_e32 v1, s3
v_mov_b32_e32 v2, s4
v_mov_b32_e32 v3, s5
exp mrt0 v0, v1, v2, v3 done vm
s_endpgm
