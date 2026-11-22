; Uncompressed exp mrtz (SPI_SHADER_32_ABGR): depth (x), stencil (y[7:0]) and sample mask (z).
; attr0.xy = (u, v) in [0, 1] over the 64x64 target (vs_ids / vs_tri).
;   x = floor(u * 64), y = floor(v * 64), stripe = x >> 3 in 0..7.
;   depth   = 0.25 + 0.5 * u  (in (0.25, 0.75), passes LESS against clear 1.0)
;   stencil = ((x + 3 * y + 11) & 0xff) | 0x00beef00  (non-zero bits [31:8] must be masked off)
;   mask    = nibble `stripe` of 0xf7584210: [0, 1, 2, 4, 8, 5, 7, 15], plus 0xf0 when stripe == 0 and y >= 32.
; Run 1 (VKH_STENCIL_OUT=1): vk_harness vs_ids.spv ps_mrtz_uncompr.spv out.raw
;   Kept pixels (mask & 1 != 0, stripes 1/5/6/7): R=1, G=1, B=depth, A=(x + 3*y + 11) & 0xff.
;   Discarded pixels (mask & 1 == 0, stripes 0/2/3/4): clear values R=-1, G=-1, B=1, A=0.
; Run 2 (VKH_MSAA=4): vk_harness vs_ids.spv ps_mrtz_uncompr.spv out.raw
;   4x MSAA resolve: R = popcount(mask & 0xf) / 4.0 -> stripes 0..7 give 0, 0.25, 0.25, 0.25, 0.25, 0.5, 0.75, 1.0.
v_interp_p1_f32_e32 v2, v0, attr0.x
v_interp_p2_f32_e32 v2, v1, attr0.x
v_interp_p1_f32_e32 v3, v0, attr0.y
v_interp_p2_f32_e32 v3, v1, attr0.y
v_mul_f32_e32 v4, 0x42800000, v2
v_mul_f32_e32 v5, 0x42800000, v3
v_cvt_u32_f32_e32 v4, v4
v_cvt_u32_f32_e32 v5, v5
; Sample mask from 8-nibble LUT 0xf7584210 indexed by stripe = x >> 3.
v_lshrrev_b32_e32 v6, 3, v4
v_lshlrev_b32_e32 v7, 2, v6
v_mov_b32_e32 v8, 0xf7584210
v_lshrrev_b32_e32 v8, v7, v8
v_and_b32_e32 v8, 0xf, v8
; Stripe 0, y >= 32: set bits 4..7 (0xf0), which must cover 0 of 4 samples.
v_cmp_eq_u32_e32 vcc_lo, 0, v6
v_cmp_ge_u32_e64 s6, v5, 32
s_and_b32 s7, vcc_lo, s6
v_mov_b32_e32 v9, 0xf0
v_cndmask_b32_e64 v9, 0, v9, s7
v_or_b32_e32 v8, v8, v9
; Depth = 0.25 + 0.5 * u.
v_mov_b32_e32 v10, 0x3e800000
v_fma_f32 v10, v2, 0.5, v10
; Stencil = ((x + 3 * y + 11) & 0xff) | 0x00beef00.
v_mul_lo_u32 v11, v5, 3
v_add_nc_u32_e32 v11, v4, v11
v_add_nc_u32_e32 v11, 11, v11
v_and_b32_e32 v11, 0xff, v11
v_or_b32_e32 v11, 0x00beef00, v11
exp mrtz v10, v11, v8, off vm
v_mov_b32_e32 v12, 1.0
exp mrt0 v12, v12, v12, v12 done vm
s_endpgm
