; group: 32,1,1
; rsrc2: 0x4
; D3D-251: root table in s[0:1], two R32G32_UINT UAVs (2D, then 2D array).
; Texel x in the array means layer 1 of the UAV (layer 2 of the resource), y=0 in both textures.
; Each pair of VGPRs is low word first. The D3D12 test uploads these initial values and reads back both textures.
; 2D x   initial                operation                  final                 old value saved at
;  0     0x00000001fffffff0     32 lanes add 0x100000001    0x0000002200000010    -
;  1     0x01234567ffffffff     add 0x100000001             0x0123456900000000    x8
;  2     0x0000000200000001     signed min with -2^32       0xffffffff00000000    x9
;  3     0x8000000000000042     unsigned min with 2^63-1   0x7fffffffffffffff    x10
;  4     0x1234567800000042     compare/exchange, hit       0xabcdef0100000007    x11
;  5     0x9876543200000042     compare/exchange, miss      0x9876543200000042    x12
;  6     0x0000000200000003     add 0x200000001, no glc     0x0000000400000004    x13 (operand, unchanged)
;  7     0x0000000200000003     sub 0x100000004             0x00000000ffffffff    -
; 14     0xfffffff0fedcba98     and 0x0f0f0f0f00ff00ff     0x0f0f0f0000dc0098    -
; 15     0xaa00000011223344     or  0x00bb000044000000     0xaabb000055223344    -
; Array
;  0     0x8000000000000000     signed max with -2^32       0xffffffff00000000    x8
;  1     0x0000000100000042     unsigned max with 2^63     0x8000000000000000    x9
;  2     0x80000000deadbeef     xor 0x800000010000ffff     0x00000001dead4110    -
; All other texels keep their initial values, including layers outside the selected array slice.
s_load_dwordx8 s[16:23], s[0:1], 0
s_load_dwordx8 s[24:31], s[0:1], 0x20
s_waitcnt lgkmcnt(0)
v_mov_b32_e32 v1, 0
v_mov_b32_e32 v2, 0
v_mov_b32_e32 v3, 1
v_mov_b32_e32 v4, 1
v_mov_b32_e32 v5, 1
image_atomic_add v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
v_cmpx_eq_u32_e32 0, v0
v_mov_b32_e32 v1, 1
image_atomic_add v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
v_mov_b32_e32 v1, 8
image_atomic_swap v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm

v_mov_b32_e32 v1, 2
v_mov_b32_e32 v4, 0
v_mov_b32_e32 v5, -1
image_atomic_smin v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
v_mov_b32_e32 v1, 9
image_atomic_swap v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm

v_mov_b32_e32 v1, 3
v_mov_b32_e32 v4, -1
v_mov_b32_e32 v5, 0x7fffffff
image_atomic_umin v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
v_mov_b32_e32 v1, 10
image_atomic_swap v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm

v_mov_b32_e32 v1, 4
v_mov_b32_e32 v4, 7
v_mov_b32_e32 v5, 0xabcdef01
v_mov_b32_e32 v6, 0x42
v_mov_b32_e32 v7, 0x12345678
image_atomic_cmpswap v[4:7], v[1:2], s[16:23] dmask:0xf dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
v_mov_b32_e32 v1, 11
image_atomic_swap v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm

v_mov_b32_e32 v1, 5
v_mov_b32_e32 v4, 7
v_mov_b32_e32 v5, 0xabcdef01
v_mov_b32_e32 v6, 0x42
v_mov_b32_e32 v7, 0x98765433
image_atomic_cmpswap v[4:7], v[1:2], s[16:23] dmask:0xf dim:SQ_RSRC_IMG_2D unorm glc
s_waitcnt vmcnt(0)
v_mov_b32_e32 v1, 12
image_atomic_swap v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm

v_mov_b32_e32 v1, 6
v_mov_b32_e32 v4, 1
v_mov_b32_e32 v5, 2
image_atomic_add v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 13
image_atomic_swap v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm

v_mov_b32_e32 v1, 7
v_mov_b32_e32 v4, 4
v_mov_b32_e32 v5, 1
image_atomic_sub v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 14
v_mov_b32_e32 v4, 0x00ff00ff
v_mov_b32_e32 v5, 0x0f0f0f0f
image_atomic_and v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
v_mov_b32_e32 v1, 15
v_mov_b32_e32 v4, 0x44000000
v_mov_b32_e32 v5, 0x00bb0000
image_atomic_or v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm

v_mov_b32_e32 v1, 0
v_mov_b32_e32 v4, 0
v_mov_b32_e32 v5, -1
image_atomic_smax v[4:5], v[1:3], s[24:31] dmask:0x3 dim:SQ_RSRC_IMG_2D_ARRAY unorm glc
s_waitcnt vmcnt(0)
v_mov_b32_e32 v1, 8
image_atomic_swap v[4:5], v[1:3], s[24:31] dmask:0x3 dim:SQ_RSRC_IMG_2D_ARRAY unorm

v_mov_b32_e32 v1, 1
v_mov_b32_e32 v4, 0
v_mov_b32_e32 v5, 0x80000000
image_atomic_umax v[4:5], v[1:3], s[24:31] dmask:0x3 dim:SQ_RSRC_IMG_2D_ARRAY unorm glc
s_waitcnt vmcnt(0)
v_mov_b32_e32 v1, 9
image_atomic_swap v[4:5], v[1:3], s[24:31] dmask:0x3 dim:SQ_RSRC_IMG_2D_ARRAY unorm
v_mov_b32_e32 v1, 2
v_mov_b32_e32 v4, 0x0000ffff
v_mov_b32_e32 v5, 0x80000001
image_atomic_xor v[4:5], v[1:3], s[24:31] dmask:0x3 dim:SQ_RSRC_IMG_2D_ARRAY unorm
s_endpgm
