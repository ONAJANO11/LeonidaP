; v_mad_u64_u32: unsigned product, 64-bit add across the dword boundary and the carry out in sdst, run once on a GPU.
; RDNA2 ISA: D.u64 = S0.u32 * S1.u32 + S2.u64, sdst = bit 64 of the 65-bit sum (the carry out).
; The lanes all compute the same, so sdst is 0 or 0xFFFFFFFF; out[6] packs bit n = (case n's sdst & 1). Cases:
;   U1: 0xFFFFFFFF * 0xFFFFFFFF = 0xFFFFFFFE_00000001, + 0xFFFFFFFF_FFFFFFFF = 0x1_FFFFFFFE_00000000: result 0xFFFFFFFE_00000000, carry 1
;       (the 32-bit v_mad_u64_u32 test in cs_w32_salu64 reads only the high word of this case)
;   U2: 3 * 5 + 0x1_FFFFFFFF = 0x2_0000000E; no carry out of bit 63 (only from the low dword into the high one)
;   U3: 0x80000000 * 2 = 0x1_00000000, + 0xFFFFFFFF_80000000 = 0x1_00000000_80000000: result 0x00000000_80000000, carry 1
;   U4: 0xFFFFFFFF * 1 + 0 = 0x0_FFFFFFFF; a signed product would give -1 (hi 0xFFFFFFFF, sdst 1); sdst 0
;   U5: 0xFFFFFFFF * 0xFFFFFFFF + 0 = 0xFFFFFFFE_00000001; no carry; a signed product would give 1
;   out[0] = U1 lo = 0           out[1] = U1 hi = 0xFFFFFFFE
;   out[2] = U2 lo = 0xE         out[3] = U2 hi = 2
;   out[4] = U3 lo ^ hi = 0x80000000   out[5] = U4 lo ^ hi = 0xFFFFFFFF
;   out[6] = sdst bits U1..U5 = 0b00101 = 5
;   out[7] = U5 lo ^ hi = 1 ^ 0xFFFFFFFE = 0xFFFFFFFF
; group: 32,1,1
; Run: vk_compute_test cs_w32_valu64_mad_u64.spv 1 dump -> out[0..7] = 0 4294967294 14 2 2147483648 4294967295 5 4294967295
; mattr: +wavefrontsize32,-wavefrontsize64
v_mov_b32_e32 v4, 0xffffffff
v_mov_b32_e32 v5, 0xffffffff
v_mov_b32_e32 v6, 0xffffffff
v_mov_b32_e32 v7, 0xffffffff
v_mad_u64_u32 v[10:11], s20, v4, v5, v[6:7]
v_mov_b32_e32 v4, 0x3
v_mov_b32_e32 v5, 0x5
v_mov_b32_e32 v6, 0xffffffff
v_mov_b32_e32 v7, 0x1
v_mad_u64_u32 v[12:13], s21, v4, v5, v[6:7]
v_mov_b32_e32 v4, 0x80000000
v_mov_b32_e32 v5, 0x2
v_mov_b32_e32 v6, 0x80000000
v_mov_b32_e32 v7, 0xffffffff
v_mad_u64_u32 v[14:15], s22, v4, v5, v[6:7]
v_mov_b32_e32 v4, 0xffffffff
v_mov_b32_e32 v5, 0x1
v_mov_b32_e32 v6, 0x0
v_mov_b32_e32 v7, 0x0
v_mad_u64_u32 v[16:17], s23, v4, v5, v[6:7]
v_mov_b32_e32 v4, 0xffffffff
v_mov_b32_e32 v5, 0xffffffff
v_mov_b32_e32 v6, 0x0
v_mov_b32_e32 v7, 0x0
v_mad_u64_u32 v[18:19], s24, v4, v5, v[6:7]
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
v_mov_b32_e32 v41, 8
buffer_store_dword v12, v41, s[0:3], 0 offen
v_mov_b32_e32 v41, 12
buffer_store_dword v13, v41, s[0:3], 0 offen
v_xor_b32_e32 v40, v14, v15
v_mov_b32_e32 v41, 16
buffer_store_dword v40, v41, s[0:3], 0 offen
v_xor_b32_e32 v40, v16, v17
v_mov_b32_e32 v41, 20
buffer_store_dword v40, v41, s[0:3], 0 offen
v_mov_b32_e32 v40, s40
v_mov_b32_e32 v41, 24
buffer_store_dword v40, v41, s[0:3], 0 offen
v_xor_b32_e32 v40, v18, v19
v_mov_b32_e32 v41, 28
buffer_store_dword v40, v41, s[0:3], 0 offen
s_endpgm
