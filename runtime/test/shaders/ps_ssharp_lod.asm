; image_get_lod with ps_ssharp's T#/S# and texture, so the S#'s LOD_BIAS, MIN_LOD, MAX_LOD and mip filter apply.
; Run: vk_sampler_test vs_ids.spv ps_ssharp_lod.spv out.raw <S# words> [<base level> <last level>]
; (check.py: SAMPLER_CASES, judged by sampler_lod_mismatch). vs_ids runs UV 0..1 over the 64x64 target and the texture
; is 128x128, so the query at (u, v) sees two texels a pixel (LOD 1) and the one at (2u, v) four along x (LOD 2).
; The translator maps image_get_lod to OpImageQueryLod, which returns (d_l - level_base, lambda'): lambda' is the
; calculated LOD plus the sampler's bias, d_l the level a sample would read after the LOD range, mip filter and the
; view's levels (check.py's sampler_level).
;   -> mrt0 = (clamped at (u, v), calculated at (u, v), clamped at (2u, v), calculated at (2u, v))
s_load_dwordx8 s[8:15], s[0:1], 0x0
s_load_dwordx4 s[16:19], s[0:1], 0x20
s_waitcnt lgkmcnt(0)
v_interp_p1_f32_e32 v2, v0, attr0.x
v_interp_p2_f32_e32 v2, v1, attr0.x
v_interp_p1_f32_e32 v3, v0, attr0.y
v_interp_p2_f32_e32 v3, v1, attr0.y
v_add_f32_e32 v8, v2, v2
v_mov_b32_e32 v9, v3
image_get_lod v[4:5], v[2:3], s[8:15], s[16:19] dmask:0x3 dim:SQ_RSRC_IMG_2D
image_get_lod v[6:7], v[8:9], s[8:15], s[16:19] dmask:0x3 dim:SQ_RSRC_IMG_2D
s_waitcnt vmcnt(0)
exp mrt0 v4, v5, v6, v7 done vm
s_endpgm
