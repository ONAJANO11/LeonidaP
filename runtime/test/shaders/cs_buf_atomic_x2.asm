; buffer_atomic_and_x2, buffer_atomic_or_x2 and buffer_atomic_swap_x2 on one 64-bit element (two dwords, low
; first), which had CPU checks only. RDNA2 ISA MUBUF: M[63:0] = op(M[63:0], DATA[63:0]) with op AND, OR or the
; exchange (DATA); with glc, vdata[0:1] gets the pre-op M, without glc vdata is not written.
; Thread 0 seeds the element at heap byte 0x40 with M = 0x9abcdef0_12345678 (lo 0x12345678, hi 0x9abcdef0) through
; the V# in s[0:3] (base 0, num_records 0xffffffff), then, on that element:
;   1. buffer_atomic_and_x2  v[10:11] = (0xff00ff00, 0x0ff00ff0), off, offset:0x40, no glc:
;        M = (0x12345678 & 0xff00ff00, 0x9abcdef0 & 0x0ff00ff0) = (0x12005600, 0x0ab00ef0); v[10:11] unchanged
;   2. buffer_atomic_or_x2   v[12:13] = (0x00ff0000, 0x0000000f), v5 = 0x40 offen, glc:
;        v[12:13] = old M = (0x12005600, 0x0ab00ef0); M = (0x12ff5600, 0x0ab00eff)
;   3. buffer_atomic_swap_x2 v[14:15] = (0xcafef00d, 0xdeadbeef), off, soffset s20 = 0x40, glc:
;        v[14:15] = old M = (0x12ff5600, 0x0ab00eff); M = (0xcafef00d, 0xdeadbeef)
;   4. buffer_load_dwordx2 v[16:17] at 0x40: (0xcafef00d, 0xdeadbeef)
; and writes v10..v17 to out[0..7].
; Run: vk_compute_test cs_buf_atomic_x2.spv 1 dump -> out[0..7] = 0xff00ff00 0x0ff00ff0 0x12005600 0x0ab00ef0
; 0x12ff5600 0x0ab00eff 0xcafef00d 0xdeadbeef.
v_cmpx_eq_u32_e32 0, v0
v_mov_b32 v1, 0x12345678
v_mov_b32 v2, 0x9abcdef0
buffer_store_dwordx2 v[1:2], off, s[0:3], 0 offset:64
s_waitcnt vmcnt(0)
v_mov_b32 v10, 0xff00ff00
v_mov_b32 v11, 0x0ff00ff0
v_mov_b32 v12, 0x00ff0000
v_mov_b32 v13, 0x0000000f
v_mov_b32 v14, 0xcafef00d
v_mov_b32 v15, 0xdeadbeef
v_mov_b32 v5, 0x40
s_mov_b32 s20, 0x40
buffer_atomic_and_x2 v[10:11], off, s[0:3], 0 offset:64
s_waitcnt vmcnt(0)
buffer_atomic_or_x2 v[12:13], v5, s[0:3], 0 offen glc
s_waitcnt vmcnt(0)
buffer_atomic_swap_x2 v[14:15], off, s[0:3], s20 glc
s_waitcnt vmcnt(0)
buffer_load_dwordx2 v[16:17], off, s[0:3], 0 offset:64
s_waitcnt vmcnt(0)
buffer_store_dwordx4 v[10:13], off, s[0:3], 0
buffer_store_dwordx4 v[14:17], off, s[0:3], 0 offset:16
s_endpgm
