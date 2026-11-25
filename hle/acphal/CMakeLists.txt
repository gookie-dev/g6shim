# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
g6shim_add_hle_module(hle_acphal
    DLL "AcpHal.dll"
    DEF "${CMAKE_CURRENT_SOURCE_DIR}/acphal.def"
    SOURCES ${sources})
target_link_libraries(hle_acphal PRIVATE mmdevapi ole32)
