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
    enum class Source : std::uint32_t {
        Root, ConstantBufferAddressLow, ShaderBufferAddressLow, ImageBaseAddressLow,
        ShaderBufferAddressHigh, ShaderBufferComponentMapping, StructuredBufferHighWord,
        LinearRawBufferHighWord, LinearRawBufferFormatWord, StructuredBufferFormatWord,
        ImageSamplingFields, TypedBufferFormatWord, TypedBufferHighWord,
        ConstantBufferHighWord, ConstantBufferRecordCount, ConstantBufferFormatWord,
        ShaderBufferRecordCount, ImageWidthLowBits, ImageDimensions, SamplerLodWord, ImageBaseMip
    };

    std::uint32_t index = 0;
    std::uint8_t slot = 0;
    RootType root;
    std::uint32_t dword = 0;
    Source source = Source::Root;

    static constexpr std::uint32_t kBufferAddressHighMask = 0xffff;
    static constexpr std::uint32_t kBufferComponentMappingMask = 0xfff;
    static constexpr std::uint32_t kBufferStrideShift = 16;
    static constexpr std::uint32_t kBufferStrideMask = 0x3fff;
    static constexpr std::uint32_t kImageWidthLowShift = 30;
    static constexpr std::uint32_t kImageWidthHighBits = 14;
    static constexpr std::uint32_t kImageDimensionsMask = 0x3fff'ffff;
    static constexpr std::uint32_t kImageBaseMipShift = 12;
    static constexpr std::uint32_t kImageBaseMipMask = 0xfu << kImageBaseMipShift;
    static constexpr std::uint32_t kImageSamplingPerformanceShift = 20;
    static constexpr std::uint32_t kImageSamplingPerformanceMask = 0x7u << kImageSamplingPerformanceShift;
    static constexpr std::uint32_t kImageLodWarningMask = 0xfffu << 8;
    static constexpr std::uint32_t kStructuredBufferHighMask =
        (kBufferStrideMask << kBufferStrideShift) | kBufferAddressHighMask;

    constexpr std::uint32_t known_bits() const {
        if (source == Source::ShaderBufferAddressHigh) return kBufferAddressHighMask;
        if (source == Source::ShaderBufferComponentMapping) return kBufferComponentMappingMask;
        if (source == Source::ImageWidthLowBits) return UINT32_MAX << kImageWidthLowShift;
        if (source == Source::ImageDimensions) return kImageDimensionsMask;
        if (source == Source::ImageBaseMip) return kImageBaseMipMask;
        if (source == Source::ImageSamplingFields) return kImageSamplingPerformanceMask | kImageLodWarningMask;
        return UINT32_MAX;
    }

    constexpr bool accepts_mask() const {
        return known_bits() != UINT32_MAX || source == Source::LinearRawBufferHighWord ||
               source == Source::StructuredBufferHighWord || source == Source::LinearRawBufferFormatWord ||
               source == Source::StructuredBufferFormatWord;
    }

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
