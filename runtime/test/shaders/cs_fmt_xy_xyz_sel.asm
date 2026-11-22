; buffer_load_format_xy/xyz, tbuffer_load_format_xy/xyz and buffer_store_format_xy/xyz on what cs_fmt_xy_xyz
; (TEST-185) leaves out: 32_32 and 32_32_32 UINT/FLOAT, UNORM conversion on load, a non-identity V# DST_SEL, a
; structured V# with an out-of-range index and a 12-byte element at the num_records edge. One thread runs.
; Every expected word is derived by hand from the RDNA2 ISA (MUBUF/MTBUF chapter, buffer resource word 3) and the
; rules in docs/design.md ("Typed buffer loads", "Buffer format stores", "Buffer bounds checking"):
; - V# word3 = DST_SEL_X [2:0] | Y [5:3] | Z [8:6] | W [11:9] | FORMAT [18:12] | RESOURCE_LEVEL (bit 24) |
;   OOB_SELECT [29:28]. DST_SEL 0 = 0, 1 = 1 (1.0f for FLOAT/UNORM, integer 1 for UINT), 4..7 = X..W. Identity
;   is 0xfac. FORMAT (tools/gfx10_formats.json): 23 16_16_UNORM, 56 8_8_8_8_UNORM, 62 32_32_UINT, 72 32_32_32_UINT,
;   74 32_32_32_FLOAT, 75 32_32_32_32_UINT, 77 32_32_32_32_FLOAT, 22 32_FLOAT. Components are listed from bit 0.
; - An xy load writes 2 VGPRs, an xyz load 3; the next ones keep their 0x5a5a00xx marker. Output register c is the
;   channel DST_SEL picks for it. tbuffer loads take the instruction's format (the V# says 32_FLOAT).
; - UNORM n-bit value u reads as the correctly rounded f32 of u / (2^n - 1): 0x8000 / 65535 = 0.5 + 2^-1/65535,
;   which is 0.5 plus 128.002 f32 ulps (2^-24) -> 0x3f000080; 0xffff -> 1.0; 51/255 = 0.2 -> 0x3e4ccccd;
;   128/255 = 0.5 + 1/510, 2^24/510 = 32896.502 ulps -> 0x3f008081.
; - Bounds (OOB_SELECT 1, structured): out of range when index >= num_records; the address is
;   base + index * stride + inst_offset, stride in word1 [29:16]. OOB_SELECT 3 (raw): a typed element is all or
;   nothing and must end inside num_records. Out of range, a load returns 0 and a store writes nothing.
; - A store writes the whole element of its format: x, y (or x, y, z) and 0 for a component it does not give
;   (xy into 32_32_32 writes z = 0). UNORM stores clamp to [0, 1] and round: 0.25 * 65535 = 16383.75 -> 0x4000,
;   2.0 -> 0xffff, so 16_16_UNORM (0.25, 2.0) is 0xffff4000 and the next dword is not touched.
; Inputs: 0x11111111, 0x22222222, 0x33333333, 0x44444444 at 0x200; RGBA32F (1.5, -2.0, 0.25, 8.0) =
; 0x3fc00000, 0xc0000000, 0x3e800000, 0x41000000 at 0x210; 0xffff8000 at 0x220 (16_16: x 0x8000, y 0xffff);
; 0x80ff3300 at 0x224 (8_8_8_8: x 0x00, y 0x33, z 0xff, w 0x80).
;   out[0..3]   xy  32_32_UINT @0x200          0x11111111 0x22222222 marker marker
;   out[4..7]   xyz 32_32_32_UINT @0x200       0x11111111 0x22222222 0x33333333 marker
;   out[8..11]  xyz 32_32_32_FLOAT @0x210      0x3fc00000 0xc0000000 0x3e800000 marker
;   out[12..15] xy  16_16_UNORM @0x220         0x3f000080 0x3f800000 marker marker
;   out[16..19] xyz 8_8_8_8_UNORM sel (Y,0,W,X) @0x224: y, 0, w = 0x3e4ccccd 0x00000000 0x3f008081 marker
;   out[20..23] xy  32_32_32_32_UINT sel (W,1,Z,X) @0x200: w, 1 = 0x44444444 0x00000001 marker marker
;   out[24..27] xyz 32_32_32_32_FLOAT sel (Z,X,1,W) @0x210: z, x, 1.0 = 0x3e800000 0x3fc00000 0x3f800000 marker
;   out[28..31] tbuffer xy  32_32_UINT @0x208     0x33333333 0x44444444 marker marker
;   out[32..35] tbuffer xyz 32_32_32_FLOAT @0x210 0x3fc00000 0xc0000000 0x3e800000 marker
;   out[36..39] tbuffer xyz 32_32_32_UINT @0x200  0x11111111 0x22222222 0x33333333 marker
;   out[40..43] xyz 32_32_32_FLOAT, OOB_SELECT 1, stride 16, NR 2, offset:512, index 1 -> 0x210: the RGBA32F x,y,z
;   out[44..47] the same, index 2 >= 2: 0 0 0 marker
;   out[48..51] xy 32_32_32_FLOAT @0x210, raw NR 0x21b: x,y end at 0x218 but the element at 0x21c: 0 0 marker marker
;   out[52..55] the same with NR 0x21c: 0x3fc00000 0xc0000000 marker marker
; Stores, into regions 0x280..0x2ff that start as 0xa5a50000 | address, read back as out[56..87]:
;   0x280 xy  32_32_UINT (0xdeadbeef, 0x01234567)   -> 0xdeadbeef 0x01234567, rest kept
;   0x290 xyz 32_32_32_UINT (7, 8, 9)               -> 7 8 9, last dword kept
;   0x2a0 xyz 32_32_32_FLOAT (1.5, -2.0, 0.25)      -> 0x3fc00000 0xc0000000 0x3e800000, last dword kept
;   0x2b0 xy  32_32_32_FLOAT (1.5, -2.0)            -> 0x3fc00000 0xc0000000 0x00000000, last dword kept
;   0x2c0 xy  16_16_UNORM (0.25, 2.0)               -> 0xffff4000, the other three kept
;   0x2d0 xyz 32_32_32_FLOAT, OOB_SELECT 1, stride 16, NR 1, offset:720, index 0 -> written as at 0x2a0
;   0x2e0 the same V#, index 1 >= 1                 -> nothing written
;   0x2f0 xyz 32_32_32_FLOAT, raw NR 0x2fb (element ends at 0x2fc) -> nothing written
; Run: VKC_OUT_WORDS=192 VKC_DUMP=<file> vk_compute_test cs_fmt_xy_xyz_sel.spv 1 dump; check.py compares words
; 0..127 of the dump with FMT_XY_XYZ_SEL_WORDS and 40 untouched 0xeeeeeeee words.
v_cmpx_eq_u32_e32 0, v0
; inputs: UINT words at 0x200, RGBA32F (1.5, -2.0, 0.25, 8.0) at 0x210, 16_16 0xffff8000 at 0x220, 8_8_8_8 0x80ff3300 at 0x224
v_mov_b32_e32 v1, 0x11111111
v_mov_b32_e32 v2, 0x22222222
v_mov_b32_e32 v3, 0x33333333
v_mov_b32_e32 v4, 0x44444444
v_mov_b32_e32 v5, 0x200
buffer_store_dwordx4 v[1:4], v5, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x3fc00000
v_mov_b32_e32 v2, 0xc0000000
v_mov_b32_e32 v3, 0x3e800000
v_mov_b32_e32 v4, 0x41000000
v_mov_b32_e32 v5, 0x210
buffer_store_dwordx4 v[1:4], v5, s[0:3], 0 offen
v_mov_b32_e32 v1, 0xffff8000
v_mov_b32_e32 v2, 0x80ff3300
v_mov_b32_e32 v5, 0x220
buffer_store_dwordx2 v[1:2], v5, s[0:3], 0 offen
; the store regions 0x280..0x2ff hold 0xa5a50000 | their address
v_mov_b32_e32 v1, 0xa5a50280
v_mov_b32_e32 v2, 0xa5a50284
v_mov_b32_e32 v3, 0xa5a50288
v_mov_b32_e32 v4, 0xa5a5028c
v_mov_b32_e32 v5, 0x280
buffer_store_dwordx4 v[1:4], v5, s[0:3], 0 offen
v_mov_b32_e32 v1, 0xa5a50290
v_mov_b32_e32 v2, 0xa5a50294
v_mov_b32_e32 v3, 0xa5a50298
v_mov_b32_e32 v4, 0xa5a5029c
v_mov_b32_e32 v5, 0x290
buffer_store_dwordx4 v[1:4], v5, s[0:3], 0 offen
v_mov_b32_e32 v1, 0xa5a502a0
v_mov_b32_e32 v2, 0xa5a502a4
v_mov_b32_e32 v3, 0xa5a502a8
v_mov_b32_e32 v4, 0xa5a502ac
v_mov_b32_e32 v5, 0x2a0
buffer_store_dwordx4 v[1:4], v5, s[0:3], 0 offen
v_mov_b32_e32 v1, 0xa5a502b0
v_mov_b32_e32 v2, 0xa5a502b4
v_mov_b32_e32 v3, 0xa5a502b8
v_mov_b32_e32 v4, 0xa5a502bc
v_mov_b32_e32 v5, 0x2b0
buffer_store_dwordx4 v[1:4], v5, s[0:3], 0 offen
v_mov_b32_e32 v1, 0xa5a502c0
v_mov_b32_e32 v2, 0xa5a502c4
v_mov_b32_e32 v3, 0xa5a502c8
v_mov_b32_e32 v4, 0xa5a502cc
v_mov_b32_e32 v5, 0x2c0
buffer_store_dwordx4 v[1:4], v5, s[0:3], 0 offen
v_mov_b32_e32 v1, 0xa5a502d0
v_mov_b32_e32 v2, 0xa5a502d4
v_mov_b32_e32 v3, 0xa5a502d8
v_mov_b32_e32 v4, 0xa5a502dc
v_mov_b32_e32 v5, 0x2d0
buffer_store_dwordx4 v[1:4], v5, s[0:3], 0 offen
v_mov_b32_e32 v1, 0xa5a502e0
v_mov_b32_e32 v2, 0xa5a502e4
v_mov_b32_e32 v3, 0xa5a502e8
v_mov_b32_e32 v4, 0xa5a502ec
v_mov_b32_e32 v5, 0x2e0
buffer_store_dwordx4 v[1:4], v5, s[0:3], 0 offen
v_mov_b32_e32 v1, 0xa5a502f0
v_mov_b32_e32 v2, 0xa5a502f4
v_mov_b32_e32 v3, 0xa5a502f8
v_mov_b32_e32 v4, 0xa5a502fc
v_mov_b32_e32 v5, 0x2f0
buffer_store_dwordx4 v[1:4], v5, s[0:3], 0 offen
s_waitcnt_vscnt null, 0x0
s_mov_b32 s4, s0
s_mov_b32 s5, s1
; out[0..3]: buffer_load_format_xy, 32_32_UINT
v_mov_b32_e32 v10, 0x5a5a0000
v_mov_b32_e32 v11, 0x5a5a0004
v_mov_b32_e32 v12, 0x5a5a0008
v_mov_b32_e32 v13, 0x5a5a000c
s_mov_b32 s6, 0xffffffff
s_mov_b32 s7, 0x3103efac
v_mov_b32_e32 v5, 0x200
buffer_load_format_xy v[10:11], v5, s[4:7], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0x0
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[4..7]: buffer_load_format_xyz, 32_32_32_UINT
v_mov_b32_e32 v10, 0x5a5a0010
v_mov_b32_e32 v11, 0x5a5a0014
v_mov_b32_e32 v12, 0x5a5a0018
v_mov_b32_e32 v13, 0x5a5a001c
s_mov_b32 s6, 0xffffffff
s_mov_b32 s7, 0x31048fac
v_mov_b32_e32 v5, 0x200
buffer_load_format_xyz v[10:12], v5, s[4:7], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0x10
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[8..11]: buffer_load_format_xyz, 32_32_32_FLOAT
v_mov_b32_e32 v10, 0x5a5a0020
v_mov_b32_e32 v11, 0x5a5a0024
v_mov_b32_e32 v12, 0x5a5a0028
v_mov_b32_e32 v13, 0x5a5a002c
s_mov_b32 s6, 0xffffffff
s_mov_b32 s7, 0x3104afac
v_mov_b32_e32 v5, 0x210
buffer_load_format_xyz v[10:12], v5, s[4:7], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0x20
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[12..15]: buffer_load_format_xy, 16_16_UNORM
v_mov_b32_e32 v10, 0x5a5a0030
v_mov_b32_e32 v11, 0x5a5a0034
v_mov_b32_e32 v12, 0x5a5a0038
v_mov_b32_e32 v13, 0x5a5a003c
s_mov_b32 s6, 0xffffffff
s_mov_b32 s7, 0x31017fac
v_mov_b32_e32 v5, 0x220
buffer_load_format_xy v[10:11], v5, s[4:7], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0x30
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[16..19]: buffer_load_format_xyz, 8_8_8_8_UNORM, DST_SEL (Y, 0, W, X)
v_mov_b32_e32 v10, 0x5a5a0040
v_mov_b32_e32 v11, 0x5a5a0044
v_mov_b32_e32 v12, 0x5a5a0048
v_mov_b32_e32 v13, 0x5a5a004c
s_mov_b32 s6, 0xffffffff
s_mov_b32 s7, 0x310389c5
v_mov_b32_e32 v5, 0x224
buffer_load_format_xyz v[10:12], v5, s[4:7], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0x40
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[20..23]: buffer_load_format_xy, 32_32_32_32_UINT, DST_SEL (W, 1, Z, X)
v_mov_b32_e32 v10, 0x5a5a0050
v_mov_b32_e32 v11, 0x5a5a0054
v_mov_b32_e32 v12, 0x5a5a0058
v_mov_b32_e32 v13, 0x5a5a005c
s_mov_b32 s6, 0xffffffff
s_mov_b32 s7, 0x3104b98f
v_mov_b32_e32 v5, 0x200
buffer_load_format_xy v[10:11], v5, s[4:7], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0x50
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[24..27]: buffer_load_format_xyz, 32_32_32_32_FLOAT, DST_SEL (Z, X, 1, W)
v_mov_b32_e32 v10, 0x5a5a0060
v_mov_b32_e32 v11, 0x5a5a0064
v_mov_b32_e32 v12, 0x5a5a0068
v_mov_b32_e32 v13, 0x5a5a006c
s_mov_b32 s6, 0xffffffff
s_mov_b32 s7, 0x3104de66
v_mov_b32_e32 v5, 0x210
buffer_load_format_xyz v[10:12], v5, s[4:7], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0x60
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[28..31]: tbuffer_load_format_xy, instruction 32_32_UINT over a V# that says 32_FLOAT
v_mov_b32_e32 v10, 0x5a5a0070
v_mov_b32_e32 v11, 0x5a5a0074
v_mov_b32_e32 v12, 0x5a5a0078
v_mov_b32_e32 v13, 0x5a5a007c
s_mov_b32 s6, 0xffffffff
s_mov_b32 s7, 0x31016fac
v_mov_b32_e32 v5, 0x208
tbuffer_load_format_xy v[10:11], v5, s[4:7], 0 format:[BUF_FMT_32_32_UINT] offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0x70
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[32..35]: tbuffer_load_format_xyz, instruction 32_32_32_FLOAT over a V# that says 32_FLOAT
v_mov_b32_e32 v10, 0x5a5a0080
v_mov_b32_e32 v11, 0x5a5a0084
v_mov_b32_e32 v12, 0x5a5a0088
v_mov_b32_e32 v13, 0x5a5a008c
s_mov_b32 s6, 0xffffffff
s_mov_b32 s7, 0x31016fac
v_mov_b32_e32 v5, 0x210
tbuffer_load_format_xyz v[10:12], v5, s[4:7], 0 format:[BUF_FMT_32_32_32_FLOAT] offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0x80
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[36..39]: tbuffer_load_format_xyz, instruction 32_32_32_UINT over a V# that says 32_FLOAT
v_mov_b32_e32 v10, 0x5a5a0090
v_mov_b32_e32 v11, 0x5a5a0094
v_mov_b32_e32 v12, 0x5a5a0098
v_mov_b32_e32 v13, 0x5a5a009c
s_mov_b32 s6, 0xffffffff
s_mov_b32 s7, 0x31016fac
v_mov_b32_e32 v5, 0x200
tbuffer_load_format_xyz v[10:12], v5, s[4:7], 0 format:[BUF_FMT_32_32_32_UINT] offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0x90
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[40..43]: buffer_load_format_xyz, 32_32_32_FLOAT, structured (OOB_SELECT 1), stride 16, NR 2, index 1: 0x200 + 16
s_or_b32 s5, s1, 0x100000
v_mov_b32_e32 v10, 0x5a5a00a0
v_mov_b32_e32 v11, 0x5a5a00a4
v_mov_b32_e32 v12, 0x5a5a00a8
v_mov_b32_e32 v13, 0x5a5a00ac
s_mov_b32 s6, 0x2
s_mov_b32 s7, 0x1104afac
v_mov_b32_e32 v5, 0x1
buffer_load_format_xyz v[10:12], v5, s[4:7], 0 idxen offset:512
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0xa0
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[44..47]: buffer_load_format_xyz, the same V#, index 2 >= NR 2: out of range
v_mov_b32_e32 v10, 0x5a5a00b0
v_mov_b32_e32 v11, 0x5a5a00b4
v_mov_b32_e32 v12, 0x5a5a00b8
v_mov_b32_e32 v13, 0x5a5a00bc
s_mov_b32 s6, 0x2
s_mov_b32 s7, 0x1104afac
v_mov_b32_e32 v5, 0x2
buffer_load_format_xyz v[10:12], v5, s[4:7], 0 idxen offset:512
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0xb0
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[48..51]: buffer_load_format_xy, 32_32_32_FLOAT, NR 0x21b: x,y end at 0x218, the element at 0x21c
s_mov_b32 s5, s1
v_mov_b32_e32 v10, 0x5a5a00c0
v_mov_b32_e32 v11, 0x5a5a00c4
v_mov_b32_e32 v12, 0x5a5a00c8
v_mov_b32_e32 v13, 0x5a5a00cc
s_mov_b32 s6, 0x21b
s_mov_b32 s7, 0x3104afac
v_mov_b32_e32 v5, 0x210
buffer_load_format_xy v[10:11], v5, s[4:7], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0xc0
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[52..55]: buffer_load_format_xy, 32_32_32_FLOAT, NR 0x21c: the whole element in range
v_mov_b32_e32 v10, 0x5a5a00d0
v_mov_b32_e32 v11, 0x5a5a00d4
v_mov_b32_e32 v12, 0x5a5a00d8
v_mov_b32_e32 v13, 0x5a5a00dc
s_mov_b32 s6, 0x21c
s_mov_b32 s7, 0x3104afac
v_mov_b32_e32 v5, 0x210
buffer_load_format_xy v[10:11], v5, s[4:7], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0xd0
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; 0x280: buffer_store_format_xy, 32_32_UINT
v_mov_b32_e32 v20, 0xdeadbeef
v_mov_b32_e32 v21, 0x1234567
s_mov_b32 s6, 0xffffffff
s_mov_b32 s7, 0x3103efac
v_mov_b32_e32 v5, 0x280
buffer_store_format_xy v[20:21], v5, s[4:7], 0 offen
; 0x290: buffer_store_format_xyz, 32_32_32_UINT
v_mov_b32_e32 v20, 0x7
v_mov_b32_e32 v21, 0x8
v_mov_b32_e32 v22, 0x9
s_mov_b32 s6, 0xffffffff
s_mov_b32 s7, 0x31048fac
v_mov_b32_e32 v5, 0x290
buffer_store_format_xyz v[20:22], v5, s[4:7], 0 offen
; 0x2a0: buffer_store_format_xyz, 32_32_32_FLOAT
v_mov_b32_e32 v20, 0x3fc00000
v_mov_b32_e32 v21, 0xc0000000
v_mov_b32_e32 v22, 0x3e800000
s_mov_b32 s6, 0xffffffff
s_mov_b32 s7, 0x3104afac
v_mov_b32_e32 v5, 0x2a0
buffer_store_format_xyz v[20:22], v5, s[4:7], 0 offen
; 0x2b0: buffer_store_format_xy, 32_32_32_FLOAT (z is written as 0)
v_mov_b32_e32 v20, 0x3fc00000
v_mov_b32_e32 v21, 0xc0000000
s_mov_b32 s6, 0xffffffff
s_mov_b32 s7, 0x3104afac
v_mov_b32_e32 v5, 0x2b0
buffer_store_format_xy v[20:21], v5, s[4:7], 0 offen
; 0x2c0: buffer_store_format_xy, 16_16_UNORM, x = 0.25, y = 2.0 clamps
v_mov_b32_e32 v20, 0x3e800000
v_mov_b32_e32 v21, 0x40000000
s_mov_b32 s6, 0xffffffff
s_mov_b32 s7, 0x31017fac
v_mov_b32_e32 v5, 0x2c0
buffer_store_format_xy v[20:21], v5, s[4:7], 0 offen
; 0x2d0: buffer_store_format_xyz, 32_32_32_FLOAT, structured, stride 16, NR 1, index 0: 0x2d0
s_or_b32 s5, s1, 0x100000
v_mov_b32_e32 v20, 0x3fc00000
v_mov_b32_e32 v21, 0xc0000000
v_mov_b32_e32 v22, 0x3e800000
s_mov_b32 s6, 0x1
s_mov_b32 s7, 0x1104afac
v_mov_b32_e32 v5, 0x0
buffer_store_format_xyz v[20:22], v5, s[4:7], 0 idxen offset:720
; 0x2e0: the same V#, index 1 >= NR 1: out of range, nothing written
v_mov_b32_e32 v20, 0x3fc00000
v_mov_b32_e32 v21, 0xc0000000
v_mov_b32_e32 v22, 0x3e800000
s_mov_b32 s6, 0x1
s_mov_b32 s7, 0x1104afac
v_mov_b32_e32 v5, 0x1
buffer_store_format_xyz v[20:22], v5, s[4:7], 0 idxen offset:720
; 0x2f0: buffer_store_format_xyz, 32_32_32_FLOAT, NR 0x2fb: the element ends at 0x2fc, nothing written
s_mov_b32 s5, s1
v_mov_b32_e32 v20, 0x3fc00000
v_mov_b32_e32 v21, 0xc0000000
v_mov_b32_e32 v22, 0x3e800000
s_mov_b32 s6, 0x2fb
s_mov_b32 s7, 0x3104afac
v_mov_b32_e32 v5, 0x2f0
buffer_store_format_xyz v[20:22], v5, s[4:7], 0 offen
s_waitcnt_vscnt null, 0x0
; out[56..59]: the 16 bytes at 0x280
v_mov_b32_e32 v5, 0x280
buffer_load_dwordx4 v[10:13], v5, s[0:3], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0xe0
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[60..63]: the 16 bytes at 0x290
v_mov_b32_e32 v5, 0x290
buffer_load_dwordx4 v[10:13], v5, s[0:3], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0xf0
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[64..67]: the 16 bytes at 0x2a0
v_mov_b32_e32 v5, 0x2a0
buffer_load_dwordx4 v[10:13], v5, s[0:3], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0x100
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[68..71]: the 16 bytes at 0x2b0
v_mov_b32_e32 v5, 0x2b0
buffer_load_dwordx4 v[10:13], v5, s[0:3], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0x110
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[72..75]: the 16 bytes at 0x2c0
v_mov_b32_e32 v5, 0x2c0
buffer_load_dwordx4 v[10:13], v5, s[0:3], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0x120
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[76..79]: the 16 bytes at 0x2d0
v_mov_b32_e32 v5, 0x2d0
buffer_load_dwordx4 v[10:13], v5, s[0:3], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0x130
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[80..83]: the 16 bytes at 0x2e0
v_mov_b32_e32 v5, 0x2e0
buffer_load_dwordx4 v[10:13], v5, s[0:3], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0x140
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
; out[84..87]: the 16 bytes at 0x2f0
v_mov_b32_e32 v5, 0x2f0
buffer_load_dwordx4 v[10:13], v5, s[0:3], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v6, 0x150
buffer_store_dwordx4 v[10:13], v6, s[0:3], 0 offen
s_endpgm
