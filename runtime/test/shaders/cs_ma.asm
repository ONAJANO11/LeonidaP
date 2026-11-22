; group: 64,1,1
; rsrc2: 0x808c
; fssm-at: 0x150
; fssm-data: 0x4d5f5343 0x3d414d5f
; Multi-argument ExecuteIndirect compute shader: out[g] = (((g + a) ^ b) + c) - d for g = 64 * group + lane, written
; through a root UAV (the root signature [0] four 32-bit constants a, b, c, d = s0..s3, [1] a root UAV = s[4:5]; the
; workgroup id is s6, as in cs_m4_indirect). Run: d3d12_indirect_multiarg_test.
s_lshl_b32 s7, s6, 6
v_add_nc_u32_e32 v1, s7, v0
v_add_nc_u32_e32 v2, s0, v1
v_xor_b32_e32 v2, s1, v2
v_add_nc_u32_e32 v2, s2, v2
v_subrev_nc_u32_e32 v2, s3, v2
v_lshlrev_b32_e32 v3, 2, v1
global_store_dword v3, v2, s[4:5]
s_endpgm
