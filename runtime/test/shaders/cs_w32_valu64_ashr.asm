; v_ashrrev_i64 amount rule (S0[5:0]) and sign fill across the dword boundary, run once on a GPU.
; RDNA2 ISA: V_ASHRREV_I64: D.i64 = (S1.i64 >> S0[5:0].u32), copies of bit 63 shifted in.
; X1 = 0x9ABCDEF1_F3579BDF (bit 63 set), X2 = 0x7ABCDEF1_F3579BDF (bit 63 clear, bit 31 of the low dword set).
; Lane 0 of a wave32 group stores eight dwords (lo/hi = the two dwords of the D pair, fold = lo ^ hi):
;   out[0] = fold, X1 >> 0:  D = X1;  F3^9A,57^BC,9B^DE,DF^F1                            = 0x69EB452E
;   out[1] = lo,   X1 >> 31: (0x9ABCDEF1 << 1 mod 2^32 = 0x3579BDE2) | bit 31 of L (1)    = 0x3579BDE3
;   out[2] = hi,   X1 >> 31: bits 63..32 = bit 63 = 1                                     = 0xFFFFFFFF
;   out[3] = fold, X1 >> 32: lo = 0x9ABCDEF1, hi = 0xFFFFFFFF; 9ABCDEF1 ^ FFFFFFFF        = 0x6543210E
;   out[4] = lo,   X1 >> 33: the high dword shifted right arithmetically by 1: 0xCD5E6F78 = 0xCD5E6F78
;   out[5] = fold, X2 >> 33: sign is bit 63 = 0, not bit 31 of the low dword: lo = 0x7ABCDEF1 >> 1 = 0x3D5E6F78, hi = 0
;   out[6] = lo,   X1 >> 63: all copies of bit 63                                         = 0xFFFFFFFF
;   out[7] = hi,   X1 >> 127: 127 & 63 = 63, as out[6] (a 5-bit mask would shift by 31)   = 0xFFFFFFFF
; group: 32,1,1
; Run: vk_compute_test cs_w32_valu64_ashr.spv 1 dump -> out[0..7] = 1777026350 897170915 4294967295 1698898190 3445518200 1029599096 4294967295 4294967295
; mattr: +wavefrontsize32,-wavefrontsize64
v_mov_b32_e32 v4, 0xF3579BDF
v_mov_b32_e32 v5, 0x9ABCDEF1
v_mov_b32_e32 v6, 0xF3579BDF
v_mov_b32_e32 v7, 0x7ABCDEF1
v_mov_b32_e32 v8, 0
v_ashrrev_i64 v[10:11], v8, v[4:5]
v_mov_b32_e32 v8, 31
v_ashrrev_i64 v[12:13], v8, v[4:5]
v_mov_b32_e32 v8, 32
v_ashrrev_i64 v[14:15], v8, v[4:5]
v_mov_b32_e32 v8, 33
v_ashrrev_i64 v[16:17], v8, v[4:5]
v_mov_b32_e32 v8, 33
v_ashrrev_i64 v[18:19], v8, v[6:7]
v_mov_b32_e32 v8, 63
v_ashrrev_i64 v[20:21], v8, v[4:5]
v_mov_b32_e32 v8, 127
v_ashrrev_i64 v[22:23], v8, v[4:5]
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
v_xor_b32_e32 v40, v18, v19
v_mov_b32_e32 v41, 20
buffer_store_dword v40, v41, s[0:3], 0 offen
v_mov_b32_e32 v41, 24
buffer_store_dword v20, v41, s[0:3], 0 offen
v_mov_b32_e32 v41, 28
buffer_store_dword v23, v41, s[0:3], 0 offen
s_endpgm
