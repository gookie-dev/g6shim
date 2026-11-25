// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"
#include "shader_lowering.hpp"

#include "d3d12_xs/descriptor_host.hpp"
#include "d3d12_xs/host.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>
#include <string_view>
#include <vector>

namespace {

using namespace g6shim::shader;
using namespace g6shim::shader::emit;
using namespace g6shim::test::lowering;

struct Fixture {
    const char* name;
    std::vector<std::uint32_t> words;
};

const Fixture kFixtures[] = {
#include "gfx10_fixtures.inc"
};

constexpr std::uint16_t kFixtureVgprs = 8;
constexpr std::uint8_t kConstantsUserData = 0;
constexpr std::uint8_t kTableUserData = 4;
constexpr std::uint8_t kConstantsSlot = 0;
constexpr std::uint8_t kTableSlot = 1;
constexpr std::uint32_t kUserSgprs = 5;
constexpr std::uint32_t kUserSgprShift = 1;
constexpr std::uint32_t kSecondDescriptorDword = 8;
constexpr std::string_view kScalarBufferLength = "scalar_buffer_length";
constexpr std::string_view kScalarBufferCountOnly = "scalar_buffer_count_only";
constexpr std::string_view kScalarBufferAddress = "scalar_buffer_address";
constexpr std::string_view kScalarBufferFields = "scalar_buffer_fields";
constexpr std::string_view kScalarConstantBufferWords = "scalar_constant_buffer_words";
constexpr std::string_view kScalarStructuredFields = "scalar_structured_fields";
constexpr std::string_view kScalarRawHighWord = "scalar_raw_buffer_high_word";
constexpr std::string_view kScalarTypedFormat = "scalar_typed_buffer_format";
constexpr std::string_view kScalarImageAddress = "scalar_image_address";
constexpr std::string_view kImageSampleBiasA16 = "image_sample_bias_a16";
constexpr std::string_view kImageShapeOnly = "image_shape_only";
constexpr std::string_view kImageHeapShape = "image_heap_shape";
constexpr std::string_view kBufferHeapStride = "buffer_heap_stride";
constexpr std::string_view kSamplerLodData = "sampler_lod_data";
constexpr std::string_view kPackedInputPartner = "packed_input_partner";
constexpr std::string_view kSelectReadLaneMask = "select_readlane_mask";

const Fixture* find_fixture(std::string_view name) {
    const auto found = std::find_if(std::begin(kFixtures), std::end(kFixtures),
                                   [name](const Fixture& entry) { return entry.name == name; });
    return found == std::end(kFixtures) ? nullptr : found;
}

Lowered lower_fixture(const Fixture& fixture, const std::vector<UsedResource>& used) {
    return lower_compute(fixture.words, kFixtureVgprs,
                         {root_param(RootType::Kind::Constants, kConstantsUserData, kConstantsSlot),
                          root_param(RootType::Kind::Table, kTableUserData, kTableSlot)},
                         used, kUserSgprs << kUserSgprShift, {1, 1, 1});
}

void check_indexed_buffer_results(Lowered lowered) {
    using namespace g6shim::d3d12_xs;
    using Microsoft::WRL::ComPtr;
    host::Host* gpu = host::host();
    auto validator = load_validator();
    if (gpu == nullptr || !validator) return;
    validator->validate(lowered.dxil);

    constexpr std::uint32_t kOutputDescriptor = 0;
    constexpr std::uint32_t kMetadata = 1;
    constexpr std::uint32_t kAlias = 2;
    const auto heap = descriptor_host::HostHeap::create(false, kAlias + 1);
    const auto input = gpu->upload_buffer(64);
    const auto output = gpu->shared_buffer(16);
    using Data = BufferHeapData;
    const std::array<Data::Record, 2> records{{{kAlias + 1, 12, 12, 4}, {kAlias + 1, 20, 8, 2}}};
    const auto metadata = gpu->upload_buffer(sizeof records);
    CHECK((heap && input && output && metadata));
    if (!heap || !input || !output || !metadata) return;
    std::array<std::uint32_t, 16> samples{};
    for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = 100 + static_cast<std::uint32_t>(i);
    std::memcpy(input->second, samples.data(), sizeof samples);
    std::memcpy(metadata->second, records.data(), sizeof records);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    srv.Buffer.NumElements = static_cast<UINT>(samples.size());
    gpu->device()->CreateShaderResourceView(input->first.Get(), &srv, heap->cpu(kAlias));
    srv.Buffer.NumElements = static_cast<UINT>(records.size()) * Data::kWords;
    gpu->device()->CreateShaderResourceView(metadata->first.Get(), &srv, heap->cpu(kMetadata));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 4;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    gpu->device()->CreateUnorderedAccessView(output->first.Get(), nullptr, &uav, heap->cpu(kOutputDescriptor));

    std::array<std::uint32_t, 64> constants{};
    std::uint32_t count = 0;
    for (const UserData& field : lowered.user_data) {
        if (field.index >= constants.size()) { CHECK(false); return; }
        count = std::max(count, field.index + 1);
        if (field.source == UserData::Source::BufferHeapData) constants[field.index] = kMetadata;
        if (field.root.kind == RootType::Kind::Table) constants[field.index] = kOutputDescriptor * 32;
    }
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.Num32BitValues = count;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &parameter;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> blob;
    CHECK(SUCCEEDED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)));
    if (!blob) return;
    ComPtr<ID3D12RootSignature> root;
    CHECK(SUCCEEDED(gpu->device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                      IID_PPV_ARGS(&root))));
    if (!root) return;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root.Get();
    pipeline_desc.CS = {lowered.dxil.data(), lowered.dxil.size()};
    ComPtr<ID3D12PipelineState> pipeline;
    CHECK(SUCCEEDED(gpu->device()->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline))));
    if (!pipeline) return;
    const auto run = [&](std::uint32_t descriptor, std::uint32_t index, std::uint32_t offset) {
        std::array<std::uint32_t, 3> result{UINT32_MAX, UINT32_MAX, UINT32_MAX};
        const auto list = gpu->new_list();
        if (!list) return result;
        std::memcpy(output->second, result.data(), sizeof result);
        constants[0] = descriptor;
        constants[1] = index;
        constants[2] = offset;
        auto* commands = list->list();
        ID3D12DescriptorHeap* heaps[] = {heap->heap()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(root.Get());
        commands->SetPipelineState(pipeline.Get());
        commands->SetComputeRoot32BitConstants(0, count, constants.data(), 0);
        commands->Dispatch(1, 1, 1);
        list->close();
        gpu->execute({list.get()});
        std::memcpy(result.data(), output->second, sizeof result);
        return result;
    };
    CHECK((run(0, 1, 0) == std::array{107u, 108u, 109u}));
    CHECK((run(1, 1, 0) == std::array{108u, 109u, 110u}));
    CHECK((run(0, 3, 0) == std::array{0u, 0u, 0u}));
    CHECK((run(0, 0, UINT32_MAX - 3) == std::array{0u, 0u, 0u}));
    CHECK((run(2, 0, 0) == std::array{0u, 0u, 0u}));
}

}  // namespace

TEST(shader_fixtures_translate_and_validate) {
    CHECK(std::size(kFixtures) > 0);
    for (const Fixture& fixture : kFixtures) {
        if (fixture.name == kScalarBufferLength || fixture.name == kScalarBufferAddress ||
            fixture.name == kScalarBufferFields || fixture.name == kScalarStructuredFields ||
            fixture.name == kScalarRawHighWord || fixture.name == kScalarTypedFormat ||
            fixture.name == kScalarImageAddress || fixture.name == kScalarConstantBufferWords ||
            fixture.name == kImageSampleBiasA16 || fixture.name == kScalarBufferCountOnly ||
            fixture.name == kPackedInputPartner || fixture.name == kSelectReadLaneMask ||
            fixture.name == kImageShapeOnly || fixture.name == kImageHeapShape ||
            fixture.name == kBufferHeapStride || fixture.name == kSamplerLodData)
            continue;
        bool translated = true;
        Lowered lowered;
        try {
            lowered = lower_fixture(fixture, {used_uav(kTableUserData, kTableSlot)});
        } catch (const std::exception& error) {
            std::printf("fixture %s did not translate: %s\n", fixture.name, error.what());
            translated = false;
        }
        CHECK(translated);
        if (!translated) continue;
        CHECK(lowered.user_data.size() == kUserSgprs);
        check_dxil(lowered.dxil, fixture.name);
    }
}

TEST(shader_fixtures_translate_biased_sample_with_packed_coordinates) {
    const Fixture* fixture = find_fixture(kImageSampleBiasA16);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;

    UsedResource texture = used_uav(kTableUserData, kTableSlot);
    texture.kind.kind = ResourceType::Kind::Srv;
    texture.dwords = 8;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    constexpr std::uint8_t kSamplerUserData = 6;
    constexpr std::uint8_t kSamplerSlot = 2;
    NativeProgram native;
    native.vgprs = kFixtureVgprs;
    native.layout = {root_param(RootType::Kind::Table, kTableUserData, kTableSlot),
                     root_param(RootType::Kind::SamplerHeap, kSamplerUserData, kSamplerSlot)};
    native.used = {texture, output};
    native.code = bytes_of(fixture->words);
    for (const HwStage stage : {HwStage::Ps, HwStage::Cs}) {
        native.stage = stage;
        if (stage == HwStage::Ps)
            native.io = PsIo{};
        else
            native.io = ComputeIo{(kSamplerUserData + 1u) << kUserSgprShift, {32, 1, 1}};
        try {
            const Lowered lowered = lower_native(native);
            check_dxil(lowered.dxil, stage == HwStage::Ps ? fixture->name : "image_sample_compute");
        } catch (const std::exception& error) {
            std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
            CHECK(false);
        }
    }
}

TEST(shader_fixtures_translate_an_implicit_packed_pixel_input_pair) {
    const Fixture* fixture = find_fixture(kPackedInputPartner);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;

    NativeProgram native;
    native.stage = HwStage::Ps;
    native.vgprs = kFixtureVgprs;
    PsIo io;
    io.input_addr = 2;
    PsInput packed;
    packed.usage = Usage{Usage::Kind::Generic, 0};
    packed.mask = 15;
    packed.fp16 = true;
    packed.pack2 = true;
    io.inputs.push_back(packed);
    native.io = io;
    native.code = bytes_of(fixture->words);

    const std::vector<SigElement> inputs{
        {"TEXCOORD", 0, 0, 9, 2, 0u, 0, 4, 0, true},
        {"TEXCOORD", 1, 0, 9, 2, 1u, 0, 4, 0, true},
    };
    const std::vector<SigElement> outputs{{"SV_Target", 0, 16, 9, 0, 0u, 0, 4, 0, false}};
    try {
        check_dxil(lower_native(native, inputs, outputs).dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }

    PsInput independent;
    independent.usage = Usage{Usage::Kind::Generic, 1};
    independent.row = 1;
    independent.row2 = 1;
    independent.mask = 15;
    independent.attr = 1;
    io.inputs.push_back(independent);
    native.io = io;
    bool rejected = false;
    try {
        (void)lower_native(native, inputs, outputs);
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("a packed input half no element holds") !=
                   std::string_view::npos;
    }
    CHECK(rejected);

    auto separated_inputs = inputs;
    separated_inputs[1].row = 3;
    separated_inputs[1].cols = 2;
    SigElement separate = inputs[1];
    separate.index = 2;
    separate.half = false;
    separated_inputs.push_back(separate);
    try {
        check_dxil(lower_native(native, separated_inputs, outputs).dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate a separated partner: %s\n", fixture->name, error.what());
        CHECK(false);
    }
}

TEST(shader_fixtures_translate_a_select_on_raw_lane_bits) {
    const Fixture* fixture = find_fixture(kSelectReadLaneMask);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    NativeProgram native;
    native.stage = HwStage::Cs;
    native.vgprs = kFixtureVgprs;
    native.io = ComputeIo{kUserSgprs << kUserSgprShift, {32, 2, 1}};
    native.layout = {root_param(RootType::Kind::Table, kTableUserData, kTableSlot)};
    native.used = {used_uav(kTableUserData, kTableSlot)};
    native.code = bytes_of(fixture->words);
    for (const bool wave64 : {false, true}) {
        native.wave64 = wave64;
        try {
            check_dxil(lower_native(native).dxil, fixture->name);
        } catch (const std::exception& error) {
            std::printf("fixture %s (wave %d) did not translate: %s\n", fixture->name, wave64 ? 64 : 32,
                        error.what());
            CHECK(false);
        }
    }
}

TEST(shader_fixtures_translate_image_dimensions_without_an_image_operation) {
    const Fixture* fixture = find_fixture(kImageShapeOnly);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Srv;
    input.dwords = 8;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    try {
        const auto lowered = lower_fixture(*fixture, {input, output});
        using Source = UserData::Source;
        for (const auto source : {Source::ImageWidthLowBits, Source::ImageDimensions, Source::ImageBaseMip}) {
            const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(),
                [source](const UserData& entry) { return entry.source == source; });
            CHECK(field != lowered.user_data.end());
            if (field != lowered.user_data.end()) CHECK(field->dword == 0);
        }
        check_dxil(lowered.dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }

    Fixture unknown = *fixture;
    const auto shift = std::find(unknown.words.begin(), unknown.words.end(), 0x9006'9E09u);
    CHECK(shift != unknown.words.end());
    if (shift == unknown.words.end()) return;
    *shift = 0x9006'9D09u;
    bool rejected = false;
    try {
        (void)lower_fixture(unknown, {input, output});
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("a descriptor's address or format read as data") !=
                   std::string_view::npos;
    }
    CHECK(rejected);

    Fixture unknown_mip = *fixture;
    const auto extract = std::find(unknown_mip.words.begin(), unknown_mip.words.end(), 0x0004'000Cu);
    CHECK(extract != unknown_mip.words.end());
    if (extract == unknown_mip.words.end()) return;
    ++*extract;
    rejected = false;
    try {
        (void)lower_fixture(unknown_mip, {input, output});
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("a descriptor's address or format read as data") !=
                   std::string_view::npos;
    }
    CHECK(rejected);

    Fixture variable = *fixture;
    const auto alignment = std::find(variable.words.begin(), variable.words.end(), 0x0278'120Au);
    CHECK(alignment != variable.words.end());
    if (alignment == variable.words.end()) return;
    *alignment &= ~(0x1FFu << 18);
    rejected = false;
    try {
        (void)lower_fixture(variable, {input, output});
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("a descriptor's address or format read as data") !=
                   std::string_view::npos;
    }
    CHECK(rejected);
}

TEST(shader_fixtures_translate_sampler_lod_data) {
    const Fixture* fixture = find_fixture(kSamplerLodData);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    constexpr std::uint8_t kSamplerUserData = 6;
    constexpr std::uint8_t kSamplerSlot = 2;
    try {
        const auto lowered = lower_compute(fixture->words, kFixtureVgprs,
            {root_param(RootType::Kind::Table, kTableUserData, kTableSlot),
             root_param(RootType::Kind::SamplerHeap, kSamplerUserData, kSamplerSlot)},
            {used_uav(kTableUserData, kTableSlot)}, (kSamplerUserData + 1u) << kUserSgprShift, {1, 1, 1});
        const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(), [](const UserData& entry) {
            return entry.source == UserData::Source::SamplerLodWord;
        });
        CHECK(field != lowered.user_data.end());
        if (field != lowered.user_data.end()) {
            CHECK(field->root.kind == RootType::Kind::SamplerHeap);
            CHECK(field->slot == kSamplerSlot);
            CHECK(field->dword == 4);
        }
        check_dxil(lowered.dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }
}

TEST(shader_fixtures_translate_image_shape_at_a_computed_heap_index) {
    const Fixture* fixture = find_fixture(kImageHeapShape);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    constexpr std::uint8_t kHeapUserData = 4;
    constexpr std::uint8_t kHeapSlot = 2;
    constexpr std::uint8_t kOutputUserData = 6;
    constexpr std::uint8_t kOutputSlot = 1;
    const auto translate = [&](const Fixture& code) {
        return lower_compute(code.words, kFixtureVgprs,
            {root_param(RootType::Kind::Constants, kConstantsUserData, kConstantsSlot),
             root_param(RootType::Kind::ResourceHeap, kHeapUserData, kHeapSlot),
             root_param(RootType::Kind::Table, kOutputUserData, kOutputSlot)},
            {used_uav(kOutputUserData, kOutputSlot)}, (kOutputUserData + 1u) << kUserSgprShift, {1, 1, 1});
    };
    try {
        const auto lowered = translate(*fixture);
        const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(), [](const UserData& entry) {
            return entry.source == UserData::Source::ImageHeapData;
        });
        CHECK(field != lowered.user_data.end());
        if (field != lowered.user_data.end()) {
            CHECK(field->root.kind == RootType::Kind::ResourceHeap);
            CHECK(field->slot == kHeapSlot);
        }
        check_dxil(lowered.dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }

    Fixture unknown = *fixture;
    const auto align = std::find(unknown.words.begin(), unknown.words.end(), 0x0278'120Eu);
    CHECK(align != unknown.words.end());
    if (align == unknown.words.end()) return;
    *align += 1u << 18;
    bool rejected = false;
    try {
        (void)translate(unknown);
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("a descriptor's address or format read as data") !=
                   std::string_view::npos;
    }
    CHECK(rejected);

    Fixture unused = *fixture;
    unused.words.erase(unused.words.begin() + 4, unused.words.end() - 1);
    try {
        const auto lowered = translate(unused);
        CHECK(std::none_of(lowered.user_data.begin(), lowered.user_data.end(), [](const UserData& entry) {
            return entry.source == UserData::Source::ImageHeapData;
        }));
        check_dxil(lowered.dxil, unused.name);
    } catch (const std::exception& error) {
        std::printf("fixture %s with an unused load did not translate: %s\n", unused.name, error.what());
        CHECK(false);
    }
}

TEST(shader_fixtures_translate_a_buffer_load_with_a_runtime_stride) {
    const Fixture* fixture = find_fixture(kBufferHeapStride);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    constexpr std::uint8_t kHeapUserData = 4;
    constexpr std::uint8_t kHeapSlot = 2;
    constexpr std::uint8_t kOutputUserData = 6;
    constexpr std::uint8_t kOutputSlot = 1;
    try {
        const auto lowered = lower_compute(fixture->words, kFixtureVgprs,
            {root_param(RootType::Kind::Constants, kConstantsUserData, kConstantsSlot),
             root_param(RootType::Kind::ResourceHeap, kHeapUserData, kHeapSlot),
             root_param(RootType::Kind::Table, kOutputUserData, kOutputSlot)},
            {used_uav(kOutputUserData, kOutputSlot)}, (kOutputUserData + 1u) << kUserSgprShift, {1, 1, 1});
        const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(), [](const UserData& entry) {
            return entry.source == UserData::Source::BufferHeapData;
        });
        CHECK(field != lowered.user_data.end());
        if (field != lowered.user_data.end()) {
            CHECK(field->root.kind == RootType::Kind::ResourceHeap);
            CHECK(field->slot == kHeapSlot);
        }
        check_dxil(lowered.dxil, fixture->name);
        check_indexed_buffer_results(lowered);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }
}

TEST(shader_fixtures_translate_a_buffer_count_without_using_the_view) {
    const Fixture* fixture = find_fixture(kScalarBufferCountOnly);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Srv;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    try {
        const Lowered lowered = lower_fixture(*fixture, {input, output});
        const auto count = std::find_if(lowered.user_data.begin(), lowered.user_data.end(), [](const UserData& entry) {
            return entry.source == UserData::Source::ShaderBufferRecordCount;
        });
        CHECK(count != lowered.user_data.end());
        if (count != lowered.user_data.end()) {
            CHECK(count->slot == kTableSlot);
            CHECK(count->dword == 0);
        }
        check_dxil(lowered.dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }
}

TEST(shader_fixtures_translate_scalar_buffer_length_and_load) {
    const Fixture* fixture = find_fixture(kScalarBufferLength);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Srv;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    for (const std::uint32_t stride : {0u, 1u}) {
        input.stride = stride;
        const Lowered lowered = lower_fixture(*fixture, {input, output});
        check_dxil(lowered.dxil, fixture->name);
    }
}

TEST(shader_fixtures_translate_scalar_buffer_address_at_a_join) {
    const Fixture* fixture = find_fixture(kScalarBufferAddress);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    using Kind = ResourceType::Kind;
    using Source = UserData::Source;
    for (const Kind kind : {Kind::Srv, Kind::Cbv}) {
        input.kind.kind = kind;
        const Lowered lowered = lower_fixture(*fixture, {input, output});
        const Source source = kind == Kind::Cbv ? Source::ConstantBufferAddressLow : Source::ShaderBufferAddressLow;
        const auto address = std::find_if(lowered.user_data.begin(), lowered.user_data.end(),
                                          [source](const UserData& entry) { return entry.source == source; });
        CHECK(address != lowered.user_data.end());
        if (address != lowered.user_data.end()) {
            CHECK(address->index == kUserSgprs);
            CHECK(address->slot == kTableSlot);
            CHECK(address->dword == 0);
        }
        check_dxil(lowered.dxil, fixture->name);
    }
}

TEST(shader_fixtures_translate_scalar_constant_buffer_words) {
    const Fixture* fixture = find_fixture(kScalarConstantBufferWords);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Cbv;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    try {
        const Lowered lowered = lower_fixture(*fixture, {input, output});
        using Source = UserData::Source;
        for (const Source source : {Source::ConstantBufferHighWord, Source::ConstantBufferRecordCount,
                                    Source::ConstantBufferFormatWord}) {
            const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(),
                                            [source](const UserData& entry) { return entry.source == source; });
            CHECK(field != lowered.user_data.end());
            if (field != lowered.user_data.end()) {
                CHECK(field->slot == kTableSlot);
                CHECK(field->dword == 0);
            }
        }
        check_dxil(lowered.dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
    }
}

TEST(shader_fixtures_translate_masked_scalar_buffer_fields_at_a_join) {
    const Fixture* fixture = find_fixture(kScalarBufferFields);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Srv;
    input.stride = 2;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    const Lowered lowered = lower_fixture(*fixture, {input, output});
    using Source = UserData::Source;
    for (const Source source : {Source::ShaderBufferAddressHigh, Source::ShaderBufferComponentMapping}) {
        const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(),
                                        [source](const UserData& entry) { return entry.source == source; });
        CHECK(field != lowered.user_data.end());
        if (field != lowered.user_data.end()) {
            CHECK(field->slot == kTableSlot);
            CHECK(field->dword == 0);
        }
    }
    check_dxil(lowered.dxil, fixture->name);

    Fixture unknown = *fixture;
    const auto mask = std::find(unknown.words.begin(), unknown.words.end(), UserData::kBufferAddressHighMask);
    CHECK(mask != unknown.words.end());
    if (mask == unknown.words.end()) return;
    *mask |= UserData::kBufferAddressHighMask + 1;
    bool rejected = false;
    try {
        (void)lower_fixture(unknown, {input, output});
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("a descriptor's address or format read as data") !=
                   std::string_view::npos;
    }
    CHECK(rejected);
}

TEST(shader_fixtures_translate_structured_buffer_stride_and_zero_test) {
    const Fixture* fixture = find_fixture(kScalarStructuredFields);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Srv;
    input.stride = 16;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    const Lowered lowered = lower_fixture(*fixture, {input, output});
    using Source = UserData::Source;
    for (const Source source : {Source::ShaderBufferAddressLow, Source::StructuredBufferHighWord,
                                Source::StructuredBufferFormatWord}) {
        const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(),
                                        [source](const UserData& data) { return data.source == source; });
        CHECK(field != lowered.user_data.end());
        if (field != lowered.user_data.end()) {
            CHECK(field->slot == kTableSlot);
            CHECK(field->dword == 0);
        }
    }
    check_dxil(lowered.dxil, fixture->name);
}

TEST(shader_fixtures_translate_raw_buffer_high_word_with_vector_user) {
    const Fixture* fixture = find_fixture(kScalarRawHighWord);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Srv;
    input.stride = 1;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    const Lowered lowered = lower_fixture(*fixture, {input, output});
    using Source = UserData::Source;
    for (const Source source : {Source::LinearRawBufferHighWord, Source::LinearRawBufferFormatWord}) {
        const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(),
                                        [source](const UserData& data) { return data.source == source; });
        CHECK(field != lowered.user_data.end());
        if (field != lowered.user_data.end()) {
            CHECK(field->slot == kTableSlot);
            CHECK(field->dword == 0);
        }
    }
    check_dxil(lowered.dxil, fixture->name);
}

TEST(shader_fixtures_translate_the_typed_buffer_format_word) {
    const Fixture* fixture = find_fixture(kScalarTypedFormat);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Srv;
    input.stride = 4;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    const Lowered lowered = lower_fixture(*fixture, {input, output});
    using Source = UserData::Source;
    for (const Source source : {Source::TypedBufferHighWord, Source::TypedBufferFormatWord}) {
        const auto field = std::find_if(lowered.user_data.begin(), lowered.user_data.end(),
                                        [source](const UserData& data) { return data.source == source; });
        CHECK(field != lowered.user_data.end());
        if (field != lowered.user_data.end()) {
            CHECK(field->slot == kTableSlot);
            CHECK(field->dword == 0);
        }
    }
    check_dxil(lowered.dxil, fixture->name);
}

TEST(shader_fixtures_translate_scalar_reads_of_image_addresses) {
    const Fixture* fixture = find_fixture(kScalarImageAddress);
    CHECK(fixture != nullptr);
    if (fixture == nullptr) return;
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Srv;
    input.dwords = 8;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = kSecondDescriptorDword;
    const Lowered lowered = lower_fixture(*fixture, {input, output});
    const auto address = std::find_if(lowered.user_data.begin(), lowered.user_data.end(), [](const UserData& entry) {
        return entry.source == UserData::Source::ImageBaseAddressLow;
    });
    CHECK(address != lowered.user_data.end());
    if (address != lowered.user_data.end()) {
        CHECK(address->index == kUserSgprs);
        CHECK(address->slot == kTableSlot);
        CHECK(address->dword == 0);
    }
    const auto sampling = std::find_if(lowered.user_data.begin(), lowered.user_data.end(), [](const UserData& entry) {
        return entry.source == UserData::Source::ImageSamplingFields;
    });
    CHECK(sampling != lowered.user_data.end());
    if (sampling != lowered.user_data.end()) {
        CHECK(sampling->slot == kTableSlot);
        CHECK(sampling->dword == 0);
    }
    check_dxil(lowered.dxil, fixture->name);

    Fixture unknown = *fixture;
    constexpr std::uint32_t kPerformanceBit = 1u << UserData::kImageSamplingPerformanceShift;
    constexpr std::uint32_t kCornerSampling = 1u << 23;
    const auto mask = std::find(unknown.words.begin(), unknown.words.end(), kPerformanceBit);
    CHECK(mask != unknown.words.end());
    if (mask == unknown.words.end()) return;
    *mask = kCornerSampling;
    bool rejected = false;
    try {
        (void)lower_fixture(unknown, {input, output});
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("a descriptor's address or format read as data") !=
                   std::string_view::npos;
    }
    CHECK(rejected);
}
