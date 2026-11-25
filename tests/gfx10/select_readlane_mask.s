; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_mov_b32 s5, 0
s_load_dwordx4 s[8:11], s[4:5], 0x0
v_mov_b32 v0, 0
v_writelane_b32 v0, 0xaaaa5555, 3
v_writelane_b32 v0, 0x5555aaaa, 4
v_readlane_b32 s6, v0, 3
v_readlane_b32 s7, v0, 4
v_cndmask_b32_e64 v1, 0, 1, s6
v_mbcnt_lo_u32_b32 v2, -1, 0
v_mbcnt_hi_u32_b32 v2, -1, v2
v_lshlrev_b32 v2, 2, v2
s_waitcnt lgkmcnt(0)
buffer_store_dword v1, v2, s[8:11], 0 offen
s_endpgm
