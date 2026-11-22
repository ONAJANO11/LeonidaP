; v_mad_legacy_f32 (VOP3) and v_mac_legacy_f32 (VOP2, D is the addend), which had CPU checks only.
; RDNA ISA: D = S0 * S1 + S2 with the legacy multiply: if S0 == 0.0 or S1 == 0.0 the product is +0.0, whatever the
; other operand is (inf and NaN included); otherwise the product is an ordinary f32 multiply. docs/design.md
; (SHADER-118, "Denormal flush in v_mad_f32 ..."): these opcodes have no f32 denormals, so a subnormal operand, the
; rounded product and the result read as zero of their sign.
; v0 = +0, v1 = -0, v2 = +inf, v3 = qNaN, v7 = 2^-70 (0x1c800000), v9 = 0x80000001 (the smallest negative denormal),
; v10 = FLT_MIN = 2^-126 (0x00800000), v8 = 3.0.
;   x: v_mad_legacy_f32 v4, v0, v2, 1.0:   S0 is 0, so 0 * inf is +0 (an ordinary multiply gives NaN);
;                                          +0 + 1.0 = 1.0 -> 0x3f800000
;   y: v_mad_legacy_f32 v5, v3, v1, -2.0:  S1 is -0, so NaN * -0 is +0; +0 + -2.0 = -2.0 -> 0xc0000000
;   z: v_mad_legacy_f32 v6, v7, v7, v10:   2^-70 * 2^-70 = 2^-140, a denormal product, flushed to +0;
;                                          +0 + 2^-126 = 0x00800000 (unflushed it would be 2^-126 + 2^-140 =
;                                          0x00800200, 2^-140 being 2^9 ulps of 2^-126)
;   w: v_mac_legacy_f32 v8, v9, v2:        S0 = -denormal flushed to -0, so the legacy product is +0 (unflushed
;                                          it is -inf); D = +0 + 3.0 = 3.0 -> 0x40400000
; Expected mrt0 bits: 0x3f800000, 0xc0000000, 0x00800000, 0x40400000
v_mov_b32 v0, 0
v_mov_b32 v1, 0x80000000
v_mov_b32 v2, 0x7f800000
v_mov_b32 v3, 0x7fc00000
v_mov_b32 v7, 0x1c800000
v_mov_b32 v9, 0x80000001
v_mov_b32 v10, 0x00800000
v_mov_b32 v8, 0x40400000
v_mad_legacy_f32 v4, v0, v2, 1.0
v_mad_legacy_f32 v5, v3, v1, -2.0
v_mad_legacy_f32 v6, v7, v7, v10
v_mac_legacy_f32 v8, v9, v2
exp mrt0 v4, v5, v6, v8 done vm
s_endpgm
