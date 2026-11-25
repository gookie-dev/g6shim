// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "descriptor.hpp"
#include "host.hpp"
#include "object.hpp"
#include "resource.hpp"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace g6shim::d3d12_xs::descriptor_host {

inline constexpr std::uint32_t kPortableViews = 1'000'000;
inline constexpr std::uint32_t kDynamicSlots = 1 << 18;
inline constexpr std::uint32_t kMinDynamicSlots = 1 << 16;

class HostHeap {
public:
    static std::unique_ptr<HostHeap> create(bool sampler, std::uint32_t count);

    std::uint32_t count() const { return count_; }
    std::uint32_t dynamic_slots() const { return dynamic_; }
    bool samplers() const { return sampler_; }
    std::uint32_t step() const { return step_; }
    std::uint64_t gpu_start() const { return gpu_; }
    ID3D12DescriptorHeap* heap() const { return heap_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(std::uint32_t index) const;

    std::optional<std::uint32_t> dynamic(std::uint32_t count);
    void release(std::uint32_t first, std::uint32_t count);
    void make_at(std::uint32_t index, const descriptor::View& view);
    void make_null(std::uint32_t index);
    void constant_at(std::uint32_t index, std::uint64_t gpu, std::uint32_t bytes);
    std::optional<descriptor::View> mirror(std::uint32_t index, std::uint64_t handle, bool for_draw);

private:
    HostHeap(Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap, std::uint64_t cpu, std::uint64_t gpu,
             std::uint32_t step, std::uint32_t count, std::uint32_t dynamic, bool sampler);

    bool fill_null(D3D12_DESCRIPTOR_HEAP_TYPE kind, std::uint32_t total);
    void null_sampler(std::uint32_t index);
    void keep(std::uint32_t index, Microsoft::WRL::ComPtr<ID3D12Resource> resource);

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    std::uint64_t cpu_ = 0;
    std::uint64_t gpu_ = 0;
    std::uint32_t step_ = 0;
    std::uint32_t count_ = 0;
    std::uint32_t dynamic_ = 0;
    bool sampler_ = false;
    std::mutex made_mutex_;
    std::vector<std::uint64_t> made_;
    std::mutex kept_mutex_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> kept_;
    std::mutex next_mutex_;
    std::uint32_t next_ = 0;
    std::vector<bool> taken_;
};

void register_heap(Object* heap, std::uint64_t start, std::uint64_t end);
void drop_heap(Object* heap);
void mirror(std::uint64_t handle);
void release_dynamic(Object* heap, std::uint32_t first, std::uint32_t count);
HostHeap* for_heap(Object* heap);

bool view_is_buffer(const descriptor::View& view);
std::optional<std::pair<std::uint64_t, std::uint64_t>> buffer_range(const descriptor::View& view);
std::optional<std::uint64_t> table_buffer_address(const descriptor::Heap& heap, std::uint64_t table,
                                                  std::uint32_t dword, bool constant_buffer);
std::optional<std::uint32_t> table_buffer_component_mapping(const descriptor::Heap& heap, std::uint64_t table,
                                                            std::uint32_t dword);
std::optional<std::uint32_t> table_image_base_word(const descriptor::Heap& heap, std::uint64_t table,
                                                 std::uint32_t dword);
std::optional<descriptor::View> placed_texture(const descriptor::View& view);
std::string near_heaps(std::uint64_t va);
D3D12_UNORDERED_ACCESS_VIEW_DESC null_uav_desc(std::optional<std::uint32_t> dimension);

}  // namespace g6shim::d3d12_xs::descriptor_host
