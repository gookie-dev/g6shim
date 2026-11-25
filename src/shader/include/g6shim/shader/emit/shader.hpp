// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "g6shim/shader/emit/bitcode.hpp"
#include "g6shim/shader/emit/container.hpp"

namespace g6shim::shader::emit {

inline constexpr std::uint32_t kUserDataSpace = 0;
inline constexpr std::uint32_t kSamplerSpace = 0;
inline constexpr std::uint32_t kRootCbvSpace = 1;

namespace feature {

inline constexpr std::uint64_t kRawBuffers = 1ull << 4;
inline constexpr std::uint64_t kStencilRef = 1ull << 11;
inline constexpr std::uint64_t kTypedUavLoads = 1ull << 13;
inline constexpr std::uint64_t kWaveOps = 1ull << 19;
inline constexpr std::uint64_t kInt64 = 1ull << 20;
inline constexpr std::uint64_t kAtomicInt64OnTypedResource = 1ull << 27;
inline constexpr std::uint64_t kAtomicInt64OnHeapResource = 1ull << 32;
inline constexpr std::uint64_t kResourceHeap = 1ull << 30;
inline constexpr std::uint64_t kSamplerHeap = 1ull << 31;

}  // namespace feature

struct Types {
    Ty void_type;
    Ty i1;
    Ty i32;
    Ty f32;
    Ty handle;
    Ty props;
    Ty bind;
    Ty cbuf_ret;
    Ty res_ret;
};

struct Shader {
    Module m;
    Types t;
    bool pixel = false;
    std::vector<SigElement> inputs;
    std::vector<SigElement> outputs;
    std::vector<SigElement> prims;
    std::optional<Mesh> mesh;
    std::optional<Geometry> geometry;
    std::uint32_t user_data = 0;
    std::uint32_t samplers = 0;
    std::uint64_t cmp_samplers = 0;
    std::vector<std::pair<std::uint8_t, V>> root_cbvs;
    std::vector<std::pair<std::uint8_t, V>> root_srvs;
    std::uint64_t flags = 0;
    std::uint32_t nonuniform_handles = 0;
    std::optional<std::array<std::uint32_t, 3>> threads;
    std::optional<std::uint32_t> wave_size;
    std::uint32_t lds_dwords = 0;
    V main;
    std::size_t entry = 0;

    explicit Shader(bool pixel);

    V lds();
    V op(std::size_t block, const std::string& name, Attr attr, Ty ret, std::uint32_t opcode,
         const std::vector<V>& args);
    V user_data_handle();
    V root_cbv_handle(std::uint8_t slot);
    V root_srv_handle(std::uint8_t slot);
    V sampler_handle(std::size_t block, std::uint32_t n);
    V annotate(std::size_t block, V handle, std::uint32_t props0, std::uint32_t props1);
    V heap_handle(std::size_t block, V index, bool sampler, bool nonuniform,
                  std::pair<std::uint32_t, std::uint32_t> props);
    std::vector<std::uint8_t> finish();

private:
    void fix_user_data_size(std::uint32_t size);
    std::optional<Md> signature_md(std::vector<SigElement> elements);

    std::optional<V> lds_;
    bool uses_implicit_lod_ = false;
};

}  // namespace g6shim::shader::emit
