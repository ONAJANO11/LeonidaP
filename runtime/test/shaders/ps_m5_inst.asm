; M5 pixel shader for the indirect draws: a colour from the cell number and the variant the vertex shader passed.
; Run: d3d12_m5_world_test.
; attr0.xy = (k, variant), k = 0..63, variant 0 or 1 (constant over the quad; the +0.5 and the truncation take the
; interpolation's rounding error out)
;   mrt0 = (4k, 4(63 - k), 128 * variant, 255) / 255
; fssm-at: 0x10
; fssm-data: 0x35535350 0x20535020
v_interp_p1_f32_e32 v2, v0, attr0.x
v_interp_p2_f32_e32 v2, v1, attr0.x
v_interp_p1_f32_e32 v3, v0, attr0.y
v_interp_p2_f32_e32 v3, v1, attr0.y
v_add_f32_e32 v2, 0.5, v2
v_add_f32_e32 v3, 0.5, v3
v_cvt_u32_f32_e32 v2, v2
v_cvt_u32_f32_e32 v3, v3
v_cvt_f32_u32_e32 v4, v2
v_cvt_f32_u32_e32 v5, v3
v_sub_f32_e32 v6, 0x427c0000, v4
v_mul_f32_e32 v7, 0x3c808081, v4
v_mul_f32_e32 v8, 0x3c808081, v6
v_mul_f32_e32 v9, 0x3f008081, v5
v_mov_b32_e32 v10, 1.0
exp mrt0 v7, v8, v9, v10 done vm
s_endpgm
