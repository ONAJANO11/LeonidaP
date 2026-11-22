; Wave64 EXEC and VCC read as plain data (#210), next to mask readers that must keep one bit per lane.
; EXEC = lanes 16-47 (exec_lo = 0xffff0000, exec_hi = 0xffff). v_cmp_gt_u32 40 > lane under that EXEC sets VCC for
; lanes 16-39 (lanes outside EXEC write 0): vcc_lo = 0xffff0000, vcc_hi = 0xff; s[4:5] is a copy of it. Lane 16
; stores (it is in EXEC and in VCC).
; Assemble (as scripts/run_gpu_tests.py / build.py do): llvm-mc -triple=amdgcn--amdpal -mcpu=gfx1013 -mattr=+wavefrontsize64 -show-encoding
; mattr: +wavefrontsize64,-wavefrontsize32
; Run: VKC_SUBGROUP_SIZE=64 vk_compute_test cs_wm_as_data2.spv 1 dump (or translated with --wave64-emulate)
; Expect: out[0..7] = 0xffff0000 0x10000 0xff 0xff 24 2 0xffff0001 0xffff0000
;   0: v_mov_b32 of exec_lo, stored         1: s_add_u32 exec_hi, 1                2: v_mov_b32 of vcc_hi, stored
;   3: v_cndmask_b32 with VCC as the selector and s5 (the copy's high half) as a value: VCC is a mask, s5 data
;   4: s_bcnt1_i32_b64 of VCC (a mask reader, unchanged)
;   5: v_cndmask_b32 1, 2 selected by s[4:5] & EXEC (a mask from two masks, still one bit per lane)
;   6: s_or_b32 s4, 1                       7: exec_lo through v_readfirstlane_b32, then read as data
v_mov_b32_e32 v1, 0
s_mov_b32 exec_lo, 0xffff0000
s_mov_b32 exec_hi, 0xffff
v_mov_b32_e32 v10, exec_lo
s_add_u32 s40, exec_hi, 1
v_cmp_gt_u32_e32 vcc, 40, v0
s_mov_b64 s[4:5], vcc
s_bcnt1_i32_b64 s41, vcc
s_and_b64 s[42:43], s[4:5], exec
v_readfirstlane_b32 s48, v10
s_mov_b32 exec_lo, -1
s_mov_b32 exec_hi, -1
v_mov_b32_e32 v11, s40
v_mov_b32_e32 v12, vcc_hi
v_cndmask_b32_e64 v13, 7, s5, vcc
v_mov_b32_e32 v14, s41
v_cndmask_b32_e64 v15, 1, 2, s[42:43]
s_or_b32 s44, s4, 1
v_mov_b32_e32 v16, s44
v_mov_b32_e32 v17, s48
v_cmpx_eq_u32_e32 16, v0
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
