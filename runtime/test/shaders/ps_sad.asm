; v_sad_u8, v_sad_hi_u8, v_sad_u16, v_sad_u32 (docs/design.md "Sum of absolute differences"). RDNA2 ISA VOP3
; pseudocode, read as unsigned (the op names; LLVM VOP3Instructions.td selects V_SAD_U32 from
; add(sub(umax(a, b), umin(a, b)), c)): D = S2 + sum of |S0.x - S1.x| over the lanes, wrapping at 32 bits.
; Every S2 makes the sum carry out of bit 31, so a saturating add would give 0xffffffff instead. vk_harness renders
; to R32G32B32A32_SFLOAT, so a result below 2^23 (an f32 denormal) is XORed with 0x40000000 before the export.
;   x: v_sad_u8, S0 = 0x80ff0170, S1 = 0x7f00fe80, bytes (b0..b3) 70/80 01/fe ff/00 80/7f:
;      16 + 253 + 255 + 1 = 525 = 0x20d; S2 = 0xffffff00: 0x1_0000010d -> 0x0000010d, ^ 0x40000000 = 0x4000010d.
;      (bytes read as signed: 240 + 3 + 1 + 255 = 499; lanes swapped or summed as one word give other sums)
;   y: v_sad_hi_u8, same S0/S1: 0x20d << 16 = 0x020d0000; S2 = 0xfef0abcd: 0x1_00fdabcd -> 0x00fdabcd.
;   z: v_sad_u16, S0 = 0x0001c000, S1 = 0xffff3fff: lo |0xc000 - 0x3fff| = 0x8001, hi |0x0001 - 0xffff| = 0xfffe,
;      sum 0x17fff (not cut to 16 bits); S2 = 0xfffe8005: 0x1_00000004 -> 0x00000004, ^ 0x40000000 = 0x40000004.
;      (halves read as signed: 0x7fff + 2 = 0x8001, giving 0xffff0006)
;   w: v_sad_u32, S0 = 0x12345678 < S1 = 0xc0000001: |S0 - S1| = 0xadcba989; S2 = 0x80000000:
;      0x1_2dcba989 -> 0x2dcba989. (signed, or S0 - S1 without the abs: 0x52345677 + S2 = 0xd2345677)
; Expected mrt0 bits: 0x4000010d, 0x00fdabcd, 0x40000004, 0x2dcba989
v_mov_b32_e32 v0, 0x80ff0170
v_mov_b32_e32 v1, 0x7f00fe80
v_mov_b32_e32 v3, 0xffffff00
v_sad_u8 v2, v0, v1, v3
v_xor_b32_e32 v2, 0x40000000, v2
v_mov_b32_e32 v5, 0xfef0abcd
v_sad_hi_u8 v4, v0, v1, v5
v_mov_b32_e32 v6, 0x0001c000
v_mov_b32_e32 v7, 0xffff3fff
v_mov_b32_e32 v8, 0xfffe8005
v_sad_u16 v9, v6, v7, v8
v_xor_b32_e32 v9, 0x40000000, v9
v_mov_b32_e32 v10, 0x12345678
v_mov_b32_e32 v11, 0xc0000001
v_mov_b32_e32 v12, 0x80000000
v_sad_u32 v13, v10, v11, v12
exp mrt0 v2, v4, v9, v13 done vm
s_endpgm
