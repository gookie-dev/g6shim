// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"
#include "shader_lowering.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
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

}  // namespace

TEST(shader_fixtures_translate_and_validate) {
    CHECK(std::size(kFixtures) > 0);
    for (const Fixture& fixture : kFixtures) {
        if (fixture.name == kScalarBufferLength || fixture.name == kScalarBufferAddress ||
            fixture.name == kScalarBufferFields || fixture.name == kScalarStructuredFields ||
            fixture.name == kScalarRawHighWord || fixture.name == kScalarTypedFormat ||
            fixture.name == kScalarImageAddress || fixture.name == kScalarConstantBufferWords ||
            fixture.name == kImageSampleBiasA16 || fixture.name == kScalarBufferCountOnly ||
            fixture.name == kPackedInputPartner || fixture.name == kSelectReadLaneMask)
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
    native.stage = HwStage::Ps;
    native.vgprs = kFixtureVgprs;
    native.io = PsIo{};
    native.layout = {root_param(RootType::Kind::Table, kTableUserData, kTableSlot),
                     root_param(RootType::Kind::SamplerHeap, kSamplerUserData, kSamplerSlot)};
    native.used = {texture, output};
    native.code = bytes_of(fixture->words);
    try {
        const Lowered lowered = lower_native(native);
        check_dxil(lowered.dxil, fixture->name);
    } catch (const std::exception& error) {
        std::printf("fixture %s did not translate: %s\n", fixture->name, error.what());
        CHECK(false);
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
