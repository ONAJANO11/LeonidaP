; v_frexp_mant_f32 and v_frexp_exp_i32_f32 (RDNA2 ISA, VOP1 opcodes 64 and 63), CPU-only checks until now.
; x = mant * 2^exp with |mant| in [0.5, 1.0). For a normal x the mantissa keeps x's sign and fraction bits with the
; biased exponent set to 126, and exp = biased exponent - 126. For +-inf and NaN, frexp_mant returns x and
; frexp_exp returns 0; for +-0, frexp_mant returns x and frexp_exp returns 0.
; Lane 0 of a wave32 group stores one word per case:
;   out[0] mant(12.0    = 0x41400000) = 0.75   (sign 0, fraction 0x400000, exponent 126)  = 0x3F400000
;   out[1] mant(-0.1    = 0xBDCCCCCD) = -0.8   (sign 1, fraction 0x4CCCCD, exponent 126)  = 0xBF4CCCCD
;   out[2] mant(-inf    = 0xFF800000) = -inf                                               = 0xFF800000
;   out[3] mant(qNaN    = 0x7FC00123) = the same NaN                                       = 0x7FC00123
;   out[4] exp(12.0)  = 130 - 126 = 4                                                      = 0x00000004
;   out[5] exp(0.1    = 0x3DCCCCCD) = 123 - 126 = -3                                      = 0xFFFFFFFD
;   out[6] exp(FLT_MAX = 0x7F7FFFFF) = 254 - 126 = 128                                    = 0x00000080
;   out[7] exp(+inf) | exp(qNaN) | exp(-0.0) = 0 | 0 | 0, plus exp(1.0) = 1 (v_add3_u32)  = 0x00000001
; group: 32,1,1
; Run: vk_compute_test cs_w32_frexp_f32.spv 1 dump
;   -> out[0..7] = 0x3F400000 0xBF4CCCCD 0xFF800000 0x7FC00123 0x00000004 0xFFFFFFFD 0x00000080 0x00000001
; mattr: +wavefrontsize32,-wavefrontsize64
v_mov_b32_e32 v1, 0x41400000
v_frexp_mant_f32_e32 v10, v1
v_mov_b32_e32 v2, 0xbdcccccd
v_frexp_mant_f32_e32 v11, v2
v_mov_b32_e32 v3, 0xff800000
v_frexp_mant_f32_e32 v12, v3
v_mov_b32_e32 v4, 0x7fc00123
v_frexp_mant_f32_e32 v13, v4
v_frexp_exp_i32_f32_e32 v14, v1
v_mov_b32_e32 v5, 0x3dcccccd
v_frexp_exp_i32_f32_e32 v15, v5
v_mov_b32_e32 v6, 0x7f7fffff
v_frexp_exp_i32_f32_e32 v16, v6
v_mov_b32_e32 v7, 0x7f800000
v_frexp_exp_i32_f32_e32 v20, v7
v_frexp_exp_i32_f32_e32 v21, v4
v_mov_b32_e32 v8, 0x80000000
v_frexp_exp_i32_f32_e32 v22, v8
v_mov_b32_e32 v9, 1.0
v_frexp_exp_i32_f32_e32 v23, v9
v_or3_b32 v20, v20, v21, v22
v_add3_u32 v17, v20, v23, 0
v_cmpx_eq_u32_e32 0, v0
v_mov_b32_e32 v30, 0
buffer_store_dword v10, v30, s[0:3], 0 offen
v_mov_b32_e32 v30, 4
buffer_store_dword v11, v30, s[0:3], 0 offen
v_mov_b32_e32 v30, 8
buffer_store_dword v12, v30, s[0:3], 0 offen
v_mov_b32_e32 v30, 12
buffer_store_dword v13, v30, s[0:3], 0 offen
v_mov_b32_e32 v30, 16
buffer_store_dword v14, v30, s[0:3], 0 offen
v_mov_b32_e32 v30, 20
buffer_store_dword v15, v30, s[0:3], 0 offen
v_mov_b32_e32 v30, 24
buffer_store_dword v16, v30, s[0:3], 0 offen
v_mov_b32_e32 v30, 28
buffer_store_dword v17, v30, s[0:3], 0 offen
s_endpgm
