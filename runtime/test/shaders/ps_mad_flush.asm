; v_mad_f32 and v_mac_f32 (D is the addend): D = S0 * S1 + S2 with the product rounded to f32 before the add (not
; fused). docs/design.md (SHADER-118, "Denormal flush in v_mad_f32 ..."): these opcodes have no f32 denormals, so a
; subnormal operand, the rounded product and the result (after omod) read as zero of their sign.
; v0 = 0x00000001 (the smallest denormal), v1 = 2^126 (0x7e800000), v2 = 2^-70 (0x1c800000),
; v3 = FLT_MIN = 2^-126 (0x00800000), v4 = -2^-125 (0x81000000), v5 = 1.5 * 2^-126 (0x00c00000), v8 = FLT_MIN.
;   x: v_mad_f32 v6, v0, v1, 1.0:      S0 flushed to +0, so 0 * 2^126 + 1.0 = 1.0 -> 0x3f800000
;                                      (unflushed: 2^-149 * 2^126 = 2^-23, 1.0 + 2^-23 = 0x3f800001)
;   y: v_mac_f32 v8, v2, v2:           2^-70 * 2^-70 = 2^-140, a denormal product, flushed to +0;
;                                      D = +0 + 2^-126 = 0x00800000 (unflushed 2^-126 + 2^-140 = 0x00800200)
;   z: v_mad_f32 v7, v4, 1.0, v5:      -2^-125 + 1.5 * 2^-126 = -0.5 * 2^-126 = -2^-127, a denormal result,
;                                      flushed to -0 -> 0x80000000 (unflushed 0x80400000)
;   w: v_mad_f32 v9, v3, 1.0, 0 div:2: 2^-126 + 0 = 2^-126, div:2 gives 2^-127, then the flush: +0 -> 0x00000000
;                                      (a flush before the omod would leave 0x00400000)
; Expected mrt0 bits: 0x3f800000, 0x00800000, 0x80000000, 0x00000000
v_mov_b32 v0, 1
v_mov_b32 v1, 0x7e800000
v_mov_b32 v2, 0x1c800000
v_mov_b32 v3, 0x00800000
v_mov_b32 v4, 0x81000000
v_mov_b32 v5, 0x00c00000
v_mov_b32 v8, 0x00800000
v_mad_f32 v6, v0, v1, 1.0
v_mac_f32 v8, v2, v2
v_mad_f32 v7, v4, 1.0, v5
v_mad_f32 v9, v3, 1.0, 0 div:2
exp mrt0 v6, v8, v7, v9 done vm
s_endpgm
