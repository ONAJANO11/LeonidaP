; MIMG encodes both widths in the same opcode. CMPSWAP uses two low/high pairs.
image_atomic_add v0, v[8:9], s[0:7] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm glc
image_atomic_cmpswap v[0:1], v[8:9], s[0:7] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
image_atomic_add v[0:1], v[8:9], s[0:7] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
image_atomic_sub v[0:1], v[8:9], s[0:7] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
image_atomic_and v[0:1], v[8:9], s[0:7] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
image_atomic_or v[0:1], v[8:9], s[0:7] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
image_atomic_xor v[0:1], v[8:9], s[0:7] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
image_atomic_umin v[0:1], v[8:9], s[0:7] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
image_atomic_umax v[0:1], v[8:10], s[0:7] dmask:0x3 dim:SQ_RSRC_IMG_2D_ARRAY unorm
image_atomic_smin v[0:1], v[8:9], s[0:7] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc
image_atomic_smax v[0:1], v[8:10], s[0:7] dmask:0x3 dim:SQ_RSRC_IMG_2D_ARRAY unorm
image_atomic_swap v[0:1], v[8:9], s[0:7] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm
image_atomic_cmpswap v[0:3], v[8:9], s[0:7] dmask:0xf dim:SQ_RSRC_IMG_2D unorm glc
s_endpgm
