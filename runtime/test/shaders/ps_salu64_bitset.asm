; s_bitset0_b64 / s_bitset1_b64 on an SGPR pair that holds 64-bit data (two s_mov_b32 halves, not a lane mask).
; RDNA2 ISA: D.u64[S0.u[5:0]] = 0 (bitset0) or 1 (bitset1); D is both source and destination. Both pairs start
; as 0xfedcba98_76543210 (hi:lo).
; s_bitset0_b64 s[0:1], 0x7d: 0x7d & 63 = 61 = bit 29 of the high word, set in 0xfedcba98 (0xf = 1111), so
;   hi = 0xdedcba98 and lo stays 0x76543210. Indexing with & 31 would clear bit 29 of lo instead (0x56543210).
; s_bitset1_b64 s[2:3], s4 with s4 = 98: 98 & 63 = 34 = bit 2 of the high word, clear in 0xfedcba98 (0x8 = 1000),
;   so hi = 0xfedcba9c and lo stays 0x76543210. Indexing with & 31 would set bit 2 of lo instead (0x76543214).
; Expected mrt0 bits: 0x76543210, 0xdedcba98, 0x76543210, 0xfedcba9c
s_mov_b32 s0, 0x76543210
s_mov_b32 s1, 0xfedcba98
s_mov_b32 s2, 0x76543210
s_mov_b32 s3, 0xfedcba98
s_movk_i32 s4, 0x62
s_bitset0_b64 s[0:1], 0x7d
s_bitset1_b64 s[2:3], s4
v_mov_b32_e32 v0, s0
v_mov_b32_e32 v1, s1
v_mov_b32_e32 v2, s2
v_mov_b32_e32 v3, s3
exp mrt0 v0, v1, v2, v3 done vm
s_endpgm
