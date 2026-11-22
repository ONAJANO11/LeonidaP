; TEST-298: VOP3 mul:2 on the integer and f16 to f32 conversions (ps_f32_mods covers clamp, mul:4 and div:2).
; The output modifier scales the converted float and clamp then limits it to [0, 1] (docs/design.md, "Multiply-add
; and output modifiers"); each word is wrong if the modifier is dropped or applied in the other order.
;   x v_cvt_f32_i32 -5 mul:2        = -10.0       0xc1200000
;   y v_cvt_f32_u32  7 mul:2        =  14.0       0x41600000
;   z v_cvt_f32_ubyte0 0x12347856 mul:2 (byte 0x56 = 86) = 172.0  0x432c0000
;   w v_cvt_f32_f16 1.0 mul:2 (= 2.0) + v_cvt_f32_i32 3 mul:2 clamp (6.0 clamped to 1.0) = 3.0  0x40400000
; Expected mrt0 bits: 0xc1200000, 0x41600000, 0x432c0000, 0x40400000
v_mov_b32_e32 v0, -5
v_cvt_f32_i32_e64 v1, v0 mul:2
v_mov_b32_e32 v2, 7
v_cvt_f32_u32_e64 v3, v2 mul:2
v_mov_b32_e32 v4, 0x12347856
v_cvt_f32_ubyte0_e64 v5, v4 mul:2
v_mov_b32_e32 v6, 0x3c00
v_cvt_f32_f16_e64 v7, v6 mul:2
v_mov_b32_e32 v8, 3
v_cvt_f32_i32_e64 v9, v8 clamp mul:2
v_add_f32_e32 v10, v7, v9
exp mrt0 v1, v3, v5, v10 done vm
s_endpgm
