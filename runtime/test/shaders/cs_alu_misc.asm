; GPU check for two docs/design.md rows that had only CPU tests: v_mqsad_u32_u8 and
; v_cvt_flr_i32_f32 / v_cvt_rpi_i32_f32.
;
; v_mqsad_u32_u8: D[k] = S2[k] + masked byte-SAD of the 32-bit window S0[8k+31:8k]
; against S1, wrapping at 32 bits (RDNA ISA pseudocode). With S0 = 0x0807060504030201,
; S1 = 0x04030201 and S2 = 0, the four windows give SADs 0, 4, 8 and 12.
;
; v_cvt_flr_i32_f32: f32_to_i32(floor(S0)); v_cvt_rpi_i32_f32: f32_to_i32(floor(S0 + 0.5)),
; the +0.5 rounded in binary32. Both saturate at +-2^31 and give 0 for NaN, as v_cvt_i32_f32.
;
; Run: vk_compute_test cs_alu_misc.spv 1 dump
;   out[0..7] = 0 4 8 12 0xfffffffd 3 0x80000000 1
; rsrc2: 0x8088
; group: 32,1,1
; mattr: +wavefrontsize32,-wavefrontsize64
v_cmpx_eq_u32_e32 0, v0

; v_mqsad_u32_u8: 0x0807060504030201 against 0x04030201, accumulators 0.
v_mov_b32_e32 v1, 0x04030201
v_mov_b32_e32 v2, 0x04030201
v_mov_b32_e32 v3, 0x08070605
v_mov_b32_e32 v4, 0
v_mov_b32_e32 v5, 0
v_mov_b32_e32 v6, 0
v_mov_b32_e32 v7, 0
v_mqsad_u32_u8 v[8:11], v[2:3], v1, v[4:7]

; floor(-2.5) = -3
v_mov_b32_e32 v13, 0xc0200000
v_cvt_flr_i32_f32_e32 v12, v13
; floor(2.5 + 0.5) = 3
v_mov_b32_e32 v15, 0x40200000
v_cvt_rpi_i32_f32_e32 v14, v15
; below -2^31: saturate to INT_MIN
v_mov_b32_e32 v17, 0xcf000001
v_cvt_flr_i32_f32_e32 v16, v17
; 0.49999997 + 0.5 rounds to 1.0 in binary32: 1
v_mov_b32_e32 v19, 0x3effffff
v_cvt_rpi_i32_f32_e32 v18, v19

v_mov_b32_e32 v20, 0
buffer_store_dword v8, v20, s[0:3], 0 offen
v_mov_b32_e32 v20, 4
buffer_store_dword v9, v20, s[0:3], 0 offen
v_mov_b32_e32 v20, 8
buffer_store_dword v10, v20, s[0:3], 0 offen
v_mov_b32_e32 v20, 12
buffer_store_dword v11, v20, s[0:3], 0 offen
v_mov_b32_e32 v20, 16
buffer_store_dword v12, v20, s[0:3], 0 offen
v_mov_b32_e32 v20, 20
buffer_store_dword v14, v20, s[0:3], 0 offen
v_mov_b32_e32 v20, 24
buffer_store_dword v16, v20, s[0:3], 0 offen
v_mov_b32_e32 v20, 28
buffer_store_dword v18, v20, s[0:3], 0 offen
s_endpgm
