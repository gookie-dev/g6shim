// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/descriptor.hpp"
#include "d3d12_xs/descriptor_host.hpp"
#include "d3d12_xs/object.hpp"
#include "d3d12_xs/resource.hpp"

#include <g6shim/shader/emit/emit.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

using namespace g6shim::d3d12_xs;

namespace {

struct Extended {
    descriptor::HeapDesc desc;
    descriptor::Placement placement;
};

HRESULT create_heap_with(const descriptor::HeapDesc& desc, const descriptor::Placement& placement, Object** out) {
    const Extended extended{desc, placement};
    return descriptor::create_heap(nullptr, &extended.desc, &descriptor::kIids[0], out);
}

}  // namespace

TEST(d3d12_xs_heaps_hold_count_descriptors_from_their_start) {
    const descriptor::HeapDesc desc{0, 1000, 1, 0};
    const descriptor::Placement none{0, 0};
    Object* heap = nullptr;
    CHECK(create_heap_with(desc, none, &heap) == S_OK);
    const auto cpu = slot<std::uint64_t (*)(Object*)>(heap, 0x48)(heap);
    CHECK(cpu % 64 == 0);
    CHECK(slot<std::uint64_t (*)(Object*)>(heap, 0x50)(heap) == cpu);
    CHECK(descriptor::increment_size(nullptr, 0) == 32);
    *reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(cpu) + 999 * 32 + 31) = 1;
    CHECK(descriptor::increment_size(nullptr, 9) == 0);
    const descriptor::HeapDesc bad{9, 1000, 1, 0};
    CHECK(create_heap_with(bad, none, &heap) == E_INVALIDARG);
    CHECK(slot<std::uint32_t (*)(Object*)>(heap, 0x10)(heap) == 0);
}

TEST(d3d12_xs_views_survive_copies) {
    std::array<std::uint64_t, 16> heap{};
    const auto at = [&heap](std::size_t index) {
        return reinterpret_cast<std::uint64_t>(heap.data()) + 32 * index;
    };
    const std::uint64_t s0 = at(0), s1 = at(1), s2 = at(2), s3 = at(3);
    const std::array<std::uint32_t, 4> constant{0x1000, 1, 256, 0};
    descriptor::create_cbv(nullptr, constant.data(), s0);
    CHECK(descriptor::view(s0) == std::optional<descriptor::View>(descriptor::ConstantView{0x1'0000'1000, 256}));
    descriptor::create_srv(nullptr, nullptr, nullptr, s1);
    CHECK(descriptor::view(s1) == std::optional<descriptor::View>(descriptor::ShaderView{0, std::nullopt}));
    descriptor::create_srv(nullptr, nullptr, nullptr, s2);
    std::array<std::uint32_t, 3> first_words{};
    std::array<std::uint32_t, 3> second_words{};
    std::memcpy(first_words.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(s1)),
                sizeof first_words);
    std::memcpy(second_words.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(s2)),
                sizeof second_words);
    CHECK(first_words == second_words);

    const std::array<std::uint64_t, 1> destination{s2};
    const std::array<std::uint32_t, 1> destination_sizes{2};
    const std::array<std::uint64_t, 2> source{s0, s1};
    const std::array<std::uint32_t, 2> source_sizes{1, 1};
    descriptor::copy(nullptr, 1, destination.data(), destination_sizes.data(), 2, source.data(),
                     source_sizes.data(), 0);
    CHECK(descriptor::view(s2) == descriptor::view(s0));
    CHECK(descriptor::view(s3) == descriptor::view(s1));
    CHECK(descriptor::view(at(0) + 8) == std::nullopt);
}

TEST(d3d12_xs_views_die_with_their_resource) {
    std::array<std::uint64_t, 8> slots{};
    const auto a = reinterpret_cast<std::uint64_t>(slots.data());
    const auto b = a + 32;
    auto* resource = reinterpret_cast<Object*>(static_cast<std::uintptr_t>(0x7777'0000));
    descriptor::create_srv(nullptr, resource, nullptr, a);
    CHECK(descriptor::view(a) ==
          std::optional<descriptor::View>(descriptor::ShaderView{0x7777'0000, std::nullopt}));
    descriptor::forget(resource);
    CHECK(descriptor::view(a) == std::nullopt);
    descriptor::create_srv(nullptr, resource, nullptr, b);
    CHECK(descriptor::view(b) ==
          std::optional<descriptor::View>(descriptor::ShaderView{0x7777'0000, std::nullopt}));
    std::array<std::uint32_t, 3> old_words{};
    std::array<std::uint32_t, 3> new_words{};
    std::memcpy(old_words.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(a)), sizeof old_words);
    std::memcpy(new_words.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(b)), sizeof new_words);
    CHECK(old_words != new_words);
}

TEST(d3d12_xs_table_buffer_addresses_are_bound_to_the_requested_slot) {
    constexpr std::uint32_t kSlots = 4;
    constexpr std::uint32_t kSlotDwords = 8;
    const descriptor::HeapDesc desc{0, kSlots, 1, 0};
    const descriptor::Placement none{0, 0};
    Object* object = nullptr;
    CHECK(create_heap_with(desc, none, &object) == S_OK);
    if (object == nullptr) return;
    const auto& heap = *static_cast<descriptor::Heap*>(object);
    const std::uint64_t step = descriptor::increment_size(nullptr, desc.kind);
    const std::uint64_t table = heap.start + step;
    const std::array<std::uint32_t, 4> constant{0x1122'0000, 1, 256, 0};
    descriptor::create_cbv(nullptr, constant.data(), table);

    std::array<std::uint32_t, 14> resource_words{};
    resource_words[0] = D3D12_RESOURCE_DIMENSION_BUFFER;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(3, 4, 5, 0);
    srv.Buffer.FirstElement = 3;
    srv.Buffer.NumElements = 8;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    std::array<std::uint32_t, 16> shader_words{};
    std::memcpy(shader_words.data(), &srv, sizeof srv);
    shader_words[10] = 0x3344'0000;
    shader_words[11] = 2;
    descriptor::create_placed_srv(nullptr, resource_words.data(), shader_words.data(), table + step);
    descriptor::create_srv(nullptr, nullptr, shader_words.data(), table + 2 * step);

    using descriptor_host::table_buffer_address;
    CHECK(table_buffer_address(heap, table, 0, true) == 0x1'1122'0000ull);
    CHECK(table_buffer_address(heap, table, kSlotDwords, false) == 0x2'3344'000Cull);
    CHECK(table_buffer_address(heap, table, 2 * kSlotDwords, false) == 0ull);
    CHECK(!table_buffer_address(heap, table, 0, false));
    CHECK(!table_buffer_address(heap, table, kSlotDwords, true));
    CHECK(!table_buffer_address(heap, table, kSlotDwords + 1, false));
    CHECK(!table_buffer_address(heap, table + 4, 0, true));
    CHECK(!table_buffer_address(heap, table, 3 * kSlotDwords, false));
    using descriptor_host::table_constant_buffer_words;
    CHECK((table_constant_buffer_words(heap, table, 0) == std::array{0x0010'0001u, 16u, 0x1101'4facu}));
    CHECK(!table_constant_buffer_words(heap, table, kSlotDwords));
    CHECK(!table_constant_buffer_words(heap, table, 1));
    const std::array<std::uint32_t, 4> null_constant{};
    descriptor::create_cbv(nullptr, null_constant.data(), table + 2 * step);
    CHECK((table_constant_buffer_words(heap, table, 2 * kSlotDwords) == std::array{0u, 0u, 0u}));
    const std::array<std::uint32_t, 4> unsized_constant{0x1122'0000, 1, 17, 0};
    descriptor::create_cbv(nullptr, unsized_constant.data(), table + 2 * step);
    CHECK(!table_constant_buffer_words(heap, table, 2 * kSlotDwords));
    descriptor::create_srv(nullptr, nullptr, shader_words.data(), table + 2 * step);
    using descriptor_host::table_buffer_component_mapping;
    CHECK(table_buffer_component_mapping(heap, table, kSlotDwords) == 0x847u);
    CHECK(table_buffer_component_mapping(heap, table, 2 * kSlotDwords) == 0u);
    CHECK(!table_buffer_component_mapping(heap, table, 0));
    CHECK(!table_buffer_component_mapping(heap, table, kSlotDwords + 1));
    CHECK(!table_buffer_component_mapping(heap, table, 3 * kSlotDwords));
    using descriptor_host::table_linear_raw_buffer_high_word;
    using descriptor_host::table_buffer_record_count;
    using descriptor_host::table_untyped_buffer_format_word;
    CHECK(!table_linear_raw_buffer_high_word(heap, table, kSlotDwords));
    CHECK(!table_buffer_record_count(heap, table, kSlotDwords));
    CHECK(!table_buffer_record_count(heap, table, 0));
    CHECK(table_buffer_record_count(heap, table, 2 * kSlotDwords) == 0u);
    CHECK(table_linear_raw_buffer_high_word(heap, table, 2 * kSlotDwords) == 0u);
    CHECK(table_untyped_buffer_format_word(heap, table, 2 * kSlotDwords, true) == 0u);
    CHECK(table_untyped_buffer_format_word(heap, table, 2 * kSlotDwords, false) == 0u);
    srv.Buffer.FirstElement = 4;
    std::memcpy(shader_words.data(), &srv, sizeof srv);
    descriptor::create_placed_srv(nullptr, resource_words.data(), shader_words.data(), table + step);
    CHECK(table_linear_raw_buffer_high_word(heap, table, kSlotDwords) == 2u);
    CHECK(table_buffer_record_count(heap, table, kSlotDwords) == 32u);
    CHECK(!table_untyped_buffer_format_word(heap, table, kSlotDwords, false));
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    std::memcpy(shader_words.data(), &srv, sizeof srv);
    descriptor::create_placed_srv(nullptr, resource_words.data(), shader_words.data(), table + step);
    CHECK(table_untyped_buffer_format_word(heap, table, kSlotDwords, false) == 0x3101'4facu);
    CHECK(!table_untyped_buffer_format_word(heap, table, kSlotDwords, true));
    shader_words[10] = 0xFFFF'FFFC;
    shader_words[11] = 0xFFFF'FFFF;
    descriptor::create_placed_srv(nullptr, resource_words.data(), shader_words.data(), table + step);
    CHECK(!table_buffer_address(heap, table, kSlotDwords, false));
    srv.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 1, 6, 3);
    std::memcpy(shader_words.data(), &srv, sizeof srv);
    descriptor::create_placed_srv(nullptr, resource_words.data(), shader_words.data(), table + step);
    CHECK(!table_buffer_component_mapping(heap, table, kSlotDwords));
    using descriptor_host::table_structured_buffer_high_word;
    CHECK(!table_structured_buffer_high_word(heap, table, kSlotDwords));
    CHECK(table_structured_buffer_high_word(heap, table, 2 * kSlotDwords) == 0u);
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.Buffer.StructureByteStride = 16;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    std::memcpy(shader_words.data(), &srv, sizeof srv);
    shader_words[10] = 0x3344'0000;
    shader_words[11] = 2;
    descriptor::create_placed_srv(nullptr, resource_words.data(), shader_words.data(), table + step);
    CHECK(table_structured_buffer_high_word(heap, table, kSlotDwords) == 0x0010'0002u);
    CHECK(table_buffer_record_count(heap, table, kSlotDwords) == 8u);
    CHECK(table_untyped_buffer_format_word(heap, table, kSlotDwords, true) == 0x1101'4facu);
    CHECK(!table_linear_raw_buffer_high_word(heap, table, kSlotDwords));
    CHECK(!table_untyped_buffer_format_word(heap, table, kSlotDwords + 1, true));
    CHECK(!table_structured_buffer_high_word(heap, table, kSlotDwords + 1));
    CHECK(!table_structured_buffer_high_word(heap, table, 3 * kSlotDwords));
    using descriptor_host::table_typed_buffer_word;
    CHECK(!table_typed_buffer_word(heap, table, kSlotDwords, false));
    CHECK(table_typed_buffer_word(heap, table, 2 * kSlotDwords, false) == 0u);
    CHECK(table_typed_buffer_word(heap, table, 2 * kSlotDwords, true) == 0u);
    srv.Format = DXGI_FORMAT_R32_UINT;
    srv.Buffer.StructureByteStride = 0;
    std::memcpy(shader_words.data(), &srv, sizeof srv);
    descriptor::create_placed_srv(nullptr, resource_words.data(), shader_words.data(), table + step);
    CHECK(table_typed_buffer_word(heap, table, kSlotDwords, true) == 0x0004'0002u);
    CHECK(table_buffer_record_count(heap, table, kSlotDwords) == 8u);
    CHECK(!table_buffer_record_count(heap, table, kSlotDwords + 1));
    CHECK(table_typed_buffer_word(heap, table, kSlotDwords, false) == 0x1101'4204u);
    srv.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(3, 4, 5, 0);
    std::memcpy(shader_words.data(), &srv, sizeof srv);
    descriptor::create_placed_srv(nullptr, resource_words.data(), shader_words.data(), table + step);
    CHECK(table_typed_buffer_word(heap, table, kSlotDwords, false) == 0x1101'4841u);
    CHECK(!table_typed_buffer_word(heap, table, kSlotDwords + 1, false));
    release(object);
}

TEST(d3d12_xs_table_image_address_uses_the_title_base) {
    constexpr std::uint32_t kSlotDwords = 8;
    const descriptor::HeapDesc desc{0, 3, 1, 0};
    const descriptor::Placement none{0, 0};
    Object* object = nullptr;
    CHECK(create_heap_with(desc, none, &object) == S_OK);
    if (object == nullptr) return;
    const auto& heap = *static_cast<descriptor::Heap*>(object);
    const auto step = descriptor::increment_size(nullptr, desc.kind);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MostDetailedMip = 1;
    srv.Texture2D.MipLevels = 1;
    std::array<std::uint32_t, 16> words{};
    std::memcpy(words.data(), &srv, sizeof srv);

    resource::ResourceDesc texture{};
    texture.dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.width = 16;
    texture.height = 16;
    texture.depth_or_array_size = 1;
    texture.mip_levels = 3;
    texture.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.sample_count = 1;
    std::array<std::uint32_t, 14> resource_words{};
    std::memcpy(resource_words.data(), &texture, sizeof texture);
    words[10] = 0x5678'9A00;
    words[11] = 0x1234;
    descriptor::create_placed_srv(nullptr, resource_words.data(), words.data(), heap.start + step);
    descriptor::create_srv(nullptr, nullptr, words.data(), heap.start + 2 * step);
    CHECK(descriptor_host::table_image_base_word(heap, heap.start, kSlotDwords) == 0x3456'789Au);
    CHECK(descriptor_host::table_image_base_word(heap, heap.start, 2 * kSlotDwords) == 0u);
    CHECK(!descriptor_host::table_image_base_word(heap, heap.start, 3 * kSlotDwords));
    using descriptor_host::table_image_shape;
    constexpr descriptor_host::ImageShapeWords kShape{0xC000'0000u, 0x0003'C003u, 0x0000'1000u};
    CHECK(table_image_shape(heap, heap.start, kSlotDwords) == kShape);
    CHECK(table_image_shape(heap, heap.start, 2 * kSlotDwords) == descriptor_host::ImageShapeWords{});
    CHECK(!table_image_shape(heap, heap.start, kSlotDwords + 1));
    CHECK(!table_image_shape(heap, heap.start, 3 * kSlotDwords));
    const auto valid_words = words;
    srv.Texture2D.MostDetailedMip = texture.mip_levels;
    std::memcpy(words.data(), &srv, sizeof srv);
    descriptor::create_placed_srv(nullptr, resource_words.data(), words.data(), heap.start + step);
    CHECK(!table_image_shape(heap, heap.start, kSlotDwords));
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray = {2, 1, 0, 1, 0, 0.0f};
    std::memcpy(words.data(), &srv, sizeof srv);
    descriptor::create_placed_srv(nullptr, resource_words.data(), words.data(), heap.start + step);
    constexpr descriptor_host::ImageShapeWords kArrayShape{0xC000'0000u, 0x0003'C003u, 0x0000'2000u};
    CHECK(table_image_shape(heap, heap.start, kSlotDwords) == kArrayShape);
    words = valid_words;
    descriptor::create_placed_srv(nullptr, resource_words.data(), words.data(), heap.start + step);
    using descriptor_host::table_image_sampling_fields;
    CHECK(table_image_sampling_fields(heap, heap.start, kSlotDwords) == 0x0040'0000u);
    CHECK(table_image_sampling_fields(heap, heap.start, 2 * kSlotDwords) == 0u);
    CHECK(!table_image_sampling_fields(heap, heap.start, 3 * kSlotDwords));
    ++words[10];
    descriptor::create_placed_srv(nullptr, resource_words.data(), words.data(), heap.start + step);
    CHECK(!descriptor_host::table_image_base_word(heap, heap.start, kSlotDwords));
    CHECK(!table_image_sampling_fields(heap, heap.start, kSlotDwords));
    CHECK(!table_image_shape(heap, heap.start, kSlotDwords));
    resource_words[0] = D3D12_RESOURCE_DIMENSION_BUFFER;
    --words[10];
    descriptor::create_placed_srv(nullptr, resource_words.data(), words.data(), heap.start + step);
    CHECK(!descriptor_host::table_image_base_word(heap, heap.start, kSlotDwords));
    CHECK(!table_image_sampling_fields(heap, heap.start, kSlotDwords));

    const resource::HeapProperties properties{1, 0, 0, 1, 1};
    Object* image = nullptr;
    CHECK(resource::create_committed(nullptr, &properties, 0, &texture, 0, nullptr,
                                     &resource::kResourceIids[0], &image) == S_OK);
    if (image != nullptr) {
        const auto memory = resource::texture_memory(image);
        CHECK(memory.has_value());
        if (memory) {
            descriptor::create_srv(nullptr, image, words.data(), heap.start);
            CHECK(descriptor_host::table_image_base_word(heap, heap.start, 0) ==
                  static_cast<std::uint32_t>(reinterpret_cast<std::uint64_t>(memory->first) >> 8));
            CHECK(table_image_shape(heap, heap.start, 0) == kShape);
        }
        release(image);
    }
    release(object);
}

TEST(d3d12_xs_indexed_image_shape_follows_view_writes_and_copies) {
    if (host::host() == nullptr) return;
    const descriptor::HeapDesc desc{descriptor::kViewsKind, 3, descriptor::kShaderVisible, 0};
    const descriptor::Placement none{0, 0};
    Object* object = nullptr;
    CHECK(create_heap_with(desc, none, &object) == S_OK);
    if (object == nullptr) return;
    const auto& heap = *static_cast<descriptor::Heap*>(object);
    const auto step = descriptor::increment_size(nullptr, desc.kind);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MostDetailedMip = 2;
    srv.Texture2D.MipLevels = 1;
    std::array<std::uint32_t, 16> words{};
    std::memcpy(words.data(), &srv, sizeof srv);
    words[10] = 0x1000;
    resource::ResourceDesc texture{};
    texture.dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.width = 32;
    texture.height = 64;
    texture.depth_or_array_size = 1;
    texture.mip_levels = 4;
    texture.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.sample_count = 1;
    std::array<std::uint32_t, 14> resource_words{};
    std::memcpy(resource_words.data(), &texture, sizeof texture);
    descriptor::create_placed_srv(nullptr, resource_words.data(), words.data(), heap.start);
    const auto* host_heap = descriptor_host::for_heap(object);
    const auto binding = descriptor_host::image_heap_data(object);
    CHECK(host_heap != nullptr);
    CHECK(binding.has_value());
    if (binding && host_heap != nullptr) {
        CHECK(binding->index >= host_heap->count());
        using Data = g6shim::shader::emit::ImageHeapData;
        const D3D12_RANGE read_range{0, desc.count * Data::kBytes};
        void* mapped = nullptr;
        CHECK(binding->resource->Map(0, &read_range, &mapped) == S_OK);
        if (mapped != nullptr) {
            const auto record = [mapped](std::size_t slot) {
                Data::Record result{};
                std::memcpy(result.data(), static_cast<const std::uint8_t*>(mapped) + slot * Data::kBytes,
                            Data::kBytes);
                return result;
            };
            constexpr Data::Record kShape{0xC000'0000u, 0x000F'C007u, 0x2000u, 1};
            CHECK(record(0) == kShape);
            CHECK(record(1) == Data::Record{});
            descriptor::copy_simple(nullptr, 1, heap.start + step, heap.start, desc.kind);
            CHECK(record(1) == kShape);
            descriptor::create_srv(nullptr, nullptr, words.data(), heap.start);
            constexpr Data::Record kNullImage{0, 0, 0, 1};
            CHECK(record(0) == kNullImage);
            constexpr std::array kConstant{0x1000u, 0u, 256u, 0u};
            descriptor::create_cbv(nullptr, kConstant.data(), heap.start + step);
            CHECK(record(1) == Data::Record{});
            CHECK(descriptor_host::image_heap_data(object)->index == binding->index);
            const D3D12_RANGE no_write{0, 0};
            binding->resource->Unmap(0, &no_write);
        }
    }
    release(object);
}

TEST(d3d12_xs_sampler_lod_word_uses_the_bound_descriptor) {
    constexpr std::uint32_t kSlotDwords = 4;
    const descriptor::HeapDesc desc{descriptor::kSamplersKind, 3, descriptor::kShaderVisible, 0};
    const descriptor::Placement none{0, 0};
    Object* object = nullptr;
    CHECK(create_heap_with(desc, none, &object) == S_OK);
    if (object == nullptr) return;
    const auto& heap = *static_cast<descriptor::Heap*>(object);
    const auto step = descriptor::increment_size(nullptr, desc.kind);
    D3D12_SAMPLER_DESC sampler{};
    sampler.MinLOD = 1.25f;
    sampler.MaxLOD = 9.5f;
    std::array<std::uint32_t, 13> words{};
    std::memcpy(words.data(), &sampler, sizeof sampler);
    descriptor::create_sampler(nullptr, words.data(), heap.start + step);
    using descriptor_host::sampler_lod_word;
    CHECK(sampler_lod_word(heap, kSlotDwords) == 0x0098'0140u);

    sampler.MinLOD = 1.003f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    std::memcpy(words.data(), &sampler, sizeof sampler);
    descriptor::create_sampler(nullptr, words.data(), heap.start + step);
    CHECK(sampler_lod_word(heap, kSlotDwords) == 0x00FF'F100u);
    CHECK(!sampler_lod_word(heap, 1));
    CHECK(!sampler_lod_word(heap, 0));
    CHECK(!sampler_lod_word(heap, 3 * kSlotDwords));

    const std::array<std::uint32_t, 27> extended{};
    descriptor::create_sampler_x(nullptr, extended.data(), heap.start + 2 * step);
    CHECK(!sampler_lod_word(heap, 2 * kSlotDwords));
    release(object);
}

TEST(d3d12_xs_placed_heaps_start_at_the_title_memory) {
    std::vector<std::uint8_t> memory(64 * 32);
    const auto address = reinterpret_cast<std::uint64_t>(memory.data());
    const descriptor::HeapDesc desc{0, 64, 1, 1};
    Object* heap = nullptr;
    CHECK(create_heap_with(desc, descriptor::Placement{address, 64 * 32}, &heap) == S_OK);
    CHECK(slot<std::uint64_t (*)(Object*)>(heap, 0x48)(heap) == address);
    CHECK(slot<std::uint64_t (*)(Object*)>(heap, 0x50)(heap) == address);
    CHECK(slot<std::uint32_t (*)(Object*)>(heap, 0x10)(heap) == 0);
    CHECK(create_heap_with(desc, descriptor::Placement{address, 7}, &heap) == S_OK);
    CHECK(slot<std::uint64_t (*)(Object*)>(heap, 0x48)(heap) != address);
    CHECK(slot<std::uint32_t (*)(Object*)>(heap, 0x10)(heap) == 0);
}
