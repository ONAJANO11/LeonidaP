; Sub-dword global memory ops: global_load_sbyte / _ushort, the d16 loads (global_load_{ubyte,sbyte}_d16[_hi],
; global_load_short_d16[_hi]) and the byte / d16_hi stores (global_store_byte, global_store_byte_d16_hi,
; global_store_short_d16_hi). Expected words derived by hand from the RDNA2 ISA (FLAT/GLOBAL instruction pseudocode):
;   global_load_ubyte        VGPR[vdst][31:0]  = zext(MEM[addr].u8)        global_load_sbyte  = sext(MEM[addr].i8)
;   global_load_ushort       VGPR[vdst][31:0]  = zext(MEM[addr].u16)
;   global_load_ubyte_d16    VGPR[vdst][15:0]  = zext(MEM[addr].u8), [31:16] kept   (_hi: the same into [31:16], [15:0] kept)
;   global_load_sbyte_d16    VGPR[vdst][15:0]  = sext(MEM[addr].i8) to 16 bits, [31:16] kept
;   global_load_short_d16    VGPR[vdst][15:0]  = MEM[addr].u16, [31:16] kept        (_hi: into [31:16], [15:0] kept)
;   global_store_byte        MEM[addr].u8  = VGPR[vdata][7:0]       _byte_d16_hi: = VGPR[vdata][23:16]
;   global_store_short_d16_hi MEM[addr].u16 = VGPR[vdata][31:16]
; The neighbouring bytes of a store stay. address = (off: v[a:a+1] | saddr + zero-extended vaddr) + signed 12-bit offset.
; global_store_short and global_load_sbyte_d16_hi are in cs_global_mem_ext (#1760) and not repeated here.
; Global addresses are heap offsets (vk_compute_test maps VA 0 to the heap, s[0:3] is a V# with base 0). One thread runs.
; Seeds (MUBUF stores): loads read the bytes at 0x200..0x207 = A3 F2 81 80 | E5 C4 01 7F (dwords 0x8081f2a3, 0x7f01c4e5).
; Load destinations start at 0xdeadbeef (whole-register loads) or 0xa1b2c3d4 (d16 loads: low half c3d4, high half a1b2).
;   out[ 0] sbyte  0x200 (a3)          -> 0xffffffa3     out[ 1] sbyte 0x203 (80) -> 0xffffff80
;   out[ 2] sbyte  0x206 (01)          -> 0x00000001     out[ 3] sbyte 0x207 (7f) -> 0x0000007f
;   out[ 4] ushort 0x200 (f2a3)        -> 0x0000f2a3     out[ 5] ushort 0x202 (8081, zero-extended) -> 0x00008081
;   out[ 6] ushort 0x201 (bytes f2 81, unaligned within the dword) -> 0x000081f2
;   out[ 7] ushort 0x205 (bytes c4 01) -> 0x000001c4
;   out[ 8] ubyte_d16    0x200 (a3)    -> low half 00a3, high kept a1b2           -> 0xa1b200a3
;   out[ 9] ubyte_d16_hi 0x202 (81)    -> high half 0081, low kept c3d4           -> 0x0081c3d4
;   out[10] sbyte_d16    0x203 (80)    -> low half ff80 (16-bit sign extension)   -> 0xa1b2ff80
;   out[11] sbyte_d16    0x207 (7f)    -> low half 007f                           -> 0xa1b2007f
;   out[12] ubyte_d16_hi 0x203 (80)    -> high half 0080 (zero-extended)          -> 0x0080c3d4
;   out[13] short_d16    0x200 (f2a3)  -> 0xa1b2f2a3     out[14] short_d16 0x201 (bytes f2 81 = 81f2) -> 0xa1b281f2
;   out[15] short_d16_hi 0x204 (c4e5)  -> 0xc4e5c3d4     out[16] short_d16_hi 0x206 (7f01) -> 0x7f01c3d4
;   out[17] ubyte_d16 with saddr s[20:21] = 0x200, vaddr 4, offset:1 = 0x205 (c4) -> 0xa1b200c4
;   out[18] ubyte_d16_hi with v[4:5] = 0x208, offset:-8 = 0x200 (a3)              -> 0x00a3c3d4
;   out[19] sbyte  saddr 0x200 + 4, offset:-3 = 0x201 (f2)  -> 0xfffffff2
;   out[20] ushort saddr 0x200 + 4, offset:-2 = 0x202 (8081) -> 0x00008081
; Stores: the data VGPR is 0xa1b2c3d4 (bytes d4 c3 b2 a1). Each target dword is a seed, then read back through MUBUF:
;   out[21] store_byte 0x240, dword 0x44332211 byte 0   -> 0x443322d4   out[22] store_byte 0x245, 0x88776655 byte 1 -> 0x8877d455
;   out[23] store_byte 0x24a, dword 0xccbbaa99 byte 2   -> 0xccd4aa99   out[24] store_byte 0x24f, 0x00ffeedd byte 3 -> 0xd4ffeedd
;   out[25] store_byte_d16_hi (byte b2) at 0x250 byte 0 -> 0x443322b2   out[26] 0x255 byte 1 of 0x88776655 -> 0x8877b255
;   out[27] store_byte_d16_hi 0x25a byte 2 of 0xccbbaa99 -> 0xccb2aa99  out[28] 0x25f byte 3 of 0x00ffeedd -> 0xb2ffeedd
;   out[29] store_short_d16_hi (halfword a1b2) at 0x260, low half of 0x44332211  -> 0x4433a1b2
;   out[30] store_short_d16_hi 0x266, high half of 0x88776655                    -> 0xa1b26655
;   out[31] store_short_d16_hi 0x269, bytes 1..2 of 0xccbbaa99 (unaligned in the dword) -> 0xcca1b299
;   out[32] store_byte saddr s[22:23] = 0x270 + vaddr 2, offset:1 = 0x273, byte 3 of 0x44332211 -> 0xd4332211
;   out[33] store_short_d16_hi v[8:9] = 0x27c, offset:-2 = 0x27a, high half of 0x13579bdf        -> 0xa1b29bdf
; Run: VKC_OUT_WORDS=192 VKC_DUMP=<file> vk_compute_test cs_global_subdword.spv 1 dump; check.py compares words 0..33
; and 94 untouched 0xeeeeeeee words after them (up to the seed region at 0x200).
v_cmpx_eq_u32_e32 0, v0
v_mov_b32_e32 v1, 0x8081f2a3
v_mov_b32_e32 v31, 0x200
buffer_store_dword v1, v31, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x7f01c4e5
v_mov_b32_e32 v31, 0x204
buffer_store_dword v1, v31, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x44332211
v_mov_b32_e32 v31, 0x240
buffer_store_dword v1, v31, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x88776655
v_mov_b32_e32 v31, 0x244
buffer_store_dword v1, v31, s[0:3], 0 offen
v_mov_b32_e32 v1, 0xccbbaa99
v_mov_b32_e32 v31, 0x248
buffer_store_dword v1, v31, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x00ffeedd
v_mov_b32_e32 v31, 0x24c
buffer_store_dword v1, v31, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x44332211
v_mov_b32_e32 v31, 0x250
buffer_store_dword v1, v31, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x88776655
v_mov_b32_e32 v31, 0x254
buffer_store_dword v1, v31, s[0:3], 0 offen
v_mov_b32_e32 v1, 0xccbbaa99
v_mov_b32_e32 v31, 0x258
buffer_store_dword v1, v31, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x00ffeedd
v_mov_b32_e32 v31, 0x25c
buffer_store_dword v1, v31, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x44332211
v_mov_b32_e32 v31, 0x260
buffer_store_dword v1, v31, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x88776655
v_mov_b32_e32 v31, 0x264
buffer_store_dword v1, v31, s[0:3], 0 offen
v_mov_b32_e32 v1, 0xccbbaa99
v_mov_b32_e32 v31, 0x268
buffer_store_dword v1, v31, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x44332211
v_mov_b32_e32 v31, 0x270
buffer_store_dword v1, v31, s[0:3], 0 offen
v_mov_b32_e32 v1, 0x13579bdf
v_mov_b32_e32 v31, 0x278
buffer_store_dword v1, v31, s[0:3], 0 offen
s_waitcnt_vscnt null, 0x0
s_mov_b32 s20, 0x200
s_mov_b32 s21, 0
s_mov_b32 s22, 0x270
s_mov_b32 s23, 0
v_mov_b32_e32 v2, 0x200
v_mov_b32_e32 v3, 0x0
v_mov_b32_e32 v4, 0x208
v_mov_b32_e32 v5, 0x0
v_mov_b32_e32 v7, 0x4
v_mov_b32_e32 v6, 0xa1b2c3d4
v_mov_b32_e32 v10, 0xdeadbeef
v_mov_b32_e32 v11, 0xdeadbeef
v_mov_b32_e32 v12, 0xdeadbeef
v_mov_b32_e32 v13, 0xdeadbeef
v_mov_b32_e32 v14, 0xdeadbeef
v_mov_b32_e32 v15, 0xdeadbeef
v_mov_b32_e32 v16, 0xdeadbeef
v_mov_b32_e32 v17, 0xdeadbeef
v_mov_b32_e32 v18, 0xa1b2c3d4
v_mov_b32_e32 v19, 0xa1b2c3d4
v_mov_b32_e32 v20, 0xa1b2c3d4
v_mov_b32_e32 v21, 0xa1b2c3d4
v_mov_b32_e32 v22, 0xa1b2c3d4
v_mov_b32_e32 v23, 0xa1b2c3d4
v_mov_b32_e32 v24, 0xa1b2c3d4
v_mov_b32_e32 v25, 0xa1b2c3d4
v_mov_b32_e32 v26, 0xa1b2c3d4
v_mov_b32_e32 v27, 0xa1b2c3d4
v_mov_b32_e32 v28, 0xa1b2c3d4
v_mov_b32_e32 v29, 0xdeadbeef
v_mov_b32_e32 v30, 0xdeadbeef
global_load_sbyte v10, v[2:3], off
global_load_sbyte v11, v[2:3], off offset:3
global_load_sbyte v12, v[2:3], off offset:6
global_load_sbyte v13, v[2:3], off offset:7
global_load_ushort v14, v[2:3], off
global_load_ushort v15, v[2:3], off offset:2
global_load_ushort v16, v[2:3], off offset:1
global_load_ushort v17, v[2:3], off offset:5
global_load_ubyte_d16 v18, v[2:3], off
global_load_ubyte_d16_hi v19, v[2:3], off offset:2
global_load_sbyte_d16 v20, v[2:3], off offset:3
global_load_sbyte_d16 v21, v[2:3], off offset:7
global_load_ubyte_d16_hi v22, v[2:3], off offset:3
global_load_short_d16 v23, v[2:3], off
global_load_short_d16 v24, v[2:3], off offset:1
global_load_short_d16_hi v25, v[2:3], off offset:4
global_load_short_d16_hi v26, v[2:3], off offset:6
global_load_ubyte_d16 v27, v7, s[20:21] offset:1
global_load_ubyte_d16_hi v28, v[4:5], off offset:-8
global_load_sbyte v29, v7, s[20:21] offset:-3
global_load_ushort v30, v7, s[20:21] offset:-2
v_mov_b32_e32 v8, 0x240
v_mov_b32_e32 v9, 0
global_store_byte v[8:9], v6, off
v_mov_b32_e32 v8, 0x245
v_mov_b32_e32 v9, 0
global_store_byte v[8:9], v6, off
v_mov_b32_e32 v8, 0x24a
v_mov_b32_e32 v9, 0
global_store_byte v[8:9], v6, off
v_mov_b32_e32 v8, 0x24f
v_mov_b32_e32 v9, 0
global_store_byte v[8:9], v6, off
v_mov_b32_e32 v8, 0x250
v_mov_b32_e32 v9, 0
global_store_byte_d16_hi v[8:9], v6, off
v_mov_b32_e32 v8, 0x255
v_mov_b32_e32 v9, 0
global_store_byte_d16_hi v[8:9], v6, off
v_mov_b32_e32 v8, 0x25a
v_mov_b32_e32 v9, 0
global_store_byte_d16_hi v[8:9], v6, off
v_mov_b32_e32 v8, 0x25f
v_mov_b32_e32 v9, 0
global_store_byte_d16_hi v[8:9], v6, off
v_mov_b32_e32 v8, 0x260
v_mov_b32_e32 v9, 0
global_store_short_d16_hi v[8:9], v6, off
v_mov_b32_e32 v8, 0x266
v_mov_b32_e32 v9, 0
global_store_short_d16_hi v[8:9], v6, off
v_mov_b32_e32 v8, 0x269
v_mov_b32_e32 v9, 0
global_store_short_d16_hi v[8:9], v6, off
v_mov_b32_e32 v8, 2
global_store_byte v8, v6, s[22:23] offset:1
v_mov_b32_e32 v8, 0x27c
v_mov_b32_e32 v9, 0
global_store_short_d16_hi v[8:9], v6, off offset:-2
s_waitcnt vmcnt(0)
s_waitcnt_vscnt null, 0x0
v_mov_b32_e32 v31, 0x240
buffer_load_dword v40, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x244
buffer_load_dword v41, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x248
buffer_load_dword v42, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x24c
buffer_load_dword v43, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x250
buffer_load_dword v44, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x254
buffer_load_dword v45, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x258
buffer_load_dword v46, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x25c
buffer_load_dword v47, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x260
buffer_load_dword v48, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x264
buffer_load_dword v49, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x268
buffer_load_dword v50, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x270
buffer_load_dword v51, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x278
buffer_load_dword v52, v31, s[0:3], 0 offen
s_waitcnt vmcnt(0)
v_mov_b32_e32 v31, 0x0
buffer_store_dword v10, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x4
buffer_store_dword v11, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x8
buffer_store_dword v12, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0xc
buffer_store_dword v13, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x10
buffer_store_dword v14, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x14
buffer_store_dword v15, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x18
buffer_store_dword v16, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x1c
buffer_store_dword v17, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x20
buffer_store_dword v18, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x24
buffer_store_dword v19, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x28
buffer_store_dword v20, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x2c
buffer_store_dword v21, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x30
buffer_store_dword v22, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x34
buffer_store_dword v23, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x38
buffer_store_dword v24, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x3c
buffer_store_dword v25, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x40
buffer_store_dword v26, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x44
buffer_store_dword v27, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x48
buffer_store_dword v28, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x4c
buffer_store_dword v29, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x50
buffer_store_dword v30, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x54
buffer_store_dword v40, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x58
buffer_store_dword v41, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x5c
buffer_store_dword v42, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x60
buffer_store_dword v43, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x64
buffer_store_dword v44, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x68
buffer_store_dword v45, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x6c
buffer_store_dword v46, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x70
buffer_store_dword v47, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x74
buffer_store_dword v48, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x78
buffer_store_dword v49, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x7c
buffer_store_dword v50, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x80
buffer_store_dword v51, v31, s[0:3], 0 offen
v_mov_b32_e32 v31, 0x84
buffer_store_dword v52, v31, s[0:3], 0 offen
s_endpgm
