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
constexpr std::string_view kScalarBufferAddress = "scalar_buffer_address";
constexpr std::string_view kScalarBufferFields = "scalar_buffer_fields";
constexpr std::string_view kScalarStructuredFields = "scalar_structured_fields";
constexpr std::string_view kScalarRawHighWord = "scalar_raw_buffer_high_word";
constexpr std::string_view kScalarImageAddress = "scalar_image_address";

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
            fixture.name == kScalarRawHighWord || fixture.name == kScalarImageAddress)
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
    for (const Source source : {Source::StructuredBufferHighWord, Source::StructuredBufferFormatWord}) {
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
    check_dxil(lowered.dxil, fixture->name);
}
