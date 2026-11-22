; v_qsad_pk_u16_u8 and v_mqsad_pk_u16_u8 (docs/design.md "Vector integer additions"), RDNA2 ISA VOP3 pseudocode:
; S0 and S2 are 64-bit VGPR pairs, D a pair. For k = 0..3 the window W_k = S0 bytes k..k+3 (S0[8k+31:8k]) is
; byte-SADed against S1 and added to S2.u16[k], wrapping at 16 bits: D.u16[k] = (S2.u16[k] + SAD(W_k, S1)) & 0xffff.
; The m form skips a byte where S1's byte is 0.
;   S0 = v[2:3] = 0x337f00ff_8005f010, bytes 0..7: 10 f0 05 80 ff 00 7f 33; S1 = v4 = 0x7fff0020, bytes 20 00 ff 7f
;   (S1.b1 = 0 is the masked byte); S2 = v[6:7] = 0xffa0ffff_0001ff00, u16 lanes 0..3: ff00 0001 ffff ffa0.
;   W0 = 10 f0 05 80: 16 + 240 + 250 + 1 = 507 = 0x1fb (masked, no 240: 267 = 0x10b)
;   W1 = f0 05 80 ff: 208 + 5 + 127 + 128 = 468 = 0x1d4 (masked 463 = 0x1cf)
;   W2 = 05 80 ff 00: 27 + 128 + 0 + 127 = 282 = 0x11a (masked 154 = 0x9a)
;   W3 = 80 ff 00 7f: 96 + 255 + 255 + 0 = 606 = 0x25e (masked 351 = 0x15f)
;   qsad:  0xff00 + 0x1fb -> 0x00fb; 0x0001 + 0x1d4 = 0x01d5; 0xffff + 0x11a -> 0x0119; 0xffa0 + 0x25e -> 0x01fe
;   mqsad: 0xff00 + 0x10b -> 0x000b; 0x0001 + 0x1cf = 0x01d0; 0xffff + 0x9a -> 0x0099; 0xffa0 + 0x15f -> 0x00ff
;   (a carry kept into the next lane would add 1 to lanes 1 and 3)
;   x, y = qsad D lo, hi: 0x01d500fb, 0x01fe0119; z, w = mqsad D lo, hi: 0x01d0000b, 0x00ff0099
; Expected mrt0 bits: 0x01d500fb, 0x01fe0119, 0x01d0000b, 0x00ff0099
v_mov_b32_e32 v2, 0x8005f010
v_mov_b32_e32 v3, 0x337f00ff
v_mov_b32_e32 v4, 0x7fff0020
v_mov_b32_e32 v6, 0x0001ff00
v_mov_b32_e32 v7, 0xffa0ffff
v_qsad_pk_u16_u8 v[0:1], v[2:3], v4, v[6:7]
v_mqsad_pk_u16_u8 v[8:9], v[2:3], v4, v[6:7]
exp mrt0 v0, v1, v8, v9 done vm
s_endpgm
