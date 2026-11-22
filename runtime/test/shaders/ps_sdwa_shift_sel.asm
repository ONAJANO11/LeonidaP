; SDWA VOP2 shifts whose shift amount (src0) is an SGPR holding a known constant. SDWA narrows src0 to src0_sel
; before the op ("RDNA 2" ISA guide, SDWA: SRC0_SEL picks BYTE_0..3 / WORD_0..1 / DWORD, zero-extended without
; sext), and v_lshlrev_b32 / v_lshrrev_b32 / v_ashrrev_i32 then shift S1 by S0[4:0]. The narrowing must not be lost
; when the translator folds a constant shift amount.
; s2 = 0x00040001: WORD_1 = 4, BYTE_2 = 4 (the whole dword & 31 would be 1). s3 = 0x00000300: BYTE_1 = 3 (whole: 0).
; v0 = 6.0 (0x40c00000), v1 = -3.0 (0xc0400000).
;   x: v_lshrrev_b32_sdwa v2, s2, v0 src0_sel:WORD_1   0x40c00000 >> 4          = 0x040c0000
;   y: v_lshlrev_b32_sdwa v3, s2, v1 src0_sel:WORD_1   0xc0400000 << 4 (32 bit) = 0x04000000
;   z: v_ashrrev_i32_sdwa v4, s2, v1 src0_sel:BYTE_2   0xc0400000 >> 4 (arith.) = 0xfc040000
;   w: v_lshlrev_b32_sdwa v5, s3, v0 src0_sel:BYTE_1   0x40c00000 << 3          = 0x06000000
; All four are finite floats, so the RGBA32F target keeps them bit-exact.
; Expected mrt0 bits: 0x040c0000, 0x04000000, 0xfc040000, 0x06000000
s_mov_b32 s2, 0x40001
s_mov_b32 s3, 0x300
v_mov_b32 v0, 0x40c00000
v_mov_b32 v1, 0xc0400000
v_lshrrev_b32_sdwa v2, s2, v0 dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:WORD_1 src1_sel:DWORD
v_lshlrev_b32_sdwa v3, s2, v1 dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:WORD_1 src1_sel:DWORD
v_ashrrev_i32_sdwa v4, s2, v1 dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_2 src1_sel:DWORD
v_lshlrev_b32_sdwa v5, s3, v0 dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:BYTE_1 src1_sel:DWORD
exp mrt0 v2, v3, v4, v5 done vm
s_endpgm
