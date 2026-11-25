// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/emit/emit.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "g6shim/shader/emit/lower.hpp"
#include "g6shim/shader/error.hpp"

namespace g6shim::shader::emit {

namespace {

constexpr std::uint32_t kPsvThreadsOffset = 4 + 36;
constexpr std::uint32_t kPsvMinimumSize = 48;
constexpr std::uint32_t kSignatureRowSize = 32;
constexpr std::uint8_t kComparisonFilter = 0x80;

std::uint32_t u32_at(std::span<const std::uint8_t> data, std::size_t offset) {
    if (offset + 4 > data.size()) detail::fail_malformed("truncated");
    return static_cast<std::uint32_t>(data[offset]) |
           static_cast<std::uint32_t>(data[offset + 1]) << 8 |
           static_cast<std::uint32_t>(data[offset + 2]) << 16 |
           static_cast<std::uint32_t>(data[offset + 3]) << 24;
}

std::optional<std::uint8_t> kind_of(std::uint32_t raw) {
    constexpr auto u8 = [](std::uint32_t value) { return static_cast<std::uint8_t>(value); };
    switch (raw) {
        case 0: return u8(0);
        case 1: return u8(3);
        case 2: return u8(6);
        case 3: return u8(7);
        case 4: return u8(4);
        case 5: return u8(5);
        case 6: return u8(1);
        case 7: return u8(10);
        case 8: return u8(2);
        case 9: return u8(13);
        case 10: return u8(12);
        case 64: return u8(16);
        case 65: return u8(17);
        case 66: return u8(14);
        case 67: return u8(19);
        case 68: return u8(18);
        case 69: return u8(20);
        case 70: return u8(15);
        case 25: return u8(30);
        default: return std::nullopt;
    }
}

std::vector<SigElement> signature(std::span<const std::uint8_t> part) {
    const std::uint32_t count = u32_at(part, 0);
    const std::uint32_t base = u32_at(part, 4);
    std::vector<SigElement> out;
    out.reserve(std::min<std::size_t>(count, 1024));
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = static_cast<std::size_t>(base) + kSignatureRowSize * i;
        const std::uint32_t name_at = u32_at(part, at + 4);
        if (name_at > part.size()) detail::fail_malformed("signature name out of bounds");
        std::size_t length = 0;
        while (name_at + length < part.size() && part[name_at + length] != 0) ++length;
        if (at + 24 >= part.size()) detail::fail_malformed("truncated");
        const std::uint8_t mask = part[at + 24];
        if (u32_at(part, at + 28) != 0)
            detail::fail_unsupported(0, "a minimum-precision signature element");
        if (u32_at(part, at) != 0)
            detail::fail_unsupported(0, "a signature element of another stream");
        const std::optional<std::uint8_t> found = kind_of(u32_at(part, at + 12));
        if (!found) detail::fail_unsupported(0, "a system value in the signature");
        const std::uint8_t kind = *found;
        const std::uint32_t raw = u32_at(part, at + 16);
        std::uint8_t comp = 0;
        if (kind == 13) {
            comp = 5;
        } else if (raw == 1 || raw == 4) {
            comp = 5;
        } else if (raw == 2 || raw == 5) {
            comp = 4;
        } else if (raw == 3 || raw == 6) {
            comp = 9;
        } else if (raw == 0) {
            detail::fail_unsupported(0, "a signature element of unknown type");
        } else {
            detail::fail_unsupported(0, "a 64-bit signature element");
        }
        const std::uint32_t register_offset = u32_at(part, at + 20);
        SigElement element;
        element.name.assign(reinterpret_cast<const char*>(part.data() + name_at), length);
        element.index = u32_at(part, at + 8);
        element.kind = kind;
        element.comp = comp;
        element.interp = 0;
        if (register_offset != UINT32_MAX) element.row = register_offset;
        element.col = static_cast<std::uint8_t>(
            std::min(static_cast<unsigned int>(std::countr_zero(static_cast<unsigned int>(mask))), 3u));
        element.cols = static_cast<std::uint8_t>(
            std::max(static_cast<unsigned int>(std::popcount(static_cast<unsigned int>(mask))), 1u));
        element.usage = 0;
        element.half = raw >= 4 && raw <= 6;
        out.push_back(std::move(element));
    }
    return out;
}

}  // namespace

std::uint32_t HostShader::user_data_dwords() const {
    std::uint32_t n = 0;
    for (const UserData& entry : user_data) n = std::max(n, entry.index + 1);
    return (n + 3) / 4 * 4;
}

std::uint32_t parse_loop_limit(std::optional<std::string_view> value) {
    if (!value) return kLoopLimit;
    std::string_view text = *value;
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' ||
                             text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' ||
                             text.back() == '\r'))
        text.remove_suffix(1);
    std::uint64_t parsed = 0;
    bool ok = !text.empty();
    std::string_view digits = text;
    int base = 10;
    if (text.size() >= 2 && text.substr(0, 2) == "0x") {
        digits = text.substr(2);
        base = 16;
        ok = !digits.empty();
    }
    for (char c : digits) {
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else ok = false;
        if (digit < 0) break;
        parsed = parsed * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit);
        if (parsed > UINT32_MAX) ok = false;
    }
    if (!ok || parsed == 0) return kLoopLimit;
    return static_cast<std::uint32_t>(parsed);
}

std::uint32_t loop_limit() { return kLoopLimit; }

HostShader translate(std::span<const std::uint8_t> container) {
    const Container parsed = Container::parse(container);
    const Part* part = parsed.part(kDxil);
    if (part == nullptr) detail::fail_malformed("no DXIL part");
    const std::vector<NativeProgram> programs = native_programs(part->data);
    const NativeProgram* native = nullptr;
    for (const NativeProgram& program : programs) {
        if (program.stage == HwStage::Vs || program.stage == HwStage::Ps ||
            program.stage == HwStage::Cs || program.stage == HwStage::Ms ||
            program.stage == HwStage::Gs) {
            native = &program;
            break;
        }
    }
    if (native == nullptr)
        detail::fail_unsupported(0, "a container without a vertex, pixel, compute, mesh or geometry program");
    const Program program = ::g6shim::shader::build(native->patched_code(), native->wave64, native->vgprs);
    std::exception_ptr structure_failure;
    std::optional<Structured> structured;
    try {
        structured = structure(program);
    } catch (...) {
        structure_failure = std::current_exception();
    }
    Io io;
    if (native->stage == HwStage::Gs) io = raise(program, *native);
    const Resources resources = track(program, *native, structured ? &*structured : nullptr,
                                      io.ring_address_loads);
    if (native->stage != HwStage::Gs) io = raise(program, *native);
    if (!structured) std::rethrow_exception(structure_failure);
    return emit(parsed, *native, program, *structured, io, resources);
}

HostShader emit(const Container& container, const NativeProgram& native, const Program& program,
                const Structured& structured, const Io& io, const Resources& resources) {
    const auto sig = [&container](FourCC fourcc) -> std::vector<SigElement> {
        const Part* part = container.part(fourcc);
        if (part == nullptr) return {};
        return signature(part->data);
    };
    const bool pixel = native.stage == HwStage::Ps;
    std::vector<SigElement> inputs = sig(kIsg1);
    std::vector<SigElement> outputs = sig(kOsg1);
    std::vector<SigElement>& interpolated = pixel ? inputs : outputs;
    const auto assign_interp = [](SigElement& e) {
        const std::uint8_t kind = e.kind;
        const std::uint8_t comp = e.comp;
        if (kind == 3) e.interp = 4;
        else if (kind == 13) e.interp = 1;
        else if ((kind == 0 || kind == 6 || kind == 7) && comp == 9) e.interp = 2;
        else if (kind == 0) e.interp = 1;
        else e.interp = 0;
    };
    for (SigElement& e : interpolated) assign_interp(e);
    if (native.stage == HwStage::Gs)
        for (SigElement& e : inputs) assign_interp(e);
    if (pixel && std::any_of(outputs.begin(), outputs.end(),
                             [](const SigElement& e) { return e.kind == 18 || e.kind == 19; })) {
        for (SigElement& e : inputs)
            if (e.kind == 3) e.interp = 5;
    }
    std::vector<SigElement> prims = sig(kPsg1);
    prims.erase(std::remove_if(prims.begin(), prims.end(),
                               [](const SigElement& e) { return e.kind == 10 || e.kind == 30; }),
                prims.end());
    for (SigElement& e : prims) e.interp = 1;
    std::uint32_t mesh_stride = 0;
    if (const auto* mesh = std::get_if<MeshIo>(&native.io)) {
        std::uint32_t threads = mesh->threads;
        const Part* psv = container.part(kPsv0);
        if (psv != nullptr && psv->data.size() >= 4 && u32_at(psv->data, 0) >= kPsvMinimumSize) {
            const std::uint32_t found = u32_at(psv->data, kPsvThreadsOffset);
            threads = std::max(found, std::max(mesh->max_vertices, mesh->max_primitives));
        }
        mesh_stride = threads;
    }
    Parts parts;
    parts.native = &native;
    parts.p = &program;
    parts.st = &structured;
    parts.io = &io;
    parts.res = &resources;
    parts.inputs = std::move(inputs);
    parts.outputs = std::move(outputs);
    parts.prims = std::move(prims);
    parts.mesh_stride = mesh_stride;
    Lowered lowered = lower(parts);
    HostShader out;
    out.stage = native.stage;
    out.dxil = std::move(lowered.dxil);
    out.user_data = std::move(lowered.user_data);
    out.samplers = lowered.samplers;
    out.static_samplers.reserve(lowered.samplers.size());
    for (std::size_t n = 0; n < lowered.samplers.size(); ++n) {
        StaticSampler sampler = StaticSampler::from_words(lowered.samplers[n]);
        if ((lowered.cmp_samplers >> n & 1) != 0) sampler.filter |= kComparisonFilter;
        out.static_samplers.push_back(sampler);
    }
    out.root_cbvs = std::move(lowered.root_cbvs);
    out.root_srvs = std::move(lowered.root_srvs);
    return out;
}

}  // namespace g6shim::shader::emit
