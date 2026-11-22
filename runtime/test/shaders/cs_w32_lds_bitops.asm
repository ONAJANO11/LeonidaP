; LDS atomics with CPU-only checks until now: ds_and_b32, ds_or_b32, ds_xor_b32, ds_sub_u32 and ds_and_b64
; (RDNA2 ISA, DS opcodes 9, 10, 11, 1 and 73). All 32 lanes of one wave32 group hit the same LDS words, so the
; result is only right when every lane's read-modify-write lands. L is the lane (v0). Pseudocode:
;   ds_sub_u32  M[A] = M[A] - DATA          ds_and_b32  M[A] = M[A] & DATA
;   ds_or_b32   M[A] = M[A] | DATA          ds_xor_b32  M[A] = M[A] ^ DATA
;   ds_and_b64  M64[A] = M64[A] & DATA64 (A = ADDR + offset, dwords low first)
; Lane 0 seeds the words (EXEC = 1), then s_barrier, the atomics in all lanes, s_barrier, and lane 0 reads them back:
;   out[0] 0x00 = 0xFFFFFFFF, and with ~(1 << (L & 15))        -> bits 0..15 cleared          = 0xFFFF0000
;   out[1] 0x04 = 0x80000000, or with 1 << (L & 30)            -> | 0x55555555                = 0xD5555555
;   out[2] 0x08 = 0x12345678, xor with 1 << L (each bit once)  -> ^ 0xFFFFFFFF                = 0xEDCBA987
;   out[3] 0x0C = 100, sub L + 1 in lanes 0..19 only (EXEC = 0x000FFFFF): 100 - 210 = -110 = 0xFFFFFF92 (wraps)
;   out[4] 0x10 low  = 0xFFFFFFFF, ds_and_b64 offset:16 with low  ~(1 << (L & 15))         = 0xFFFF0000
;   out[5] 0x14 high = 0xF0F0F0F0, ds_and_b64 offset:16 with high ~(1 << ((L & 15) + 16))  = 0x0000F0F0
;   out[6] 0x4C = 1000, ds_sub_u32 of L at address 4 * (L & 3) + offset:64: lanes 3, 7, .., 31 sum to 136 -> 864
;   out[7] 0x40 = 1000, the same for lanes 0, 4, .., 28 (sum 112)                                     -> 888
; group: 32,1,1
; Run: vk_compute_test cs_w32_lds_bitops.spv 1 dump
;   -> out[0..7] = 0xFFFF0000 0xD5555555 0xEDCBA987 0xFFFFFF92 0xFFFF0000 0x0000F0F0 0x00000360 0x00000378
; mattr: +wavefrontsize32,-wavefrontsize64
s_mov_b32 exec_lo, 1
v_mov_b32_e32 v1, 0
v_mov_b32_e32 v2, -1
ds_write_b32 v1, v2
v_mov_b32_e32 v2, 0x80000000
ds_write_b32 v1, v2 offset:4
v_mov_b32_e32 v2, 0x12345678
ds_write_b32 v1, v2 offset:8
v_mov_b32_e32 v2, 0x64
ds_write_b32 v1, v2 offset:12
v_mov_b32_e32 v2, -1
v_mov_b32_e32 v3, 0xf0f0f0f0
ds_write_b64 v1, v[2:3] offset:16
v_mov_b32_e32 v2, 0x3e8
ds_write_b32 v1, v2 offset:64
ds_write_b32 v1, v2 offset:68
ds_write_b32 v1, v2 offset:72
ds_write_b32 v1, v2 offset:76
s_waitcnt lgkmcnt(0)
s_mov_b32 exec_lo, -1
s_barrier
v_mov_b32_e32 v1, 0
v_and_b32_e32 v4, 15, v0
v_lshlrev_b32_e64 v5, v4, 1
v_not_b32_e32 v5, v5
ds_and_b32 v1, v5
v_and_b32_e32 v6, 30, v0
v_lshlrev_b32_e64 v6, v6, 1
ds_or_b32 v1, v6 offset:4
v_lshlrev_b32_e64 v7, v0, 1
ds_xor_b32 v1, v7 offset:8
v_add_nc_u32_e32 v8, 16, v4
v_lshlrev_b32_e64 v9, v8, 1
v_not_b32_e32 v11, v9
v_mov_b32_e32 v10, v5
ds_and_b64 v1, v[10:11] offset:16
v_and_b32_e32 v12, 3, v0
v_lshlrev_b32_e32 v12, 2, v12
ds_sub_u32 v12, v0 offset:64
s_mov_b32 exec_lo, 0xfffff
v_add_nc_u32_e32 v13, 1, v0
ds_sub_u32 v1, v13 offset:12
s_mov_b32 exec_lo, -1
s_waitcnt lgkmcnt(0)
s_barrier
v_cmpx_eq_u32_e32 0, v0
ds_read_b32 v20, v1
ds_read_b32 v21, v1 offset:4
ds_read_b32 v22, v1 offset:8
ds_read_b32 v23, v1 offset:12
ds_read_b64 v[24:25], v1 offset:16
ds_read_b32 v26, v1 offset:76
ds_read_b32 v27, v1 offset:64
s_waitcnt lgkmcnt(0)
v_mov_b32_e32 v30, 0
buffer_store_dword v20, v30, s[0:3], 0 offen
v_mov_b32_e32 v30, 4
buffer_store_dword v21, v30, s[0:3], 0 offen
v_mov_b32_e32 v30, 8
buffer_store_dword v22, v30, s[0:3], 0 offen
v_mov_b32_e32 v30, 12
buffer_store_dword v23, v30, s[0:3], 0 offen
v_mov_b32_e32 v30, 16
buffer_store_dword v24, v30, s[0:3], 0 offen
v_mov_b32_e32 v30, 20
buffer_store_dword v25, v30, s[0:3], 0 offen
v_mov_b32_e32 v30, 24
buffer_store_dword v26, v30, s[0:3], 0 offen
v_mov_b32_e32 v30, 28
buffer_store_dword v27, v30, s[0:3], 0 offen
s_endpgm
