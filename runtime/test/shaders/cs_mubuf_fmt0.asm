; Raw MUBUF with FORMAT=INVALID (0) issues no memory access. FORMAT=32_FLOAT (22) is the control.
; Both descriptors start at byte 64, and every access is within bounds.
; out[0..3]: literal OOB_SELECT 3, each load reads offset 0 of its V#. Stores target offsets 4 and 8.
; out[4..7]: run-time OOB_SELECT 1, same loads and stores at offsets 12 and 16.
; In each group the words are: invalid load 0, valid load 0xeeeeeeee (heap fill),
; invalid store target still 0xeeeeeeee, valid store target 0x12345678.
v_cmpx_eq_u32_e32 0, v0
s_mov_b32 s4, 64
s_mov_b32 s5, 0
s_mov_b32 s6, 32
s_mov_b32 s7, 0x31000fac
s_mov_b32 s8, 64
s_mov_b32 s9, 0
s_mov_b32 s10, 32
s_mov_b32 s11, 0x31016fac
v_mov_b32 v1, 0x12345678
buffer_load_dword v2, off, s[4:7], 0
buffer_load_dword v3, off, s[8:11], 0
buffer_store_dword v1, off, s[4:7], 0 offset:4
buffer_store_dword v1, off, s[8:11], 0 offset:8
s_mov_b32 s12, 0x20000000
s_xor_b32 s7, s7, s12
s_xor_b32 s11, s11, s12
buffer_load_dword v6, off, s[4:7], 0
buffer_load_dword v7, off, s[8:11], 0
buffer_store_dword v1, off, s[4:7], 0 offset:12
buffer_store_dword v1, off, s[8:11], 0 offset:16
s_waitcnt vmcnt(0)
s_waitcnt_vscnt null, 0
buffer_load_dword v4, off, s[0:3], 0 offset:68
buffer_load_dword v5, off, s[0:3], 0 offset:72
buffer_load_dword v8, off, s[0:3], 0 offset:76
buffer_load_dword v9, off, s[0:3], 0 offset:80
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[2:5], off, s[0:3], 0
buffer_store_dwordx4 v[6:9], off, s[0:3], 0 offset:16
s_endpgm
