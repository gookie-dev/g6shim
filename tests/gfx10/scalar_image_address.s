; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_load_dwordx8 s[8:15], s[4:5], 0x0
s_load_dwordx4 s[16:19], s[4:5], 0x20
v_mov_b32 v2, s0
v_mov_b32 v3, s1
s_waitcnt lgkmcnt(0)
image_load v1, v[2:3], s[8:15] dmask:0x1 dim:SQ_RSRC_IMG_2D
s_lshl_b32 s6, s8, 5
s_and_b32 s20, s13, 0x100000
s_cmp_lt_u32 s13, 0x1000000
s_cselect_b32 s21, 1, 0
s_and_b32 s22, s15, 0x100000
s_and_b32 s23, s13, 0x200
v_mov_b32 v0, s6
v_mov_b32 v4, s20
v_mov_b32 v5, s21
v_mov_b32 v6, s22
v_mov_b32 v7, s23
s_waitcnt vmcnt(0)
buffer_store_dword v0, off, s[16:19], 0
buffer_store_dword v1, off, s[16:19], 0 offset:4
buffer_store_dword v4, off, s[16:19], 0 offset:8
buffer_store_dword v5, off, s[16:19], 0 offset:12
buffer_store_dword v6, off, s[16:19], 0 offset:16
buffer_store_dword v7, off, s[16:19], 0 offset:20
s_endpgm
