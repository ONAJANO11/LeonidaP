; v_mqsad_u32_u8 (docs/design.md "v_mqsad_u32_u8"), RDNA2 ISA VOP3 pseudocode: S0 is a 64-bit VGPR pair, S2 and D
; are 128-bit (four VGPRs). For k = 0..3, D[k] = S2[k] + masked byte-SAD of the window S0[8k+31:8k] (S0 bytes
; k..k+3) against S1, a byte where S1's byte is 0 adding nothing, wrapping at 32 bits. vk_harness renders to
; R32G32B32A32_SFLOAT, so x (below 2^23, an f32 denormal) is XORed with 0x40000000 before the export.
;   S0 = v[0:1] = 0x337f00ff_8005f010, bytes 0..7: 10 f0 05 80 ff 00 7f 33; S1 = v2 = 0x7fff0020, bytes 20 00 ff 7f.
;   Masked SADs (byte 1 skipped), as in ps_qsad.asm: W0 16 + 250 + 1 = 267 = 0x10b; W1 208 + 127 + 128 = 463 = 0x1cf;
;   W2 27 + 0 + 127 = 154 = 0x9a; W3 96 + 255 + 0 = 351 = 0x15f.
;   S2 = v[4:7] = 0xffffff00, 0x00ffffff, 0xbfffffff, 0x12345678:
;   x: 0xffffff00 + 0x10b = 0x1_0000000b -> 0x0000000b (32-bit wrap), ^ 0x40000000 = 0x4000000b
;   y: 0x00ffffff + 0x1cf = 0x010001ce (a 16-bit accumulator would keep 0x01ce)
;   z: 0xbfffffff + 0x9a = 0xc0000099
;   w: 0x12345678 + 0x15f = 0x123457d7
; Expected mrt0 bits: 0x4000000b, 0x010001ce, 0xc0000099, 0x123457d7
v_mov_b32_e32 v0, 0x8005f010
v_mov_b32_e32 v1, 0x337f00ff
v_mov_b32_e32 v2, 0x7fff0020
v_mov_b32_e32 v4, 0xffffff00
v_mov_b32_e32 v5, 0x00ffffff
v_mov_b32_e32 v6, 0xbfffffff
v_mov_b32_e32 v7, 0x12345678
v_mqsad_u32_u8 v[10:13], v[0:1], v2, v[4:7]
v_xor_b32_e32 v10, 0x40000000, v10
exp mrt0 v10, v11, v12, v13 done vm
s_endpgm
