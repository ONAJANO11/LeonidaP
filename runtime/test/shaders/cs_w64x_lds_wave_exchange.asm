; LDS data exchange between the lanes of ONE wave64 without s_barrier, the halves of the wave included (phase C
; reads the other half). Same program and arithmetic as cs_lds_wave_exchange.asm with 64 lanes: a 64-thread group
; is one wave, 512 groups, 256 rounds of four dependent exchanges, each a read-after-write and a write-after-read
; between lanes:
;   phase A: read lane (l + 1) mod 64 (rotation)   with s_waitcnt between write and read
;   phase B: read lane 63 - l (reversal)           with s_waitcnt
;   phase C: read lane l ^ 32 (the other half)     no s_waitcnt between write and read (LDS ops stay in order)
;   phase D: read lane l ^ 5  (butterfly)          no s_waitcnt
; v[l] <- v[p(l)] + 1 each phase, v starts at 7 * (group * 64 + l) + 3, so after R = 256 rounds
;   v[l] = 7 * (group * 64 + Q^R(l)) + 3 + 4 * R,  Q(l) = pA(pB(pC(pD(l)))), computed on the side in VALU.
; Each thread stores actual XOR expected to out[group * 64 + tid]; every word is 0 when the exchange is ordered.
; Native wave64 runs on a 64-wide subgroup (AMD); a 32-wide device needs --wave64-emulate (LEONIDA_W64_EMULATE=1).
; Run: vk_compute_test cs_w64x_lds_wave_exchange.spv 512 zero64 --require-wave64   -> 0 mismatches
; (the lane id comes from v_mbcnt_lo/hi: that is what marks the program as wave64 for the translator)
; mattr: +wavefrontsize64,-wavefrontsize32
v_mbcnt_lo_u32_b32 v1, -1, 0
v_mbcnt_hi_u32_b32 v1, -1, v1
v_mov_b32_e32 v2, 0
v_lshl_add_u32 v3, s4, 6, v2
v_lshlrev_b32_e32 v4, 2, v0
v_add_nc_u32_e32 v6, v3, v1
v_mad_u32_u24 v5, v6, 7, 3
v_mov_b32_e32 v7, v1
v_add_nc_u32_e32 v11, 1, v1
v_and_b32_e32 v11, 63, v11
v_add_nc_u32_e32 v11, v2, v11
v_lshlrev_b32_e32 v11, 2, v11
v_sub_nc_u32_e32 v12, 63, v1
v_add_nc_u32_e32 v12, v2, v12
v_lshlrev_b32_e32 v12, 2, v12
v_xor_b32_e32 v13, 32, v1
v_add_nc_u32_e32 v13, v2, v13
v_lshlrev_b32_e32 v13, 2, v13
v_xor_b32_e32 v14, 5, v1
v_add_nc_u32_e32 v14, v2, v14
v_lshlrev_b32_e32 v14, 2, v14
s_movk_i32 s6, 0x100
ds_write_b32 v4, v5
s_waitcnt lgkmcnt(0)
ds_read_b32 v10, v11
s_waitcnt lgkmcnt(0)
v_add_nc_u32_e32 v5, 1, v10
ds_write_b32 v4, v5
s_waitcnt lgkmcnt(0)
ds_read_b32 v10, v12
s_waitcnt lgkmcnt(0)
v_add_nc_u32_e32 v5, 1, v10
ds_write_b32 v4, v5
ds_read_b32 v10, v13
s_waitcnt lgkmcnt(0)
v_add_nc_u32_e32 v5, 1, v10
ds_write_b32 v4, v5
ds_read_b32 v10, v14
s_waitcnt lgkmcnt(0)
v_add_nc_u32_e32 v5, 1, v10
v_xor_b32_e32 v7, 5, v7
v_xor_b32_e32 v7, 32, v7
v_sub_nc_u32_e32 v7, 63, v7
v_add_nc_u32_e32 v7, 1, v7
v_and_b32_e32 v7, 63, v7
s_sub_u32 s6, s6, 1
s_cmp_lg_u32 s6, 0
s_cbranch_scc1 -34
v_add_nc_u32_e32 v8, v3, v7
v_mad_u32_u24 v8, v8, 7, 3
v_add_nc_u32_e32 v8, 0x400, v8
v_xor_b32_e32 v9, v5, v8
v_lshl_add_u32 v15, s4, 6, v0
v_lshlrev_b32_e32 v15, 2, v15
buffer_store_dword v9, v15, s[0:3], 0 offen
s_endpgm
