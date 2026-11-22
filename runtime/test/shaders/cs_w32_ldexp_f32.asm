; v_ldexp_f32 (RDNA2 ISA, VOP3 opcode 354), CPU-only checks until now: D = S0 * 2^S1, S1 a signed 32-bit integer.
; A finite result above the binary32 range is +-inf, one far below it (no denormal in reach) is +-0; +-inf stays
; +-inf. The exponent sum must not wrap in 32 bits (S1 = 0x7FFFFFFF / 0x80000000).
; Lane 0 of a wave32 group stores one word per case:
;   out[0] ldexp(1.5, 3)            = 12.0                        = 0x41400000
;   out[1] ldexp(-inf, -300)        = -inf                        = 0xFF800000
;   out[2] ldexp(-1.5, 127)         = -1.5 * 2^127 (biased 254)   = 0xFF400000
;   out[3] ldexp(1.0, 128)          = 2^128 overflows             = 0x7F800000
;   out[4] ldexp(1.0, -126)         = 2^-126, smallest normal     = 0x00800000
;   out[5] ldexp(-1.0, -200)        = -2^-200 underflows to -0    = 0x80000000
;   out[6] ldexp(1.0, 0x7FFFFFFF)   = +inf                        = 0x7F800000
;   out[7] ldexp(-2.0, 0x80000000)  = -0                          = 0x80000000
; group: 32,1,1
; Run: vk_compute_test cs_w32_ldexp_f32.spv 1 dump
;   -> out[0..7] = 0x41400000 0xFF800000 0xFF400000 0x7F800000 0x00800000 0x80000000 0x7F800000 0x80000000
; mattr: +wavefrontsize32,-wavefrontsize64
v_mov_b32_e32 v1, 0x3fc00000
v_mov_b32_e32 v2, 3
v_ldexp_f32 v10, v1, v2
v_mov_b32_e32 v1, 0xff800000
v_mov_b32_e32 v2, 0xfffffed4
v_ldexp_f32 v11, v1, v2
v_mov_b32_e32 v1, 0xbfc00000
v_mov_b32_e32 v2, 0x7f
v_ldexp_f32 v12, v1, v2
v_mov_b32_e32 v1, 1.0
v_mov_b32_e32 v2, 0x80
v_ldexp_f32 v13, v1, v2
v_mov_b32_e32 v2, 0xffffff82
v_ldexp_f32 v14, v1, v2
v_mov_b32_e32 v3, -1.0
v_mov_b32_e32 v2, 0xffffff38
v_ldexp_f32 v15, v3, v2
v_mov_b32_e32 v2, 0x7fffffff
v_ldexp_f32 v16, v1, v2
v_mov_b32_e32 v3, -2.0
v_mov_b32_e32 v2, 0x80000000
v_ldexp_f32 v17, v3, v2
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
