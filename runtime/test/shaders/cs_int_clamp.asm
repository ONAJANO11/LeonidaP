; s[0:3] is the output V#. VOP3 clamp on integer add/sub (docs/design.md, Integer clamp). RDNA 2 ISA / LLVM
; uaddsat, usubsat, saddsat, ssubsat: with clamp the result saturates (u32 to [0, 0xFFFFFFFF], i32 to
; [0x80000000, 0x7FFFFFFF], u16 to [0, 0xFFFF]); without it the result wraps. For each op three input pairs, each run
; without and with clamp: out[6k + 2c + m] = op k, case c, m = 0 wrapped / 1 clamped. out[42..63] stay 0xEEEEEEEE.
; The u16 ops write the low half only: v2 is preloaded with 0xBEEF0000, so the high half 0xBEEF is kept, and the
; sources carry nonzero high halves (0xAAAA / 0x5555) that must be ignored. Expected words (from the ISA rule):
;  [0..1] u32 add case 0 (0xffffffff, 0x00000001): max+1 overflows: clamp max, wrap 0: wrapped 0x00000000, clamped 0xffffffff
;  [2..3] u32 add case 1 (0x00000005, 0x00000007): in range: wrapped 0x0000000c, clamped 0x0000000c
;  [4..5] u32 add case 2 (0x80000000, 0x80000001): 2^31+2^31+1 overflows: clamp max, wrap 1: wrapped 0x00000001, clamped 0xffffffff
;  [6..7] u32 sub (s0-s1) case 0 (0x00000000, 0x00000001): 0-1 underflows: clamp 0, wrap max: wrapped 0xffffffff, clamped 0x00000000
;  [8..9] u32 sub (s0-s1) case 1 (0x00000009, 0x00000004): in range: wrapped 0x00000005, clamped 0x00000005
;  [10..11] u32 sub (s0-s1) case 2 (0x7fffffff, 0x80000000): underflow by 1: clamp 0, wrap max: wrapped 0xffffffff, clamped 0x00000000
;  [12..13] u32 subrev (s1-s0) case 0 (0x00000001, 0x00000000): 0-1 underflows: clamp 0, wrap max: wrapped 0xffffffff, clamped 0x00000000
;  [14..15] u32 subrev (s1-s0) case 1 (0x00000004, 0x00000009): in range: wrapped 0x00000005, clamped 0x00000005
;  [16..17] u32 subrev (s1-s0) case 2 (0x80000000, 0x7fffffff): underflow by 1: clamp 0, wrap max: wrapped 0xffffffff, clamped 0x00000000
;  [18..19] i32 add case 0 (0x7fffffff, 0x00000001): INT_MAX+1 overflows: clamp INT_MAX, wrap INT_MIN: wrapped 0x80000000, clamped 0x7fffffff
;  [20..21] i32 add case 1 (0xfffffffb, 0x00000007): -5+7 in range: wrapped 0x00000002, clamped 0x00000002
;  [22..23] i32 add case 2 (0x80000000, 0xffffffff): INT_MIN+-1 underflows: clamp INT_MIN, wrap INT_MAX: wrapped 0x7fffffff, clamped 0x80000000
;  [24..25] i32 sub (s0-s1) case 0 (0x80000000, 0x00000001): INT_MIN-1 underflows: clamp INT_MIN, wrap INT_MAX: wrapped 0x7fffffff, clamped 0x80000000
;  [26..27] i32 sub (s0-s1) case 1 (0x00000003, 0x0000000a): 3-10=-7 in range: wrapped 0xfffffff9, clamped 0xfffffff9
;  [28..29] i32 sub (s0-s1) case 2 (0x7fffffff, 0xffffffff): INT_MAX--1 overflows: clamp INT_MAX, wrap INT_MIN: wrapped 0x80000000, clamped 0x7fffffff
;  [30..31] u16 add case 0 (0xaaaaffff, 0x55550001): 0xFFFF+1: clamp 0xFFFF, wrap 0: wrapped 0xbeef0000, clamped 0xbeefffff
;  [32..33] u16 add case 1 (0xaaaa0005, 0x55550007): in range: wrapped 0xbeef000c, clamped 0xbeef000c
;  [34..35] u16 add case 2 (0xaaaa8000, 0x55558001): 0x8000+0x8001: clamp 0xFFFF, wrap 1: wrapped 0xbeef0001, clamped 0xbeefffff
;  [36..37] u16 sub (s0-s1) case 0 (0xaaaa0000, 0x55550001): 0-1 underflows: clamp 0, wrap 0xFFFF: wrapped 0xbeefffff, clamped 0xbeef0000
;  [38..39] u16 sub (s0-s1) case 1 (0xaaaa0009, 0x55550004): in range: wrapped 0xbeef0005, clamped 0xbeef0005
;  [40..41] u16 sub (s0-s1) case 2 (0xaaaa0001, 0x55558000): 1-0x8000 underflows: clamp 0, wrap 0x8001: wrapped 0xbeef8001, clamped 0xbeef0000
; group: 1,1,1
; rsrc2: 0x808a
; Run: vk_compute_test cs_int_clamp.spv 1 dump with VKC_OUT_WORDS=64 VKC_DUMP=<file>
v_mov_b32_e32 v1, 0
v_mov_b32_e32 v3, 0xffffffff
v_mov_b32_e32 v4, 0x1
v_add_nc_u32_e64 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:0
v_add_nc_u32_e64 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:4
v_mov_b32_e32 v3, 0x5
v_mov_b32_e32 v4, 0x7
v_add_nc_u32_e64 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:8
v_add_nc_u32_e64 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:12
v_mov_b32_e32 v3, 0x80000000
v_mov_b32_e32 v4, 0x80000001
v_add_nc_u32_e64 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:16
v_add_nc_u32_e64 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:20
v_mov_b32_e32 v3, 0x0
v_mov_b32_e32 v4, 0x1
v_sub_nc_u32_e64 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:24
v_sub_nc_u32_e64 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:28
v_mov_b32_e32 v3, 0x9
v_mov_b32_e32 v4, 0x4
v_sub_nc_u32_e64 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:32
v_sub_nc_u32_e64 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:36
v_mov_b32_e32 v3, 0x7fffffff
v_mov_b32_e32 v4, 0x80000000
v_sub_nc_u32_e64 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:40
v_sub_nc_u32_e64 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:44
v_mov_b32_e32 v3, 0x1
v_mov_b32_e32 v4, 0x0
v_subrev_nc_u32_e64 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:48
v_subrev_nc_u32_e64 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:52
v_mov_b32_e32 v3, 0x4
v_mov_b32_e32 v4, 0x9
v_subrev_nc_u32_e64 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:56
v_subrev_nc_u32_e64 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:60
v_mov_b32_e32 v3, 0x80000000
v_mov_b32_e32 v4, 0x7fffffff
v_subrev_nc_u32_e64 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:64
v_subrev_nc_u32_e64 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:68
v_mov_b32_e32 v3, 0x7fffffff
v_mov_b32_e32 v4, 0x1
v_add_nc_i32_e64 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:72
v_add_nc_i32_e64 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:76
v_mov_b32_e32 v3, 0xfffffffb
v_mov_b32_e32 v4, 0x7
v_add_nc_i32_e64 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:80
v_add_nc_i32_e64 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:84
v_mov_b32_e32 v3, 0x80000000
v_mov_b32_e32 v4, 0xffffffff
v_add_nc_i32_e64 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:88
v_add_nc_i32_e64 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:92
v_mov_b32_e32 v3, 0x80000000
v_mov_b32_e32 v4, 0x1
v_sub_nc_i32_e64 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:96
v_sub_nc_i32_e64 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:100
v_mov_b32_e32 v3, 0x3
v_mov_b32_e32 v4, 0xa
v_sub_nc_i32_e64 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:104
v_sub_nc_i32_e64 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:108
v_mov_b32_e32 v3, 0x7fffffff
v_mov_b32_e32 v4, 0xffffffff
v_sub_nc_i32_e64 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:112
v_sub_nc_i32_e64 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:116
v_mov_b32_e32 v3, 0xaaaaffff
v_mov_b32_e32 v4, 0x55550001
v_mov_b32_e32 v2, 0xbeef0000
v_add_nc_u16 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:120
v_mov_b32_e32 v2, 0xbeef0000
v_add_nc_u16 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:124
v_mov_b32_e32 v3, 0xaaaa0005
v_mov_b32_e32 v4, 0x55550007
v_mov_b32_e32 v2, 0xbeef0000
v_add_nc_u16 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:128
v_mov_b32_e32 v2, 0xbeef0000
v_add_nc_u16 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:132
v_mov_b32_e32 v3, 0xaaaa8000
v_mov_b32_e32 v4, 0x55558001
v_mov_b32_e32 v2, 0xbeef0000
v_add_nc_u16 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:136
v_mov_b32_e32 v2, 0xbeef0000
v_add_nc_u16 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:140
v_mov_b32_e32 v3, 0xaaaa0000
v_mov_b32_e32 v4, 0x55550001
v_mov_b32_e32 v2, 0xbeef0000
v_sub_nc_u16 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:144
v_mov_b32_e32 v2, 0xbeef0000
v_sub_nc_u16 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:148
v_mov_b32_e32 v3, 0xaaaa0009
v_mov_b32_e32 v4, 0x55550004
v_mov_b32_e32 v2, 0xbeef0000
v_sub_nc_u16 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:152
v_mov_b32_e32 v2, 0xbeef0000
v_sub_nc_u16 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:156
v_mov_b32_e32 v3, 0xaaaa0001
v_mov_b32_e32 v4, 0x55558000
v_mov_b32_e32 v2, 0xbeef0000
v_sub_nc_u16 v2, v3, v4
buffer_store_dword v2, v1, s[0:3], 0 offen offset:160
v_mov_b32_e32 v2, 0xbeef0000
v_sub_nc_u16 v2, v3, v4 clamp
buffer_store_dword v2, v1, s[0:3], 0 offen offset:164
s_endpgm
