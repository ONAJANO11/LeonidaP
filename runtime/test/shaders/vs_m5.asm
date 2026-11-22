; M5 vertex shader: a quad from the vertex index, placed in an 8x8 grid of cells by the bits above the corner. Run:
; d3d12_m5_world_test (found by the SHA-256 of its FSSM program, as vs_m2 is).
; v0 = VertexIndex: the test draws triangle strips of vertices 8k + 4w + 0..3 and indexed lists 8k + 4w + {0 1 2 2 1 3}
; (base vertex 0), for cell k = 0..63 and variant w = 0, 1. Nothing here reads the instance index, so the result does
; not depend on whether StartInstanceLocation is part of it.
;   corner  u = v0 & 1, v = (v0 >> 1) & 1;   variant w = (v0 >> 2) & 1;   cell k = (v0 >> 3) & 63, cx = k & 7, cy = k >> 3
;   pos0 = (-1 + (cx + u) / 4, 1 - (cy + v) / 4, 0, 1): a cell is a quarter of the clip range per side, so every
;   corner is a multiple of 1/4 and each operation is exact however it is fused
;   param0 = (k, w, 0, 1)
; fssm-at: 0x10
; fssm-data: 0x35534d56 0x20535620
v_and_b32_e32 v1, 1, v0
v_and_b32_e32 v2, 2, v0
v_lshrrev_b32_e32 v7, 2, v0
v_and_b32_e32 v7, 1, v7
v_lshrrev_b32_e32 v12, 3, v0
v_and_b32_e32 v12, 63, v12
v_and_b32_e32 v8, 7, v12
v_lshrrev_b32_e32 v9, 3, v12
v_cvt_f32_u32_e32 v1, v1
v_cvt_f32_u32_e32 v2, v2
v_mul_f32_e32 v2, 0.5, v2
v_cvt_f32_u32_e32 v8, v8
v_cvt_f32_u32_e32 v9, v9
v_cvt_f32_u32_e32 v7, v7
v_cvt_f32_u32_e32 v12, v12
v_fma_f32 v8, v8, 0.25, -1.0
v_fma_f32 v9, v9, -0.25, 1.0
v_fma_f32 v4, v1, 0.25, v8
v_fma_f32 v5, v2, -0.25, v9
v_mov_b32_e32 v6, 0
v_mov_b32_e32 v10, 1.0
exp pos0 v4, v5, v6, v10 done
exp param0 v12, v7, v6, v10
s_endpgm
