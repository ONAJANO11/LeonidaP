; v_msad_u8 and v_lerp_u8 (docs/design.md "Sum of absolute differences" and "v_lerp_u8"), RDNA2 ISA VOP3 pseudocode.
; v_msad_u8: D = S2 + sum over the bytes of |S0.b - S1.b|, a byte whose reference S1.b is 0 adding nothing; 32-bit wrap.
; v_lerp_u8: D.b = (S0.b + S1.b + (S2.b & 1)) >> 1 per byte; only bit 0 of each S2 byte rounds, and the 9-bit sum is
; shifted before it is cut to the byte. vk_harness renders to R32G32B32A32_SFLOAT, so x (below 2^23, an f32 denormal)
; is XORed with 0x40000000 before the export.
;   x: v_msad_u8, S0 = 0x40c00033, S1 = 0x00ff0280, bytes (b0..b3) 33/80 00/02 c0/ff 40/00:
;      77 + 2 + 63 + (b3 masked, S1.b3 = 0) = 142 = 0x8e; S2 = 0xffffff80: 0x1_0000000e -> 0x0000000e,
;      ^ 0x40000000 = 0x4000000e. (no mask: 206; masked where S0's byte is 0 instead: 204)
;   y: v_msad_u8, S0 = 0x11223344, S1 = 0x00008000: only b1 counts, |0x33 - 0x80| = 77 = 0x4d;
;      S2 = 0x3f800000: 0x3f80004d.
;   z: v_lerp_u8, S0 = 0xff80017f, S1 = 0xff7f0480, S2 = 0x01fe0301 (byte bit 0: 1, 1, 0, 1):
;      b0 (0x7f + 0x80 + 1) >> 1 = 0x80, b1 (1 + 4 + 1) >> 1 = 3, b2 (0x80 + 0x7f + 0) >> 1 = 0x7f (S2.b2 = 0xfe: only
;      bit 0 counts), b3 (0xff + 0xff + 1) >> 1 = 0xff (an 8-bit sum would give 0x7f) -> 0xff7f0380.
;   w: v_lerp_u8, S0 = s0 = 0x00fe33f1, S1 = 0x01ff44e0, S2 = inline 1 (rounds byte 0 only):
;      b0 (0xf1 + 0xe0 + 1) >> 1 = 0xe9, b1 (0x33 + 0x44) >> 1 = 0x3b, b2 (0xfe + 0xff) >> 1 = 0xfe, b3 (0 + 1) >> 1 = 0
;      -> 0x00fe3be9.
; Expected mrt0 bits: 0x4000000e, 0x3f80004d, 0xff7f0380, 0x00fe3be9
v_mov_b32_e32 v0, 0x40c00033
v_mov_b32_e32 v1, 0x00ff0280
v_mov_b32_e32 v2, 0xffffff80
v_msad_u8 v3, v0, v1, v2
v_xor_b32_e32 v3, 0x40000000, v3
v_mov_b32_e32 v4, 0x11223344
v_mov_b32_e32 v5, 0x00008000
v_mov_b32_e32 v6, 0x3f800000
v_msad_u8 v7, v4, v5, v6
v_mov_b32_e32 v8, 0xff80017f
v_mov_b32_e32 v9, 0xff7f0480
v_mov_b32_e32 v10, 0x01fe0301
v_lerp_u8 v11, v8, v9, v10
s_mov_b32 s0, 0x00fe33f1
v_mov_b32_e32 v12, 0x01ff44e0
v_lerp_u8 v13, s0, v12, 1
exp mrt0 v3, v7, v11, v13 done vm
s_endpgm
