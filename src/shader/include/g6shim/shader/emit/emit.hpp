// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "g6shim/shader/dxbc.hpp"
#include "g6shim/shader/emit/sampler.hpp"
#include "g6shim/shader/io.hpp"
#include "g6shim/shader/ir.hpp"
#include "g6shim/shader/native.hpp"
#include "g6shim/shader/resource.hpp"
#include "g6shim/shader/structure.hpp"

namespace g6shim::shader::emit {

struct UserData {
    enum class Source : std::uint32_t { Root, ConstantBufferAddressLow, ShaderBufferAddressLow };

    std::uint32_t index = 0;
    std::uint8_t slot = 0;
    RootType root;
    std::uint32_t dword = 0;
    Source source = Source::Root;

    friend bool operator==(const UserData&, const UserData&) = default;
};

struct HostShader {
    HwStage stage = HwStage::Ps;
    std::vector<std::uint8_t> dxil;
    std::vector<UserData> user_data;
    std::vector<std::array<std::uint32_t, 4>> samplers;
    std::vector<StaticSampler> static_samplers;
    std::vector<std::uint8_t> root_cbvs;
    std::vector<std::uint8_t> root_srvs;

    std::uint32_t user_data_dwords() const;
};

inline constexpr std::uint32_t kLoopLimit = 1u << 20;

std::uint32_t parse_loop_limit(std::optional<std::string_view> value);
std::uint32_t loop_limit();

HostShader translate(std::span<const std::uint8_t> container);

HostShader emit(const Container& container, const NativeProgram& native, const Program& program,
                const Structured& structured, const Io& io, const Resources& resources);

}  // namespace g6shim::shader::emit
