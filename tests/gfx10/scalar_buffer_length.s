; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_load_dwordx4 s[8:11], s[4:5], 0x0
s_load_dwordx4 s[12:15], s[4:5], 0x20
s_waitcnt lgkmcnt(0)
s_buffer_load_dword s6, s[8:11], 0x14
s_waitcnt lgkmcnt(0)
v_mov_b32 v0, s10
v_mov_b32 v1, s6
buffer_store_dword v0, off, s[12:15], 0
buffer_store_dword v1, off, s[12:15], 0 offset:4
s_endpgm
