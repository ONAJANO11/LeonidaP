; v_mad_i64_i32 with negative operands: the 64-bit add across the dword boundary and the sdst bit, run once on a GPU.
; RDNA2 ISA: D.i64 = S0.i32 * S1.i32 + S2.i64, sdst = bit 64 of the exact 65-bit two's-complement sum (docs/design.md).
; The lanes all compute the same, so sdst is 0 or 0xFFFFFFFF; out[6] packs bit n = (case n's sdst & 1). Cases:
;   A: -3 * 7 + 0x10 = -21 + 16 = -5 = 0xFFFFFFFF_FFFFFFFB; negative sum, sdst 1
;   B: 0x7FFFFFFF * 0x7FFFFFFF = 0x3FFFFFFF_00000001, + 0xFFFFFFFF = 0x40000000_00000000 (carry into the high dword); sdst 0
;   C: -65536 * -65536 = 2^32 = 0x1_00000000, + (-1) = 0x0_FFFFFFFF; sdst 0
;   D: -2^31 * 0x7FFFFFFF = -(2^62 - 2^31) = 0xC0000000_80000000, + INT64_MIN (0x80000000_00000000): the exact sum is
;      negative, so sdst 1, while the 64-bit result wraps to 0x40000000_80000000 (sdst is the sign, not an overflow flag)
;   E: -1 * -2^31 = 2^31, + 0x80000000 = 0x1_00000000; sdst 0
;   out[0] = A lo = 0xFFFFFFFB       out[1] = A hi = 0xFFFFFFFF
;   out[2] = B lo ^ hi = 0x40000000  out[3] = C lo ^ hi = 0xFFFFFFFF
;   out[4] = D lo = 0x80000000       out[5] = D hi = 0x40000000
;   out[6] = sdst bits A..E = 0b01001 = 9
;   out[7] = E lo ^ hi = 0 ^ 1 = 1
; group: 32,1,1
; Run: vk_compute_test cs_w32_valu64_mad_i64.spv 1 dump -> out[0..7] = 4294967291 4294967295 1073741824 4294967295 2147483648 1073741824 9 1
; mattr: +wavefrontsize32,-wavefrontsize64
v_mov_b32_e32 v4, 0xfffffffd
v_mov_b32_e32 v5, 0x7
v_mov_b32_e32 v6, 0x10
v_mov_b32_e32 v7, 0x0
v_mad_i64_i32 v[10:11], s20, v4, v5, v[6:7]
v_mov_b32_e32 v4, 0x7fffffff
v_mov_b32_e32 v5, 0x7fffffff
v_mov_b32_e32 v6, 0xffffffff
v_mov_b32_e32 v7, 0x0
v_mad_i64_i32 v[12:13], s21, v4, v5, v[6:7]
v_mov_b32_e32 v4, 0xffff0000
v_mov_b32_e32 v5, 0xffff0000
v_mov_b32_e32 v6, 0xffffffff
v_mov_b32_e32 v7, 0xffffffff
v_mad_i64_i32 v[14:15], s22, v4, v5, v[6:7]
v_mov_b32_e32 v4, 0x80000000
v_mov_b32_e32 v5, 0x7fffffff
v_mov_b32_e32 v6, 0x0
v_mov_b32_e32 v7, 0x80000000
v_mad_i64_i32 v[16:17], s23, v4, v5, v[6:7]
v_mov_b32_e32 v4, 0xffffffff
v_mov_b32_e32 v5, 0x80000000
v_mov_b32_e32 v6, 0x80000000
v_mov_b32_e32 v7, 0x0
v_mad_i64_i32 v[18:19], s24, v4, v5, v[6:7]
s_mov_b32 s40, 0
s_and_b32 s41, s20, 1
s_or_b32 s40, s40, s41
s_and_b32 s41, s21, 2
s_or_b32 s40, s40, s41
s_and_b32 s41, s22, 4
s_or_b32 s40, s40, s41
s_and_b32 s41, s23, 8
s_or_b32 s40, s40, s41
s_and_b32 s41, s24, 16
s_or_b32 s40, s40, s41
v_cmpx_eq_u32_e32 0, v0
v_mov_b32_e32 v41, 0
buffer_store_dword v10, v41, s[0:3], 0 offen
v_mov_b32_e32 v41, 4
buffer_store_dword v11, v41, s[0:3], 0 offen
v_xor_b32_e32 v40, v12, v13
v_mov_b32_e32 v41, 8
buffer_store_dword v40, v41, s[0:3], 0 offen
v_xor_b32_e32 v40, v14, v15
v_mov_b32_e32 v41, 12
buffer_store_dword v40, v41, s[0:3], 0 offen
v_mov_b32_e32 v41, 16
buffer_store_dword v16, v41, s[0:3], 0 offen
v_mov_b32_e32 v41, 20
buffer_store_dword v17, v41, s[0:3], 0 offen
v_mov_b32_e32 v40, s40
v_mov_b32_e32 v41, 24
buffer_store_dword v40, v41, s[0:3], 0 offen
v_xor_b32_e32 v40, v18, v19
v_mov_b32_e32 v41, 28
buffer_store_dword v40, v41, s[0:3], 0 offen
s_endpgm
