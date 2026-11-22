; Packed 16-bit integer VOP3P max/min and op_sel on add/sub, run once on a GPU (they had CPU-only checks).
; Lane 0 of a wave32 group stores eight dwords. Each is hand-derived from the RDNA2 ISA: result half 0 takes source i's
; half op_sel[i], result half 1 takes op_sel_hi[i] (defaults [0,..] and [1,..]); each half is computed on its own
; and wraps to 16 bits; signed ops compare the halves as i16, unsigned as u16. Halves are written hi:lo.
;   out[0] = v_pk_max_u16(0x7FFF:0x8000, 0x8000:0xFFFF)         = 0x8000:0xFFFF  (unsigned: 0x8000 > 0x7FFF; as i16 the high half would be 0x7FFF)
;   out[1] = v_pk_max_u16 op_sel:[1,0] op_sel_hi:[0,1](0xC000:0x0001, 0x0002:0x0003) = 0x0002:0xC000  (lo: max(src0.hi, src1.lo) = max(0xC000, 3); hi: max(src0.lo, src1.hi) = max(1, 2))
;   out[2] = v_pk_min_i16(0x8001:0x8000, 0x7FFE:0x7FFF)         = 0x8001:0x8000  (signed: -32768 < 32767, -32767 < 32766)
;   out[3] = v_pk_min_u16(same)                                 = 0x7FFE:0x7FFF  (unsigned: 0x7FFF < 0x8000, 0x7FFE < 0x8001)
;   out[4] = v_pk_min_i16 op_sel:[1,1](same)                    = 0x8001:0x8001  (both result halves read the high halves: min(-32767, 32766))
;   out[5] = v_pk_min_u16 op_sel_hi:[0,0](same)                 = 0x7FFF:0x7FFF  (both result halves read the low halves: min(0x8000, 0x7FFF))
;   out[6] = v_pk_add_i16 op_sel:[1,1] op_sel_hi:[0,0](0x7FFE:0x0003, 0x0003:0xFFFF) = 0x0002:0x8001  (lo: 0x7FFE+3 = 0x8001 wraps; hi: 3 + -1)
;   out[7] = v_pk_sub_u16 op_sel:[1,0] op_sel_hi:[0,1](0x0005:0x0009, 0x0002:0x0003) = 0x0007:0x0002  (lo: src0.hi - src1.lo = 5-3; hi: src0.lo - src1.hi = 9-2)
; group: 32,1,1
; Run: vk_compute_test cs_w32_pk_int16_b.spv 1 dump -> out[0..7] = 2147549183 180224 2147581952 2147385343 2147581953 2147450879 163841 458754
; mattr: +wavefrontsize32,-wavefrontsize64
v_mov_b32_e32 v1, 0x7fff8000
v_mov_b32_e32 v2, 0x8000ffff
v_pk_max_u16 v10, v1, v2
v_mov_b32_e32 v1, 0xc0000001
v_mov_b32_e32 v2, 0x20003
v_pk_max_u16 v11, v1, v2 op_sel:[1,0] op_sel_hi:[0,1]
v_mov_b32_e32 v1, 0x80018000
v_mov_b32_e32 v2, 0x7ffe7fff
v_pk_min_i16 v12, v1, v2
v_pk_min_u16 v13, v1, v2
v_pk_min_i16 v14, v1, v2 op_sel:[1,1]
v_pk_min_u16 v15, v1, v2 op_sel_hi:[0,0]
v_mov_b32_e32 v1, 0x7ffe0003
v_mov_b32_e32 v2, 0x3ffff
v_pk_add_i16 v16, v1, v2 op_sel:[1,1] op_sel_hi:[0,0]
v_mov_b32_e32 v1, 0x50009
v_mov_b32_e32 v2, 0x20003
v_pk_sub_u16 v17, v1, v2 op_sel:[1,0] op_sel_hi:[0,1]
v_cmpx_eq_u32_e32 0, v0
v_mov_b32_e32 v21, 0
buffer_store_dword v10, v21, s[0:3], 0 offen
v_mov_b32_e32 v21, 4
buffer_store_dword v11, v21, s[0:3], 0 offen
v_mov_b32_e32 v21, 8
buffer_store_dword v12, v21, s[0:3], 0 offen
v_mov_b32_e32 v21, 12
buffer_store_dword v13, v21, s[0:3], 0 offen
v_mov_b32_e32 v21, 16
buffer_store_dword v14, v21, s[0:3], 0 offen
v_mov_b32_e32 v21, 20
buffer_store_dword v15, v21, s[0:3], 0 offen
v_mov_b32_e32 v21, 24
buffer_store_dword v16, v21, s[0:3], 0 offen
v_mov_b32_e32 v21, 28
buffer_store_dword v17, v21, s[0:3], 0 offen
s_endpgm
