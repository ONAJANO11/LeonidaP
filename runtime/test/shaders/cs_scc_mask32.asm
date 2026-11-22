; From @kotorvr (GPU fuzzer, posted in #477 as a test for the wave32 SCC fix and s_not_b32 follow-up).
; Wave32 SCC of 32-bit lane-mask SALU ops. SCC is one bit per wave: after s_and/s_or/s_andn2/s_not/s_xnor_b32 on a
; lane mask (EXEC, VCC, an SGPR written by v_cmp) it says whether the wave's whole result is non-zero, so every lane
; sees the same SCC. Each lane holds only its own bit in the translator, so it has to vote, as the b64 forms do.
; Two wave32s (64 threads); v1 = lane in the wave.
; (A) EXEC &= (lane != 0): 31 lanes left, SCC = 1 -> s_cselect s6 = 7 in every lane (lane 0 alone: 3).
; (B) the shape LLVM's discard bookkeeping uses: s8 = lanes 0-7, EXEC = lanes 0-3, s_andn2_b32 s10, s8, exec_lo =
;     lanes 4-7, SCC = 1, s_cbranch_scc1 taken by the wave -> `s_mov_b32 s7, 5` skipped: s7 = 0 (per lane: 5 in
;     lanes 0-3 and 8-31).
; (C) s_not_b32 s11, s8 = lanes 8-31, SCC = 1 -> s12 = 9 (per lane: 4 in lanes 0-7).
; (D) s_or_b32 s13, s8, vcc_lo (lanes 0-7), SCC = 1 -> s14 = 11 (per lane: 6 in lanes 8-31).
; (E) s_xnor_b32 s15, s8, vcc_lo with vcc = lanes 0-3: lanes 0-3 and 8-31, SCC = 1 -> s16 = 13 (per lane: 2 in 4-7).
; Stores: out[0] thread 0 s6, out[1] thread 2 s7, out[2] thread 40 s7, out[3] thread 3 s12, out[4] thread 20 s14,
;   out[5] thread 5 s16, out[6] thread 32 s6, out[7] thread 37 s16.
; Run: vk_compute_test cs_scc_mask32.spv 1 dump
;   -> out[0..7] = 7 0 0 9 11 13 7 13   (per-lane SCC gave 3 5 5 4 6 2 3 2)
; mattr: +wavefrontsize32,-wavefrontsize64
v_mbcnt_lo_u32_b32 v1, -1, 0
s_mov_b32 s20, exec_lo
v_cmp_ne_u32_e32 vcc_lo, 0, v1
s_and_b32 exec_lo, exec_lo, vcc_lo
s_cselect_b32 s6, 7, 3
s_mov_b32 exec_lo, s20
v_cmp_gt_u32_e64 s8, 8, v1
v_cmp_gt_u32_e32 vcc_lo, 4, v1
s_and_saveexec_b32 s9, vcc_lo
s_mov_b32 s7, 0
s_andn2_b32 s10, s8, exec_lo
s_cbranch_scc1 1
s_mov_b32 s7, 5
s_mov_b32 exec_lo, s9
s_not_b32 s11, s8
s_cselect_b32 s12, 9, 4
s_or_b32 s13, s8, vcc_lo
s_cselect_b32 s14, 11, 6
s_xnor_b32 s15, s8, vcc_lo
s_cselect_b32 s16, 13, 2
v_cmpx_eq_u32_e32 0, v0
v_mov_b32_e32 v20, s6
v_mov_b32_e32 v21, 0
buffer_store_dword v20, v21, s[0:3], 0 offen
s_mov_b32 exec_lo, -1
v_cmpx_eq_u32_e32 2, v0
v_mov_b32_e32 v20, s7
v_mov_b32_e32 v21, 4
buffer_store_dword v20, v21, s[0:3], 0 offen
s_mov_b32 exec_lo, -1
v_cmpx_eq_u32_e32 40, v0
v_mov_b32_e32 v20, s7
v_mov_b32_e32 v21, 8
buffer_store_dword v20, v21, s[0:3], 0 offen
s_mov_b32 exec_lo, -1
v_cmpx_eq_u32_e32 3, v0
v_mov_b32_e32 v20, s12
v_mov_b32_e32 v21, 12
buffer_store_dword v20, v21, s[0:3], 0 offen
s_mov_b32 exec_lo, -1
v_cmpx_eq_u32_e32 20, v0
v_mov_b32_e32 v20, s14
v_mov_b32_e32 v21, 16
buffer_store_dword v20, v21, s[0:3], 0 offen
s_mov_b32 exec_lo, -1
v_cmpx_eq_u32_e32 5, v0
v_mov_b32_e32 v20, s16
v_mov_b32_e32 v21, 20
buffer_store_dword v20, v21, s[0:3], 0 offen
s_mov_b32 exec_lo, -1
v_cmpx_eq_u32_e32 32, v0
v_mov_b32_e32 v20, s6
v_mov_b32_e32 v21, 24
buffer_store_dword v20, v21, s[0:3], 0 offen
s_mov_b32 exec_lo, -1
v_cmpx_eq_u32_e32 37, v0
v_mov_b32_e32 v20, s16
v_mov_b32_e32 v21, 28
buffer_store_dword v20, v21, s[0:3], 0 offen
s_endpgm
