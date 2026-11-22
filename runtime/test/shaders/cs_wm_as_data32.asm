; Wave32 lane mask read as plain data (#210). v_cmp_ne_u32 (lane & 5) != 0 sets lanes 1, 3-7 of every 8:
; s20 = 0xfafafafa. Reading it as a value has to give the wave's bits, not this lane's bool (0 / 0xffffffff).
; Assemble (as scripts/run_gpu_tests.py / build.py do): llvm-mc -triple=amdgcn--amdpal -mcpu=gfx1013 -show-encoding
; Run: vk_compute_test cs_wm_as_data32.spv 1 dump
; Expect: out[0..3] = 0xfafafafa 0xfafafaf 0xfafbf900 0xfafafafa
;   0: v_mov_b32 of the mask, stored     1: s_lshr_b32 by 4     2: s_lshl_b32 by 8, plus 0xff00
;   3: v_or_b32 of the mask and 0 into a VGPR (a mask), stored
v_and_b32_e32 v5, 5, v0
v_cmp_ne_u32_e64 s20, 0, v5
v_mov_b32_e32 v10, s20
s_lshr_b32 s40, s20, 4
v_mov_b32_e32 v11, s40
s_lshl_b32 s41, s20, 8
s_add_u32 s41, s41, 0xff00
v_mov_b32_e32 v12, s41
v_or_b32_e64 v13, s20, 0
v_cmpx_eq_u32_e32 0, v0
v_mov_b32_e32 v2, 0
buffer_store_dword v10, v2, s[0:3], 0 offen
v_mov_b32_e32 v2, 4
buffer_store_dword v11, v2, s[0:3], 0 offen
v_mov_b32_e32 v2, 8
buffer_store_dword v12, v2, s[0:3], 0 offen
v_mov_b32_e32 v2, 12
buffer_store_dword v13, v2, s[0:3], 0 offen
s_endpgm
