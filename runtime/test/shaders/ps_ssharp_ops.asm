; The S# mip fields on the other sample forms: ps_ssharp's T#/S# and texture, read with image_sample_l, _b, _d and
; _d_cl. Run: vk_sampler_test vs_ids.spv ps_ssharp_ops.spv out.raw <S# words> [<base level> <last level>]
; (check.py: SAMPLER_CASES, judged by sampler_ops_mismatch). Base LODs before the S#'s bias and clamp:
;   x = image_sample_l, LOD 0.625
;   y = image_sample_b, bias -0.25 on the implicit LOD 1: 0.75
;   z = image_sample_d, ds/dx = dt/dy = 1/128 and ds/dy = dt/dx = 0 on the 128-texel level 0: 0
;   w = image_sample_d_cl, gradients of 1/256 (LOD -1) and a min-LOD clamp of 0.75
; -> mrt0 = the r channel of each. Level 0 has r = 1 or 0 (its quadrants), level 1 r = 128/255.
s_load_dwordx8 s[8:15], s[0:1], 0x0
s_load_dwordx4 s[16:19], s[0:1], 0x20
s_waitcnt lgkmcnt(0)
v_interp_p1_f32_e32 v2, v0, attr0.x
v_interp_p2_f32_e32 v2, v1, attr0.x
v_interp_p1_f32_e32 v3, v0, attr0.y
v_interp_p2_f32_e32 v3, v1, attr0.y
v_mov_b32_e32 v4, 0x3f200000
v_mov_b32_e32 v5, 0xbe800000
v_mov_b32_e32 v6, v2
v_mov_b32_e32 v7, v3
v_mov_b32_e32 v8, 0x3c000000
v_mov_b32_e32 v9, 0
v_mov_b32_e32 v10, 0
v_mov_b32_e32 v11, 0x3c000000
v_mov_b32_e32 v12, v2
v_mov_b32_e32 v13, v3
v_mov_b32_e32 v16, 0x3b800000
v_mov_b32_e32 v17, 0
v_mov_b32_e32 v18, 0
v_mov_b32_e32 v19, 0x3b800000
v_mov_b32_e32 v20, v2
v_mov_b32_e32 v21, v3
v_mov_b32_e32 v22, 0x3f400000
image_sample_l v24, v[2:4], s[8:15], s[16:19] dmask:0x1 dim:SQ_RSRC_IMG_2D
image_sample_b v25, v[5:7], s[8:15], s[16:19] dmask:0x1 dim:SQ_RSRC_IMG_2D
image_sample_d v26, v[8:13], s[8:15], s[16:19] dmask:0x1 dim:SQ_RSRC_IMG_2D
image_sample_d_cl v27, v[16:22], s[8:15], s[16:19] dmask:0x1 dim:SQ_RSRC_IMG_2D
s_waitcnt vmcnt(0)
exp mrt0 v24, v25, v26, v27 done vm
s_endpgm
