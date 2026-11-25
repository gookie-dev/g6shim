// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace g6shim::shader::emit {

struct SigElement {
    std::string name;
    std::uint32_t index = 0;
    std::uint8_t kind = 0;
    std::uint8_t comp = 0;
    std::uint8_t interp = 0;
    std::optional<std::uint32_t> row;
    std::uint8_t col = 0;
    std::uint8_t cols = 0;
    std::uint8_t usage = 0;
    bool half = false;

    friend bool operator==(const SigElement&, const SigElement&) = default;
};

struct Mesh {
    std::uint32_t max_vertices = 0;
    std::uint32_t max_primitives = 0;
    std::uint8_t topology = 0;

    friend bool operator==(const Mesh&, const Mesh&) = default;
};

struct Geometry {
    std::uint32_t input_primitive = 3;
    std::uint32_t max_vertices = 1024;
    std::uint32_t stream_mask = 1;
    std::uint32_t output_topology = 5;
    std::uint32_t instances = 1;

    friend bool operator==(const Geometry&, const Geometry&) = default;
};

std::vector<std::uint8_t> signature(std::span<const SigElement> elements, bool input);

struct Binding {
    std::uint32_t kind = 0;
    std::uint32_t space = 0;
    std::uint32_t register_index = 0;
    std::uint32_t dxil_kind = 0;

    friend bool operator==(const Binding&, const Binding&) = default;
};

struct Psv {
    std::uint8_t stage = 0;
    bool flag = false;
    std::span<const SigElement> inputs;
    std::span<const SigElement> outputs;
    std::span<const SigElement> prims;
    std::optional<std::pair<Mesh, std::uint32_t>> mesh;
    std::optional<Geometry> geometry;
    std::span<const Binding> bindings;
    std::array<std::uint32_t, 3> threads{};
    std::optional<std::uint32_t> wave_size;
    std::string_view entry_name{};
};

std::vector<std::uint8_t> psv(const Psv& p);

struct ContainerPart {
    std::array<std::uint8_t, 4> name{};
    std::span<const std::uint8_t> data;
};

std::vector<std::uint8_t> build(std::span<const ContainerPart> parts);

std::array<std::uint8_t, 16> hash(std::span<const std::uint8_t> data);

void md5_block(std::array<std::uint32_t, 4>& state, const std::uint8_t* block);

}  // namespace g6shim::shader::emit
