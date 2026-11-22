; group: 1,1,1
; rsrc2: 0x392
; s[0:7] = UAV T#, s8 = test dimension (0: 1D, 1: 1D array, 2: 3D UINT, 3: 2D, 4: 3D FLOAT, 5: 1D array FLOAT).
; s[9:11] = workgroup XYZ. Add 1000 + x + 16*y + 256*z to each existing texel (z is the array
; layer for 1D arrays). d3d12_texture_uav_test pushes production heap descriptors and compares
; every texel against the CPU expectation and, for UINT, against Windows WARP.
v_mov_b32_e32 v0, s9
v_mov_b32_e32 v1, s10
v_mov_b32_e32 v2, s11
v_lshlrev_b32_e32 v3, 8, v2
v_lshl_add_u32 v3, v1, 4, v3
v_add_nc_u32_e32 v3, v0, v3
v_add_nc_u32_e32 v3, 1000, v3
s_cmp_eq_u32 s8, 0
s_cbranch_scc0 6
image_load v4, v0, s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_1D unorm
v_add_nc_u32_e32 v3, v3, v4
image_store v3, v0, s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_1D unorm
s_endpgm
s_cmp_eq_u32 s8, 1
s_cbranch_scc0 7
v_mov_b32_e32 v1, v2
image_load v4, v[0:1], s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_1D_ARRAY unorm
v_add_nc_u32_e32 v3, v3, v4
image_store v3, v[0:1], s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_1D_ARRAY unorm
s_endpgm
s_cmp_eq_u32 s8, 2
s_cbranch_scc0 6
image_load v4, v[0:2], s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_3D unorm
v_add_nc_u32_e32 v3, v3, v4
image_store v3, v[0:2], s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_3D unorm
s_endpgm
s_cmp_eq_u32 s8, 3
s_cbranch_scc0 6
image_load v4, v[0:1], s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_add_nc_u32_e32 v3, v3, v4
image_store v3, v[0:1], s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
s_endpgm
s_cmp_eq_u32 s8, 5
s_cbranch_scc0 13
v_mov_b32_e32 v1, v2
image_load v4, v[0:1], s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_1D_ARRAY unorm
v_cvt_f32_u32_e32 v3, v3
v_add_f32_e32 v3, v3, v4
; Float array view covers two layers. Verify that the next-layer read does not contribute.
v_add_nc_u32_e32 v1, 2, v1
image_load v4, v[0:1], s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_1D_ARRAY unorm
v_add_f32_e32 v3, v3, v4
v_mov_b32_e32 v1, v2
image_store v3, v[0:1], s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_1D_ARRAY unorm
s_endpgm
image_load v4, v[0:2], s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_3D unorm
v_cvt_f32_u32_e32 v3, v3
v_add_f32_e32 v3, v3, v4
; Float test uses WSize=2: an outside-Z fetch must be zero even if still inside the image.
v_add_nc_u32_e32 v2, 2, v2
image_load v4, v[0:2], s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_3D unorm
v_add_f32_e32 v3, v3, v4
v_mov_b32_e32 v2, s11
image_store v3, v[0:2], s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_3D unorm
s_endpgm
