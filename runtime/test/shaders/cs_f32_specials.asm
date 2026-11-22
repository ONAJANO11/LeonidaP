; s[0:3] is the output V#, s4 is one binary32 input as a raw word.
; RDNA2 RX 6800 hardware words for v_rcp_f32, v_rsq_f32, v_sqrt_f32, v_log_f32, v_exp_f32, v_rcp_iflag_f32:
; denormal inputs read as signed zero, denormal results flush to signed zero, invalid negative inputs
; (v_sqrt/v_rsq/v_log) return 0xffc00000, and v_rcp_iflag_f32 is bit-identical to v_rcp_f32.
; out[0..5] = v_rcp_f32, v_rsq_f32, v_sqrt_f32, v_log_f32, v_exp_f32, v_rcp_iflag_f32.
; group: 1,1,1
; rsrc2: 0x808a
v_mov_b32_e32 v0, s4
v_mov_b32_e32 v1, 0
v_rcp_f32_e32 v2, v0
v_rsq_f32_e32 v3, v0
v_sqrt_f32_e32 v4, v0
v_log_f32_e32 v5, v0
v_exp_f32_e32 v6, v0
v_rcp_iflag_f32_e32 v7, v0
buffer_store_dwordx4 v[2:5], v1, s[0:3], 0 offen
v_mov_b32_e32 v1, 16
buffer_store_dwordx2 v[6:7], v1, s[0:3], 0 offen
s_endpgm
