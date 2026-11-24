// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"
#include "shader_lowering.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <iterator>
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

Lowered lower_fixture(const Fixture& fixture) {
    return lower_compute(fixture.words, kFixtureVgprs,
                         {root_param(RootType::Kind::Constants, kConstantsUserData, kConstantsSlot),
                          root_param(RootType::Kind::Table, kTableUserData, kTableSlot)},
                         {used_uav(kTableUserData, kTableSlot)}, kUserSgprs << kUserSgprShift, {1, 1, 1});
}

}  // namespace

TEST(shader_fixtures_translate_and_validate) {
    CHECK(std::size(kFixtures) > 0);
    for (const Fixture& fixture : kFixtures) {
        bool translated = true;
        Lowered lowered;
        try {
            lowered = lower_fixture(fixture);
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
    UsedResource input = used_uav(kTableUserData, kTableSlot);
    input.kind.kind = ResourceType::Kind::Srv;
    UsedResource output = used_uav(kTableUserData, kTableSlot);
    output.table_offset_dwords = 8;
    const std::vector<std::uint32_t> words{
        0xBE850380u, 0xF4080202u, 0xFA000000u, 0xF4080302u, 0xFA000020u, 0xBF8CC07Fu,
        0xF4200184u, 0xFA000014u, 0xBF8CC07Fu, 0x7E00020Au, 0x7E020206u,
        0xE0700000u, 0x80030000u, 0xE0700004u, 0x80030100u, 0xBF810000u,
    };
    for (const std::uint32_t stride : {0u, 1u}) {
        input.stride = stride;
        const Lowered lowered = lower_compute(
            words, kFixtureVgprs,
            {root_param(RootType::Kind::Constants, kConstantsUserData, kConstantsSlot),
             root_param(RootType::Kind::Table, kTableUserData, kTableSlot)},
            {input, output}, kUserSgprs << kUserSgprShift, {1, 1, 1});
        check_dxil(lowered.dxil, "scalar buffer length and load");
    }
}
