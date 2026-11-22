; v_lshrrev_b64 amount rule (S0[5:0], so 32/33/63 and >= 64 wrap at 6 bits, not 5), run once on a GPU.
; RDNA2 ISA: V_LSHRREV_B64: D.u64 = (S1.u64 >> S0[5:0].u32), zeros shifted in. X = 0x9ABCDEF1_F3579BDF (H:L).
; Lane 0 of a wave32 group stores eight dwords (lo/hi = the two dwords of the D pair, fold = lo ^ hi):
;   out[0] = fold, amount 0:  D = X;  L ^ H = F3^9A,57^BC,9B^DE,DF^F1                    = 0x69EB452E
;   out[1] = lo,   amount 31: (H << 1 mod 2^32 = 0x3579BDE2) | bit 31 of L (1)            = 0x3579BDE3
;   out[2] = hi,   amount 31: H >> 31                                                     = 1
;   out[3] = fold, amount 32: lo = H, hi = 0                                              = 0x9ABCDEF1
;   out[4] = lo,   amount 33: H >> 1 (logical)                                            = 0x4D5E6F78
;   out[5] = hi,   amount 33                                                              = 0
;   out[6] = fold, amount 63: lo = H >> 31 = 1, hi = 0                                    = 1
;   out[7] = fold, amount 127: 127 & 63 = 63, as out[6] (a 5-bit mask would shift by 31)  = 1
; group: 32,1,1
; Run: vk_compute_test cs_w32_valu64_shr.spv 1 dump -> out[0..7] = 1777026350 897170915 1 2596069105 1298034552 0 1 1
; mattr: +wavefrontsize32,-wavefrontsize64
v_mov_b32_e32 v4, 0xF3579BDF
v_mov_b32_e32 v5, 0x9ABCDEF1
v_mov_b32_e32 v6, 0xF3579BDF
v_mov_b32_e32 v7, 0x7ABCDEF1
v_mov_b32_e32 v8, 0
v_lshrrev_b64 v[10:11], v8, v[4:5]
v_mov_b32_e32 v8, 31
v_lshrrev_b64 v[12:13], v8, v[4:5]
v_mov_b32_e32 v8, 32
v_lshrrev_b64 v[14:15], v8, v[4:5]
v_mov_b32_e32 v8, 33
v_lshrrev_b64 v[16:17], v8, v[4:5]
v_mov_b32_e32 v8, 63
v_lshrrev_b64 v[18:19], v8, v[4:5]
v_mov_b32_e32 v8, 127
v_lshrrev_b64 v[20:21], v8, v[4:5]
v_cmpx_eq_u32_e32 0, v0
v_xor_b32_e32 v40, v10, v11
v_mov_b32_e32 v41, 0
buffer_store_dword v40, v41, s[0:3], 0 offen
v_mov_b32_e32 v41, 4
buffer_store_dword v12, v41, s[0:3], 0 offen
v_mov_b32_e32 v41, 8
buffer_store_dword v13, v41, s[0:3], 0 offen
v_xor_b32_e32 v40, v14, v15
v_mov_b32_e32 v41, 12
buffer_store_dword v40, v41, s[0:3], 0 offen
v_mov_b32_e32 v41, 16
buffer_store_dword v16, v41, s[0:3], 0 offen
v_mov_b32_e32 v41, 20
buffer_store_dword v17, v41, s[0:3], 0 offen
v_xor_b32_e32 v40, v18, v19
v_mov_b32_e32 v41, 24
buffer_store_dword v40, v41, s[0:3], 0 offen
v_xor_b32_e32 v40, v20, v21
v_mov_b32_e32 v41, 28
buffer_store_dword v40, v41, s[0:3], 0 offen
s_endpgm
