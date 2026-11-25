// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"
#include "shader_lowering.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "g6shim/shader/emit/shader.hpp"
#include "g6shim/shader/emit/validator.hpp"

namespace {

using namespace g6shim::shader::emit;

std::vector<std::uint8_t> constant_color() {
    Shader shader(true);
    SigElement target;
    target.name = "SV_Target";
    target.index = 0;
    target.kind = 16;
    target.comp = 9;
    target.interp = 0;
    target.row = 0;
    target.col = 0;
    target.cols = 4;
    target.usage = 15;
    shader.outputs.push_back(target);
    const std::size_t block = shader.entry;
    const float colours[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    for (std::uint8_t channel = 0; channel < 4; ++channel) {
        const std::vector<V> args = {shader.m.i32(0), shader.m.i32(0), shader.m.i8(channel),
                                     shader.m.f32(std::bit_cast<std::uint32_t>(colours[channel]))};
        shader.op(block, "storeOutput.f32", Attr::NoUnwind, shader.t.void_type, 5, args);
    }
    shader.m.terminate(block, Term::ret());
    return shader.finish();
}

TEST(validator_signs_a_constant_colour) {
    const std::optional<std::filesystem::path> path = Validator::default_path();
    std::optional<Validator> validator;
    std::string reason;
    try {
        validator.emplace(Validator::load(path ? &*path : nullptr));
    } catch (const std::exception& error) {
        reason = error.what();
    }
    if (!validator) {
        CHECK(std::getenv("G6SHIM_DXIL") == nullptr);
        std::printf("no dxil.dll; skipped: %s\n", reason.c_str());
        return;
    }
    std::vector<std::uint8_t> container = constant_color();
    std::array<std::uint8_t, 16> ours{};
    std::copy_n(container.begin() + 4, 16, ours.begin());
    bool accepted = true;
    try {
        validator->validate(container);
    } catch (const std::exception& error) {
        std::printf("validation failed: %s\n", error.what());
        accepted = false;
    }
    CHECK(accepted);
    CHECK(std::equal(ours.begin(), ours.end(), container.begin() + 4));
}

TEST(validator_accepts_signature_elements_sharing_a_register) {
    Shader shader(false);
    shader.outputs = {
        SigElement{"TEXCOORD", 1, 0, 9, 2, 0, 2, 1, 1},
        SigElement{"TEXCOORD", 0, 0, 9, 2, 0, 0, 1, 1},
    };
    for (std::uint32_t id = 0; id < shader.outputs.size(); ++id) {
        const V value = shader.m.f32(std::bit_cast<std::uint32_t>(1.0f));
        shader.op(shader.entry, "storeOutput.f32", Attr::NoUnwind, shader.t.void_type, 5,
                  {shader.m.i32(id), shader.m.i32(0), shader.m.i8(0), value});
    }
    shader.m.terminate(shader.entry, Term::ret());
    g6shim::test::lowering::check_dxil(shader.finish(), "signature elements sharing a register");
}

}  // namespace
