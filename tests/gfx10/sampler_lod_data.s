; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_mov_b32 s7, 0
s_load_dwordx4 s[8:11], s[6:7], 0x10
s_load_dwordx4 s[12:15], s[4:5], 0x0
s_waitcnt lgkmcnt(0)
s_cmp_lt_u32 s9, 0x1000000
s_cselect_b32 s16, 1, 0
v_mov_b32 v0, s9
v_mov_b32 v1, s16
buffer_store_dword v0, off, s[12:15], 0
buffer_store_dword v1, off, s[12:15], 0 offset:4
s_endpgm
