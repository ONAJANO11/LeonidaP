; Wave64 lane mask combined with 64-bit data in SALU pair ops (#210). s[20:21] = 0xfafafafa_fafafafa (lanes 1, 3-7 of
; every 8, as in cs_wm_as_data), s[2:3] = 0xff00ff00_0f0f0f0f (data). Each register of the result pair has to be
; its own word; the mask-only path wrote one word into both. Lane 0 stores.
; Assemble (as scripts/run_gpu_tests.py / build.py do): llvm-mc -triple=amdgcn--amdpal -mcpu=gfx1013 -mattr=+wavefrontsize64 -show-encoding
; mattr: +wavefrontsize64,-wavefrontsize32
; Run: VKC_SUBGROUP_SIZE=64 vk_compute_test cs_wm_as_data64.spv 1 dump (or translated with --wave64-emulate)
; Expect: out[0..7] = 0xa0a0a0a 0xfa00fa00 0xf5f5f5f5 0x5fa05fa 0xfafafafa 0xfafafafa 0xff00ff00 0xfafafafa
;   0, 1: s_and_b64 mask, data     2, 3: s_xor_b64 mask, data     4, 5: s_cselect_b64 mask, data with SCC = 1
;   6: high word of s_or_b64 data, 1 (data only)
;   7: v_cndmask_b32 s20 (a value), 0, selected by s[20:21] (a mask): lane 0's bit is clear, so it keeps s20's bits
s_mov_b32 exec_hi, -1
v_and_b32_e32 v5, 5, v0
v_cmp_ne_u32_e64 s[20:21], 0, v5
s_mov_b32 s2, 0x0f0f0f0f
s_mov_b32 s3, 0xff00ff00
s_and_b64 s[50:51], s[20:21], s[2:3]
s_xor_b64 s[52:53], s[20:21], s[2:3]
s_cmp_eq_u32 s2, s2
s_cselect_b64 s[54:55], s[20:21], s[2:3]
s_or_b64 s[56:57], s[2:3], 1
v_cndmask_b32_e64 v17, s20, 0, s[20:21]
v_mov_b32_e32 v10, s50
v_mov_b32_e32 v11, s51
v_mov_b32_e32 v12, s52
v_mov_b32_e32 v13, s53
v_mov_b32_e32 v14, s54
v_mov_b32_e32 v15, s55
v_mov_b32_e32 v16, s57
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
