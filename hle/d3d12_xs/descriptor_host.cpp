// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "descriptor_host.hpp"

#include "sampler.hpp"
#include "shadow.hpp"
#include "texture.hpp"

#include <g6shim/log.hpp>
#include <g6shim/shader/emit/emit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>

namespace g6shim::d3d12_xs::descriptor_host {

namespace {

constexpr std::uint32_t kBlockSlots = 1 << 16;
constexpr std::uint32_t kBufferDimension = 1;
constexpr std::uint32_t kRawElementBytes = 4;
constexpr std::uint32_t kElementAlignment = 256;
constexpr std::uint32_t kImageBaseAddressShift = 8;
constexpr std::uint32_t kBufferResourceLevel = 1u << 24;
constexpr std::uint32_t kBufferIndexBounds = 1u << 28;
constexpr std::uint32_t kBufferByteBounds = 3u << 28;
constexpr std::uint32_t kBufferIdentitySelectors = 4u | (5u << 3) | (6u << 6) | (7u << 9);
constexpr std::uint32_t kBufferUint32Format = 20u << 12;
constexpr std::uint32_t kDepthUnorderedFlags = 0x7;
constexpr std::uint32_t kTextureViews = 2;
constexpr std::uint32_t kTextureViewsEnd = 10;

static_assert(sizeof(D3D12_SHADER_RESOURCE_VIEW_DESC) == 40);
static_assert(sizeof(D3D12_UNORDERED_ACCESS_VIEW_DESC) == 40);

std::mutex g_ranges_mutex;

struct GpuRange {
    std::uint64_t start;
    std::uint64_t bytes;
    std::uint64_t step;
    bool sampler;
};

std::vector<GpuRange> g_ranges;

void register_range(std::uint64_t start, std::uint64_t bytes, std::uint64_t step, bool sampler) {
    std::lock_guard<std::mutex> lock(g_ranges_mutex);
    g_ranges.push_back(GpuRange{start, bytes, step, sampler});
}

using ImageData = g6shim::shader::emit::ImageHeapData;
using BufferData = g6shim::shader::emit::BufferHeapData;

struct HeapData {
    HeapDataBinding binding;
    std::uint8_t* data;
    std::uint32_t count;
};

struct BufferHeapAlias {
    std::uint32_t index;
    std::shared_ptr<shadow::Shadow> backing;
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
};

struct BufferHeap {
    HeapData data;
    std::map<std::uint32_t, BufferHeapAlias> aliases;
};

struct HeapEntry {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    Object* heap = nullptr;
    std::uint32_t count = 0;
    std::uint32_t step = 0;
    bool sampler = false;
    bool tried = false;
    std::unique_ptr<HostHeap> host;
    std::optional<HeapData> image_data;
    std::optional<BufferHeap> buffer_data;
    std::vector<std::uint32_t> pending;
    std::mutex mutex;
};

std::mutex g_heaps_mutex;
std::vector<std::shared_ptr<HeapEntry>> g_heaps;

std::shared_ptr<HeapEntry> entry_for_handle(std::uint64_t handle) {
    std::lock_guard<std::mutex> lock(g_heaps_mutex);
    for (const auto& entry : g_heaps)
        if (handle >= entry->start && handle < entry->end) return entry;
    return nullptr;
}

std::shared_ptr<HeapEntry> entry_for_heap(Object* heap) {
    std::lock_guard<std::mutex> lock(g_heaps_mutex);
    for (const auto& entry : g_heaps)
        if (entry->heap == heap) return entry;
    return nullptr;
}

void create_null_sampler(D3D12_CPU_DESCRIPTOR_HANDLE at) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    D3D12_SAMPLER_DESC desc{};
    desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    gpu->device()->CreateSampler(&desc, at);
}

D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc(const std::array<std::uint32_t, 10>& words) {
    std::array<std::uint32_t, 10> values = words;
    values[0] = resource::pc_format(values[0]);
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    std::memcpy(&desc, values.data(), sizeof desc);
    return desc;
}

D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc(const std::array<std::uint32_t, 10>& words) {
    std::array<std::uint32_t, 10> values = words;
    values[0] = resource::pc_format(values[0]);
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    std::memcpy(&desc, values.data(), sizeof desc);
    return desc;
}

std::uint64_t element_size(const D3D12_SHADER_RESOURCE_VIEW_DESC& desc) {
    if (desc.Buffer.StructureByteStride > 0) return desc.Buffer.StructureByteStride;
    if ((static_cast<std::uint32_t>(desc.Buffer.Flags) & D3D12_BUFFER_SRV_FLAG_RAW) != 0) return kRawElementBytes;
    const auto element = resource::element(static_cast<std::uint32_t>(desc.Format));
    return element ? element->first : 0;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> unordered_range(std::uint64_t base,
                                                                       const std::array<std::uint32_t, 10>& words) {
    const D3D12_UNORDERED_ACCESS_VIEW_DESC desc = uav_desc(words);
    const auto format = resource::element(static_cast<std::uint32_t>(desc.Format));
    const std::uint64_t element =
        desc.Buffer.StructureByteStride > 0
            ? desc.Buffer.StructureByteStride
            : (static_cast<std::uint32_t>(desc.Buffer.Flags) & D3D12_BUFFER_UAV_FLAG_RAW) != 0
                  ? kRawElementBytes
                  : format ? format->first : 0;
    if (element == 0) return std::nullopt;
    return std::make_pair(base + desc.Buffer.FirstElement * element,
                          static_cast<std::uint64_t>(desc.Buffer.NumElements) * element);
}

std::optional<Microsoft::WRL::ComPtr<ID3D12Resource>> buffer_srv(std::uint64_t address, std::uint64_t bytes,
                                                                 D3D12_SHADER_RESOURCE_VIEW_DESC desc,
                                                                 D3D12_CPU_DESCRIPTOR_HANDLE at) {
    const auto shadow = shadow::find(address, bytes);
    if (!shadow) return std::nullopt;
    const std::uint64_t element = element_size(desc);
    const std::uint64_t offset = (*shadow)->offset(address);
    if (element == 0 || offset % element != 0) {
        G6SHIM_WARN("d3d12_xs: buffer view not on an element of its host copy (offset %llu, element %llu)",
                    static_cast<unsigned long long>(offset), static_cast<unsigned long long>(element));
        return std::nullopt;
    }
    desc.Buffer.FirstElement += static_cast<UINT>(offset / element);
    host::Host* gpu = host::host();
    if (gpu == nullptr) return std::nullopt;
    gpu->device()->CreateShaderResourceView((*shadow)->buffer(), &desc, at);
    return Microsoft::WRL::ComPtr<ID3D12Resource>((*shadow)->buffer());
}

std::optional<Microsoft::WRL::ComPtr<ID3D12Resource>> buffer_uav(std::uint64_t address, std::uint64_t bytes,
                                                                 D3D12_UNORDERED_ACCESS_VIEW_DESC desc,
                                                                 D3D12_CPU_DESCRIPTOR_HANDLE at) {
    const auto shadow = shadow::find(address, bytes);
    if (!shadow) return std::nullopt;
    if ((static_cast<std::uint32_t>((*shadow)->buffer()->GetDesc().Flags) &
         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        G6SHIM_WARN("d3d12_xs: unordered access to a buffer whose host copy shaders cannot write");
        return std::nullopt;
    }
    const auto format = resource::element(static_cast<std::uint32_t>(desc.Format));
    const std::uint64_t element =
        desc.Buffer.StructureByteStride > 0
            ? desc.Buffer.StructureByteStride
            : (static_cast<std::uint32_t>(desc.Buffer.Flags) & D3D12_BUFFER_UAV_FLAG_RAW) != 0
                  ? kRawElementBytes
                  : format ? format->first : 0;
    const std::uint64_t offset = (*shadow)->offset(address);
    if (element == 0 || offset % element != 0) {
        G6SHIM_WARN("d3d12_xs: unordered buffer view not on an element of its host copy");
        return std::nullopt;
    }
    desc.Buffer.FirstElement += static_cast<UINT>(offset / element);
    desc.Buffer.CounterOffsetInBytes = 0;
    host::Host* gpu = host::host();
    if (gpu == nullptr) return std::nullopt;
    gpu->device()->CreateUnorderedAccessView((*shadow)->buffer(), nullptr, &desc, at);
    return Microsoft::WRL::ComPtr<ID3D12Resource>((*shadow)->buffer());
}

bool fits(ID3D12Resource* texture, std::uint32_t* format, bool unordered) {
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    const std::uint32_t flags = static_cast<std::uint32_t>(desc.Flags);
    const bool allowed = unordered ? (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0
                                   : (flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;
    const std::uint32_t own = static_cast<std::uint32_t>(desc.Format);
    const auto depth = std::find_if(resource::kDepthFormats.begin(), resource::kDepthFormats.end(),
                                    [own](const auto& pair) { return pair.first == own; });
    if (!allowed || depth != resource::kDepthFormats.end()) {
        G6SHIM_WARN("d3d12_xs: view the host does not allow (format %u, flags 0x%x)", own, flags);
        return false;
    }
    const bool typeless = resource::family(own) == own && resource::family(own + 1) == own;
    if (format != nullptr && unordered) {
        const auto srgb = std::find_if(resource::kSrgbFormats.begin(), resource::kSrgbFormats.end(),
                                       [format](const auto& pair) { return pair.first == *format; });
        if (srgb != resource::kSrgbFormats.end()) *format = srgb->second;
    }
    const std::uint32_t view = format != nullptr ? *format : 0;
    if (view == 0) return !typeless;
    if (resource::family(view) != resource::family(own)) {
        G6SHIM_WARN("d3d12_xs: view format %u of another family than %u", view, own);
        return false;
    }
    if (format != nullptr && !typeless) *format = own;
    return true;
}

void null_view(D3D12_CPU_DESCRIPTOR_HANDLE at) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;
    gpu->device()->CreateShaderResourceView(nullptr, &desc, at);
}

void null_buffer_view(D3D12_CPU_DESCRIPTOR_HANDLE at) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.NumElements = 1;
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    gpu->device()->CreateShaderResourceView(nullptr, &desc, at);
}

void null_like(const std::array<std::uint32_t, 10>& words, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    if (words[1] < kTextureViews || words[1] > kTextureViewsEnd) {
        null_view(at);
        return;
    }
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    std::array<std::uint32_t, 10> values{};
    values[1] = words[1];
    switch (words[1]) {
    case 6: break;
    case 7: values[5] = 1; break;
    case 3:
    case 5:
    case 10: values[5] = 1; values[7] = 1; break;
    default: values[5] = 1; break;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = srv_desc(values);
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    gpu->device()->CreateShaderResourceView(nullptr, &desc, at);
}

std::optional<Microsoft::WRL::ComPtr<ID3D12Resource>> make(const descriptor::View& view,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE at);

void null_for(const descriptor::View& view, D3D12_CPU_DESCRIPTOR_HANDLE at) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    bool buffer = false;
    std::optional<std::uint32_t> dimension;
    if (const auto* constant = std::get_if<descriptor::ConstantView>(&view)) {
        (void)constant;
        D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
        gpu->device()->CreateConstantBufferView(&desc, at);
        return;
    }
    if (const auto* shader = std::get_if<descriptor::ShaderView>(&view)) {
        const bool is_buffer =
            resource::buffer_bytes(reinterpret_cast<Object*>(shader->resource)) != std::nullopt;
        if (is_buffer || (shader->desc && (*shader->desc)[1] == kBufferDimension)) {
            null_buffer_view(at);
            return;
        }
        if (shader->desc) {
            null_like(*shader->desc, at);
            return;
        }
        null_view(at);
        return;
    }
    if (const auto* unordered = std::get_if<descriptor::UnorderedView>(&view)) {
        buffer = resource::buffer_bytes(reinterpret_cast<Object*>(unordered->resource)) != std::nullopt ||
                 (unordered->desc && (*unordered->desc)[1] == kBufferDimension);
        if (unordered->desc) dimension = (*unordered->desc)[1];
    } else if (const auto* placed = std::get_if<descriptor::PlacedView>(&view)) {
        if (!placed->unordered && placed->resource[0] == kBufferDimension) {
            null_buffer_view(at);
            return;
        }
        if (!placed->unordered) {
            std::array<std::uint32_t, 10> words{};
            std::copy_n(placed->desc.begin(), 10, words.begin());
            null_like(words, at);
            return;
        }
        buffer = placed->resource[0] == kBufferDimension;
        dimension = placed->desc[1];
    } else {
        null_view(at);
        return;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    if (buffer) {
        desc.Format = DXGI_FORMAT_R32_UINT;
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    } else {
        desc = null_uav_desc(dimension);
    }
    gpu->device()->CreateUnorderedAccessView(nullptr, nullptr, &desc, at);
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> srv_range(std::uint64_t base,
                                                                 const std::array<std::uint32_t, 10>& words) {
    const D3D12_SHADER_RESOURCE_VIEW_DESC desc = srv_desc(words);
    if (desc.ViewDimension != D3D12_SRV_DIMENSION_BUFFER) return std::nullopt;
    const std::uint64_t element = element_size(desc);
    if (element == 0 || desc.Buffer.FirstElement > (std::numeric_limits<std::uint64_t>::max() - base) / element)
        return std::nullopt;
    return std::make_pair(base + desc.Buffer.FirstElement * element,
                          static_cast<std::uint64_t>(desc.Buffer.NumElements) * element);
}

std::uint64_t placed_address(const descriptor::PlacedView& view) {
    constexpr std::size_t kAddressLow = 10;
    constexpr std::size_t kAddressHigh = 11;
    return std::uint64_t{view.desc[kAddressLow]} | std::uint64_t{view.desc[kAddressHigh]} << 32;
}

std::optional<descriptor::View> table_view(const descriptor::Heap& heap, std::uint64_t table, std::uint32_t dword,
                                          std::uint32_t kind = descriptor::kViewsKind) {
    if (heap.desc.kind != kind || !heap.index(table))
        return std::nullopt;
    const std::uint32_t step = descriptor::increment_size(nullptr, heap.desc.kind);
    const std::uint64_t offset = sizeof(std::uint32_t) * std::uint64_t{dword};
    if ((table - heap.start) % step != 0 || offset % step != 0 ||
        table > std::numeric_limits<std::uint64_t>::max() - offset || !heap.index(table + offset))
        return std::nullopt;
    return descriptor::view(table + offset);
}

std::optional<std::uint64_t> image_address(const descriptor::View& view) {
    if (const auto* placed = std::get_if<descriptor::PlacedView>(&view)) {
        if (placed->resource[0] < D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
            placed->resource[0] > D3D12_RESOURCE_DIMENSION_TEXTURE3D)
            return std::nullopt;
        return placed_address(*placed);
    }
    std::uintptr_t address = 0;
    if (const auto* srv = std::get_if<descriptor::ShaderView>(&view)) {
        if (srv->desc && srv_desc(*srv->desc).ViewDimension <= D3D12_SRV_DIMENSION_BUFFER)
            return std::nullopt;
        address = srv->resource;
    } else if (const auto* uav = std::get_if<descriptor::UnorderedView>(&view)) {
        if (uav->desc && uav_desc(*uav->desc).ViewDimension <= D3D12_UAV_DIMENSION_BUFFER)
            return std::nullopt;
        address = uav->resource;
    } else {
        return std::nullopt;
    }
    if (address == 0) return 0;
    const auto texture = resource::texture_memory(reinterpret_cast<Object*>(address));
    if (!texture || texture->first == nullptr) return std::nullopt;
    return reinterpret_cast<std::uint64_t>(texture->first);
}

std::optional<Microsoft::WRL::ComPtr<ID3D12Resource>> make(const descriptor::View& view,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE at) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return std::nullopt;
    if (const auto* constant = std::get_if<descriptor::ConstantView>(&view)) {
        const auto shadow = shadow::find(constant->address, constant->size);
        if (!shadow) return std::nullopt;
        const std::uint64_t address = (*shadow)->gpu(constant->address);
        if (address % kElementAlignment != 0) {
            G6SHIM_WARN("d3d12_xs: constant buffer not 256-byte aligned on the host");
            return std::nullopt;
        }
        D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
        desc.BufferLocation = address;
        desc.SizeInBytes = (constant->size + 255) / 256 * 256;
        gpu->device()->CreateConstantBufferView(&desc, at);
        return Microsoft::WRL::ComPtr<ID3D12Resource>((*shadow)->buffer());
    }
    if (const auto* shader = std::get_if<descriptor::ShaderView>(&view)) {
        Object* object = reinterpret_cast<Object*>(shader->resource);
        ID3D12Resource* texture = texture::host_texture(object);
        if (texture != nullptr) {
            std::optional<D3D12_SHADER_RESOURCE_VIEW_DESC> desc;
            if (shader->desc) desc = srv_desc(*shader->desc);
            std::uint32_t format = desc ? static_cast<std::uint32_t>(desc->Format) : 0;
            if (!fits(texture, desc ? &format : nullptr, false)) return std::nullopt;
            if (desc) desc->Format = static_cast<DXGI_FORMAT>(format);
            gpu->device()->CreateShaderResourceView(texture, desc ? &*desc : nullptr, at);
            return Microsoft::WRL::ComPtr<ID3D12Resource>(texture);
        }
        const auto bytes = resource::buffer_bytes(object);
        if (bytes && shader->desc)
            return buffer_srv(reinterpret_cast<std::uint64_t>(bytes->first), bytes->second,
                              srv_desc(*shader->desc), at);
        return std::nullopt;
    }
    if (const auto* unordered = std::get_if<descriptor::UnorderedView>(&view)) {
        Object* object = reinterpret_cast<Object*>(unordered->resource);
        ID3D12Resource* texture = texture::host_texture(object);
        if (texture == nullptr) {
            const auto bytes = resource::buffer_bytes(object);
            if (bytes && unordered->desc)
                return buffer_uav(reinterpret_cast<std::uint64_t>(bytes->first), bytes->second,
                                  uav_desc(*unordered->desc), at);
            return std::nullopt;
        }
        std::optional<D3D12_UNORDERED_ACCESS_VIEW_DESC> desc;
        if (unordered->desc) desc = uav_desc(*unordered->desc);
        std::uint32_t format = desc ? static_cast<std::uint32_t>(desc->Format) : 0;
        if (!fits(texture, desc ? &format : nullptr, true)) return std::nullopt;
        if (desc) desc->Format = static_cast<DXGI_FORMAT>(format);
        gpu->device()->CreateUnorderedAccessView(texture, nullptr, desc ? &*desc : nullptr, at);
        return Microsoft::WRL::ComPtr<ID3D12Resource>(texture);
    }
    if (const auto* sampler_view = std::get_if<descriptor::SamplerView>(&view)) {
        D3D12_SAMPLER_DESC desc{};
        std::memcpy(&desc, sampler_view->desc.data(), sizeof desc);
        gpu->device()->CreateSampler(&desc, at);
        return Microsoft::WRL::ComPtr<ID3D12Resource>();
    }
    if (const auto* console = std::get_if<descriptor::ConsoleSamplerView>(&view)) {
        const D3D12_SAMPLER_DESC desc = sampler::console(console->desc);
        gpu->device()->CreateSampler(&desc, at);
        return Microsoft::WRL::ComPtr<ID3D12Resource>();
    }
    const auto* placed = std::get_if<descriptor::PlacedView>(&view);
    if (placed == nullptr) return std::nullopt;
    if (const auto texture = placed_texture(view)) return make(*texture, at);
    if (placed->resource[0] != kBufferDimension) {
        G6SHIM_WARN("d3d12_xs: placed unordered texture view not mirrored (dimension %u)", placed->resource[0]);
        return std::nullopt;
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(placed->resource[4]) |
                                static_cast<std::uint64_t>(placed->resource[5]) << 32;
    const std::uint64_t address = placed_address(*placed);
    std::array<std::uint32_t, 10> words{};
    std::copy_n(placed->desc.begin(), 10, words.begin());
    if (placed->unordered) return buffer_uav(address, bytes, uav_desc(words), at);
    return buffer_srv(address, bytes, srv_desc(words), at);
}

std::mutex g_placed_mutex;
std::map<std::pair<std::uint64_t, std::array<std::uint32_t, 14>>, Object*> g_placed;

}  // namespace

D3D12_UNORDERED_ACCESS_VIEW_DESC null_uav_desc(std::optional<std::uint32_t> dimension) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    if (!dimension) return desc;
    switch (*dimension) {
    case 2: desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D; break;
    case 3:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
        desc.Texture1DArray.ArraySize = 1;
        break;
    case 5:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.ArraySize = 1;
        break;
    case 8:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        desc.Texture3D.WSize = std::numeric_limits<UINT>::max();
        break;
    default: break;
    }
    return desc;
}

HostHeap::HostHeap(Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap, std::uint64_t cpu, std::uint64_t gpu,
                   std::uint32_t step, std::uint32_t count, std::uint32_t dynamic, bool sampler)
    : heap_(std::move(heap)),
      cpu_(cpu),
      gpu_(gpu),
      step_(step),
      count_(count),
      dynamic_(dynamic),
      sampler_(sampler),
      made_(count, 0),
      kept_(static_cast<std::size_t>(count) + dynamic),
      taken_(dynamic, false) {}

std::unique_ptr<HostHeap> HostHeap::create(bool sampler, std::uint32_t count) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return nullptr;
    const D3D12_DESCRIPTOR_HEAP_TYPE kind =
        sampler ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    const auto make_heap = [gpu, kind](std::uint32_t slots) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = kind;
        desc.NumDescriptors = slots;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        if (FAILED(gpu->device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)))) return heap;
        return heap;
    };
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    std::uint32_t dynamic = 0;
    if (sampler) {
        heap = make_heap(count);
        if (!heap) {
            G6SHIM_WARN("d3d12_xs: no host sampler heap of %u descriptors", count);
            return nullptr;
        }
    } else {
        count = std::min(count, std::numeric_limits<std::uint32_t>::max() - kDynamicSlots);
        heap = make_heap(count + kDynamicSlots);
        if (heap) {
            dynamic = kDynamicSlots;
        } else if ((heap = make_heap(count + kMinDynamicSlots)) != nullptr) {
            dynamic = kMinDynamicSlots;
        } else if (count + kMinDynamicSlots > kPortableViews) {
            G6SHIM_WARN("d3d12_xs: host view heap capped at %u descriptors", kPortableViews);
            heap = make_heap(kPortableViews);
            count = kPortableViews - kMinDynamicSlots;
            dynamic = kMinDynamicSlots;
        } else {
            G6SHIM_WARN("d3d12_xs: no host view heap of %u descriptors", count + kMinDynamicSlots);
            return nullptr;
        }
        if (!heap) {
            G6SHIM_WARN("d3d12_xs: host view heap capped at %u descriptors", kPortableViews);
            return nullptr;
        }
    }
    const std::uint64_t cpu = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    const std::uint64_t address = heap->GetGPUDescriptorHandleForHeapStart().ptr;
    const std::uint32_t step = gpu->device()->GetDescriptorHandleIncrementSize(kind);
    register_range(address, static_cast<std::uint64_t>(count + dynamic) * step, step, sampler);
    std::unique_ptr<HostHeap> made(new HostHeap(std::move(heap), cpu, address, step, count, dynamic, sampler));
    if (!made->fill_null(kind, count + dynamic))
        G6SHIM_WARN("d3d12_xs: host descriptor heap not made null");
    return made;
}

bool HostHeap::fill_null(D3D12_DESCRIPTOR_HEAP_TYPE kind, std::uint32_t total) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return true;
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = kind;
    desc.NumDescriptors = kBlockSlots;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> block;
    if (FAILED(gpu->device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&block)))) return false;
    const D3D12_CPU_DESCRIPTOR_HANDLE start = block->GetCPUDescriptorHandleForHeapStart();
    const auto at = [start, this](std::uint32_t index) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = start;
        handle.ptr += static_cast<SIZE_T>(index) * step_;
        return handle;
    };
    if (sampler_)
        create_null_sampler(start);
    else
        null_view(start);
    std::uint32_t filled = 1;
    while (filled < kBlockSlots) {
        const std::uint32_t amount = std::min(filled, kBlockSlots - filled);
        gpu->device()->CopyDescriptorsSimple(amount, at(filled), start, kind);
        filled += amount;
    }
    std::uint32_t done = 0;
    while (done < total) {
        const std::uint32_t amount = std::min(kBlockSlots, total - done);
        gpu->device()->CopyDescriptorsSimple(amount, cpu(done), start, kind);
        done += amount;
    }
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE HostHeap::cpu(std::uint32_t index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = cpu_ + static_cast<SIZE_T>(index) * step_;
    return handle;
}

void HostHeap::null_sampler(std::uint32_t index) { create_null_sampler(cpu(index)); }

void HostHeap::keep(std::uint32_t index, Microsoft::WRL::ComPtr<ID3D12Resource> resource) {
    std::lock_guard<std::mutex> lock(kept_mutex_);
    if (index < kept_.size()) kept_[index] = std::move(resource);
}

std::optional<std::uint32_t> HostHeap::dynamic(std::uint32_t count) {
    if (sampler_ || count == 0 || count > dynamic_) return std::nullopt;
    std::lock_guard<std::mutex> lock(next_mutex_);
    const std::size_t needed = count;
    std::optional<std::uint32_t> found;
    std::size_t run = 0;
    std::size_t last = std::numeric_limits<std::size_t>::max();
    for (std::size_t step = 0; step < dynamic_; ++step) {
        const std::size_t at = (static_cast<std::size_t>(next_) + step) % dynamic_;
        if (at != last + 1) run = 0;
        last = at;
        run = taken_[at] ? 0 : run + 1;
        if (run == needed) {
            found = static_cast<std::uint32_t>(at + 1 - needed);
            break;
        }
    }
    if (!found) {
        G6SHIM_WARN("d3d12_xs: no free dynamic descriptor slots for %u", count);
        return std::nullopt;
    }
    for (std::size_t i = 0; i < needed; ++i) taken_[*found + i] = true;
    next_ = (*found + count) % dynamic_;
    return count_ + *found;
}

void HostHeap::release(std::uint32_t first, std::uint32_t count) {
    if (first < count_) return;
    const std::uint32_t at = first - count_;
    std::lock_guard<std::mutex> lock(next_mutex_);
    const auto end = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(at) + count, dynamic_));
    for (std::uint32_t slot = at; slot < end; ++slot) taken_[slot] = false;
}

void HostHeap::make_at(std::uint32_t index, const descriptor::View& view) {
    auto kept = make(view, cpu(index));
    if (!kept) null_for(view, cpu(index));
    keep(index, kept ? *kept : Microsoft::WRL::ComPtr<ID3D12Resource>());
}

void HostHeap::make_null(std::uint32_t index) {
    if (sampler_)
        null_sampler(index);
    else
        null_view(cpu(index));
    keep(index, Microsoft::WRL::ComPtr<ID3D12Resource>());
}

void HostHeap::constant_at(std::uint32_t index, std::uint64_t gpu, std::uint32_t bytes) {
    keep(index, Microsoft::WRL::ComPtr<ID3D12Resource>());
    host::Host* device = host::host();
    if (device == nullptr) return;
    D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
    desc.BufferLocation = gpu;
    desc.SizeInBytes = (bytes + 255) / 256 * 256;
    device->device()->CreateConstantBufferView(&desc, cpu(index));
}

std::optional<descriptor::View> HostHeap::mirror(std::uint32_t index, std::uint64_t handle, bool for_draw) {
    if (index >= count_) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true, std::memory_order_relaxed))
            G6SHIM_WARN("d3d12_xs: title descriptor slot %u past the host heap of %u", index, count_);
        return std::nullopt;
    }
    std::array<std::uint32_t, 3> record{};
    std::memcpy(record.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(handle)),
                sizeof record);
    const std::uint64_t key = static_cast<std::uint64_t>(record[2]) << 32 | record[1];
    std::lock_guard<std::mutex> lock(made_mutex_);
    std::optional<descriptor::View> view = descriptor::view(handle);
    if (!view) {
        if (made_[index] != 0) {
            made_[index] = 0;
            make_null(index);
        }
        return std::nullopt;
    }
    if (made_[index] != key) {
        const bool later = !for_draw && view_is_buffer(*view);
        made_[index] = later ? 0 : key;
        std::optional<Microsoft::WRL::ComPtr<ID3D12Resource>> kept =
            later ? std::optional<Microsoft::WRL::ComPtr<ID3D12Resource>>(Microsoft::WRL::ComPtr<ID3D12Resource>())
                  : make(*view, cpu(index));
        if (!kept && !sampler_) null_for(*view, cpu(index));
        keep(index, kept ? *kept : Microsoft::WRL::ComPtr<ID3D12Resource>());
    }
    return view;
}

bool view_is_buffer(const descriptor::View& view) {
    if (std::holds_alternative<descriptor::ConstantView>(view)) return true;
    if (std::holds_alternative<descriptor::PlacedView>(view)) return true;
    if (const auto* shader = std::get_if<descriptor::ShaderView>(&view))
        return resource::buffer_bytes(reinterpret_cast<Object*>(shader->resource)) != std::nullopt;
    if (const auto* unordered = std::get_if<descriptor::UnorderedView>(&view))
        return resource::buffer_bytes(reinterpret_cast<Object*>(unordered->resource)) != std::nullopt;
    return false;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> buffer_range(const descriptor::View& view) {
    if (const auto* shader = std::get_if<descriptor::ShaderView>(&view)) {
        if (!shader->desc) return std::nullopt;
        const auto bytes = resource::buffer_bytes(reinterpret_cast<Object*>(shader->resource));
        if (!bytes) return std::nullopt;
        return srv_range(reinterpret_cast<std::uint64_t>(bytes->first), *shader->desc);
    }
    if (const auto* placed = std::get_if<descriptor::PlacedView>(&view)) {
        if (placed->resource[0] != kBufferDimension) return std::nullopt;
        const std::uint64_t base = placed_address(*placed);
        std::array<std::uint32_t, 10> words{};
        std::copy_n(placed->desc.begin(), 10, words.begin());
        if (placed->unordered) return unordered_range(base, words);
        return srv_range(base, words);
    }
    if (const auto* unordered = std::get_if<descriptor::UnorderedView>(&view)) {
        if (!unordered->desc) return std::nullopt;
        const auto bytes = resource::buffer_bytes(reinterpret_cast<Object*>(unordered->resource));
        if (!bytes) return std::nullopt;
        return unordered_range(reinterpret_cast<std::uint64_t>(bytes->first), *unordered->desc);
    }
    return std::nullopt;
}

namespace {

std::optional<std::uint64_t> shader_buffer_address(const descriptor::View& view) {
    if (const auto* srv = std::get_if<descriptor::ShaderView>(&view)) {
        if (srv->resource == 0) {
            if (!srv->desc || srv_desc(*srv->desc).ViewDimension == D3D12_SRV_DIMENSION_BUFFER) return 0;
            return std::nullopt;
        }
        if (!srv->desc) {
            const auto bytes = resource::buffer_bytes(reinterpret_cast<Object*>(srv->resource));
            if (!bytes) return std::nullopt;
            return reinterpret_cast<std::uint64_t>(bytes->first);
        }
    } else {
        const auto* placed = std::get_if<descriptor::PlacedView>(&view);
        if (placed == nullptr || placed->unordered) return std::nullopt;
    }
    const auto range = buffer_range(view);
    if (!range) return std::nullopt;
    return range->first;
}

std::optional<D3D12_SHADER_RESOURCE_VIEW_DESC> shader_view_desc(const descriptor::View& view) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    if (const auto* srv = std::get_if<descriptor::ShaderView>(&view)) {
        if (!srv->desc) return std::nullopt;
        desc = srv_desc(*srv->desc);
    } else {
        const auto* placed = std::get_if<descriptor::PlacedView>(&view);
        if (placed == nullptr || placed->unordered) return std::nullopt;
        std::array<std::uint32_t, 10> words{};
        std::copy_n(placed->desc.begin(), words.size(), words.begin());
        desc = srv_desc(words);
    }
    return desc;
}

std::optional<D3D12_SHADER_RESOURCE_VIEW_DESC> shader_buffer_desc(const descriptor::View& view) {
    const auto* placed = std::get_if<descriptor::PlacedView>(&view);
    if (placed != nullptr && placed->resource[0] != kBufferDimension) return std::nullopt;
    const auto desc = shader_view_desc(view);
    if (!desc || desc->ViewDimension != D3D12_SRV_DIMENSION_BUFFER) return std::nullopt;
    return desc;
}

bool null_shader_buffer(const descriptor::View& view) {
    const auto* srv = std::get_if<descriptor::ShaderView>(&view);
    return srv != nullptr && srv->resource == 0 && (!srv->desc || shader_buffer_desc(view));
}

std::optional<std::uint32_t> structured_buffer_stride(const D3D12_SHADER_RESOURCE_VIEW_DESC& desc) {
    if (desc.Format != DXGI_FORMAT_UNKNOWN || desc.Buffer.Flags != D3D12_BUFFER_SRV_FLAG_NONE)
        return std::nullopt;
    const std::uint32_t stride = desc.Buffer.StructureByteStride;
    if (stride < 4 || stride % 4 != 0 || stride > g6shim::shader::emit::UserData::kBufferStrideMask)
        return std::nullopt;
    return stride;
}

bool linear_raw_buffer(const D3D12_SHADER_RESOURCE_VIEW_DESC& desc) {
    return desc.Format == DXGI_FORMAT_R32_TYPELESS && desc.Buffer.Flags == D3D12_BUFFER_SRV_FLAG_RAW &&
           desc.Buffer.StructureByteStride == 0;
}

struct TypedBufferFormat {
    DXGI_FORMAT dxgi;
    std::uint32_t gfx10;
    std::uint32_t components;
};

constexpr TypedBufferFormat kTypedBufferFormats[] = {
    {DXGI_FORMAT_R32_UINT, 20, 1}, {DXGI_FORMAT_R32_SINT, 21, 1}, {DXGI_FORMAT_R32_FLOAT, 22, 1},
    {DXGI_FORMAT_R16G16_UNORM, 23, 2}, {DXGI_FORMAT_R16G16_SNORM, 24, 2}, {DXGI_FORMAT_R16G16_UINT, 27, 2},
    {DXGI_FORMAT_R16G16_SINT, 28, 2}, {DXGI_FORMAT_R16G16_FLOAT, 29, 2},
    {DXGI_FORMAT_R8G8B8A8_UNORM, 56, 4}, {DXGI_FORMAT_R8G8B8A8_SNORM, 57, 4},
    {DXGI_FORMAT_R8G8B8A8_UINT, 60, 4}, {DXGI_FORMAT_R8G8B8A8_SINT, 61, 4},
};

const TypedBufferFormat* typed_buffer_format(const D3D12_SHADER_RESOURCE_VIEW_DESC& desc) {
    if (desc.Buffer.StructureByteStride != 0 || desc.Buffer.Flags != D3D12_BUFFER_SRV_FLAG_NONE)
        return nullptr;
    const auto found = std::find_if(std::begin(kTypedBufferFormats), std::end(kTypedBufferFormats),
                                    [&](const TypedBufferFormat& format) { return format.dxgi == desc.Format; });
    return found != std::end(kTypedBufferFormats) ? found : nullptr;
}

std::optional<std::uint32_t> buffer_component_mapping(std::uint32_t mapping, std::uint32_t components) {
    constexpr std::uint32_t kMappingMark = D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
    if (mapping / kMappingMark != 1) return std::nullopt;
    constexpr std::array<std::uint32_t, 6> kGfx10Selectors{4, 5, 6, 7, 0, 1};
    constexpr std::uint32_t kSelectorBits = 3;
    std::uint32_t word = 0;
    for (std::uint32_t i = 0; i < 4; ++i) {
        const auto index = static_cast<std::size_t>(D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(i, mapping));
        if (index >= kGfx10Selectors.size()) return std::nullopt;
        const std::uint32_t selector = index >= components && index < 4 ? (index == 3 ? 1u : 0u)
                                                                     : kGfx10Selectors[index];
        word |= selector << (i * kSelectorBits);
    }
    return word;
}

}  // namespace

std::optional<std::uint64_t> table_buffer_address(const descriptor::Heap& heap, std::uint64_t table,
                                                  std::uint32_t dword, bool constant_buffer) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    if (!constant_buffer) return shader_buffer_address(*view);
    const auto* buffer = std::get_if<descriptor::ConstantView>(&*view);
    if (buffer == nullptr) return std::nullopt;
    return buffer->address;
}

std::optional<std::array<std::uint32_t, 3>> table_constant_buffer_words(const descriptor::Heap& heap,
                                                                       std::uint64_t table, std::uint32_t dword) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    const auto* buffer = std::get_if<descriptor::ConstantView>(&*view);
    if (buffer == nullptr) return std::nullopt;
    if (buffer->address == 0 && buffer->size == 0) return std::array<std::uint32_t, 3>{};
    constexpr std::uint32_t kRecordBytes = 4 * sizeof(std::uint32_t);
    if (buffer->address == 0 || buffer->size == 0 || buffer->size % kRecordBytes != 0 ||
        buffer->size / kRecordBytes > D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT)
        return std::nullopt;

    using UserData = g6shim::shader::emit::UserData;
    const std::uint32_t high = static_cast<std::uint32_t>(buffer->address >> 32) & UserData::kBufferAddressHighMask;
    return std::array{high | (kRecordBytes << UserData::kBufferStrideShift), buffer->size / kRecordBytes,
                      kBufferIdentitySelectors | kBufferUint32Format | kBufferResourceLevel | kBufferIndexBounds};
}

std::optional<std::uint32_t> table_structured_buffer_high_word(const descriptor::Heap& heap, std::uint64_t table,
                                                              std::uint32_t dword) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    if (null_shader_buffer(*view)) return 0;
    const auto desc = shader_buffer_desc(*view);
    if (!desc) return std::nullopt;
    const auto stride = structured_buffer_stride(*desc);
    if (!stride) return std::nullopt;
    using UserData = g6shim::shader::emit::UserData;
    const auto address = shader_buffer_address(*view);
    if (!address) return std::nullopt;
    const std::uint32_t high = static_cast<std::uint32_t>(*address >> 32) & UserData::kBufferAddressHighMask;
    return (*stride << UserData::kBufferStrideShift) | high;
}

std::optional<std::uint32_t> table_buffer_record_count(const descriptor::Heap& heap, std::uint64_t table,
                                                       std::uint32_t dword) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    if (null_shader_buffer(*view)) return 0;
    const auto desc = shader_buffer_desc(*view);
    if (!desc) return std::nullopt;
    const auto address = shader_buffer_address(*view);
    if (!address) return std::nullopt;
    if (linear_raw_buffer(*desc)) {
        if (*address % D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT != 0 ||
            desc->Buffer.NumElements > UINT32_MAX / kRawElementBytes)
            return std::nullopt;
        return desc->Buffer.NumElements * kRawElementBytes;
    }
    if (structured_buffer_stride(*desc) || typed_buffer_format(*desc) != nullptr)
        return desc->Buffer.NumElements;
    return std::nullopt;
}

std::optional<std::uint32_t> table_linear_raw_buffer_high_word(const descriptor::Heap& heap, std::uint64_t table,
                                                              std::uint32_t dword) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    if (null_shader_buffer(*view)) return 0;
    const auto desc = shader_buffer_desc(*view);
    if (!desc || !linear_raw_buffer(*desc)) return std::nullopt;
    const auto address = shader_buffer_address(*view);
    if (!address || *address % D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT != 0) return std::nullopt;
    return static_cast<std::uint32_t>(*address >> 32) & g6shim::shader::emit::UserData::kBufferAddressHighMask;
}

std::optional<std::uint32_t> table_untyped_buffer_format_word(const descriptor::Heap& heap, std::uint64_t table,
                                                              std::uint32_t dword, bool structured) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    if (null_shader_buffer(*view)) return 0;
    const auto desc = shader_buffer_desc(*view);
    if (!desc || desc->Shader4ComponentMapping != D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING) return std::nullopt;
    if (structured ? !structured_buffer_stride(*desc) : !linear_raw_buffer(*desc)) return std::nullopt;
    const auto address = shader_buffer_address(*view);
    if (!address || (!structured && *address % D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT != 0)) return std::nullopt;
    return kBufferIdentitySelectors | kBufferUint32Format | kBufferResourceLevel |
           (structured ? kBufferIndexBounds : kBufferByteBounds);
}

std::optional<std::uint32_t> table_typed_buffer_word(const descriptor::Heap& heap, std::uint64_t table,
                                                     std::uint32_t dword, bool high_address) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    if (null_shader_buffer(*view)) return 0;
    const auto desc = shader_buffer_desc(*view);
    if (!desc) return std::nullopt;
    const auto* found = typed_buffer_format(*desc);
    if (found == nullptr) return std::nullopt;
    const auto address = shader_buffer_address(*view);
    if (!address) return std::nullopt;
    if (high_address) {
        using UserData = g6shim::shader::emit::UserData;
        constexpr std::uint32_t kTypedElementBytes = 4;
        const std::uint32_t high = static_cast<std::uint32_t>(*address >> 32) & UserData::kBufferAddressHighMask;
        return (kTypedElementBytes << UserData::kBufferStrideShift) | high;
    }
    const auto mapping = buffer_component_mapping(desc->Shader4ComponentMapping, found->components);
    if (!mapping) return std::nullopt;
    constexpr std::uint32_t kDataFormatShift = 12;
    return *mapping | (found->gfx10 << kDataFormatShift) | kBufferResourceLevel | kBufferIndexBounds;
}

std::optional<std::uint32_t> table_buffer_component_mapping(const descriptor::Heap& heap, std::uint64_t table,
                                                            std::uint32_t dword) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    if (null_shader_buffer(*view)) return 0;
    const auto desc = shader_buffer_desc(*view);
    if (!desc) return std::nullopt;
    return buffer_component_mapping(desc->Shader4ComponentMapping, 4);
}

std::optional<std::uint32_t> table_image_base_word(const descriptor::Heap& heap, std::uint64_t table,
                                                 std::uint32_t dword) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    const auto address = image_address(*view);
    constexpr std::uint64_t kImageAlignment = 1ull << kImageBaseAddressShift;
    if (!address || *address % kImageAlignment != 0) return std::nullopt;
    return static_cast<std::uint32_t>(*address >> kImageBaseAddressShift);
}

namespace {

std::optional<ImageShapeWords> image_shape(const descriptor::View& view) {
    const auto desc = shader_view_desc(view);
    if (!desc || (desc->ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D &&
                  desc->ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2DARRAY))
        return std::nullopt;

    resource::ResourceDesc resource_desc{};
    if (const auto* placed = std::get_if<descriptor::PlacedView>(&view)) {
        constexpr std::uint64_t kAlignment = 1ull << kImageBaseAddressShift;
        const std::uint64_t address = placed_address(*placed);
        if (address == 0 || address % kAlignment != 0) return std::nullopt;
        std::memcpy(&resource_desc, placed->resource.data(), sizeof resource_desc);
    } else {
        const auto* srv = std::get_if<descriptor::ShaderView>(&view);
        if (srv == nullptr) return std::nullopt;
        if (srv->resource == 0) return ImageShapeWords{};
        const auto memory = resource::texture_memory(reinterpret_cast<Object*>(srv->resource));
        if (!memory) return std::nullopt;
        resource_desc = memory->second;
    }

    if (resource_desc.dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || resource_desc.sample_count != 1 ||
        resource_desc.width == 0 || resource_desc.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        resource_desc.height == 0 || resource_desc.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return std::nullopt;
    using UserData = g6shim::shader::emit::UserData;
    const std::uint32_t base_mip = desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D
                                       ? desc->Texture2D.MostDetailedMip
                                       : desc->Texture2DArray.MostDetailedMip;
    if (base_mip > (UserData::kImageBaseMipMask >> UserData::kImageBaseMipShift) ||
        (resource_desc.mip_levels != 0 && base_mip >= resource_desc.mip_levels))
        return std::nullopt;
    constexpr std::uint32_t kWidthLowBits = 32u - UserData::kImageWidthLowShift;
    const auto width = static_cast<std::uint32_t>(resource_desc.width - 1);
    const std::uint32_t height = resource_desc.height - 1;
    return ImageShapeWords{width << UserData::kImageWidthLowShift,
                           (width >> kWidthLowBits) | (height << UserData::kImageWidthHighBits),
                           base_mip << UserData::kImageBaseMipShift};
}

void write_image_slot(const HeapData& heap, std::uint32_t index, const std::optional<descriptor::View>& view) {
    if (index >= heap.count) return;
    ImageData::Record record{};
    if (view) {
        if (const auto shape = image_shape(*view)) {
            record[ImageData::kWidthLow] = shape->width_low;
            record[ImageData::kDimensions] = shape->dimensions;
            record[ImageData::kBaseMip] = shape->base_mip;
            record[ImageData::kPresent] = 1;
        }
    }
    std::memcpy(heap.data + static_cast<std::size_t>(index) * ImageData::kBytes, record.data(), ImageData::kBytes);
}

struct IndexedBufferView {
    std::uint32_t stride;
    std::uint32_t records;
    std::uint64_t address;
    std::uint64_t bytes;
};

std::optional<IndexedBufferView> indexed_buffer_view(const descriptor::View& view) {
    const auto desc = shader_buffer_desc(view);
    if (!desc || null_shader_buffer(view)) return std::nullopt;
    const auto range = buffer_range(view);
    if (!range || range->second == 0 || range->second > UINT32_MAX) return std::nullopt;
    std::uint64_t resource_bytes = 0;
    if (const auto* srv = std::get_if<descriptor::ShaderView>(&view)) {
        const auto memory = resource::buffer_bytes(reinterpret_cast<Object*>(srv->resource));
        if (memory) resource_bytes = memory->second;
    } else if (const auto* placed = std::get_if<descriptor::PlacedView>(&view)) {
        resource::ResourceDesc description{};
        std::memcpy(&description, placed->resource.data(), sizeof description);
        resource_bytes = description.width;
    }
    const std::uint64_t element = element_size(*desc);
    if (element == 0 || desc->Buffer.FirstElement > resource_bytes / element) return std::nullopt;
    const std::uint64_t offset = desc->Buffer.FirstElement * element;
    if (range->second > resource_bytes - offset) return std::nullopt;
    if (const auto stride = structured_buffer_stride(*desc))
        return IndexedBufferView{*stride, desc->Buffer.NumElements, range->first, range->second};
    if (typed_buffer_format(*desc) != nullptr)
        return IndexedBufferView{kRawElementBytes, desc->Buffer.NumElements, range->first, range->second};
    if (linear_raw_buffer(*desc))
        return IndexedBufferView{0, static_cast<std::uint32_t>(range->second), range->first, range->second};
    return std::nullopt;
}

std::optional<BufferData::Record> buffer_slot(HostHeap& host_heap, BufferHeap& heap, std::uint32_t index,
                                               const IndexedBufferView& view) {
    const auto backing = shadow::find(view.address, view.bytes);
    host::Host* gpu = host::host();
    if (!backing || gpu == nullptr) return std::nullopt;
    const std::uint64_t offset = (*backing)->offset(view.address);
    if (offset % kRawElementBytes != 0) return std::nullopt;
    constexpr std::uint32_t kAlignment = D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT;
    const auto prefix = static_cast<std::uint32_t>(offset % kAlignment);
    if (view.bytes > UINT32_MAX - prefix) return std::nullopt;
    auto alias = heap.aliases.find(index);
    if (alias == heap.aliases.end()) {
        const auto reserved = host_heap.dynamic(1);
        if (!reserved) return std::nullopt;
        alias = heap.aliases.emplace(index, BufferHeapAlias{*reserved, nullptr}).first;
    }
    if (alias->second.backing != *backing || alias->second.address != view.address ||
        alias->second.bytes != view.bytes) {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Buffer.FirstElement = (offset - prefix) / kRawElementBytes;
        desc.Buffer.NumElements = static_cast<std::uint32_t>((view.bytes + prefix) / kRawElementBytes);
        desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        gpu->device()->CreateShaderResourceView((*backing)->buffer(), &desc, host_heap.cpu(alias->second.index));
        alias->second = {alias->second.index, *backing, view.address, view.bytes};
    }
    return BufferData::Record{alias->second.index + 1, view.stride, prefix, view.records};
}

void write_buffer_slot(HostHeap& host_heap, BufferHeap& heap, std::uint32_t index,
                       const std::optional<descriptor::View>& descriptor) {
    if (index >= heap.data.count) return;
    const auto view = descriptor ? indexed_buffer_view(*descriptor) : std::nullopt;
    const auto record = view ? buffer_slot(host_heap, heap, index, *view) : std::nullopt;
    if (!record) {
        const auto alias = heap.aliases.find(index);
        if (alias != heap.aliases.end()) alias->second.backing.reset();
        if (view || (descriptor && !null_shader_buffer(*descriptor) && shader_buffer_desc(*descriptor))) {
            static std::once_flag once;
            std::call_once(once, [] {
                G6SHIM_WARN("d3d12_xs: indexed reads cannot access an unsupported buffer view");
            });
        }
    }
    const BufferData::Record words = record.value_or(BufferData::Record{});
    std::memcpy(heap.data.data + static_cast<std::size_t>(index) * BufferData::kBytes,
                words.data(), BufferData::kBytes);
}

std::optional<HeapData> create_heap_data(HeapEntry& entry) {
    host::Host* gpu = host::host();
    if (!entry.host || gpu == nullptr) return std::nullopt;
    const std::uint32_t count = std::min({entry.count, entry.host->count(), kPortableViews});
    if (count == 0) return std::nullopt;
    const auto reserved = entry.host->dynamic(1);
    if (!reserved) return std::nullopt;
    static_assert(ImageData::kBytes == BufferData::kBytes);
    auto upload = gpu->upload_buffer(static_cast<std::uint64_t>(count) * ImageData::kBytes);
    if (!upload) {
        entry.host->release(*reserved, 1);
        return std::nullopt;
    }
    HeapData data{{*reserved, std::move(upload->first)}, upload->second, count};
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.NumElements = count * ImageData::kWords;
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    gpu->device()->CreateShaderResourceView(data.binding.resource.Get(), &desc, entry.host->cpu(*reserved));
    return data;
}

}  // namespace

std::optional<ImageShapeWords> table_image_shape(const descriptor::Heap& heap, std::uint64_t table,
                                                 std::uint32_t dword) {
    const auto view = table_view(heap, table, dword);
    return view ? image_shape(*view) : std::nullopt;
}

std::optional<std::uint32_t> table_image_sampling_fields(const descriptor::Heap& heap, std::uint64_t table,
                                                        std::uint32_t dword) {
    const auto view = table_view(heap, table, dword);
    if (!view) return std::nullopt;
    const auto desc = shader_view_desc(*view);
    if (!desc || desc->ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D) return std::nullopt;
    const auto address = image_address(*view);
    if (!address || *address % (1ull << kImageBaseAddressShift) != 0) return std::nullopt;
    constexpr std::uint32_t kSamplingPerformance = 4;
    return *address == 0 ? 0u : kSamplingPerformance << g6shim::shader::emit::UserData::kImageSamplingPerformanceShift;
}

std::optional<std::uint32_t> sampler_lod_word(const descriptor::Heap& heap, std::uint32_t dword) {
    const auto view = table_view(heap, heap.start, dword, descriptor::kSamplersKind);
    if (!view) return std::nullopt;
    const auto* sampler = std::get_if<descriptor::SamplerView>(&*view);
    if (sampler == nullptr) return std::nullopt;
    D3D12_SAMPLER_DESC desc{};
    std::memcpy(&desc, sampler->desc.data(), sizeof desc);
    if (!std::isfinite(desc.MinLOD) || !std::isfinite(desc.MaxLOD) || desc.MinLOD > desc.MaxLOD)
        return std::nullopt;
    constexpr std::uint32_t kLodFieldBits = 12;
    constexpr float kLodScale = 256.0f;
    constexpr float kMaximumLod = static_cast<float>((1u << kLodFieldBits) - 1u) / kLodScale;
    const auto encode = [=](float lod) {
        return static_cast<std::uint32_t>(std::clamp(lod, 0.0f, kMaximumLod) * kLodScale);
    };
    return encode(desc.MinLOD) | (encode(desc.MaxLOD) << kLodFieldBits);
}

std::optional<descriptor::View> placed_texture(const descriptor::View& view) {
    const auto* placed = std::get_if<descriptor::PlacedView>(&view);
    if (placed == nullptr || placed->unordered || placed->resource[0] == kBufferDimension) return std::nullopt;
    const std::uint64_t address = placed_address(*placed);
    const auto key = std::make_pair(address, placed->resource);
    {
        std::lock_guard<std::mutex> lock(g_placed_mutex);
        const auto found = g_placed.find(key);
        if (found != g_placed.end()) {
            Object* object = found->second;
            if (!resource::host_resource(object)) return std::nullopt;
            std::array<std::uint32_t, 10> words{};
            std::copy_n(placed->desc.begin(), 10, words.begin());
            return descriptor::View{descriptor::ShaderView{reinterpret_cast<std::size_t>(object), words}};
        }
    }
    resource::ResourceDesc desc{};
    std::memcpy(&desc, placed->resource.data(), sizeof desc);
    Object* out = nullptr;
    if (FAILED(resource::create_at_address(nullptr, address, &desc, 0, nullptr, &resource::kResourceIids[0], &out)) ||
        out == nullptr)
        return std::nullopt;
    {
        std::lock_guard<std::mutex> lock(g_placed_mutex);
        g_placed.insert_or_assign(key, out);
    }
    if (!resource::host_resource(out)) return std::nullopt;
    std::array<std::uint32_t, 10> words{};
    std::copy_n(placed->desc.begin(), 10, words.begin());
    return descriptor::View{descriptor::ShaderView{reinterpret_cast<std::size_t>(out), words}};
}

std::string near_heaps(std::uint64_t va) {
    std::string text;
    char line[192] = {};
    std::lock_guard<std::mutex> lock(g_ranges_mutex);
    for (const GpuRange& range : g_ranges) {
        const char* kind = range.sampler ? "sampler" : "view";
        if (va < range.start) {
            std::snprintf(line, sizeof line, "; %s heap %llx+%llx: %llx below", kind,
                          static_cast<unsigned long long>(range.start),
                          static_cast<unsigned long long>(range.bytes),
                          static_cast<unsigned long long>(range.start - va));
        } else if (va < range.start + range.bytes) {
            std::snprintf(line, sizeof line, "; %s heap %llx+%llx: inside, slot %llu", kind,
                          static_cast<unsigned long long>(range.start),
                          static_cast<unsigned long long>(range.bytes),
                          static_cast<unsigned long long>((va - range.start) / range.step));
        } else {
            std::snprintf(line, sizeof line, "; %s heap %llx+%llx: %llx past its end (slot %llu)", kind,
                          static_cast<unsigned long long>(range.start),
                          static_cast<unsigned long long>(range.bytes),
                          static_cast<unsigned long long>(va - range.start - range.bytes),
                          static_cast<unsigned long long>((va - range.start) / range.step));
        }
        text += line;
    }
    return text;
}

void register_heap(Object* heap, std::uint64_t start, std::uint64_t end) {
    if (heap == nullptr || end <= start) return;
    const auto* descriptor_heap = static_cast<const descriptor::Heap*>(heap);
    const auto step = descriptor::increment(descriptor_heap->desc.kind);
    if (!step) return;
    auto entry = std::make_shared<HeapEntry>();
    entry->start = start;
    entry->end = end;
    entry->heap = heap;
    entry->count = descriptor_heap->desc.count;
    entry->step = *step;
    entry->sampler = descriptor_heap->desc.kind == descriptor::kSamplersKind;
    std::lock_guard<std::mutex> lock(g_heaps_mutex);
    g_heaps.push_back(std::move(entry));
}

void drop_heap(Object* heap) {
    std::lock_guard<std::mutex> lock(g_heaps_mutex);
    g_heaps.erase(std::remove_if(g_heaps.begin(), g_heaps.end(),
                                 [heap](const auto& entry) { return entry->heap == heap; }),
                  g_heaps.end());
}

void mirror(std::uint64_t handle) {
    std::shared_ptr<HeapEntry> entry = entry_for_handle(handle);
    if (!entry || entry->step == 0) return;
    const std::uint32_t index = static_cast<std::uint32_t>((handle - entry->start) / entry->step);
    if (index >= entry->count) return;
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (!entry->host) {
        if (!entry->tried) entry->pending.push_back(index);
        return;
    }
    const auto view = entry->host->mirror(index, handle, false);
    if (entry->image_data) write_image_slot(*entry->image_data, index, view);
    if (entry->buffer_data) write_buffer_slot(*entry->host, *entry->buffer_data, index, view);
}

void release_dynamic(Object* heap, std::uint32_t first, std::uint32_t count) {
    std::shared_ptr<HeapEntry> entry = entry_for_heap(heap);
    if (!entry) return;
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (entry->host) entry->host->release(first, count);
}

HostHeap* for_heap(Object* heap) {
    std::shared_ptr<HeapEntry> entry = entry_for_heap(heap);
    if (!entry) return nullptr;
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (!entry->host && !entry->tried) {
        entry->tried = true;
        entry->host = HostHeap::create(entry->sampler, entry->count);
    }
    if (!entry->host) return nullptr;
    std::vector<std::uint32_t> pending = std::move(entry->pending);
    entry->pending.clear();
    for (const std::uint32_t index : pending)
        entry->host->mirror(index, entry->start + static_cast<std::uint64_t>(index) * entry->step, false);
    return entry->host.get();
}

std::optional<HeapDataBinding> image_heap_data(Object* heap) {
    const auto entry = entry_for_heap(heap);
    if (!entry || entry->sampler) return std::nullopt;
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (entry->image_data) return entry->image_data->binding;
    auto data = create_heap_data(*entry);
    if (!data) return std::nullopt;
    for (std::uint32_t index = 0; index < data->count; ++index) {
        const std::uint64_t handle = entry->start + static_cast<std::uint64_t>(index) * entry->step;
        write_image_slot(*data, index, descriptor::view(handle));
    }
    entry->image_data = std::move(data);
    return entry->image_data->binding;
}

std::optional<HeapDataBinding> buffer_heap_data(Object* heap, host::HostList& list) {
    const auto entry = entry_for_heap(heap);
    if (!entry || entry->sampler) return std::nullopt;
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (!entry->buffer_data) {
        auto data = create_heap_data(*entry);
        if (!data) return std::nullopt;
        auto& buffers = entry->buffer_data.emplace(BufferHeap{std::move(*data), {}});
        for (std::uint32_t index = 0; index < buffers.data.count; ++index) {
            const std::uint64_t handle = entry->start + static_cast<std::uint64_t>(index) * entry->step;
            write_buffer_slot(*entry->host, buffers, index, descriptor::view(handle));
        }
    }
    for (const auto& item : entry->buffer_data->aliases) {
        const BufferHeapAlias& alias = item.second;
        if (!alias.backing) continue;
        alias.backing->refresh(list, alias.address, alias.bytes);
    }
    return entry->buffer_data->data.binding;
}

}  // namespace g6shim::d3d12_xs::descriptor_host
