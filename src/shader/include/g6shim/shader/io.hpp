// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "g6shim/shader/ir.hpp"
#include "g6shim/shader/native.hpp"
#include "g6shim/shader/resource.hpp"

namespace g6shim::shader {

enum class Barycentric : std::uint8_t {
    PerspectiveSample,
    PerspectiveCenter,
    PerspectiveCentroid,
    PerspectivePullModel,
    LinearSample,
    LinearCenter,
    LinearCentroid,
};

struct Input {
    enum class Kind : std::uint8_t {
        VertexId,
        PrimitiveId,
        InstanceId,
        Attribute,
        Barycentric,
        Position,
        FrontFace,
        Ancillary,
        SampleCoverage,
        FixedPosition,
        LineStipple,
        GroupId,
        ThreadIdInGroup,
        GroupInfo,
        MeshGroup,
        MergedWaveInfo,
    };

    Kind kind = Kind::VertexId;
    std::uint8_t row = 0;
    std::uint8_t channel = 0;
    Barycentric bary = Barycentric::PerspectiveSample;
    std::uint8_t index = 0;

    static Input simple(Kind kind);
    static Input of_attribute(std::uint8_t row, std::uint8_t channel);
    static Input of_barycentric(Barycentric bary, std::uint8_t index);
    static Input of_position(std::uint8_t channel);
    static Input of_group_id(std::uint8_t component);
    static Input of_thread_id(std::uint8_t component);

    friend bool operator==(const Input&, const Input&) = default;
};

struct Attribute {
    Usage usage;
    std::uint8_t row = 0;
    std::uint8_t channel = 0;
    bool high = false;

    friend bool operator==(const Attribute&, const Attribute&) = default;
};

struct Interp {
    enum class Kind : std::uint8_t { Interpolated, Vertex };

    Kind kind = Kind::Interpolated;
    Attribute attribute;
    Barycentric bary = Barycentric::PerspectiveSample;
    std::vector<Site> first;
    std::uint8_t vertex = 0;

    static Interp interpolated(Attribute attribute, Barycentric bary, std::vector<Site> first);
    static Interp vertex_value(Attribute attribute, std::uint8_t vertex);

    friend bool operator==(const Interp&, const Interp&) = default;
};

struct Target {
    enum class Kind : std::uint8_t {
        Color,
        Depth,
        Null,
        Position,
        Param,
        Unread,
        Primitive,
        PrimitiveParam,
    };

    Kind kind = Kind::Color;
    std::uint8_t index = 0;
    Usage usage;
    std::uint8_t row = 0;

    static Target color(std::uint8_t index);
    static Target depth();
    static Target null_target();
    static Target position(std::uint8_t channel);
    static Target param(Usage usage, std::uint8_t row);
    static Target unread();
    static Target primitive();
    static Target primitive_param(Usage usage, std::uint8_t row);

    friend bool operator==(const Target&, const Target&) = default;
};

struct Export {
    Site site;
    Target target;
    std::array<std::optional<Value>, 4> values;
    bool compressed = false;
    bool done = false;

    friend bool operator==(const Export&, const Export&) = default;
};

struct RingLoad {
    Site site;
    std::uint8_t vgpr = 0;
    std::uint8_t vertex = 0;
    std::uint32_t dword = 0;

    friend bool operator==(const RingLoad&, const RingLoad&) = default;
};

struct RingStore {
    Site site;
    std::uint8_t vgpr = 0;
    std::uint32_t dword = 0;

    friend bool operator==(const RingStore&, const RingStore&) = default;
};

struct Io {
    std::vector<std::pair<Value, Input>> inputs;
    std::vector<std::pair<Site, Interp>> interps;
    std::vector<Export> exports;
    std::vector<RingLoad> ring_loads;
    std::vector<RingStore> ring_stores;
    std::vector<Site> ring_address_loads;
    std::vector<Value> geometry_primitive_counts;
    std::optional<std::uint8_t> initialized_sgprs;

    friend bool operator==(const Io&, const Io&) = default;
};

std::vector<std::pair<std::uint8_t, Input>> pixel_input_vgprs(std::uint32_t input_addr);

Io raise(const Program& program, const NativeProgram& native);

}  // namespace g6shim::shader
