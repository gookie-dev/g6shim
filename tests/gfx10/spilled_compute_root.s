; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s2, s0
s_and_b32 s3, s1, 0xff
s_load_dwordx4 s[4:7], s[2:3], 0
s_waitcnt lgkmcnt(0)
s_load_dwordx4 s[8:11], s[4:5], 0
s_waitcnt lgkmcnt(0)
v_mov_b32 v0, 0
v_mov_b32 v1, s6
buffer_store_dword v1, v0, s[8:11], 0 offen
s_endpgm
