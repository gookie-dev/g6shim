// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "draw.hpp"

#include "descriptor_host.hpp"
#include "indirect.hpp"
#include "later.hpp"
#include "pipeline.hpp"
#include "pipeline_bridge.hpp"
#include "primitive.hpp"
#include "resource.hpp"
#include "shadow.hpp"
#include "texture.hpp"

#include <g6shim/log.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace g6shim::d3d12_xs::draw {

namespace {

using ResourceKind = g6shim::shader::ResourceType;
using RootKind = g6shim::shader::RootType;

constexpr std::uint32_t kRenderTarget = 0x4;
constexpr std::uint32_t kDepthWrite = 0x10;
constexpr std::uint32_t kShaderResource = 0x40 | 0x80;
constexpr std::uint32_t kUnorderedAccess = 0x8;
constexpr std::uint64_t kSrvViewBytes = 1 << 20;
constexpr std::uint64_t kCbvViewBytes = 1 << 16;
constexpr std::uint32_t kTriangles = 4;
constexpr std::uint32_t kRectangleTopology = 17;
constexpr std::uint32_t kTriangleFan = 6;
constexpr std::uint32_t kLineLoop = 18;
constexpr std::uint32_t kQuads = 19;
constexpr std::uint32_t kQuadStrip = 20;
constexpr std::uint64_t kDescriptorSlotBytes = 32;

std::atomic<std::uint64_t> g_drawn{0};
std::atomic<std::uint64_t> g_dispatched{0};
std::atomic<std::uint64_t> g_dropped{0};

template <typename T>
std::span<const T> items(const T* data, std::uint32_t count) {
    if (data == nullptr || count == 0) return {};
    return std::span<const T>(data, count);
}

void drop() { g_dropped.fetch_add(1, std::memory_order_relaxed); }

std::uint64_t descriptor_hash(std::uint64_t table, std::uint32_t count) {
    std::uint64_t hash = 0xcbf2'9ce4'8422'2325ull;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::array<std::uint32_t, 3> words{};
        std::memcpy(words.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(
                                   table + static_cast<std::uint64_t>(index) * kDescriptorSlotBytes)),
                    sizeof words);
        for (const std::uint32_t word : words) {
            hash ^= word;
            hash *= 0x0000'0100'0000'01b3ull;
        }
    }
    return hash;
}

std::optional<std::uint64_t> root_view(host::HostList& list, std::uint64_t address, bool srv) {
    const auto found = shadow::find(address, 1);
    if (!found) return std::nullopt;
    const auto& shadow = *found;
    const std::uint64_t length = std::min(shadow->left(address), srv ? kSrvViewBytes : kCbvViewBytes);
    const std::uint64_t gpu = shadow->gpu(address);
    if (srv || gpu % 256 == 0) {
        shadow->refresh(list, address, length);
        return gpu;
    }
    const auto scratch = list.scratch(length);
    if (!scratch) return std::nullopt;
    list.defer_fill(scratch->second, reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(address)),
                    static_cast<std::size_t>(length));
    return scratch->first;
}

ID3D12CommandSignature* signature_for(std::uint32_t kind, std::uint32_t stride) {
    static std::mutex mutex;
    static std::vector<std::pair<std::pair<std::uint32_t, std::uint32_t>,
                                 Microsoft::WRL::ComPtr<ID3D12CommandSignature>>>
        made;
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& [key, signature] : made)
        if (key == std::make_pair(kind, stride)) return signature.Get();
    host::Host* gpu = host::host();
    if (gpu == nullptr) return nullptr;
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = static_cast<D3D12_INDIRECT_ARGUMENT_TYPE>(kind);
    D3D12_COMMAND_SIGNATURE_DESC description{};
    description.ByteStride = stride;
    description.NumArgumentDescs = 1;
    description.pArgumentDescs = &argument;
    description.NodeMask = 0;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> signature;
    const HRESULT made_result =
        gpu->device()->CreateCommandSignature(&description, nullptr, IID_PPV_ARGS(&signature));
    if (FAILED(made_result)) {
        static std::once_flag once;
        std::call_once(once, [] {
            G6SHIM_WARN("d3d12_xs: the host refused an indirect command signature");
        });
        return nullptr;
    }
    ID3D12CommandSignature* raw = signature.Get();
    made.emplace_back(std::make_pair(kind, stride), std::move(signature));
    return raw;
}

void readable(host::HostList& list, const g6shim::shader::UsedResource& used, const descriptor::View& original) {
    descriptor::View view = original;
    const auto placed = descriptor_host::placed_texture(original);
    if (placed) view = *placed;
    const auto range = descriptor_host::buffer_range(view);
    if (range && range->second > 0) {
        const auto found = shadow::find(range->first, range->second);
        if (found) {
            (*found)->refresh(list, range->first, range->second);
            if (used.kind.kind == ResourceKind::Kind::Uav)
                (*found)->gpu_writes(list, range->first, range->second);
        }
    }
    if (const auto* shader = std::get_if<descriptor::ShaderView>(&view);
        shader != nullptr && used.kind.kind == ResourceKind::Kind::Srv) {
        auto* object = reinterpret_cast<Object*>(shader->resource);
        ID3D12Resource* texture = texture::host_texture(object);
        if (texture != nullptr && texture::title_written(texture)) texture::upload(list, object, texture);
        host::ensure_state(list, object, kShaderResource);
    } else if (const auto* unordered = std::get_if<descriptor::UnorderedView>(&view);
               unordered != nullptr && used.kind.kind == ResourceKind::Kind::Uav) {
        host::ensure_state(list, reinterpret_cast<Object*>(unordered->resource), kUnorderedAccess);
    }
}

}  // namespace

std::uint64_t drawn() { return g_drawn.load(std::memory_order_relaxed); }

std::uint64_t dispatched() { return g_dispatched.load(std::memory_order_relaxed); }

State::~State() { release_dynamic(); }

void State::release_dynamic() {
    for (const auto& [heap, first, count] : dynamic_) later::descriptor_release_dynamic(heap, first, count);
    dynamic_.clear();
}

void State::reset(Object* pipeline) {
    release_dynamic();
    pipeline_ = reinterpret_cast<std::uintptr_t>(pipeline);
    heaps_ = {nullptr, nullptr};
    graphics_.clear();
    compute_.clear();
    vertex_ = {};
    index_ = {};
    targets_.clear();
    depth_ = nullptr;
    topology_ = 0;
    host_pipeline_ = nullptr;
    host_roots_ = {nullptr, nullptr};
    host_heaps_ = {nullptr, nullptr};
    fresh_tables_.clear();
    table_failed_ = false;
}

void State::release_host_bindings() {
    host_pipeline_ = nullptr;
    host_roots_[1] = nullptr;
}

std::vector<Root>& State::roots(bool compute) { return compute ? compute_ : graphics_; }

const std::vector<Root>& State::roots(bool compute) const { return compute ? compute_ : graphics_; }

void State::set_topology(host::HostList& list, std::uint32_t topology) {
    topology_ = topology;
    std::uint32_t host_topology = topology;
    if (topology == kRectangleTopology) {
        host_topology = kTriangles;
    } else if (topology == kTriangleFan || topology == kLineLoop || topology == kQuads || topology == kQuadStrip) {
        static std::once_flag once;
        std::call_once(once, [] {
            G6SHIM_WARN("d3d12_xs: a console topology is drawn as a triangle list");
        });
        host_topology = kTriangles;
    }
    list.list()->IASetPrimitiveTopology(static_cast<D3D_PRIMITIVE_TOPOLOGY>(host_topology));
}

void State::set_pipeline(Object* pipeline) { pipeline_ = reinterpret_cast<std::uintptr_t>(pipeline); }

void State::set_heaps(const Object* const* heaps, std::size_t count) {
    heaps_ = {nullptr, nullptr};
    if (heaps == nullptr) return;
    for (const Object* heap : std::span<const Object* const>(heaps, count)) {
        if (heap == nullptr) continue;
        const auto* descriptor_heap = static_cast<const descriptor::Heap*>(heap);
        heaps_[descriptor_heap->samplers() ? 1 : 0] = const_cast<Object*>(heap);
    }
}

void State::set_table(bool compute, std::uint32_t slot, std::uint64_t handle) {
    auto& all = roots(compute);
    if (all.size() <= slot) all.resize(static_cast<std::size_t>(slot) + 1);
    all[slot] = Root{Root::Kind::kTable, handle, {}};
}

void State::set_view(bool compute, std::uint32_t slot, std::uint64_t address) {
    auto& all = roots(compute);
    if (all.size() <= slot) all.resize(static_cast<std::size_t>(slot) + 1);
    all[slot] = Root{Root::Kind::kView, address, {}};
}

void State::set_constants(bool compute, std::uint32_t slot, std::span<const std::uint32_t> values,
                          std::uint32_t offset) {
    auto& all = roots(compute);
    if (all.size() <= slot) all.resize(static_cast<std::size_t>(slot) + 1);
    if (all[slot].kind != Root::Kind::kConstants) all[slot] = Root{Root::Kind::kConstants, 0, {}};
    auto& constants = all[slot].constants;
    const std::size_t end = static_cast<std::size_t>(offset) + values.size();
    if (constants.size() < end) constants.resize(end, 0);
    std::copy(values.begin(), values.end(), constants.begin() + offset);
}

void State::set_vertex_buffers(std::uint32_t start, std::span<const VertexView> views) {
    for (std::size_t index = 0; index < views.size(); ++index) {
        const std::size_t slot = static_cast<std::size_t>(start) + index;
        if (slot < vertex_.size()) vertex_[slot] = views[index];
    }
}

void State::clear_vertex_buffers(std::uint32_t start, std::uint32_t count) {
    for (std::size_t slot = start; slot < vertex_.size() && slot < static_cast<std::size_t>(start) + count; ++slot)
        vertex_[slot] = VertexView{};
}

void State::set_index_buffer(const IndexView* view) { index_ = view != nullptr ? *view : IndexView{}; }

void State::set_targets(host::HostList& list, std::span<const std::uint64_t> handles, std::uint64_t depth) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    targets_.clear();
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;
    for (const std::uint64_t handle : handles) {
        const auto record = descriptor::rtv_record(handle);
        if (!record) {
            static std::once_flag once;
            std::call_once(once, [] {
                G6SHIM_WARN("d3d12_xs: a render target has no recorded view");
            });
            continue;
        }
        ID3D12Resource* texture = texture::host_texture(record->resource);
        if (texture == nullptr) continue;
        targets_.push_back(record->resource);
        const auto description = descriptor::rtv_description(*record);
        rtvs.push_back(gpu->rtv(texture, description ? &*description : nullptr));
    }
    depth_ = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    bool has_depth = false;
    if (const auto record = descriptor::dsv_record(depth)) {
        ID3D12Resource* texture = texture::host_texture(record->resource);
        if (texture != nullptr) {
            depth_ = record->resource;
            const auto description = descriptor::dsv_description(*record);
            dsv = gpu->dsv(texture, description ? &*description : nullptr);
            has_depth = true;
        }
    }
    list.list()->OMSetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.empty() ? nullptr : rtvs.data(), FALSE,
                                    has_depth ? &dsv : nullptr);
}

std::uint32_t State::user_value(bool compute, std::uint8_t slot, RootKind root,
                                std::uint32_t dword) const {
    const auto& all = roots(compute);
    const Root* entry = slot < all.size() ? &all[slot] : nullptr;
    if (entry != nullptr && entry->kind == Root::Kind::kTable && root.kind == RootKind::Kind::Table) {
        const descriptor::Heap* heap = heap_of(entry->value);
        if (heap != nullptr) return static_cast<std::uint32_t>(entry->value - heap->start);
        static std::once_flag once;
        std::call_once(once, [] {
            G6SHIM_WARN("d3d12_xs: a descriptor table lies outside the bound heaps");
        });
        return 0;
    }
    if (entry != nullptr && entry->kind == Root::Kind::kConstants &&
        root.kind == RootKind::Kind::Constants)
        return dword < entry->constants.size() ? entry->constants[dword] : 0;
    static std::once_flag once;
    std::call_once(once, [] {
        G6SHIM_WARN("d3d12_xs: a shader reads a root argument the title did not set");
    });
    return 0;
}

const descriptor::Heap* State::heap_of(std::uint64_t handle) const {
    for (Object* heap : heaps_) {
        if (heap == nullptr) continue;
        const auto* descriptor_heap = static_cast<const descriptor::Heap*>(heap);
        if (descriptor_heap->index(handle)) return descriptor_heap;
    }
    return nullptr;
}

std::optional<std::uint32_t> State::descriptor_value(
    bool compute, const g6shim::shader::emit::UserData& data) const {
    const auto& all = roots(compute);
    if (data.slot >= all.size()) return std::nullopt;
    const Root& root = all[data.slot];
    if (root.kind != Root::Kind::kTable) return std::nullopt;
    const descriptor::Heap* heap = heap_of(root.value);
    if (heap == nullptr) return std::nullopt;
    using Source = g6shim::shader::emit::UserData::Source;
    if (data.source == Source::ImageBaseAddressLow)
        return descriptor_host::table_image_base_word(*heap, root.value, data.dword);
    if (data.source == Source::ImageSamplingFields)
        return descriptor_host::table_image_sampling_fields(*heap, root.value, data.dword);
    if (data.source == Source::TypedBufferFormatWord || data.source == Source::TypedBufferHighWord)
        return descriptor_host::table_typed_buffer_word(
            *heap, root.value, data.dword, data.source == Source::TypedBufferHighWord);
    if (data.source == Source::ShaderBufferComponentMapping)
        return descriptor_host::table_buffer_component_mapping(*heap, root.value, data.dword);
    if (data.source == Source::StructuredBufferHighWord)
        return descriptor_host::table_structured_buffer_high_word(*heap, root.value, data.dword);
    if (data.source == Source::LinearRawBufferHighWord)
        return descriptor_host::table_linear_raw_buffer_high_word(*heap, root.value, data.dword);
    if (data.source == Source::LinearRawBufferFormatWord || data.source == Source::StructuredBufferFormatWord)
        return descriptor_host::table_untyped_buffer_format_word(
            *heap, root.value, data.dword, data.source == Source::StructuredBufferFormatWord);
    const auto address = descriptor_host::table_buffer_address(
        *heap, root.value, data.dword, data.source == Source::ConstantBufferAddressLow);
    if (!address) return std::nullopt;
    if (data.source == Source::ShaderBufferAddressHigh)
        return static_cast<std::uint32_t>(*address >> 32) & data.known_bits();
    return static_cast<std::uint32_t>(*address);
}

bool State::prepare(host::HostList& list, bool indexed) { return prepare_for(list, indexed, false); }

bool State::prepare_for(host::HostList& list, bool indexed, bool mesh) {
    const pipeline_host::HostPipeline* pipeline =
        pipeline_bridge::host_pipeline_for(reinterpret_cast<Object*>(pipeline_));
    if (pipeline == nullptr) return false;
    if (pipeline->compute || pipeline->mesh != mesh) {
        static std::once_flag once;
        std::call_once(once, [] {
            G6SHIM_WARN("d3d12_xs: a draw or mesh dispatch with another kind of pipeline");
        });
        return false;
    }
    if (!bind(list, *pipeline, false)) return false;
    ID3D12GraphicsCommandList* commands = list.list();
    std::array<D3D12_VERTEX_BUFFER_VIEW, 32> views{};
    std::uint32_t count = 0;
    for (std::size_t slot = 0; slot < vertex_.size(); ++slot) {
        if ((pipeline->slots & (1u << slot)) == 0) continue;
        count = static_cast<std::uint32_t>(slot) + 1;
        const VertexView& view = vertex_[slot];
        if (view.address == 0 || view.size == 0) continue;
        const auto found = shadow::find(view.address, view.size);
        if (!found) continue;
        (*found)->refresh(list, view.address, view.size);
        views[slot].BufferLocation = (*found)->gpu(view.address);
        views[slot].SizeInBytes = view.size;
        views[slot].StrideInBytes = view.stride;
    }
    if (count > 0) commands->IASetVertexBuffers(0, count, views.data());
    if (indexed) {
        const auto found = shadow::find(index_.address, index_.size);
        if (!found) return false;
        (*found)->refresh(list, index_.address, index_.size);
        D3D12_INDEX_BUFFER_VIEW view{};
        view.BufferLocation = (*found)->gpu(index_.address);
        view.SizeInBytes = index_.size;
        view.Format = static_cast<DXGI_FORMAT>(index_.format);
        commands->IASetIndexBuffer(&view);
    }
    for (Object* target : targets_) host::ensure_state(list, target, kRenderTarget);
    if (depth_ != nullptr) host::ensure_state(list, depth_, kDepthWrite);
    return true;
}

bool State::bind(host::HostList& list, const pipeline_host::HostPipeline& pipeline, bool compute) {
    ID3D12GraphicsCommandList* commands = list.list();
    if (host_heaps_ != heaps_) {
        std::vector<ID3D12DescriptorHeap*> heaps;
        for (Object* heap : heaps_) {
            if (heap == nullptr) continue;
            descriptor_host::HostHeap* host_heap = descriptor_host::for_heap(heap);
            if (host_heap == nullptr) continue;
            list.keep(host_heap->heap());
            heaps.push_back(host_heap->heap());
        }
        commands->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.empty() ? nullptr : heaps.data());
        host_heaps_ = heaps_;
        host_roots_ = {nullptr, nullptr};
    }
    const std::size_t stage_index = compute ? 1 : 0;
    ID3D12RootSignature* root = pipeline.root.Get();
    if (host_roots_[stage_index] != root) {
        if (compute)
            commands->SetComputeRootSignature(root);
        else
            commands->SetGraphicsRootSignature(root);
        host_roots_[stage_index] = root;
    }
    if (host_pipeline_ != pipeline.pso.Get()) {
        commands->SetPipelineState(pipeline.pso.Get());
        host_pipeline_ = pipeline.pso.Get();
        list.used_pipeline(pipeline.name);
        list.keep(pipeline.pso.Get());
        list.keep(root);
    }
    for (const pipeline_host::Stage& stage : pipeline.stages) {
        const Translated* translated = stage.translated();
        if (translated == nullptr) {
            static std::once_flag once;
            std::call_once(once, [] {
                G6SHIM_WARN("d3d12_xs: a host pipeline stage has no translation");
            });
            return false;
        }
        using Source = g6shim::shader::emit::UserData::Source;
        std::vector<std::uint32_t> values;
        if (stage.user_data) {
            values.resize(stage.user_data->second);
            for (const auto& data : translated->host.user_data) {
                if (data.source == Source::Root) continue;
                const auto value = descriptor_value(compute, data);
                if (!value || data.index >= values.size()) {
                    static std::once_flag once;
                    std::call_once(once, [] {
                        G6SHIM_WARN("d3d12_xs: a shader reads a field of an unavailable descriptor");
                    });
                    return false;
                }
                values[data.index] = *value;
            }
        }
        table_failed_ = false;
        const auto made = tables(list, translated->used, compute);
        if (table_failed_) {
            static std::atomic<std::uint64_t> dropped{0};
            const std::uint64_t number = dropped.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((number & (number - 1)) == 0)
                G6SHIM_WARN("d3d12_xs: a draw or dispatch dropped: no dynamic slots for its tables");
            return false;
        }
        if (stage.user_data) {
            const auto [parameter, dwords] = *stage.user_data;
            for (const auto& data : translated->host.user_data) {
                if (data.source != Source::Root) continue;
                const auto fresh = std::find_if(made.begin(), made.end(), [&](const auto& entry) {
                    return entry.first == data.slot && data.root.kind == RootKind::Kind::Table;
                });
                const std::uint32_t value = fresh != made.end()
                                                ? fresh->second
                                                : user_value(compute, data.slot, data.root, data.dword);
                if (data.index < values.size()) values[data.index] = value;
            }
            if (compute)
                commands->SetComputeRoot32BitConstants(parameter, dwords, values.data(), 0);
            else
                commands->SetGraphicsRoot32BitConstants(parameter, dwords, values.data(), 0);
        }
        for (int srv = 0; srv < 2; ++srv) {
            const auto& views = srv == 0 ? stage.root_cbvs : stage.root_srvs;
            for (const auto& [slot, parameter] : views) {
                const auto& all = roots(compute);
                const Root* entry = slot < all.size() ? &all[slot] : nullptr;
                if (entry == nullptr || entry->kind != Root::Kind::kView) {
                    static std::once_flag once;
                    std::call_once(once, [] {
                        G6SHIM_WARN("d3d12_xs: a shader reads a root view the title did not set");
                    });
                    continue;
                }
                const auto gpu = root_view(list, entry->value, srv != 0);
                if (!gpu) continue;
                if (compute)
                    srv == 0 ? commands->SetComputeRootConstantBufferView(parameter, *gpu)
                             : commands->SetComputeRootShaderResourceView(parameter, *gpu);
                else
                    srv == 0 ? commands->SetGraphicsRootConstantBufferView(parameter, *gpu)
                             : commands->SetGraphicsRootShaderResourceView(parameter, *gpu);
            }
        }
    }
    return true;
}

void State::dispatch(host::HostList& list, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    const pipeline_host::HostPipeline* pipeline =
        pipeline_bridge::host_pipeline_for(reinterpret_cast<Object*>(pipeline_), true);
    if (pipeline == nullptr) {
        static std::once_flag once;
        std::call_once(once, [] {
            G6SHIM_WARN("d3d12_xs: a dispatch without a host pipeline dropped");
        });
        drop();
        return;
    }
    if (!pipeline->compute) {
        static std::once_flag once;
        std::call_once(once, [] {
            G6SHIM_WARN("d3d12_xs: a dispatch with a graphics pipeline");
        });
        return;
    }
    if (!bind(list, *pipeline, true)) {
        drop();
        return;
    }
    list.list()->Dispatch(x, y, z);
    g_dispatched.fetch_add(1, std::memory_order_relaxed);
}

void State::dispatch_mesh(host::HostList& list, std::uint32_t x) {
    bool ran = prepare_for(list, false, true);
    if (ran) {
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> commands;
        if (SUCCEEDED(list.list()->QueryInterface(IID_PPV_ARGS(&commands)))) {
            commands->DispatchMesh(x, 1, 1);
            g_drawn.fetch_add(1, std::memory_order_relaxed);
        } else {
            ran = false;
        }
    }
    if (!ran) drop();
}

void State::table_entry(host::HostList& list, const g6shim::shader::UsedResource& used, bool compute) {
    const auto& all = roots(compute);
    if (used.slot >= all.size()) return;
    const Root& root = all[used.slot];
    if (root.kind != Root::Kind::kTable) return;
    const std::uint64_t handle = root.value + static_cast<std::uint64_t>(used.table_offset_dwords) * 4;
    const descriptor::Heap* heap = heap_of(handle);
    if (heap == nullptr) return;
    descriptor_host::HostHeap* host_heap = descriptor_host::for_heap(const_cast<descriptor::Heap*>(heap));
    const auto index = heap->index(handle);
    if (host_heap == nullptr || !index) return;
    const auto view = host_heap->mirror(*index, handle, true);
    if (view) readable(list, used, *view);
}

std::vector<std::pair<std::uint8_t, std::uint32_t>> State::tables(
    host::HostList& list, std::span<const g6shim::shader::UsedResource> used, bool compute) {
    const auto& all = roots(compute);
    std::vector<std::uint8_t> fresh;
    for (const auto& resource : used)
        if (resource.root.kind == RootKind::Kind::Table && resource.kind.kind == ResourceKind::Kind::Cbv)
            fresh.push_back(resource.slot);
    for (const auto& resource : used) {
        if (resource.root.kind != RootKind::Kind::Table) continue;
        if (std::find(fresh.begin(), fresh.end(), resource.slot) != fresh.end()) continue;
        if (resource.slot >= all.size()) continue;
        const Root& root = all[resource.slot];
        if (root.kind != Root::Kind::kTable) continue;
        const std::uint64_t end = root.value + static_cast<std::uint64_t>(resource.table_offset_dwords) * 4;
        const descriptor::Heap* heap = heap_of(root.value);
        if (heap == nullptr) continue;
        descriptor_host::HostHeap* host_heap = descriptor_host::for_heap(const_cast<descriptor::Heap*>(heap));
        const auto index = heap->index(end);
        if (host_heap == nullptr || !index) continue;
        if (*index >= host_heap->count()) fresh.push_back(resource.slot);
    }
    std::sort(fresh.begin(), fresh.end());
    fresh.erase(std::unique(fresh.begin(), fresh.end()), fresh.end());
    for (const auto& resource : used) {
        if (resource.root.kind != RootKind::Kind::Table) continue;
        if (std::find(fresh.begin(), fresh.end(), resource.slot) != fresh.end()) continue;
        table_entry(list, resource, compute);
    }
    std::vector<std::pair<std::uint8_t, std::uint32_t>> made;
    for (const std::uint8_t slot : fresh) {
        if (slot >= all.size()) continue;
        const Root& root = all[slot];
        if (root.kind != Root::Kind::kTable) continue;
        std::vector<const g6shim::shader::UsedResource*> entries;
        for (const auto& resource : used)
            if (resource.root.kind == RootKind::Kind::Table && resource.slot == slot) entries.push_back(&resource);
        const descriptor::Heap* heap = heap_of(root.value);
        if (heap == nullptr) continue;
        descriptor_host::HostHeap* host_heap = descriptor_host::for_heap(const_cast<descriptor::Heap*>(heap));
        if (host_heap == nullptr) continue;
        std::uint32_t count = 1;
        for (const auto* entry : entries)
            count = std::max(count, entry->table_offset_dwords / 8 + 1);
        const auto key = std::make_pair(root.value, descriptor_hash(root.value, count));
        const auto known = fresh_tables_.find(key);
        if (known != fresh_tables_.end()) {
            for (const auto* entry : entries) {
                if (entry->kind.kind == ResourceKind::Kind::Cbv) continue;
                const std::uint64_t handle = root.value + static_cast<std::uint64_t>(entry->table_offset_dwords) * 4;
                if (const auto view = descriptor::view(handle)) readable(list, *entry, *view);
            }
            made.emplace_back(slot, known->second * 32);
            continue;
        }
        const auto first = host_heap->dynamic(count);
        if (!first) {
            table_failed_ = true;
            continue;
        }
        fresh_tables_.emplace(key, *first);
        dynamic_.emplace_back(const_cast<descriptor::Heap*>(heap), *first, count);
        for (std::uint32_t index = 0; index < count; ++index) {
            const std::uint32_t host_index = *first + index;
            const g6shim::shader::UsedResource* entry = nullptr;
            for (const auto* candidate : entries) {
                if (candidate->table_offset_dwords / 8 == index) {
                    entry = candidate;
                    break;
                }
            }
            const std::uint64_t handle = root.value + static_cast<std::uint64_t>(index) * kDescriptorSlotBytes;
            const auto view = descriptor::view(handle);
            if (view) {
                if (const auto* constant = std::get_if<descriptor::ConstantView>(&*view)) {
                    const auto scratch = list.scratch(constant->size);
                    if (!scratch) {
                        host_heap->make_null(host_index);
                        continue;
                    }
                    list.defer_fill(scratch->second,
                                    reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(
                                        constant->address)),
                                    constant->size);
                    host_heap->constant_at(host_index, scratch->first, constant->size);
                } else {
                    host_heap->make_at(host_index, *view);
                    if (entry != nullptr) readable(list, *entry, *view);
                }
            } else {
                host_heap->make_null(host_index);
                if (entry != nullptr) {
                    static std::once_flag once;
                    std::call_once(once, [] {
                        G6SHIM_WARN("d3d12_xs: a table entry has no view");
                    });
                }
            }
        }
        made.emplace_back(slot, *first * 32);
    }
    return made;
}

void State::draw(host::HostList& list, std::uint32_t vertices, std::uint32_t instances, std::uint32_t first,
                 std::uint32_t first_instance) {
    const bool rectangles = topology_ == kRectangleTopology;
    const bool ran = prepare(list, false) && (!rectangles || this->rectangles(list, vertices, first));
    if (ran) {
        const std::uint32_t count = rectangles ? vertices / 3 * 6 : vertices;
        const std::uint32_t from = rectangles ? 0 : first;
        list.list()->DrawInstanced(count, instances, from, first_instance);
        g_drawn.fetch_add(1, std::memory_order_relaxed);
    } else {
        drop();
    }
}

bool State::rectangles(host::HostList& list, std::uint32_t count, std::uint32_t first) {
    const pipeline_host::HostPipeline* pipeline =
        pipeline_bridge::host_pipeline_for(reinterpret_cast<Object*>(pipeline_));
    if (pipeline == nullptr) return false;
    const std::uint64_t rects = count / 3;
    for (std::uint32_t slot = 0; slot < 32; ++slot) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> elements;
        for (const pipeline_host::Input& input : pipeline->inputs)
            if (input.slot == slot && !input.per_instance) elements.emplace_back(input.offset, input.format);
        if (elements.empty()) continue;
        const VertexView& view = vertex_[slot];
        const std::uint64_t stride = view.stride;
        const std::uint64_t from = view.address + static_cast<std::uint64_t>(first) * stride;
        if (view.address == 0 || stride == 0 ||
            static_cast<std::uint64_t>(first + count) * stride > view.size ||
            !shadow::find(from, rects * 3 * stride)) {
            static std::once_flag once;
            std::call_once(once, [] {
                G6SHIM_WARN("d3d12_xs: rectangle vertices outside their buffer");
            });
            return false;
        }
        const auto scratch = list.scratch(rects * 6 * stride);
        if (!scratch) return false;
        const auto* source = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(from));
        std::uint8_t* destination = scratch->second;
        const std::size_t step = static_cast<std::size_t>(stride);
        const std::size_t source_bytes = static_cast<std::size_t>(rects * 3 * stride);
        const std::size_t destination_bytes = static_cast<std::size_t>(rects * 6 * stride);
        list.defer_host([source, destination, step, source_bytes, destination_bytes, elements] {
            primitive::expand_rects(std::span<const std::uint8_t>(source, source_bytes),
                                    std::span<std::uint8_t>(destination, destination_bytes), step, elements);
        });
        D3D12_VERTEX_BUFFER_VIEW host_view{};
        host_view.BufferLocation = scratch->first;
        host_view.SizeInBytes = static_cast<UINT>(rects * 6 * stride);
        host_view.StrideInBytes = view.stride;
        list.list()->IASetVertexBuffers(slot, 1, &host_view);
    }
    return true;
}

void State::draw_indexed(host::HostList& list, std::uint32_t indices, std::uint32_t instances, std::uint32_t first,
                         std::int32_t base, std::uint32_t first_instance) {
    if (topology_ == kRectangleTopology) {
        static std::once_flag once;
        std::call_once(once, [] {
            G6SHIM_WARN("d3d12_xs: indexed rectangles not drawn");
        });
    }
    const bool ran = topology_ != kRectangleTopology && prepare(list, true);
    if (ran) {
        list.list()->DrawIndexedInstanced(indices, instances, first, static_cast<INT>(base), first_instance);
        g_drawn.fetch_add(1, std::memory_order_relaxed);
    } else {
        drop();
    }
}

bool State::execute_indirect(host::HostList& list, std::tuple<std::uint32_t, std::uint32_t, std::uint32_t> signature,
                             std::uint64_t arguments, std::optional<std::uint64_t> count) {
    constexpr std::uint32_t kDraw = 0;
    constexpr std::uint32_t kIndexed = 1;
    constexpr std::uint32_t kDispatch = 2;
    constexpr std::uint32_t kMesh = 10;
    const auto [kind, stride, max] = signature;
    const std::uint64_t length = static_cast<std::uint64_t>(stride) * std::max(max, 1u);
    const auto args = shadow::find(arguments, length);
    if (!args) return false;
    std::optional<std::shared_ptr<shadow::Shadow>> counter_shadow;
    std::uint64_t counter_at = 0;
    if (count) {
        counter_shadow = shadow::find(*count, 4);
        if (!counter_shadow || !*counter_shadow) return false;
        counter_at = *count;
    }
    const bool known = kind == kDraw || kind == kIndexed || kind == kDispatch || kind == kMesh;
    const auto host_signature = known ? signature_for(kind, stride) : nullptr;
    if (!host_signature) return false;
    (*args)->refresh(list, arguments, length);
    if (counter_shadow) (*counter_shadow)->refresh(list, counter_at, 4);
    if (!indirect::bound(list, (*args)->buffer(), (*args)->gpu(arguments), kind, stride, max)) return false;
    host_pipeline_ = nullptr;
    host_roots_[1] = nullptr;
    bool ran = false;
    if (kind == kDraw || kind == kIndexed) {
        ran = topology_ != kRectangleTopology && prepare(list, kind == kIndexed);
    } else if (kind == kMesh) {
        ran = prepare_for(list, false, true);
    } else {
        const pipeline_host::HostPipeline* pipeline =
            pipeline_bridge::host_pipeline_for(reinterpret_cast<Object*>(pipeline_), true);
        ran = pipeline != nullptr && pipeline->compute && bind(list, *pipeline, true);
    }
    if (!ran) {
        indirect::done(list, (*args)->buffer());
        return false;
    }
    list.list()->ExecuteIndirect(host_signature, max, (*args)->buffer(), (*args)->offset(arguments),
                                 counter_shadow ? (*counter_shadow)->buffer() : nullptr,
                                 counter_shadow ? (*counter_shadow)->offset(counter_at) : 0);
    indirect::done(list, (*args)->buffer());
    if (kind == kDispatch)
        g_dispatched.fetch_add(1, std::memory_order_relaxed);
    else
        g_drawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void viewports(host::HostList& list, std::span<const D3D12_VIEWPORT> viewports) {
    if (viewports.empty()) return;
    list.list()->RSSetViewports(static_cast<UINT>(viewports.size()), viewports.data());
}

void scissors(host::HostList& list, std::span<const RECT> rects) {
    if (rects.empty()) return;
    list.list()->RSSetScissorRects(static_cast<UINT>(rects.size()), rects.data());
}

void blend_factor(host::HostList& list, const float* factor) { list.list()->OMSetBlendFactor(factor); }

void stencil_ref(host::HostList& list, std::uint32_t reference) { list.list()->OMSetStencilRef(reference); }

}  // namespace g6shim::d3d12_xs::draw
