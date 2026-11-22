; s_brev_b64, s_flbit_i32_b64 and s_flbit_i32_i64 on SGPR pairs that hold 64-bit data (s_mov_b32 halves, not lane
; masks). RDNA2 ISA:
; s_brev_b64: D.u64[63:0] = S0.u64[0:63]. S0 = 0x0000f00d_12345678 (hi:lo). D.lo = reverse(S0.hi) =
;   reverse(0x0000f00d): bits 0,2,3,12-15 go to 31,29,28,19-16 = 0xb00f0000; D.hi = reverse(0x12345678) = nibbles
;   8,7,6,5,4,3,2,1 each reversed (1,e,6,a,2,c,4,8) = 0x1e6a2c48.
; s_flbit_i32_b64: count of zeros from bit 63 down to the first 1, -1 if none. S0 = 0x00000000_00012345: the high
;   word is 0 (32) and the top 1 of the low word is bit 16 (15 more), so D = 47.
; s_flbit_i32_i64: count of bits from bit 63 down to the first one that differs from the sign bit, -1 if none.
;   S0 = 0xffffffff_fff0abcd: negative, so this is the zero count of ~S0 = 0x00000000_000f5432: 32 + 12 = 44.
; Counting one word alone gives 15 and 12 (low word) or -1 and -1 (high word); swapped brev halves give the
;   two words the other way round.
; Expected mrt0 bits: 0xb00f0000, 0x1e6a2c48, 47, 44
s_mov_b32 s0, 0x12345678
s_mov_b32 s1, 0xf00d
s_brev_b64 s[2:3], s[0:1]
s_mov_b32 s4, 0x00012345
s_mov_b32 s5, 0
s_flbit_i32_b64 s6, s[4:5]
s_mov_b32 s8, 0xfff0abcd
s_mov_b32 s9, -1
s_flbit_i32_i64 s7, s[8:9]
v_mov_b32_e32 v0, s2
v_mov_b32_e32 v1, s3
v_mov_b32_e32 v2, s6
v_mov_b32_e32 v3, s7
exp mrt0 v0, v1, v2, v3 done vm
s_endpgm
