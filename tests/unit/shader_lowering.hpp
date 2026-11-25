// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "g6shim/shader/emit/lower.hpp"
#include "g6shim/shader/emit/validator.hpp"

namespace g6shim::test::lowering {

std::vector<std::uint8_t> bytes_of(const std::vector<std::uint32_t>& words);

shader::RootParam root_param(shader::RootType::Kind kind, std::uint8_t user_data, std::uint8_t slot);

shader::UsedResource used_uav(std::uint8_t user_data, std::uint8_t slot, std::uint8_t dwords = 4);

shader::emit::Lowered lower_native(const shader::NativeProgram& native);

shader::emit::Lowered lower_compute(const std::vector<std::uint32_t>& words, std::uint16_t vgprs,
                                    const std::vector<shader::RootParam>& layout,
                                    const std::vector<shader::UsedResource>& used, std::uint32_t rsrc2,
                                    std::array<std::uint32_t, 3> threads);

std::optional<shader::emit::Validator> load_validator();

void check_dxil(const std::vector<std::uint8_t>& dxil, const char* label);

}  // namespace g6shim::test::lowering
