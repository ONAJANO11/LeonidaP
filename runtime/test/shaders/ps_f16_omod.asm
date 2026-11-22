; VOP3 output modifiers on f16 results do nothing with f16 denormals kept (denorm_mode_16_64 = 3, the MODE the
; translator assumes): on RDNA2 an omod on an f16 result acts only with IEEE 0 and f16 denormals flushed (gfx1036
; raw kernels, docs/shader_validation.md, SHADER-135/297). The game carries no omod on an f16 result. Each word below
; would change if the modifier were applied; the destination's high half (0xAAAA) is kept.
;   x: v_add_f16 0.75 + 1.0 = 1.75, mul:2 ignored (applied: 3.5, 0x4300) -> 0x3f00
;   y: v_mul_f16 0.75 * 1.0 = 0.75, mul:4 ignored (applied: 3.0, 0x4200) -> 0x3a00
;   z: v_fma_f16 0.75 * 1.0 + 1.0 = 1.75, div:2 ignored (applied: 0.875, 0x3b00) -> 0x3f00
;   w: v_exp_f16 2^0 = 1.0, mul:2 ignored (applied: 2.0, 0x4000) -> 0x3c00
; The same instructions as a raw gfx1036 kernel give these four words with denorm_mode_16_64 = 3 and the applied
; ones (0x4300, 0x4200, 0x3b00, 0x4000) with 0, in wave32 and wave64 alike.
; Expected mrt0 bits: 0xaaaa3f00, 0xaaaa3a00, 0xaaaa3f00, 0xaaaa3c00
v_mov_b32_e32 v0, 0x3a00
v_mov_b32_e32 v1, 0x3c00
v_mov_b32_e32 v2, 0xaaaa0000
v_mov_b32_e32 v3, 0xaaaa0000
v_mov_b32_e32 v4, 0xaaaa0000
v_mov_b32_e32 v5, 0xaaaa0000
v_mov_b32_e32 v6, 0
v_add_f16_e64 v2, v0, v1 mul:2
v_mul_f16_e64 v3, v0, v1 mul:4
v_fma_f16 v4, v0, v1, v1 div:2
v_exp_f16_e64 v5, v6 mul:2
exp mrt0 v2, v3, v4, v5 done vm
s_endpgm
