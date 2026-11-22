; SHADER-187: active f32 VOP3 omod canonicalizes -0 before scaling.
	v_mov_b32_e32 v0, 0x80000000
	v_mul_f32_e64 v1, v0, 1.0 mul:2
	v_mul_f32_e64 v2, v0, 1.0 mul:4
	v_mul_f32_e64 v3, v0, 1.0 div:2
	v_mul_f32_e64 v4, v0, 1.0
	exp mrt0 v1, v2, v3, v4 done vm
	s_endpgm
