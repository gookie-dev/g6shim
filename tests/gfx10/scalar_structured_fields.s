; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_load_dwordx4 s[8:11], s[4:5], 0x0
s_load_dwordx4 s[12:15], s[4:5], 0x20
s_waitcnt lgkmcnt(0)
s_buffer_load_dword s6, s[8:11], 0x14
s_cmp_eq_u32 s0, 0
s_cbranch_scc1 join
s_mov_b32 s9, 0
s_mov_b32 s11, 0
join:
s_cmp_eq_u32 s9, 0
s_cselect_b32 s18, 1, 0
s_and_b32 s16, s9, 0x3fff0000
s_bfe_u32 s17, s9, 0x80010
s_waitcnt lgkmcnt(0)
v_mov_b32 v0, s16
v_mov_b32 v1, s17
v_mov_b32 v2, s18
v_mov_b32 v3, s6
v_cvt_f32_ubyte3 v4, s11
buffer_store_dword v0, off, s[12:15], 0
buffer_store_dword v1, off, s[12:15], 0 offset:4
buffer_store_dword v2, off, s[12:15], 0 offset:8
buffer_store_dword v3, off, s[12:15], 0 offset:12
buffer_store_dword v4, off, s[12:15], 0 offset:16
s_endpgm
