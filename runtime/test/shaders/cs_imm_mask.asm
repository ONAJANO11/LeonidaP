; Wave64 immediates used as lane masks and as 64-bit data. An inline constant is
; sign-extended to 64 bits: EXEC = 5 is lanes 0 and 2, EXEC = -4 lanes 2-63, EXEC & 6 lanes 1 and 2. v2 counts
; which writes each lane took: lane 0 = 1, lane 1 = 100, lane 2 = 1 + 10 + 100 = 111, lane 63 = 10.
; s_bcnt1 of -16 = 60, s_bcnt0 of -1 = 0, s_ff0 of -1 = -1 (no zero bit), s_bcnt1 of -1 = 64.
; Until 2026-11-22 every lane took bit 0 of an immediate mask (EXEC = 5 turned every lane on, -4 none) and the
; high word of a 64-bit immediate read as 0. The lane comes from LocalInvocationIndex, so this is exact on 32-wide
; subgroups too. Assemble: llvm-mc -triple=amdgcn--amdpal -mcpu=gfx1013 -show-encoding
; Run: VKC_SUBGROUP_SIZE=64 vk_compute_test cs_imm_mask.spv 1 dump
; Expect: out[0..7] = 1 100 111 10 60 0 4294967295 64
v_mbcnt_lo_u32_b32 v1, -1, 0
v_mbcnt_hi_u32_b32 v1, -1, v1
v_mov_b32_e32 v2, 0
s_mov_b64 exec, 5
v_add_nc_u32_e32 v2, 1, v2
s_mov_b64 exec, -4
v_add_nc_u32_e32 v2, 10, v2
s_mov_b64 exec, -1
s_and_b64 exec, exec, 6
v_add_nc_u32_e32 v2, 100, v2
s_mov_b64 exec, -1
s_bcnt1_i32_b64 s8, -16
s_bcnt0_i32_b64 s9, -1
s_ff0_i32_b64 s10, -1
s_bcnt1_i32_b64 s11, -1
v_cmpx_eq_u32_e32 0, v1
v_mov_b32_e32 v3, 0
buffer_store_dword v2, v3, s[0:3], 0 offen
v_mov_b32_e32 v4, s8
v_mov_b32_e32 v3, 16
buffer_store_dword v4, v3, s[0:3], 0 offen
v_mov_b32_e32 v4, s9
v_mov_b32_e32 v3, 20
buffer_store_dword v4, v3, s[0:3], 0 offen
v_mov_b32_e32 v4, s10
v_mov_b32_e32 v3, 24
buffer_store_dword v4, v3, s[0:3], 0 offen
v_mov_b32_e32 v4, s11
v_mov_b32_e32 v3, 28
buffer_store_dword v4, v3, s[0:3], 0 offen
s_mov_b64 exec, -1
v_cmpx_eq_u32_e32 1, v1
v_mov_b32_e32 v3, 4
buffer_store_dword v2, v3, s[0:3], 0 offen
s_mov_b64 exec, -1
v_cmpx_eq_u32_e32 2, v1
v_mov_b32_e32 v3, 8
buffer_store_dword v2, v3, s[0:3], 0 offen
s_mov_b64 exec, -1
v_cmpx_eq_u32_e32 63, v1
v_mov_b32_e32 v3, 12
buffer_store_dword v2, v3, s[0:3], 0 offen
s_endpgm
