; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx8 s[8:15], s[4:5], 0x0
s_load_dwordx4 s[16:19], s[6:7], 0x0
s_load_dwordx4 s[20:23], s[4:5], 0x20
v_mov_b32 v0, 0x7e003c00
v_mov_b32 v3, 0x38003400
s_waitcnt lgkmcnt(0)
image_sample_b v2, [v0,v3], s[8:15], s[16:19] dmask:0x1 dim:SQ_RSRC_IMG_2D a16
s_waitcnt vmcnt(0)
buffer_store_dword v2, off, s[20:23], 0
s_endpgm
