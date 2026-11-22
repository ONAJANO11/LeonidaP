; A 32-bit literal in a 64-bit integer operand: sign-extended where the operand is i64, zero-extended where it is
; b64/u64 (measured natively on RDNA2, gfx1036; docs/design.md "64-bit operand literals"). Run once on a GPU.
; The RDNA2 ISA reference (SOP1/SOP2/VOP3) says the literal is 32 bits and does not say how it is widened; the
; hardware widens it by the operand's type. Lane 0 of a wave32 group stores eight dwords:
;   out[0] = low word of s_ashr_i64(lit 0x80000000, 4)
;            i64: 0xFFFFFFFF_80000000 >> 4 (arithmetic) = 0xFFFFFFFF_F8000000   -> 0xF8000000 (zero-ext: 0x08000000)
;   out[1] = high word of s_bfe_i64(lit 0xFFFF0000, offset 0, width 64)
;            i64: 0xFFFFFFFF_FFFF0000, whole field                            -> 0xFFFFFFFF (zero-ext: 0)
;   out[2] = s_flbit_i32_i64(lit 0xFFFF0000)
;            i64: 0xFFFFFFFF_FFFF0000, first bit unlike the sign is bit 15    -> 48         (zero-ext: 32)
;   out[3] = high word of v_ashrrev_i64(0, lit 0x80000000)
;            i64: 0xFFFFFFFF_80000000 >> 0                                    -> 0xFFFFFFFF (zero-ext: 0)
;   out[4] = high word of v_mad_i64_i32(0, 0, lit 0xFFFF0000)
;            S2 is i64: 0 * 0 + 0xFFFFFFFF_FFFF0000                           -> 0xFFFFFFFF (zero-ext: 0)
;   controls, b64 operands (zero-extended):
;   out[5] = high word of s_lshl_b64(lit 0x80000000, 4)
;            0x00000000_80000000 << 4 = 0x00000008_00000000                   -> 8          (sign-ext: 0xFFFFFFF8)
;   out[6] = s_flbit_i32_b64(lit 0x80000000)
;            0x00000000_80000000: 32 zeros above bit 31                       -> 32         (sign-ext: 0)
;   out[7] = high word of s_not_b64(lit 0x80000000)
;            ~0x00000000_80000000 = 0xFFFFFFFF_7FFFFFFF                       -> 0xFFFFFFFF (sign-ext: 0)
; group: 32,1,1
; Run: vk_compute_test cs_w32_lit64.spv 1 dump -> out[0..7] = 4160749568 4294967295 48 4294967295 4294967295 8 32 4294967295
; mattr: +wavefrontsize32,-wavefrontsize64
s_ashr_i64 s[8:9], 0x80000000, 4
s_mov_b32 s12, 0x400000
s_bfe_i64 s[10:11], 0xffff0000, s12
s_flbit_i32_i64 s13, 0xffff0000
v_ashrrev_i64 v[2:3], 0, 0x80000000
v_mov_b32_e32 v4, 0
v_mad_i64_i32 v[6:7], s22, v4, v4, 0xffff0000
s_lshl_b64 s[14:15], 0x80000000, 4
s_flbit_i32_b64 s16, 0x80000000
s_not_b64 s[18:19], 0x80000000
v_cmpx_eq_u32_e32 0, v0
v_mov_b32_e32 v20, s8
v_mov_b32_e32 v21, 0
buffer_store_dword v20, v21, s[0:3], 0 offen
v_mov_b32_e32 v20, s11
v_mov_b32_e32 v21, 4
buffer_store_dword v20, v21, s[0:3], 0 offen
v_mov_b32_e32 v20, s13
v_mov_b32_e32 v21, 8
buffer_store_dword v20, v21, s[0:3], 0 offen
v_mov_b32_e32 v21, 12
buffer_store_dword v3, v21, s[0:3], 0 offen
v_mov_b32_e32 v21, 16
buffer_store_dword v7, v21, s[0:3], 0 offen
v_mov_b32_e32 v20, s15
v_mov_b32_e32 v21, 20
buffer_store_dword v20, v21, s[0:3], 0 offen
v_mov_b32_e32 v20, s16
v_mov_b32_e32 v21, 24
buffer_store_dword v20, v21, s[0:3], 0 offen
v_mov_b32_e32 v20, s19
v_mov_b32_e32 v21, 28
buffer_store_dword v20, v21, s[0:3], 0 offen
s_endpgm
