; The wave64 saveexec / wrexec kinds no other test runs: s_xor, s_andn1, s_orn1, s_orn2, s_nand, s_nor and
; s_xnor_saveexec_b64 and s_andn1_wrexec_b64, in one 64-lane wave (v1 = lane 0..63). The b32 forms are in
; cs_w32_saveexec_ext.asm; s_and/s_or/s_andn2_saveexec_b64 and s_andn2_wrexec_b64 are in cs_w64x_saveexec_b64.asm.
; RDNA2 ISA (SOP1), with EXEC the old EXEC:
;   s_xor_saveexec_b64:   D = EXEC; EXEC = S0 ^ EXEC;     SCC = (EXEC != 0)
;   s_andn1_saveexec_b64: D = EXEC; EXEC = ~S0 & EXEC;    SCC = (EXEC != 0)
;   s_orn1_saveexec_b64:  D = EXEC; EXEC = ~S0 | EXEC;    SCC = (EXEC != 0)
;   s_orn2_saveexec_b64:  D = EXEC; EXEC = S0 | ~EXEC;    SCC = (EXEC != 0)
;   s_nand_saveexec_b64:  D = EXEC; EXEC = ~(S0 & EXEC);  SCC = (EXEC != 0)
;   s_nor_saveexec_b64:   D = EXEC; EXEC = ~(S0 | EXEC);  SCC = (EXEC != 0)
;   s_xnor_saveexec_b64:  D = EXEC; EXEC = ~(S0 ^ EXEC);  SCC = (EXEC != 0)
;   s_andn1_wrexec_b64:   EXEC = ~S0 & EXEC; D = EXEC;    SCC = (EXEC != 0)   (D gets the new EXEC)
; S0 is a v_cmp result (into VCC or an SGPR pair) with all 64 lanes on; the old EXEC is set with s_mov_b32 on each
; half. A mask is reported as code(m) = bcnt << 8 | ff1 (population count, lowest lane). The new EXEC is measured
; per lane: v2 = 1 under it, then with all lanes on, the lanes with v2 != 0 (v_cmp into VCC). Each case stores
;   out[k] = code(new EXEC) << 16 | SCC << 15 | code(D)
; Every new EXEC has lanes in both halves except case 8's (high half only, so an SCC from the low word alone is 0),
; and S0 and the old EXEC differ between the halves, so a swap of the halves moves the lowest lane.
; 1) s_xor_saveexec_b64 s[20:21], s[10:11]: S0 = lanes 0..39 (40 > lane); EXEC = lanes 16..63 (lo 0xffff0000,
;    hi 0xffffffff). New EXEC = lanes 0..15 and 40..63: 40 lanes, lowest 0: 40 << 8 = 10240 = 0x2800.
;    D = lanes 16..63: 48 << 8 | 16 = 0x3010.  out[0] = 0x2800 << 16 | 0x8000 | 0x3010 = 0x2800b010 = 671133712
; 2) s_andn1_saveexec_b64 s[22:23], vcc: S0 = lanes 0..11 (12 > lane); EXEC = lanes 8..47 (lo 0xffffff00,
;    hi 0xffff). New EXEC = lanes 12..47: 36 << 8 | 12 = 0x240c. D = lanes 8..47: 40 << 8 | 8 = 0x2808.
;    out[1] = 0x240ca808 = 604809224
; 3) s_orn1_saveexec_b64 s[24:25], s[12:13]: S0 = lanes 24..63 (24 <= lane), ~S0 = lanes 0..23; EXEC = lanes
;    40..47 (lo 0, hi 0xff00). New EXEC = lanes 0..23 and 40..47: 32 << 8 | 0 = 0x2000. D = lanes 40..47:
;    8 << 8 | 40 = 0x828.  out[2] = 0x20008828 = 536905768
; 4) s_orn2_saveexec_b64 s[26:27], vcc: S0 = lanes 48..63 (48 <= lane); EXEC = lanes 0..15 and 32..63
;    (lo 0xffff, hi 0xffffffff), ~EXEC = lanes 16..31. New EXEC = lanes 16..31 and 48..63: 32 << 8 | 16 = 0x2010.
;    D = 48 lanes, lowest 0: 0x3000.  out[3] = 0x2010b000 = 537964544
; 5) s_nand_saveexec_b64 s[28:29], s[14:15]: S0 = lanes 0..35 (36 > lane); EXEC = lanes 4..47 (lo 0xfffffff0,
;    hi 0xffff). S0 & EXEC = lanes 4..35; new EXEC = lanes 0..3 and 36..63: 32 << 8 | 0 = 0x2000.
;    D = lanes 4..47: 44 << 8 | 4 = 0x2c04.  out[4] = 0x2000ac04 = 536914948
; 6) s_nor_saveexec_b64 s[30:31], vcc: S0 = lanes 0..19 (20 > lane); EXEC = lanes 56..63 (lo 0, hi 0xff000000).
;    S0 | EXEC = lanes 0..19 and 56..63; new EXEC = lanes 20..55: 36 << 8 | 20 = 0x2414.
;    D = lanes 56..63: 8 << 8 | 56 = 0x838.  out[5] = 0x24148838 = 605325368
; 7) s_xnor_saveexec_b64 s[34:35], s[16:17]: S0 = lanes 32..63 (32 <= lane); EXEC = lanes 0..15 and 48..63
;    (lo 0xffff, hi 0xffff0000). S0 ^ EXEC = lanes 0..15 and 32..47; new EXEC = lanes 16..31 and 48..63:
;    32 << 8 | 16 = 0x2010. D = 32 lanes, lowest 0: 0x2000.  out[6] = 0x2010a000 = 537960448
; 8) s_andn1_wrexec_b64 s[36:37], vcc: S0 = lanes 0..39 (40 > lane); EXEC = lanes 24..63 (lo 0xff000000,
;    hi 0xffffffff). New EXEC = lanes 40..63: 24 << 8 | 40 = 0x1828. D = the new EXEC: 0x1828 (the old EXEC
;    would give 40 << 8 | 24 = 0x2818).  out[7] = 0x18289828 = 405313576
; Lane 0 stores the eight words.
; Run: vk_compute_test cs_w64x_saveexec_ext.spv 1 dump --require-wave64
;   -> out[0..7] = 671133712 604809224 536905768 537964544 536914948 605325368 537960448 405313576
; mattr: +wavefrontsize64,-wavefrontsize32
v_mbcnt_lo_u32_b32 v1, -1, 0
v_mbcnt_hi_u32_b32 v1, -1, v1
v_mov_b32_e32 v2, 0
; 1) s_xor_saveexec_b64, SGPR-pair source
v_cmp_gt_u32_e64 s[10:11], 40, v1
s_mov_b32 exec_lo, 0xffff0000
s_mov_b32 exec_hi, -1
s_xor_saveexec_b64 s[20:21], s[10:11]
s_cselect_b32 s40, 0x8000, 0
v_mov_b32_e32 v2, 1
s_mov_b64 exec, -1
v_cmp_ne_u32_e32 vcc, 0, v2
s_bcnt1_i32_b64 s41, vcc
s_ff1_i32_b64 s42, vcc
s_lshl_b32 s41, s41, 8
s_or_b32 s41, s41, s42
s_lshl_b32 s41, s41, 16
s_or_b32 s40, s40, s41
s_bcnt1_i32_b64 s43, s[20:21]
s_ff1_i32_b64 s44, s[20:21]
s_lshl_b32 s43, s43, 8
s_or_b32 s43, s43, s44
s_or_b32 s60, s40, s43
v_mov_b32_e32 v2, 0
; 2) s_andn1_saveexec_b64, VCC source
v_cmp_gt_u32_e32 vcc, 12, v1
s_mov_b32 exec_lo, 0xffffff00
s_mov_b32 exec_hi, 0xffff
s_andn1_saveexec_b64 s[22:23], vcc
s_cselect_b32 s40, 0x8000, 0
v_mov_b32_e32 v2, 1
s_mov_b64 exec, -1
v_cmp_ne_u32_e32 vcc, 0, v2
s_bcnt1_i32_b64 s41, vcc
s_ff1_i32_b64 s42, vcc
s_lshl_b32 s41, s41, 8
s_or_b32 s41, s41, s42
s_lshl_b32 s41, s41, 16
s_or_b32 s40, s40, s41
s_bcnt1_i32_b64 s43, s[22:23]
s_ff1_i32_b64 s44, s[22:23]
s_lshl_b32 s43, s43, 8
s_or_b32 s43, s43, s44
s_or_b32 s61, s40, s43
v_mov_b32_e32 v2, 0
; 3) s_orn1_saveexec_b64, SGPR-pair source
v_cmp_le_u32_e64 s[12:13], 24, v1
s_mov_b32 exec_lo, 0
s_mov_b32 exec_hi, 0xff00
s_orn1_saveexec_b64 s[24:25], s[12:13]
s_cselect_b32 s40, 0x8000, 0
v_mov_b32_e32 v2, 1
s_mov_b64 exec, -1
v_cmp_ne_u32_e32 vcc, 0, v2
s_bcnt1_i32_b64 s41, vcc
s_ff1_i32_b64 s42, vcc
s_lshl_b32 s41, s41, 8
s_or_b32 s41, s41, s42
s_lshl_b32 s41, s41, 16
s_or_b32 s40, s40, s41
s_bcnt1_i32_b64 s43, s[24:25]
s_ff1_i32_b64 s44, s[24:25]
s_lshl_b32 s43, s43, 8
s_or_b32 s43, s43, s44
s_or_b32 s62, s40, s43
v_mov_b32_e32 v2, 0
; 4) s_orn2_saveexec_b64, VCC source
v_cmp_le_u32_e32 vcc, 48, v1
s_mov_b32 exec_lo, 0xffff
s_mov_b32 exec_hi, -1
s_orn2_saveexec_b64 s[26:27], vcc
s_cselect_b32 s40, 0x8000, 0
v_mov_b32_e32 v2, 1
s_mov_b64 exec, -1
v_cmp_ne_u32_e32 vcc, 0, v2
s_bcnt1_i32_b64 s41, vcc
s_ff1_i32_b64 s42, vcc
s_lshl_b32 s41, s41, 8
s_or_b32 s41, s41, s42
s_lshl_b32 s41, s41, 16
s_or_b32 s40, s40, s41
s_bcnt1_i32_b64 s43, s[26:27]
s_ff1_i32_b64 s44, s[26:27]
s_lshl_b32 s43, s43, 8
s_or_b32 s43, s43, s44
s_or_b32 s63, s40, s43
v_mov_b32_e32 v2, 0
; 5) s_nand_saveexec_b64, SGPR-pair source
v_cmp_gt_u32_e64 s[14:15], 36, v1
s_mov_b32 exec_lo, 0xfffffff0
s_mov_b32 exec_hi, 0xffff
s_nand_saveexec_b64 s[28:29], s[14:15]
s_cselect_b32 s40, 0x8000, 0
v_mov_b32_e32 v2, 1
s_mov_b64 exec, -1
v_cmp_ne_u32_e32 vcc, 0, v2
s_bcnt1_i32_b64 s41, vcc
s_ff1_i32_b64 s42, vcc
s_lshl_b32 s41, s41, 8
s_or_b32 s41, s41, s42
s_lshl_b32 s41, s41, 16
s_or_b32 s40, s40, s41
s_bcnt1_i32_b64 s43, s[28:29]
s_ff1_i32_b64 s44, s[28:29]
s_lshl_b32 s43, s43, 8
s_or_b32 s43, s43, s44
s_or_b32 s64, s40, s43
v_mov_b32_e32 v2, 0
; 6) s_nor_saveexec_b64, VCC source
v_cmp_gt_u32_e32 vcc, 20, v1
s_mov_b32 exec_lo, 0
s_mov_b32 exec_hi, 0xff000000
s_nor_saveexec_b64 s[30:31], vcc
s_cselect_b32 s40, 0x8000, 0
v_mov_b32_e32 v2, 1
s_mov_b64 exec, -1
v_cmp_ne_u32_e32 vcc, 0, v2
s_bcnt1_i32_b64 s41, vcc
s_ff1_i32_b64 s42, vcc
s_lshl_b32 s41, s41, 8
s_or_b32 s41, s41, s42
s_lshl_b32 s41, s41, 16
s_or_b32 s40, s40, s41
s_bcnt1_i32_b64 s43, s[30:31]
s_ff1_i32_b64 s44, s[30:31]
s_lshl_b32 s43, s43, 8
s_or_b32 s43, s43, s44
s_or_b32 s65, s40, s43
v_mov_b32_e32 v2, 0
; 7) s_xnor_saveexec_b64, SGPR-pair source
v_cmp_le_u32_e64 s[16:17], 32, v1
s_mov_b32 exec_lo, 0xffff
s_mov_b32 exec_hi, 0xffff0000
s_xnor_saveexec_b64 s[34:35], s[16:17]
s_cselect_b32 s40, 0x8000, 0
v_mov_b32_e32 v2, 1
s_mov_b64 exec, -1
v_cmp_ne_u32_e32 vcc, 0, v2
s_bcnt1_i32_b64 s41, vcc
s_ff1_i32_b64 s42, vcc
s_lshl_b32 s41, s41, 8
s_or_b32 s41, s41, s42
s_lshl_b32 s41, s41, 16
s_or_b32 s40, s40, s41
s_bcnt1_i32_b64 s43, s[34:35]
s_ff1_i32_b64 s44, s[34:35]
s_lshl_b32 s43, s43, 8
s_or_b32 s43, s43, s44
s_or_b32 s66, s40, s43
v_mov_b32_e32 v2, 0
; 8) s_andn1_wrexec_b64, VCC source
v_cmp_gt_u32_e32 vcc, 40, v1
s_mov_b32 exec_lo, 0xff000000
s_mov_b32 exec_hi, -1
s_andn1_wrexec_b64 s[36:37], vcc
s_cselect_b32 s40, 0x8000, 0
v_mov_b32_e32 v2, 1
s_mov_b64 exec, -1
v_cmp_ne_u32_e32 vcc, 0, v2
s_bcnt1_i32_b64 s41, vcc
s_ff1_i32_b64 s42, vcc
s_lshl_b32 s41, s41, 8
s_or_b32 s41, s41, s42
s_lshl_b32 s41, s41, 16
s_or_b32 s40, s40, s41
s_bcnt1_i32_b64 s43, s[36:37]
s_ff1_i32_b64 s44, s[36:37]
s_lshl_b32 s43, s43, 8
s_or_b32 s43, s43, s44
s_or_b32 s67, s40, s43
v_cmpx_eq_u32_e32 0, v1
v_mov_b32_e32 v20, s60
v_mov_b32_e32 v21, 0
buffer_store_dword v20, v21, s[0:3], 0 offen
v_mov_b32_e32 v20, s61
v_mov_b32_e32 v21, 4
buffer_store_dword v20, v21, s[0:3], 0 offen
v_mov_b32_e32 v20, s62
v_mov_b32_e32 v21, 8
buffer_store_dword v20, v21, s[0:3], 0 offen
v_mov_b32_e32 v20, s63
v_mov_b32_e32 v21, 12
buffer_store_dword v20, v21, s[0:3], 0 offen
v_mov_b32_e32 v20, s64
v_mov_b32_e32 v21, 16
buffer_store_dword v20, v21, s[0:3], 0 offen
v_mov_b32_e32 v20, s65
v_mov_b32_e32 v21, 20
buffer_store_dword v20, v21, s[0:3], 0 offen
v_mov_b32_e32 v20, s66
v_mov_b32_e32 v21, 24
buffer_store_dword v20, v21, s[0:3], 0 offen
v_mov_b32_e32 v20, s67
v_mov_b32_e32 v21, 28
buffer_store_dword v20, v21, s[0:3], 0 offen
s_endpgm
