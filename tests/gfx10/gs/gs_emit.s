; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
s_bfe_i32 s0, s3, 0x80008
s_cmp_eq_u32 s0, 0
s_cbranch_scc1 .Ldone
s_bfm_b64 exec, s0, 0
s_cmp_eq_u32 s0, 64
s_cselect_b64 exec, -1, exec
v_lshlrev_b32 v2, 2, v0
v_and_b32 v2, 0x3fffc, v2
s_mov_b32 s1, 2
v_lshlrev_b32 v3, s1, v0 dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:DWORD src1_sel:WORD_1
v_mov_b32 v4, v1 dst_sel:DWORD dst_unused:UNUSED_PAD src0_sel:WORD_0
ds_read_b32 v5, v2 offset:16
ds_read_b32 v10, v3 offset:16
s_load_dword s4, s[8:9], 0
s_lshl_b32 s4, s4, 2
s_add_i32 s4, s4, 4
v_mad_u32_u24 v11, s4, v2, v2
v_mov_b32 v6, 0xbf000000
v_mov_b32 v7, 0xbf000000
v_mov_b32 v8, 0
v_mov_b32 v9, 0x3f800000
ds_write2_b32 v11, v6, v7 offset0:0 offset1:1
ds_write2_b32 v11, v8, v9 offset0:2 offset1:3
ds_write_b32 v11, v5 offset:16
s_sendmsg 0x22
v_mov_b32 v6, 0x3f000000
ds_write2_b32 v11, v6, v7 offset0:8 offset1:9
ds_write2_b32 v11, v8, v9 offset0:10 offset1:11
ds_write_b32 v11, v5 offset:48
s_sendmsg 0x22
v_mov_b32 v6, 0
v_mov_b32 v7, 0x3f000000
ds_write2_b32 v11, v6, v7 offset0:16 offset1:17
ds_write2_b32 v11, v8, v9 offset0:18 offset1:19
ds_write_b32 v11, v5 offset:80
s_sendmsg 0x22
s_sendmsg 0x12
.Ldone:
s_sendmsg 3
s_endpgm
