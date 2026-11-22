; LDS data exchange between the lanes of ONE wave without s_barrier. RDNA2 runs a wave in lockstep: every lane's
; ds_write of one instruction is done before any lane's ds_read of the next (the LDS queue of a wave is in order),
; so shaders write LDS in one lane and read the slot of another lane with only s_waitcnt lgkmcnt(0), or nothing,
; in between (reductions, prefix sums, transposes). SPIR-V gives no such order between invocations; the translator
; has to add one (docs/design.md "LDS between lanes of one wave"). Wave32, a 64-thread group = two waves, each
; exchanging inside its own 32 slots, 512 groups. 256 rounds of four dependent exchanges (1024 exchanges), every
; exchange both a read-after-write (this write, another lane's read) and a write-after-read (the next write
; overwrites a slot another lane may still read):
;   phase A: read lane (l + 1) mod 32 (rotation)   with s_waitcnt between write and read
;   phase B: read lane 31 - l (reversal)           with s_waitcnt
;   phase C: read lane l ^ 16 (butterfly)          no s_waitcnt between write and read (LDS ops stay in order)
;   phase D: read lane l ^ 5  (butterfly)          no s_waitcnt
; v[l] <- v[p(l)] + 1 each phase, v starts at 7 * (global thread index of the wave's lane 0 + l) + 3. One round is
; v_new[l] = v[pA(pB(pC(pD(l))))] + 4, so after R = 256 rounds, with Q(l) = pA(pB(pC(pD(l)))) applied R times to l
; (the shader computes it on the side in VALU, no LDS):
;   v[l] = 7 * (wave base + Q^R(l)) + 3 + 4 * R.
; Each thread stores actual XOR expected to out[group * 64 + tid]: every word is 0 when the exchange is ordered. A
; read that ran before another lane's write (or after the next one) returns a value of another round (off by
; 4 * k) or of the initial data, so the word is non-zero.
; Run: vk_compute_test cs_lds_wave_exchange.spv 512 zero64   -> 0 mismatches
; mattr: +wavefrontsize32,-wavefrontsize64
v_and_b32_e32 v1, 31, v0
v_and_b32_e32 v2, 32, v0
v_lshl_add_u32 v3, s4, 6, v2
v_lshlrev_b32_e32 v4, 2, v0
v_add_nc_u32_e32 v6, v3, v1
v_mad_u32_u24 v5, v6, 7, 3
v_mov_b32_e32 v7, v1
v_add_nc_u32_e32 v11, 1, v1
v_and_b32_e32 v11, 31, v11
v_add_nc_u32_e32 v11, v2, v11
v_lshlrev_b32_e32 v11, 2, v11
v_sub_nc_u32_e32 v12, 31, v1
v_add_nc_u32_e32 v12, v2, v12
v_lshlrev_b32_e32 v12, 2, v12
v_xor_b32_e32 v13, 16, v1
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
v_xor_b32_e32 v7, 16, v7
v_sub_nc_u32_e32 v7, 31, v7
v_add_nc_u32_e32 v7, 1, v7
v_and_b32_e32 v7, 31, v7
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
