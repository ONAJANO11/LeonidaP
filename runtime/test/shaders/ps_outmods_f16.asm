; VOP3 output modifiers on v_cvt_f16_f32: clamp applies to the f16 result, the omod (mul:2/mul:4/div:2) doesn't. On
; RDNA2 an omod acts on an f16 result only with IEEE off and f16 denormals flushed; with them kept, as the translator
; assumes, it is ignored (SHADER-297). The destination's high half is kept.
;   1.5 clamp -> 1.0 (0x3c00), high half 0xaaaa kept    0.375 mul:4 -> 0.375 (0x3600): no omod
;   2^-15 mul:2 -> 2^-15 (0x0200, a kept denormal)      65520 div:2 -> inf (0x7c00): 65520 rounds to inf
;   0.375 clamp mul:2 -> 0.375 (0x3600)                  2.0 clamp mul:2 -> 1.0 (0x3c00): the clamp alone
; Words 2 and 3 hold two results each, low half first. An RX 6800 gave 0x3c00 for 1.5 clamp and 0x3600 for 0.375 mul:4
; with f16 denormals kept (@shelbykb2 on #718). With the clamp dropped (main until 2026-11-22) words 0 and 3 are
; 0xaaaa3e00 and 0x40003600; with the omod applied before the rounding, words 1 to 3 are 0xaaaa3e00, 0x78000400 and
; 0x3c003a00.
; Expected mrt0 bits: 0xaaaa3c00, 0xaaaa3600, 0x7c000200, 0x3c003600
v_mov_b32 v0, 1.5
v_mov_b32 v1, 0x3ec00000
v_mov_b32 v2, 0xaaaa5555
v_mov_b32 v3, 0xaaaa5555
v_cvt_f16_f32_e64 v2, v0 clamp
v_cvt_f16_f32_e64 v3, v1 mul:4
v_mov_b32 v8, 0x38000000
v_mov_b32 v9, 0x477ff000
v_mov_b32 v11, 2.0
v_mov_b32 v4, 0
v_mov_b32 v5, 0
v_mov_b32 v6, 0
v_mov_b32 v7, 0
v_cvt_f16_f32_e64 v4, v8 mul:2
v_cvt_f16_f32_e64 v5, v9 div:2
v_cvt_f16_f32_e64 v6, v1 clamp mul:2
v_cvt_f16_f32_e64 v7, v11 clamp mul:2
v_lshl_or_b32 v4, v5, 16, v4
v_lshl_or_b32 v5, v7, 16, v6
exp mrt0 v2, v3, v4, v5 done vm
s_endpgm
