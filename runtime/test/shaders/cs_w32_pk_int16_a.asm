; Packed 16-bit integer VOP3P ops that had CPU-only checks, run once on a GPU (add/sub/mad/mul_lo).
; Lane 0 of a wave32 group stores eight dwords. Each is hand-derived from the RDNA2 ISA: result half 0 takes source i's
; half op_sel[i], result half 1 takes op_sel_hi[i] (defaults [0,..] and [1,..]); each half is computed on its own,
; wraps to 16 bits, and with clamp saturates to the type's range (i16 -32768..32767, u16 0..65535). Halves are
; written hi:lo.
;   out[0] = v_pk_add_i16(0x7FFF:0x8000, 0x0001:0x0001)         = 0x8000:0x8001  (32767+1 wraps to -32768; -32768+1)
;   out[1] = v_pk_add_i16 clamp(0x7FFF:0x8000, 0x0001:0xFFFF)   = 0x7FFF:0x8000  (32768 saturates to 32767; -32769 to -32768)
;   out[2] = v_pk_sub_u16(0x0010:0x0000, 0x0002:0x0001)         = 0x000E:0xFFFF  (0-1 wraps to 0xFFFF; 16-2)
;   out[3] = v_pk_sub_u16 clamp(same)                           = 0x000E:0x0000  (0-1 saturates to 0)
;   out[4] = v_pk_mad_i16(0xFFFE:0x0003, 0x0004:0xFFFD, 0x0001:0x0001) = 0xFFF9:0xFFF8  (-2*4+1 = -7; 3*-3+1 = -8)
;   out[5] = v_pk_mad_i16 op_sel:[1,0,1] op_sel_hi:[0,1,0], src2 = 0x0064:0x0032 = 0x003E:0x006A  (lo: -2*-3+100 = 106; hi: 3*4+50 = 62)
;   out[6] = v_pk_mul_lo_u16(0xFFFF:0x8001, 0xFFFF:0x0002)      = 0x0001:0x0002  (0x8001*2 = 0x10002 and 0xFFFF^2 = 0xFFFE0001, low 16 bits)
;   out[7] = v_pk_mul_lo_u16 op_sel:[0,1] op_sel_hi:[1,0](0x0003:0x0005, 0x0007:0x0002) = 0x0006:0x0023  (lo: src0.lo*src1.hi = 5*7; hi: src0.hi*src1.lo = 3*2)
; group: 32,1,1
; Run: vk_compute_test cs_w32_pk_int16_a.spv 1 dump -> out[0..7] = 2147516417 2147450880 983039 917504 4294574072 4063338 65538 393251
; mattr: +wavefrontsize32,-wavefrontsize64
v_mov_b32_e32 v1, 0x7fff8000
v_mov_b32_e32 v2, 0x10001
v_pk_add_i16 v10, v1, v2
v_mov_b32_e32 v2, 0x1ffff
v_pk_add_i16 v11, v1, v2 clamp
v_mov_b32_e32 v1, 0x100000
v_mov_b32_e32 v2, 0x20001
v_pk_sub_u16 v12, v1, v2
v_pk_sub_u16 v13, v1, v2 clamp
v_mov_b32_e32 v1, 0xfffe0003
v_mov_b32_e32 v2, 0x4fffd
v_mov_b32_e32 v3, 0x10001
v_pk_mad_i16 v14, v1, v2, v3
v_mov_b32_e32 v3, 0x640032
v_pk_mad_i16 v15, v1, v2, v3 op_sel:[1,0,1] op_sel_hi:[0,1,0]
v_mov_b32_e32 v1, 0xffff8001
v_mov_b32_e32 v2, 0xffff0002
v_pk_mul_lo_u16 v16, v1, v2
v_mov_b32_e32 v1, 0x30005
v_mov_b32_e32 v2, 0x70002
v_pk_mul_lo_u16 v17, v1, v2 op_sel:[0,1] op_sel_hi:[1,0]
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
