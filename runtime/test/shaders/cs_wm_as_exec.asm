; Wave64: a lane mask combined with data, then written to EXEC (#210). s[20:21] = 0xfafafafa_fafafafa (lanes 1, 3-7
; of every 8). s_and_b32 with the data word 0x0f0f0f0f gives the value 0x0a0a0a0a, and s_mov_b32 exec_lo of it turns on
; the lanes whose bit is set: lanes 1 and 3 of every 8 in lanes 0-31. Those write 1 to v20; lanes 0-7 store it.
; Assemble (as scripts/run_gpu_tests.py / build.py do): llvm-mc -triple=amdgcn--amdpal -mcpu=gfx1013 -mattr=+wavefrontsize64 -show-encoding
; mattr: +wavefrontsize64,-wavefrontsize32
; Run: VKC_SUBGROUP_SIZE=64 vk_compute_test cs_wm_as_exec.spv 1 dump (or translated with --wave64-emulate)
; Expect: out[0..7] = 0 1 0 1 0 0 0 0
s_mov_b32 exec_hi, -1
v_mov_b32_e32 v20, 0
v_and_b32_e32 v5, 5, v0
v_cmp_ne_u32_e64 s[20:21], 0, v5
s_mov_b32 s42, 0x0f0f0f0f
s_and_b32 s40, s20, s42
s_mov_b32 exec_lo, s40
v_mov_b32_e32 v20, 1
s_mov_b32 exec_lo, -1
s_mov_b32 exec_hi, -1
v_cmpx_gt_u32_e32 8, v0
v_lshlrev_b32_e32 v2, 2, v0
buffer_store_dword v20, v2, s[0:3], 0 offen
s_endpgm
