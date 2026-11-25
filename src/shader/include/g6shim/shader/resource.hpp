// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

#include "g6shim/shader/ir.hpp"
#include "g6shim/shader/native.hpp"

namespace g6shim::shader {

struct Site {
    std::size_t block = 0;
    std::size_t index = 0;

    friend constexpr bool operator==(const Site&, const Site&) = default;
    friend constexpr auto operator<=>(const Site&, const Site&) = default;
};

struct SiteHash {
    std::size_t operator()(Site site) const noexcept { return site.block * 1315423911u + site.index; }
};

inline constexpr std::size_t kCode = static_cast<std::size_t>(-1);

struct Address {
    std::size_t param = 0;
    std::uint32_t offset = 0;
    std::optional<Value> dynamic;

    friend bool operator==(const Address&, const Address&) = default;
};

struct Source {
    enum class Kind : std::uint8_t {
        Loaded,
        Built,
        Constant,
        Phi,
        Select,
        Fetched,
        Lanes,
        LanePhi,
    };

    Kind kind = Kind::Constant;
    Site load;
    std::uint8_t first = 0;
    Address address;
    std::uint32_t bits = 0;
    std::array<std::uint32_t, 2> words{};
    std::vector<std::uint32_t> constants;
    std::size_t block = 0;
    Value key;
    std::vector<Source> args;
    std::vector<std::optional<Source>> options;

    static Source loaded(Site load, std::uint8_t first);
    static Source built(Address address, std::uint32_t bits, std::array<std::uint32_t, 2> words);
    static Source constant(std::vector<std::uint32_t> constants);
    static Source phi(std::size_t block, std::vector<Source> args);
    static Source select(Value scc, std::vector<Source> args);
    static Source fetched(Site load, std::uint8_t first);
    static Source lanes(Value mask, std::vector<Source> args);
    static Source lane_phi(std::size_t block, std::vector<std::optional<Source>> options);

    std::vector<const Source*> leaves() const;
};

bool operator==(const Source& a, const Source& b);

enum class Role : std::uint8_t { Resource, Sampler };

struct Descriptor {
    Site site;
    Role role = Role::Resource;
    Source source;

    friend bool operator==(const Descriptor&, const Descriptor&) = default;
};

struct Waterfall {
    std::vector<Site> compares;
    std::vector<Value> reads;
    std::vector<Site> loads;

    friend bool operator==(const Waterfall&, const Waterfall&) = default;
};

struct Resources {
    std::vector<std::pair<Site, Address>> scalar;
    std::vector<Descriptor> descriptors;
    std::vector<std::tuple<Value, std::size_t, std::uint32_t>> params;
    std::vector<Site> spilled_root_loads;
    Waterfall waterfall;

    friend bool operator==(const Resources&, const Resources&) = default;
};

struct Structured;

Resources track(const Program& program, const std::vector<RootParam>& layout, HwStage stage,
                const Structured* lanes, std::span<const Site> ring_address_loads = {});
Resources track(const Program& program, const NativeProgram& native, const Structured* lanes,
                std::span<const Site> ring_address_loads = {});

std::optional<std::uint32_t> inline_constant(std::uint8_t code);

}  // namespace g6shim::shader
