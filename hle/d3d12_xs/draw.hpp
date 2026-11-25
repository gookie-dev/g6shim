// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "descriptor.hpp"
#include "host.hpp"
#include "object.hpp"
#include "pipeline_host.hpp"

#include <d3d12.h>
#include <g6shim/shader/native.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

namespace g6shim::shader::emit {
struct UserData;
}

namespace g6shim::d3d12_xs::draw {

struct VertexView {
    std::uint64_t address = 0;
    std::uint32_t size = 0;
    std::uint32_t stride = 0;
};
static_assert(sizeof(VertexView) == 16);

struct IndexView {
    std::uint64_t address = 0;
    std::uint32_t size = 0;
    std::uint32_t format = 0;
};
static_assert(sizeof(IndexView) == 16);

struct Root {
    enum class Kind { kNone, kTable, kConstants, kView };

    Kind kind = Kind::kNone;
    std::uint64_t value = 0;
    std::vector<std::uint32_t> constants;
};

struct State {
    State() = default;
    ~State();

    void reset(Object* pipeline);

    void set_topology(host::HostList& list, std::uint32_t topology);
    void set_pipeline(Object* pipeline);
    void set_heaps(const Object* const* heaps, std::size_t count);
    void set_table(bool compute, std::uint32_t slot, std::uint64_t handle);
    void set_view(bool compute, std::uint32_t slot, std::uint64_t address);
    void set_constants(bool compute, std::uint32_t slot, std::span<const std::uint32_t> values, std::uint32_t offset);
    void set_vertex_buffers(std::uint32_t start, std::span<const VertexView> views);
    void clear_vertex_buffers(std::uint32_t start, std::uint32_t count);
    void set_index_buffer(const IndexView* view);
    void set_targets(host::HostList& list, std::span<const std::uint64_t> handles, std::uint64_t depth);

    void draw(host::HostList& list, std::uint32_t vertices, std::uint32_t instances, std::uint32_t first,
              std::uint32_t first_instance);
    void draw_indexed(host::HostList& list, std::uint32_t indices, std::uint32_t instances, std::uint32_t first,
                      std::int32_t base, std::uint32_t first_instance);
    void dispatch(host::HostList& list, std::uint32_t x, std::uint32_t y, std::uint32_t z);
    void dispatch_mesh(host::HostList& list, std::uint32_t x);
    bool execute_indirect(host::HostList& list, std::tuple<std::uint32_t, std::uint32_t, std::uint32_t> signature,
                          std::uint64_t arguments, std::optional<std::uint64_t> count);
    void release_host_bindings();

private:
    void release_dynamic();
    std::vector<Root>& roots(bool compute);
    const std::vector<Root>& roots(bool compute) const;
    bool prepare(host::HostList& list, bool indexed);
    bool prepare_for(host::HostList& list, bool indexed, bool mesh);
    bool bind(host::HostList& list, const pipeline_host::HostPipeline& pipeline, bool compute);
    std::uint32_t user_value(bool compute, std::uint8_t slot, g6shim::shader::RootType root,
                             std::uint32_t dword) const;
    const descriptor::Heap* heap_of(std::uint64_t handle) const;
    std::optional<std::uint32_t> descriptor_value(host::HostList& list, bool compute,
                                                   const g6shim::shader::emit::UserData& data) const;
    bool rectangles(host::HostList& list, std::uint32_t count, std::uint32_t first);
    void table_entry(host::HostList& list, const g6shim::shader::UsedResource& used, bool compute);
    std::vector<std::pair<std::uint8_t, std::uint32_t>> tables(host::HostList& list,
                                                               std::span<const g6shim::shader::UsedResource> used,
                                                               bool compute);

    std::uintptr_t pipeline_ = 0;
    std::array<Object*, 2> heaps_{nullptr, nullptr};
    std::vector<Root> graphics_;
    std::vector<Root> compute_;
    std::array<VertexView, 32> vertex_{};
    IndexView index_{};
    std::vector<Object*> targets_;
    Object* depth_ = nullptr;
    std::uint32_t topology_ = 0;
    ID3D12PipelineState* host_pipeline_ = nullptr;
    std::array<ID3D12RootSignature*, 2> host_roots_{nullptr, nullptr};
    std::array<Object*, 2> host_heaps_{nullptr, nullptr};
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint32_t> fresh_tables_;
    std::vector<std::tuple<Object*, std::uint32_t, std::uint32_t>> dynamic_;
    bool table_failed_ = false;
};

void viewports(host::HostList& list, std::span<const D3D12_VIEWPORT> viewports);
void scissors(host::HostList& list, std::span<const RECT> rects);
void blend_factor(host::HostList& list, const float* factor);
void stencil_ref(host::HostList& list, std::uint32_t reference);

std::uint64_t drawn();
std::uint64_t dispatched();

}  // namespace g6shim::d3d12_xs::draw
