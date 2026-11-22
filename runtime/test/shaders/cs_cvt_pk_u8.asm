; group: 8,1,1
; mattr: +wavefrontsize32,-wavefrontsize64
; v_cvt_pk_u8_f32 across 8 lanes: NaN (quiet/signalling/negative), +-inf, negative, ties to even (12.5 -> 12,
; 13.5 -> 14), inserting the rounded/clamped byte at byte index 1 while keeping bytes 0, 2 and 3.
; out[0..7] = 0xAABB00DD 0xAABB00DD 0xAABB00DD 0xAABB00DD 0xAABBFFDD 0xAABB0CDD 0xAABB0EDD 0xAABB00DD
v_mov_b32_e32 v1, 0x7fc00000
v_mov_b32_e32 v2, 0xffc00000
v_cmp_eq_u32_e32 vcc_lo, 1, v0
v_cndmask_b32_e32 v1, v1, v2, vcc_lo
v_mov_b32_e32 v2, 0x7f800001
v_cmp_eq_u32_e32 vcc_lo, 2, v0
v_cndmask_b32_e32 v1, v1, v2, vcc_lo
v_mov_b32_e32 v2, 0xff800000
v_cmp_eq_u32_e32 vcc_lo, 3, v0
v_cndmask_b32_e32 v1, v1, v2, vcc_lo
v_mov_b32_e32 v2, 0x7f800000
v_cmp_eq_u32_e32 vcc_lo, 4, v0
v_cndmask_b32_e32 v1, v1, v2, vcc_lo
v_mov_b32_e32 v2, 0x41480000
v_cmp_eq_u32_e32 vcc_lo, 5, v0
v_cndmask_b32_e32 v1, v1, v2, vcc_lo
v_mov_b32_e32 v2, 0x41580000
v_cmp_eq_u32_e32 vcc_lo, 6, v0
v_cndmask_b32_e32 v1, v1, v2, vcc_lo
v_mov_b32_e32 v2, 0xbf800000
v_cmp_eq_u32_e32 vcc_lo, 7, v0
v_cndmask_b32_e32 v1, v1, v2, vcc_lo
v_mov_b32_e32 v2, 1
v_mov_b32_e32 v3, 0xaabbccdd
v_cvt_pk_u8_f32 v3, v1, v2, v3
v_lshlrev_b32_e32 v4, 2, v0
buffer_store_dword v3, v4, s[0:3], 0 offen
s_endpgm
