// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_lowering.hpp"

#include "check.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>

#include "g6shim/shader/emit/container.hpp"
#include "g6shim/shader/emit/emit.hpp"
#include "g6shim/shader/error.hpp"

namespace g6shim::test::lowering {

using namespace g6shim::shader;
using namespace g6shim::shader::emit;

std::vector<std::uint8_t> bytes_of(const std::vector<std::uint32_t>& words) {
    std::vector<std::uint8_t> code;
    code.reserve(words.size() * 4);
    for (std::uint32_t word : words) {
        code.push_back(static_cast<std::uint8_t>(word));
        code.push_back(static_cast<std::uint8_t>(word >> 8));
        code.push_back(static_cast<std::uint8_t>(word >> 16));
        code.push_back(static_cast<std::uint8_t>(word >> 24));
    }
    return code;
}

RootParam root_param(RootType::Kind kind, std::uint8_t user_data, std::uint8_t slot) {
    RootParam param;
    param.root = RootType{kind, 0};
    param.table_offset_dwords = 0;
    param.sgprs = kind == RootType::Kind::Constants ? 4 : 1;
    param.base_register = 0;
    param.user_data = user_data;
    param.slot = slot;
    return param;
}

UsedResource used_uav(std::uint8_t user_data, std::uint8_t slot, std::uint8_t dwords) {
    UsedResource used;
    used.kind = ResourceType{ResourceType::Kind::Uav, 0};
    used.root = RootType{RootType::Kind::Table, 0};
    used.table_offset_dwords = 0;
    used.lower_bound = 0;
    used.stride = 0;
    used.dwords = dwords;
    used.user_data = user_data;
    used.slot = slot;
    return used;
}

Lowered lower_native(const NativeProgram& native, const std::vector<SigElement>& inputs,
                     const std::vector<SigElement>& outputs) {
    const Program program = ::g6shim::shader::build(native.patched_code(), native.wave64, native.vgprs);
    const Structured structured = structure(program);
    const Io raised = raise(program, native);
    const Resources resources = track(program, native.layout, native.stage, &structured, raised.ring_address_loads);
    Parts parts;
    parts.native = &native;
    parts.p = &program;
    parts.st = &structured;
    parts.io = &raised;
    parts.res = &resources;
    parts.inputs = inputs;
    parts.outputs = outputs;
    return lower(parts);
}

Lowered lower_compute(const std::vector<std::uint32_t>& words, std::uint16_t vgprs,
                      const std::vector<RootParam>& layout, const std::vector<UsedResource>& used,
                      std::uint32_t rsrc2, std::array<std::uint32_t, 3> threads) {
    NativeProgram native;
    native.stage = HwStage::Cs;
    native.wave64 = false;
    native.vgprs = vgprs;
    native.layout = layout;
    native.used = used;
    ComputeIo io;
    io.rsrc2 = rsrc2;
    io.threads = threads;
    native.io = io;
    native.code = bytes_of(words);
    return lower_native(native);
}

std::optional<Validator> load_validator() {
    std::optional<Validator> validator;
    try {
        const std::optional<std::filesystem::path> path = Validator::default_path();
        validator.emplace(Validator::load(path ? &*path : nullptr));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return validator;
}

void check_dxil(const std::vector<std::uint8_t>& dxil, const char* label) {
    CHECK(!dxil.empty());
    const Container container = Container::parse(dxil);
    CHECK(container.part(kDxil) != nullptr);
    std::optional<Validator> validator = load_validator();
    if (!validator) {
        CHECK(std::getenv("G6SHIM_DXIL") == nullptr);
        std::printf("no dxil.dll; validation skipped for %s\n", label);
        return;
    }
    std::vector<std::uint8_t> copy = dxil;
    std::array<std::uint8_t, 16> ours{};
    std::copy_n(copy.begin() + 4, 16, ours.begin());
    bool accepted = true;
    try {
        validator->validate(copy);
    } catch (const std::exception& error) {
        std::printf("validation failed for %s: %s\n", label, error.what());
        accepted = false;
    }
    CHECK(accepted);
    CHECK((std::equal(ours.begin(), ours.end(), copy.begin() + 4)));
}

}  // namespace g6shim::test::lowering
