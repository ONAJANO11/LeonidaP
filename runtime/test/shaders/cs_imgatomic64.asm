; 64-bit image atomics (D3D-251) on an 8x8 and a 4x4x4 r64 uint image: image_atomic_add, sub, smin, smax, umin, umax,
; and, or, xor, swap and cmpswap with a VGPR pair of data. RDNA2 ISA (MIMG): an atomic's DMASK sets its data size,
; 0x1 for 32 bits and 0x3 for 64 (cmpswap, which also carries the comparand: 0x3 and 0xf); the opcodes are the same
; (LLVM SIISelLowering lowerImage; llvm-mc -mcpu=gfx1013 accepts no other mask). The texel is read, OP(texel, data)
; written back and, with glc, the previous texel returned in the data pair, low dword first; without glc the pair is
; left alone. cmpswap writes the new value (first pair) only where the texel equals the comparand (second pair), all
; 64 bits of it. A lane EXEC turns off does nothing: only thread 0 is on for every case but the last 2D one, which
; runs first with all 64 threads, each adding 1 to the same texel.
; Run: vk_store_test cs_imgatomic64.spv words64
; The tool fills the 2D r64 image (T# 0x6000..., FORMAT 62 = 32_32_UINT, texel i = 8y + x) with
; W(i) = (i + 1) << 32 | (0xfffffff0 + i) mod 2^32 and the r64 volume (T# 0x7000..., texel i = 16z + 4y + x) with
; V(i) = (0x100 + i) << 32 | (0xffffff00 + i), and the r32 uint and sint images as cs_imgatomic.asm does. 2D case c
; uses texel c (x = c mod 8, y = c / 8) and 3D case k the texel below; each stores its data pair afterwards, low
; dword into the uint image and high dword into the sint image, at texel c (2D) or 16 + k (3D).
;    c  op       data (64-bit)                       W(c)                 2D word c afterwards        pair afterwards
;    0  add      0x20                                0x00000001fffffff0   carry: 0x0000000200000010   W(0)
;    1  sub      0x00000001fffffff2                  0x00000002fffffff1   borrow: 0x00000000ffffffff  W(1)
;    2  smin     0xfffffffffffffff0 (-16)            0x00000003fffffff2   -16: 0xfffffffffffffff0     W(2)
;    3  smin     0x7fffffff00000000                  0x00000004fffffff3   kept                        W(3)
;    4  smax     -16                                 0x00000005fffffff4   kept                        W(4)
;    5  smax     0x7fffffff00000000                  0x00000006fffffff5   0x7fffffff00000000          W(5)
;    6  umin     0xfffffffffffffff0                  0x00000007fffffff6   kept                        W(6)
;    7  umin     0x00000007ffffffff                  0x00000008fffffff7   0x00000007ffffffff          W(7)
;    8  umax     0x0000000a00000000                  0x00000009fffffff8   0x0000000a00000000          W(8)
;    9  and      0x0000ffff0000ffff                  0x0000000afffffff9   0x0000000a0000fff9          W(9)
;   10  or       0xf00000000000000f                  0x0000000bfffffffa   0xf000000bffffffff          W(10)
;   11  xor      0xffffffffffffffff                  0x0000000cfffffffb   0xfffffff300000004          W(11)
;   12  swap     0x123456789abcdef0                  0x0000000dfffffffc   0x123456789abcdef0          W(12)
;   13  cmpswap  0xdeadbeefcafef00d, W(13)           0x0000000efffffffd   equal: 0xdeadbeefcafef00d   W(13)
;   14  cmpswap  1, 0x00000000fffffffe               0x0000000ffffffffe   high dword differs: kept    W(14)
;   15  add      7, no glc                           0x00000010ffffffff   carry: 0x0000001100000006   7 (the data)
;   16  add      1, 64 threads                       0x0000001100000000   0x0000001100000040          (not stored)
; Cases 7 and 8 differ from a per-dword min / max (0x00000007fffffff7, 0x0000000afffffff8), and 14 from a compare of
; the low dword alone.
;    k  op       data (64-bit)          (x, y, z)  i    V(i)                 volume word i afterwards     pair afterwards
;    0  add      0x0000000100000100     (1, 2, 3)  57   0x00000139ffffff39   0x0000013b00000039           V(57)
;    1  umin     0x00000112ffffffff     (3, 0, 1)  19   0x00000113ffffff13   0x00000112ffffffff           V(19)
;    2  smax     0x8000000000000000     (0, 3, 2)  44   0x0000012cffffff2c   kept                         V(44)
;    3  cmpswap  0xfeedfacedeadbeef, V(6) (2, 1, 0)  6  0x00000106ffffff06   equal: 0xfeedfacedeadbeef    V(6)
;    4  swap     0x0123456789abcdef     (2, 3, 1)  30   0x0000011effffff1e   0x0123456789abcdef           V(30)
;    5  xor      0xffff00000000ffff, no glc (3, 1, 2) 39 0x00000127ffffff27   0xffff0127ffff00d8           the data
; Every other word keeps its fill: uint word i = 100 + i for i = 22..63, sint word i = -2i for i = 22..63, the r32
; volume 200 + i, 2D r64 word i = W(i) for i = 17..63 and r64 volume word i = V(i) elsewhere.
s_mov_b32 s16, 0x6000
s_mov_b32 s17, 0x3e00000
s_mov_b32 s18, 0x66
s_mov_b32 s19, 0x66
s_mov_b32 s20, 0x66
s_mov_b32 s21, 0x66
s_mov_b32 s22, 0x66
s_mov_b32 s23, 0x66
s_mov_b32 s24, 0x2000
s_mov_b32 s25, 0x1400000
s_mov_b32 s26, 0x22
s_mov_b32 s27, 0x22
s_mov_b32 s28, 0x22
s_mov_b32 s29, 0x22
s_mov_b32 s30, 0x22
s_mov_b32 s31, 0x22
s_mov_b32 s32, 0x3000
s_mov_b32 s33, 0x1500000
s_mov_b32 s34, 0x33
s_mov_b32 s35, 0x33
s_mov_b32 s36, 0x33
s_mov_b32 s37, 0x33
s_mov_b32 s38, 0x33
s_mov_b32 s39, 0x33
s_mov_b32 s40, 0x7000
s_mov_b32 s41, 0x3e00000
s_mov_b32 s42, 0x77
s_mov_b32 s43, 0x77
s_mov_b32 s44, 0x77
s_mov_b32 s45, 0x77
s_mov_b32 s46, 0x77
s_mov_b32 s47, 0x77
v_mov_b32_e32 v1, 0
v_mov_b32_e32 v2, 2
v_mov_b32_e32 v4, 1
v_mov_b32_e32 v5, 0
image_atomic_add v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
v_cmpx_eq_u32_e32 0, v0
v_mov_b32_e32 v2, 0
v_mov_b32_e32 v1, 0
v_mov_b32_e32 v4, 0x20
v_mov_b32_e32 v5, 0
image_atomic_add v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[1:2], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 1
v_mov_b32_e32 v4, 0xfffffff2
v_mov_b32_e32 v5, 1
image_atomic_sub v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[1:2], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 2
v_mov_b32_e32 v4, 0xfffffff0
v_mov_b32_e32 v5, -1
image_atomic_smin v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[1:2], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 3
v_mov_b32_e32 v4, 0
v_mov_b32_e32 v5, 0x7fffffff
image_atomic_smin v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[1:2], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 4
v_mov_b32_e32 v4, 0xfffffff0
v_mov_b32_e32 v5, -1
image_atomic_smax v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[1:2], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 5
v_mov_b32_e32 v4, 0
v_mov_b32_e32 v5, 0x7fffffff
image_atomic_smax v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[1:2], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 6
v_mov_b32_e32 v4, 0xfffffff0
v_mov_b32_e32 v5, -1
image_atomic_umin v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[1:2], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 7
v_mov_b32_e32 v4, -1
v_mov_b32_e32 v5, 7
image_atomic_umin v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[1:2], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v2, 1
v_mov_b32_e32 v1, 0
v_mov_b32_e32 v4, 0
v_mov_b32_e32 v5, 10
image_atomic_umax v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[1:2], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 1
v_mov_b32_e32 v4, 0xffff
v_mov_b32_e32 v5, 0xffff
image_atomic_and v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[1:2], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 2
v_mov_b32_e32 v4, 15
v_mov_b32_e32 v5, 0xf0000000
image_atomic_or v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[1:2], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 3
v_mov_b32_e32 v4, -1
v_mov_b32_e32 v5, -1
image_atomic_xor v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[1:2], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 4
v_mov_b32_e32 v4, 0x9abcdef0
v_mov_b32_e32 v5, 0x12345678
image_atomic_swap v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[1:2], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 5
v_mov_b32_e32 v4, 0xcafef00d
v_mov_b32_e32 v5, 0xdeadbeef
v_mov_b32_e32 v6, 0xfffffffd
v_mov_b32_e32 v7, 14
image_atomic_cmpswap v[4:7], v[1:2], s[16:23] dmask:0xf dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[1:2], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 6
v_mov_b32_e32 v4, 1
v_mov_b32_e32 v5, 0
v_mov_b32_e32 v6, 0xfffffffe
v_mov_b32_e32 v7, 0
image_atomic_cmpswap v[4:7], v[1:2], s[16:23] dmask:0xf dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[1:2], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 7
v_mov_b32_e32 v4, 7
v_mov_b32_e32 v5, 0
image_atomic_add v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
image_store v4, v[1:2], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[1:2], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v9, 2
v_mov_b32_e32 v1, 1
v_mov_b32_e32 v2, 2
v_mov_b32_e32 v3, 3
v_mov_b32_e32 v4, 0x100
v_mov_b32_e32 v5, 1
image_atomic_add v[4:5], v[1:3], s[40:47] dmask:0x3 dim:SQ_RSRC_IMG_3D unorm glc
s_waitcnt vmcnt(0)
v_mov_b32_e32 v8, 0
image_store v4, v[8:9], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[8:9], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 3
v_mov_b32_e32 v2, 0
v_mov_b32_e32 v3, 1
v_mov_b32_e32 v4, -1
v_mov_b32_e32 v5, 0x112
image_atomic_umin v[4:5], v[1:3], s[40:47] dmask:0x3 dim:SQ_RSRC_IMG_3D unorm glc
s_waitcnt vmcnt(0)
v_mov_b32_e32 v8, 1
image_store v4, v[8:9], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[8:9], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 0
v_mov_b32_e32 v2, 3
v_mov_b32_e32 v3, 2
v_mov_b32_e32 v4, 0
v_mov_b32_e32 v5, 0x80000000
image_atomic_smax v[4:5], v[1:3], s[40:47] dmask:0x3 dim:SQ_RSRC_IMG_3D unorm glc
s_waitcnt vmcnt(0)
v_mov_b32_e32 v8, 2
image_store v4, v[8:9], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[8:9], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 2
v_mov_b32_e32 v2, 1
v_mov_b32_e32 v3, 0
v_mov_b32_e32 v4, 0xdeadbeef
v_mov_b32_e32 v5, 0xfeedface
v_mov_b32_e32 v6, 0xffffff06
v_mov_b32_e32 v7, 0x106
image_atomic_cmpswap v[4:7], v[1:3], s[40:47] dmask:0xf dim:SQ_RSRC_IMG_3D unorm glc
s_waitcnt vmcnt(0)
v_mov_b32_e32 v8, 3
image_store v4, v[8:9], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[8:9], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 2
v_mov_b32_e32 v2, 3
v_mov_b32_e32 v3, 1
v_mov_b32_e32 v4, 0x89abcdef
v_mov_b32_e32 v5, 0x1234567
image_atomic_swap v[4:5], v[1:3], s[40:47] dmask:0x3 dim:SQ_RSRC_IMG_3D unorm glc
s_waitcnt vmcnt(0)
v_mov_b32_e32 v8, 4
image_store v4, v[8:9], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[8:9], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 3
v_mov_b32_e32 v2, 1
v_mov_b32_e32 v3, 2
v_mov_b32_e32 v4, 0xffff
v_mov_b32_e32 v5, 0xffff0000
image_atomic_xor v[4:5], v[1:3], s[40:47] dmask:0x3 dim:SQ_RSRC_IMG_3D unorm
v_mov_b32_e32 v8, 5
image_store v4, v[8:9], s[24:31] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
image_store v5, v[8:9], s[32:39] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm
s_endpgm
