; Wave64 lane mask read as plain data (#210). v_cmp_ne_u32 (lane & 5) != 0 sets lanes 1, 3-7 of every 8:
; s[20:21] = 0xfafafafa_fafafafa. Each consumer below reads that mask as a 32- or 64-bit value, so it must see the
; wave's bits, not this lane's bool (0 / 0xffffffff), which is what it read before the fix.
; The s_mov_b32 exec_hi, -1 marks the shader as wave64 (the translator infers the wave size from it).
; Assemble (as scripts/run_gpu_tests.py / build.py do): llvm-mc -triple=amdgcn--amdpal -mcpu=gfx1013 -mattr=+wavefrontsize64 -show-encoding
; mattr: +wavefrontsize64,-wavefrontsize32
; Run: VKC_SUBGROUP_SIZE=64 vk_compute_test cs_wm_as_data.spv 1 dump (or translated with --wave64-emulate)
; Expect: out[0..7] = 0xfafafafa 0xfafafafa 0x7d7d7d7d 0xfa 0xfafafafa 0xafafafaf 0xfafafafb 0xfafafafd
;   0: v_mov_b32 of the low half          1: s_mov_b64 copy, then the high half   2: s_lshr_b32 by 1
;   3: s_and_b32 with 0xff                 4: v_writelane / v_readlane round trip  5: s_lshr_b64 by 4, low half
;   6: v_add_nc_u32 with the low half      7: s_add_u32 of the high half and 3
s_mov_b32 exec_hi, -1
v_and_b32_e32 v5, 5, v0
v_cmp_ne_u32_e64 s[20:21], 0, v5
v_mov_b32_e32 v10, s20
s_mov_b64 s[40:41], s[20:21]
v_mov_b32_e32 v11, s41
s_lshr_b32 s42, s20, 1
v_mov_b32_e32 v12, s42
s_and_b32 s43, s20, 0xff
v_mov_b32_e32 v13, s43
v_writelane_b32 v20, s20, 0
v_readlane_b32 s44, v20, 0
v_mov_b32_e32 v14, s44
s_lshr_b64 s[46:47], s[20:21], 4
v_mov_b32_e32 v15, s46
v_add_nc_u32_e64 v16, s20, 1
s_add_u32 s48, s21, 3
v_mov_b32_e32 v17, s48
v_cmpx_eq_u32_e32 0, v0
v_mov_b32_e32 v2, 0
buffer_store_dword v10, v2, s[0:3], 0 offen
v_mov_b32_e32 v2, 4
buffer_store_dword v11, v2, s[0:3], 0 offen
v_mov_b32_e32 v2, 8
buffer_store_dword v12, v2, s[0:3], 0 offen
v_mov_b32_e32 v2, 12
buffer_store_dword v13, v2, s[0:3], 0 offen
v_mov_b32_e32 v2, 16
buffer_store_dword v14, v2, s[0:3], 0 offen
v_mov_b32_e32 v2, 20
buffer_store_dword v15, v2, s[0:3], 0 offen
v_mov_b32_e32 v2, 24
buffer_store_dword v16, v2, s[0:3], 0 offen
v_mov_b32_e32 v2, 28
buffer_store_dword v17, v2, s[0:3], 0 offen
s_endpgm
