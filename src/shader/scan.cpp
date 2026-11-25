// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/scan.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "g6shim/shader/dxbc.hpp"
#include "g6shim/shader/emit/emit.hpp"
#include "g6shim/shader/error.hpp"
#include "g6shim/shader/gfx10.hpp"
#include "g6shim/shader/inflate.hpp"
#include "g6shim/shader/io.hpp"
#include "g6shim/shader/ir.hpp"
#include "g6shim/shader/native.hpp"
#include "g6shim/shader/resource.hpp"
#include "g6shim/shader/structure.hpp"
#include "reader.hpp"

namespace g6shim::shader {

namespace {

constexpr std::array<std::uint8_t, 4> kDxbcMagic{'D', 'X', 'B', 'C'};
constexpr std::array<std::uint8_t, 4> kDxilMagic{'D', 'X', 'I', 'L'};
constexpr std::array<std::uint8_t, 4> kMarkerMagic{0xE7, 0xDC, 0x0D, 0x00};
constexpr std::size_t kMarkerLeading = 6;
constexpr std::size_t kRecordHeader = 12;
constexpr std::size_t kMinimumRecord = 12;
constexpr std::size_t kContainerSizeOffset = 0x18;
constexpr std::size_t kProgramHeader = 24;
constexpr std::size_t kDxilMagicOffset = 8;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::uint64_t kSecondOffset = 0x9E3779B97F4A7C15ull;
constexpr std::uint64_t kSecondPrime = 0x9E3779B185EBCA87ull;
constexpr char kPackedFailure[] = "packed shader record";
constexpr char kContainerFailure[] = "container";
constexpr char kDxilFailure[] = "DXIL";
constexpr char kNativeFailure[] = "native";
constexpr char kDecodeFailure[] = "instruction";
constexpr char kTranslationPrefix[] = "translation: ";
constexpr char kIsaPrefix[] = "GFX10 code at ";

struct Failure {
    std::string reason;
    std::string message;
};

struct Outcome {
    std::string stage = "unknown";
    bool decoded = false;
    bool translated = false;
    std::string reason;
    std::string message;
    std::vector<Failure> translation_failures;
};

template <typename OnMatch>
void for_each_magic(std::span<const std::uint8_t> data, const std::array<std::uint8_t, 4>& magic, OnMatch&& on_match) {
    std::size_t at = 0;
    while (at + magic.size() <= data.size()) {
        const void* hit = std::memchr(data.data() + at, magic[0], data.size() - at - magic.size() + 1);
        if (hit == nullptr) break;
        const std::size_t found =
            static_cast<std::size_t>(static_cast<const std::uint8_t*>(hit) - data.data());
        if (std::memcmp(data.data() + found, magic.data(), magic.size()) == 0) on_match(found);
        at = found + 1;
    }
}

std::optional<std::uint32_t> u32_at(std::span<const std::uint8_t> data, std::size_t at) {
    if (at > data.size() || data.size() - at < 4) return std::nullopt;
    const std::uint32_t value = static_cast<std::uint32_t>(data[at]) |
                                static_cast<std::uint32_t>(data[at + 1]) << 8 |
                                static_cast<std::uint32_t>(data[at + 2]) << 16 |
                                static_cast<std::uint32_t>(data[at + 3]) << 24;
    return value;
}

bool is_graphic(std::uint8_t byte) { return byte >= 0x21 && byte <= 0x7E; }

bool valid_utf8(std::span<const std::uint8_t> text) {
    constexpr std::array<std::uint32_t, 4> kMinimum{0x0, 0x80, 0x800, 0x10000};
    std::size_t at = 0;
    while (at < text.size()) {
        const std::uint8_t lead = text[at];
        if (lead < 0x80) {
            ++at;
            continue;
        }
        std::size_t extra = 0;
        std::uint32_t code = 0;
        if ((lead & 0xE0) == 0xC0) {
            extra = 1;
            code = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
            code = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
            code = lead & 0x07;
        } else {
            return false;
        }
        if (at + extra >= text.size()) return false;
        for (std::size_t index = 1; index <= extra; ++index) {
            if ((text[at + index] & 0xC0) != 0x80) return false;
            code = code << 6 | (text[at + index] & 0x3F);
        }
        if (code < kMinimum[extra] || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
        at += extra + 1;
    }
    return true;
}

std::optional<std::string> record_name(std::span<const std::uint8_t> file, std::size_t nul) {
    std::size_t run = 0;
    for (std::size_t at = nul; at != 0;) {
        --at;
        if (!is_graphic(file[at])) {
            run = at + 1;
            break;
        }
    }
    const std::size_t start = run == 0 ? 0 : run - 1;
    for (std::size_t length_byte = start; length_byte < nul; ++length_byte) {
        if (static_cast<std::size_t>(file[length_byte]) != nul - length_byte) continue;
        const auto candidate = file.subspan(length_byte + 1, nul - length_byte - 1);
        if (!valid_utf8(candidate)) return std::nullopt;
        return std::string(candidate.begin(), candidate.end());
    }
    return std::nullopt;
}

std::optional<PackedShaderRecord> record_at(std::span<const std::uint8_t> file, std::size_t at) {
    if (at < kMarkerLeading) return std::nullopt;
    const auto size = u32_at(file, at - 4);
    const auto unpacked_size = u32_at(file, at + 8);
    if (!size || !unpacked_size) return std::nullopt;
    const auto next_size = u32_at(file, at + 4);
    if (!next_size || *next_size != *size) return std::nullopt;
    if (*size < kMinimumRecord || *unpacked_size > kMaxPackedUnpacked) return std::nullopt;
    PackedShaderRecord record;
    record.offset = at;
    record.kind = file[at - 5];
    const std::size_t nul = at - kMarkerLeading;
    if (file[nul] == 0) {
        if (auto name = record_name(file, nul)) {
            record.has_name = true;
            record.name = std::move(*name);
        }
    }
    if (*size > file.size() - at) return std::nullopt;
    record.packed = file.subspan(at + kRecordHeader, *size - kRecordHeader);
    record.unpacked_size = *unpacked_size;
    return record;
}

std::optional<ShaderKind> header_kind(std::span<const std::uint8_t> part) {
    const auto version = u32_at(part, 0);
    const auto dwords = u32_at(part, 4);
    if (!version || !dwords || part.size() < kDxilMagicOffset + kDxilMagic.size()) return std::nullopt;
    const std::uint64_t size = static_cast<std::uint64_t>(*dwords) * 4;
    if (size > part.size() || size < kProgramHeader) return std::nullopt;
    if (std::memcmp(part.data() + kDxilMagicOffset, kDxilMagic.data(), kDxilMagic.size()) != 0)
        return std::nullopt;
    return shader_kind_from_raw(static_cast<std::uint16_t>(*version >> 16));
}

std::vector<std::uint8_t> translate_program(const Container& container, const NativeProgram& native) {
    const Program program = build(native.patched_code(), native.wave64, native.vgprs);
    const Structured structured = structure(program);
    Io io;
    if (native.stage == HwStage::Gs) io = raise(program, native);
    const Resources resources = track(program, native, &structured, io.ring_address_loads);
    if (native.stage != HwStage::Gs) io = raise(program, native);
    emit::HostShader shader = emit::emit(container, native, program, structured, io, resources);
    return std::move(shader.dxil);
}

std::string translation_reason(const ParseError& error) {
    std::string reason = error.what();
    if (reason.rfind(kIsaPrefix, 0) == 0) {
        const std::size_t colon = reason.find(": ");
        if (colon != std::string::npos) reason = reason.substr(colon + 2);
    }
    return kTranslationPrefix + reason;
}

Outcome analyze(std::span<const std::uint8_t> bytes, const std::string& hash,
                const ShaderScanner::TranslatedWriter& writer) {
    Outcome outcome;
    try {
        const Container container = Container::parse(bytes);
        const Part* const dxil = container.part(kDxil);
        if (dxil == nullptr) {
            outcome.reason = kDxilFailure;
            outcome.message = "no DXIL part";
            return outcome;
        }
        const std::optional<ShaderKind> kind = header_kind(dxil->data);
        if (!kind) {
            outcome.reason = kDxilFailure;
            outcome.message = "program header does not parse";
            return outcome;
        }
        outcome.stage = shader_stage(*kind);
        std::vector<NativeProgram> programs;
        try {
            programs = native_programs(dxil->data);
        } catch (const ParseError& error) {
            outcome.reason = kNativeFailure;
            outcome.message = error.what();
            return outcome;
        }
        if (programs.empty()) {
            outcome.reason = kNativeFailure;
            outcome.message = "no native program";
            return outcome;
        }
        for (const NativeProgram& program : programs) {
            try {
                decode(program.code);
            } catch (const ParseError& error) {
                outcome.reason = kDecodeFailure;
                outcome.message = error.what();
                return outcome;
            }
        }
        outcome.decoded = true;
        for (const NativeProgram& program : programs) {
            try {
                std::vector<std::uint8_t> dxil_bytes = translate_program(container, program);
                outcome.translated = true;
                if (writer) writer(hash, dxil_bytes);
            } catch (const ParseError& error) {
                outcome.translation_failures.push_back({translation_reason(error), error.what()});
            }
        }
    } catch (const ParseError& error) {
        outcome.reason = kContainerFailure;
        outcome.message = error.what();
    }
    return outcome;
}

std::string hash_hex(std::uint64_t value) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (std::size_t index = 0; index < out.size(); ++index)
        out[out.size() - 1 - index] = kDigits[(value >> (4 * index)) & 0xF];
    return out;
}

}  // namespace

std::vector<std::uint8_t> PackedShaderRecord::unpack() const {
    std::vector<std::uint8_t> out = inflate_raw(packed, unpacked_size);
    if (out.size() != unpacked_size) throw ParseError(ErrorKind::Malformed, "packed shader record does not inflate to its stated size");
    return out;
}

std::vector<PackedShaderRecord> packed_shader_records(std::span<const std::uint8_t> file) {
    std::vector<PackedShaderRecord> out;
    for_each_magic(file, kMarkerMagic, [&](std::size_t at) {
        if (auto record = record_at(file, at)) out.push_back(std::move(*record));
    });
    return out;
}

std::vector<StoredShaderContainer> stored_shader_containers(std::span<const std::uint8_t> file) {
    std::vector<StoredShaderContainer> out;
    for_each_magic(file, kDxbcMagic, [&](std::size_t at) {
        const std::span<const std::uint8_t> bytes = file.subspan(at);
        try {
            Container::parse(bytes);
        } catch (const ParseError&) {
            return;
        }
        const std::size_t total = detail::read_u32(bytes, kContainerSizeOffset);
        if (total > bytes.size()) return;
        out.push_back(StoredShaderContainer{at, bytes.first(total)});
    });
    return out;
}

std::string shader_stage(ShaderKind kind) {
    switch (kind.id) {
        case ShaderKind::Id::Vertex: return "vs";
        case ShaderKind::Id::Pixel: return "ps";
        case ShaderKind::Id::Geometry: return "gs";
        case ShaderKind::Id::Hull: return "hs";
        case ShaderKind::Id::Domain: return "ds";
        case ShaderKind::Id::Compute: return "cs";
        case ShaderKind::Id::Mesh: return "ms";
        case ShaderKind::Id::Amplification: return "as";
        default: return "unknown";
    }
}

std::size_t ShaderScanner::KeyHash::operator()(const Key& key) const {
    std::size_t hash = std::hash<std::uint64_t>{}(key.first);
    hash ^= std::hash<std::uint64_t>{}(key.second) << 1;
    hash ^= std::hash<std::size_t>{}(key.size) << 2;
    return hash;
}

bool ShaderScanner::remember(std::span<const std::uint8_t> bytes, std::string& hash) {
    std::uint64_t first = kFnvOffset;
    std::uint64_t second = kSecondOffset;
    for (const std::uint8_t byte : bytes) {
        first = (first ^ byte) * kFnvPrime;
        second = (second ^ byte) * kSecondPrime;
    }
    if (!seen_.insert(Key{first, second, bytes.size()}).second) return false;
    hash = hash_hex(first);
    return true;
}

void ShaderScanner::add_failure(const std::string& reason, const std::string& message) {
    FailureCount& failure = totals_.failures[reason];
    if (failure.count == 0) failure.first = message;
    ++failure.count;
}

FileCounts ShaderScanner::add_file(std::span<const std::uint8_t> file) {
    FileCounts counts;
    std::vector<std::string> hashes;
    std::vector<std::vector<std::uint8_t>> fresh;

    const auto take = [&](std::span<const std::uint8_t> bytes) {
        ++counts.containers;
        std::string hash;
        if (!remember(bytes, hash)) return;
        hashes.push_back(std::move(hash));
        fresh.emplace_back(bytes.begin(), bytes.end());
        ++counts.unique;
    };

    for (const PackedShaderRecord& record : packed_shader_records(file)) {
        std::vector<std::uint8_t> unpacked;
        try {
            unpacked = record.unpack();
        } catch (const ParseError& error) {
            add_failure(kPackedFailure, error.what());
            continue;
        }
        try {
            Container::parse(unpacked);
        } catch (const ParseError&) {
            continue;
        }
        const std::size_t total = detail::read_u32(unpacked, kContainerSizeOffset);
        if (total > unpacked.size()) continue;
        take(std::span<const std::uint8_t>(unpacked).first(total));
    }

    for (const StoredShaderContainer& container : stored_shader_containers(file)) take(container.bytes);

    std::vector<Outcome> outcomes(fresh.size());
    if (fresh.size() == 1) {
        outcomes[0] = analyze(fresh[0], hashes[0], writer_);
    } else if (!fresh.empty()) {
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        const unsigned workers = static_cast<unsigned>(std::min<std::size_t>(cores, fresh.size()));
        std::atomic<std::size_t> next{0};
        std::vector<std::jthread> threads;
        threads.reserve(workers);
        for (unsigned index = 0; index < workers; ++index) {
            threads.emplace_back([&] {
                for (;;) {
                    const std::size_t job = next.fetch_add(1);
                    if (job >= fresh.size()) break;
                    outcomes[job] = analyze(fresh[job], hashes[job], writer_);
                }
            });
        }
    }

    for (const Outcome& outcome : outcomes) {
        StageCounts& stage = totals_.stages[outcome.stage];
        ++stage.total;
        if (outcome.translated) {
            ++stage.translated;
        } else if (outcome.decoded) {
            ++stage.decoded;
        } else {
            ++stage.failed;
            add_failure(outcome.reason, outcome.message);
        }
        for (const Failure& failure : outcome.translation_failures)
            add_failure(failure.reason, failure.message);
    }

    totals_.containers += counts.containers;
    totals_.unique += counts.unique;
    if (counts.containers != 0) ++totals_.files;
    return counts;
}

}  // namespace g6shim::shader
