; SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
; SPDX-License-Identifier: GPL-3.0-or-later
v_interp_p1ll_f16 v2, v0, attr0.z high
v_interp_p2_f16 v2, v1, attr0.z, v2 high
v_cvt_f32_f16 v2, v2
exp mrt0 v2, v2, v2, v2 done
s_endpgm
