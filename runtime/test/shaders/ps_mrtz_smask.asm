; Sample-mask-only compressed exp mrtz (off, off, v_zw, v_zw compr): exports SampleMask without
; FragDepth or FragStencilRefEXT (so no VK_EXT_shader_stencil_export is required).
; attr0.xy = (u, v) in [0, 1] over the 64x64 target (vs_ids / vs_tri).
;   x = floor(u * 64), y = floor(v * 64), stripe = x >> 3 in 0..7.
;   mask = nibble `stripe` of 0xf7584210 (+ 0xf0 on stripe 0, y >= 32), packed as 0xdead0000 | mask.
; Run (VKH_MSAA=4): vk_harness vs_ids.spv ps_mrtz_smask.spv out.raw
;   4x MSAA resolve: R = popcount(mask & 0xf) / 4.0 -> stripes 0..7 give 0, 0.25, 0.25, 0.25, 0.25, 0.5, 0.75, 1.0.
v_interp_p1_f32_e32 v2, v0, attr0.x
v_interp_p2_f32_e32 v2, v1, attr0.x
v_interp_p1_f32_e32 v3, v0, attr0.y
v_interp_p2_f32_e32 v3, v1, attr0.y
v_mul_f32_e32 v4, 0x42800000, v2
v_mul_f32_e32 v5, 0x42800000, v3
v_cvt_u32_f32_e32 v4, v4
v_cvt_u32_f32_e32 v5, v5
v_lshrrev_b32_e32 v6, 3, v4
v_lshlrev_b32_e32 v7, 2, v6
v_mov_b32_e32 v8, 0xf7584210
v_lshrrev_b32_e32 v8, v7, v8
v_and_b32_e32 v8, 0xf, v8
v_cmp_eq_u32_e32 vcc_lo, 0, v6
v_cmp_ge_u32_e64 s6, v5, 32
s_and_b32 s7, vcc_lo, s6
v_mov_b32_e32 v9, 0xf0
v_cndmask_b32_e64 v9, 0, v9, s7
v_or_b32_e32 v8, v8, v9
v_or_b32_e32 v11, 0xdead0000, v8
exp mrtz off, off, v11, v11 compr vm
v_mov_b32_e32 v12, 1.0
exp mrt0 v12, v12, v12, v12 done vm
s_endpgm
