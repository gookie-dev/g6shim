// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace g6shim::acphal {

void start_audio_capture();

std::vector<wchar_t> recorder_environment(std::uint32_t target, const std::wstring& capture_path);
HANDLE start_with_environment(std::wstring command, std::vector<wchar_t>& environment);

}  // namespace g6shim::acphal
