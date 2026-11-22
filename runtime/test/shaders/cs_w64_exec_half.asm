; Wave64 32-bit ops on one half of EXEC / VCC (FUZZ-8, tools/cf_diff.py). One wave64 (64 threads); v1 = lane.
; (A) vcc = lanes 0-7; s_and_b32 exec_lo, exec_lo, vcc_lo leaves lanes 0-7 and 32-63 on (EXEC_HI is untouched):
;     v5 = 1 there, 0 in lanes 8-31.
; (B) vcc = lanes 0-39; s_and_b32 exec_hi, exec_hi, vcc_hi leaves lanes 0-39 on: v6 = 1 there, 0 in lanes 40-63.
; (C) vcc = lanes 41-63. s_and_b32 s10, vcc_lo, exec_lo = 0, so SCC = 0 for the whole wave: s11 = 3.
;     s_and_b32 s12, vcc_hi, exec_hi != 0, so SCC = 1 for the whole wave: s13 = 9.
; Stores: out[0] thread 3 v5, out[1] thread 10 v5, out[2] thread 40 v5, out[3] thread 35 v6, out[4] thread 50 v6,
;   out[5] thread 50 s11, out[6] thread 0 s13, out[7] thread 60 s13.
; Run: vk_compute_test cs_w64_exec_half.spv 1 dump   (needs a 64-wide subgroup, or LEONIDA_W64_EMULATE=1)
;   -> out[0..7] = 1 0 1 1 0 3 9 9
; Before 2026-11-22 the translator turned lanes 32-63 off in (A) (out[2] = 0), rejected (B) ("destination
; exec_hi"), and took each lane's own bit as SCC in (C) (out[5] = 7, out[6] = 4).
; mattr: +wavefrontsize64,-wavefrontsize32
v_mbcnt_lo_u32_b32 v1, -1, 0
v_mbcnt_hi_u32_b32 v1, -1, v1
v_mov_b32_e32 v5, 0
v_mov_b32_e32 v6, 0
s_mov_b64 s[20:21], exec
v_cmp_gt_u32_e32 vcc, 8, v1
s_and_b32 exec_lo, exec_lo, vcc_lo
v_mov_b32_e32 v5, 1
s_mov_b64 exec, s[20:21]
v_cmp_gt_u32_e32 vcc, 40, v1
s_and_b32 exec_hi, exec_hi, vcc_hi
v_mov_b32_e32 v6, 1
s_mov_b64 exec, s[20:21]
v_cmp_lt_u32_e32 vcc, 40, v1
s_and_b32 s10, vcc_lo, exec_lo
s_cselect_b32 s11, 7, 3
s_and_b32 s12, vcc_hi, exec_hi
s_cselect_b32 s13, 9, 4
v_cmpx_eq_u32_e32 3, v0
v_mov_b32_e32 v20, v5
v_mov_b32_e32 v21, 0
buffer_store_dword v20, v21, s[0:3], 0 offen
s_mov_b64 exec, -1
v_cmpx_eq_u32_e32 10, v0
v_mov_b32_e32 v20, v5
v_mov_b32_e32 v21, 4
buffer_store_dword v20, v21, s[0:3], 0 offen
s_mov_b64 exec, -1
v_cmpx_eq_u32_e32 40, v0
v_mov_b32_e32 v20, v5
v_mov_b32_e32 v21, 8
buffer_store_dword v20, v21, s[0:3], 0 offen
s_mov_b64 exec, -1
v_cmpx_eq_u32_e32 35, v0
v_mov_b32_e32 v20, v6
v_mov_b32_e32 v21, 12
buffer_store_dword v20, v21, s[0:3], 0 offen
s_mov_b64 exec, -1
v_cmpx_eq_u32_e32 50, v0
v_mov_b32_e32 v20, v6
v_mov_b32_e32 v21, 16
buffer_store_dword v20, v21, s[0:3], 0 offen
s_mov_b64 exec, -1
v_cmpx_eq_u32_e32 50, v0
v_mov_b32_e32 v20, s11
v_mov_b32_e32 v21, 20
buffer_store_dword v20, v21, s[0:3], 0 offen
s_mov_b64 exec, -1
v_cmpx_eq_u32_e32 0, v0
v_mov_b32_e32 v20, s13
v_mov_b32_e32 v21, 24
buffer_store_dword v20, v21, s[0:3], 0 offen
s_mov_b64 exec, -1
v_cmpx_eq_u32_e32 60, v0
v_mov_b32_e32 v20, s13
v_mov_b32_e32 v21, 28
buffer_store_dword v20, v21, s[0:3], 0 offen
s_endpgm
