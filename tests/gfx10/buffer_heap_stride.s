; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_lshl_b32 s22, s0, 5
s_load_dwordx4 s[12:15], s[4:5], s22
s_mov_b32 s7, 0
s_load_dwordx4 s[16:19], s[6:7], 0x0
v_mov_b32 v3, s1
v_mov_b32 v4, s2
s_waitcnt lgkmcnt(0)
buffer_load_dwordx3 v[0:2], v[3:4], s[12:15], 0 idxen offen offset:4
s_waitcnt vmcnt(0)
buffer_store_dwordx3 v[0:2], off, s[16:19], 0
s_endpgm
