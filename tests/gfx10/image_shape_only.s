; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_load_dwordx8 s[8:15], s[4:5], 0x0
s_load_dwordx4 s[16:19], s[4:5], 0x20
s_waitcnt lgkmcnt(0)
s_lshr_b32 s6, s9, 30
s_and_b32 s7, s10, 0x3fff
v_mov_b32 v0, s6
v_mov_b32 v1, s7
buffer_store_dword v0, off, s[16:19], 0
buffer_store_dword v1, off, s[16:19], 0 offset:4
s_endpgm
