// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/emit/container.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <string_view>
#include <vector>

namespace g6shim::shader::emit {

namespace {

constexpr std::size_t kContainerHeaderSize = 0x20;
constexpr std::size_t kDigestOffset = 4;
constexpr std::size_t kHashedOffset = 20;
constexpr std::size_t kPartHeaderSize = 8;
constexpr std::uint32_t kSignatureRowSize = 32;

constexpr std::uint32_t kMd5State[4] = {0x6745'2301u, 0xefcd'ab89u, 0x98ba'dcfeu, 0x1032'5476u};

constexpr std::uint32_t kMd5Shift[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};

constexpr std::array<std::uint32_t, 64> kMd5Sine = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}

void write_u32(std::span<std::uint8_t> out, std::size_t at, std::uint32_t value) {
    out[at] = static_cast<std::uint8_t>(value);
    out[at + 1] = static_cast<std::uint8_t>(value >> 8);
    out[at + 2] = static_cast<std::uint8_t>(value >> 16);
    out[at + 3] = static_cast<std::uint8_t>(value >> 24);
}

std::uint32_t system_value(std::uint8_t kind) {
    switch (kind) {
        case 1: return 6;
        case 2: return 8;
        case 3: return 1;
        case 4: return 4;
        case 5: return 5;
        case 6: return 2;
        case 7: return 3;
        case 10: return 7;
        case 12: return 10;
        case 13: return 9;
        case 14: return 66;
        case 15: return 70;
        case 16: return 64;
        case 17: return 65;
        case 18: return 68;
        case 19: return 67;
        case 20: return 69;
        case 30: return 25;
        default: return 0;
    }
}

std::uint32_t signal_component(std::uint8_t comp) {
    switch (comp) {
        case 1:
        case 5: return 1;
        case 4: return 2;
        case 9: return 3;
        default: return 0;
    }
}

std::uint8_t component_mask(const SigElement& element) {
    return static_cast<std::uint8_t>(((1u << element.cols) - 1u) << element.col);
}

std::optional<std::size_t> find_bytes(std::span<const std::uint8_t> haystack,
                                      std::span<const std::uint8_t> needle) {
    if (needle.empty() || needle.size() > haystack.size()) return std::nullopt;
    for (std::size_t at = 0; at + needle.size() <= haystack.size(); ++at) {
        if (std::equal(needle.begin(), needle.end(), haystack.begin() + static_cast<std::ptrdiff_t>(at)))
            return at;
    }
    return std::nullopt;
}

std::uint32_t vector_count(std::span<const SigElement> elements) {
    std::uint32_t count = 0;
    for (const SigElement& element : elements)
        if (element.row) count = std::max(count, *element.row + 1);
    return count;
}

std::uint32_t scalar_count(std::span<const SigElement> elements) {
    std::uint32_t count = 0;
    for (const SigElement& element : elements)
        if (element.row)
            count = std::max(count, *element.row * 4 + static_cast<std::uint32_t>(element.col + element.cols));
    return count;
}

}  // namespace

void md5_block(std::array<std::uint32_t, 4>& state, const std::uint8_t* block) {
    std::array<std::uint32_t, 16> words{};
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::uint8_t* at = block + i * 4;
        words[i] = static_cast<std::uint32_t>(at[0]) | static_cast<std::uint32_t>(at[1]) << 8 |
                   static_cast<std::uint32_t>(at[2]) << 16 | static_cast<std::uint32_t>(at[3]) << 24;
    }
    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    for (std::uint32_t i = 0; i < 64; ++i) {
        std::uint32_t f = 0;
        std::uint32_t g = 0;
        switch (i / 16) {
            case 0:
                f = (b & c) | (~b & d);
                g = i;
                break;
            case 1:
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
                break;
            case 2:
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
                break;
            default:
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
                break;
        }
        std::uint32_t rotated = a + f + kMd5Sine[i] + words[g];
        const std::uint32_t shift = kMd5Shift[(i / 16) * 4 + i % 4];
        rotated = rotated << shift | rotated >> (32 - shift);
        const std::uint32_t next_b = b + rotated;
        a = d;
        d = c;
        c = b;
        b = next_b;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

std::vector<std::uint8_t> signature(std::span<const SigElement> elements, bool input) {
    std::vector<std::uint8_t> out;
    append_u32(out, static_cast<std::uint32_t>(elements.size()));
    append_u32(out, 8);

    std::vector<std::uint32_t> name_offsets;
    name_offsets.reserve(elements.size());
    std::uint32_t next = 8 + kSignatureRowSize * static_cast<std::uint32_t>(elements.size());
    std::vector<const std::string*> names;
    for (std::size_t i = 0; i < elements.size(); ++i) {
        const SigElement& element = elements[i];
        std::optional<std::size_t> shared;
        if (element.kind != 0) {
            for (std::size_t j = 0; j < i; ++j) {
                if (elements[j].kind == element.kind) {
                    shared = j;
                    break;
                }
            }
        }
        if (shared) {
            name_offsets.push_back(name_offsets[*shared]);
        } else {
            name_offsets.push_back(next);
            next += static_cast<std::uint32_t>(element.name.size()) + 1;
            names.push_back(&element.name);
        }
    }

    std::vector<std::size_t> order(elements.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&elements, &name_offsets](std::size_t a, std::size_t b) {
        const std::uint32_t row_a = elements[a].row.value_or(UINT32_MAX);
        const std::uint32_t row_b = elements[b].row.value_or(UINT32_MAX);
        if (row_a != row_b) return row_a < row_b;
        return name_offsets[a] < name_offsets[b];
    });

    for (const std::size_t i : order) {
        const SigElement& element = elements[i];
        const std::uint32_t offset = name_offsets[i];
        const std::uint8_t used = static_cast<std::uint8_t>(element.usage << element.col);
        const std::uint8_t rw = input ? used : static_cast<std::uint8_t>(~used & 0x0F);
        append_u32(out, 0);
        append_u32(out, offset);
        append_u32(out, element.index);
        append_u32(out, system_value(element.kind));
        append_u32(out, signal_component(element.comp));
        append_u32(out, element.row.value_or(UINT32_MAX));
        out.push_back(component_mask(element));
        out.push_back(rw);
        out.push_back(0);
        out.push_back(0);
        append_u32(out, 0);
    }
    for (const std::string* name : names) {
        out.insert(out.end(), name->begin(), name->end());
        out.push_back(0);
    }
    return out;
}

std::vector<std::uint8_t> psv(const Psv& p) {
    const std::uint32_t in_vectors = vector_count(p.inputs);
    const std::uint32_t out_vectors = vector_count(p.outputs);

    std::vector<std::uint8_t> out;
    std::array<std::uint8_t, 48> info{};
    info[0] = p.flag ? 1 : 0;
    if (p.stage == 0) {
        info[1] = std::any_of(p.inputs.begin(), p.inputs.end(), [](const SigElement& e) {
                     return e.interp == 6 || e.interp == 7 || e.kind == 12;
                 })
                     ? 1
                     : 0;
    }
    const std::uint32_t min_wave = p.wave_size.value_or(0);
    const std::uint32_t max_wave = p.wave_size.value_or(UINT32_MAX);
    info[16] = static_cast<std::uint8_t>(min_wave);
    info[17] = static_cast<std::uint8_t>(min_wave >> 8);
    info[18] = static_cast<std::uint8_t>(min_wave >> 16);
    info[19] = static_cast<std::uint8_t>(min_wave >> 24);
    info[20] = static_cast<std::uint8_t>(max_wave);
    info[21] = static_cast<std::uint8_t>(max_wave >> 8);
    info[22] = static_cast<std::uint8_t>(max_wave >> 16);
    info[23] = static_cast<std::uint8_t>(max_wave >> 24);
    info[24] = p.stage;
    if (p.geometry) {
        const Geometry& geometry = *p.geometry;
        write_u32(info, 0, geometry.input_primitive);
        write_u32(info, 4, geometry.output_topology);
        write_u32(info, 8, geometry.stream_mask);
        write_u32(info, 12, p.flag ? 1u : 0u);
        info[26] = static_cast<std::uint8_t>(geometry.max_vertices);
        info[27] = static_cast<std::uint8_t>(geometry.max_vertices >> 8);
    }
    for (std::size_t k = 0; k < p.threads.size(); ++k) {
        const std::uint32_t value = p.threads[k];
        for (std::size_t byte = 0; byte < 4; ++byte)
            info[36 + 4 * k + byte] = static_cast<std::uint8_t>(value >> (8 * byte));
    }
    info[28] = static_cast<std::uint8_t>(p.inputs.size());
    info[29] = static_cast<std::uint8_t>(p.outputs.size());
    info[31] = static_cast<std::uint8_t>(in_vectors);
    info[32] = static_cast<std::uint8_t>(out_vectors);
    if (p.mesh) {
        const Mesh& mesh = p.mesh->first;
        const std::uint32_t shared = p.mesh->second;
        write_u32(info, 0, shared);
        info[12] = static_cast<std::uint8_t>(mesh.max_vertices);
        info[13] = static_cast<std::uint8_t>(mesh.max_vertices >> 8);
        info[14] = static_cast<std::uint8_t>(mesh.max_primitives);
        info[15] = static_cast<std::uint8_t>(mesh.max_primitives >> 8);
        info[26] = static_cast<std::uint8_t>(vector_count(p.prims));
        info[27] = mesh.topology;
        info[30] = static_cast<std::uint8_t>(p.prims.size());
    }
    const bool named_entry = !p.entry_name.empty();
    const std::uint32_t info_size = static_cast<std::uint32_t>(info.size()) +
                                    (named_entry ? static_cast<std::uint32_t>(sizeof(std::uint32_t)) : 0u);
    append_u32(out, info_size);
    out.insert(out.end(), info.begin(), info.end());
    const std::size_t entry_offset = out.size();
    if (named_entry) append_u32(out, 0);

    append_u32(out, static_cast<std::uint32_t>(p.bindings.size()));
    if (!p.bindings.empty()) {
        append_u32(out, 24);
        for (const Binding& binding : p.bindings) {
            append_u32(out, binding.kind);
            append_u32(out, binding.space);
            append_u32(out, binding.register_index);
            append_u32(out, binding.register_index);
            append_u32(out, binding.dxil_kind);
            append_u32(out, 0);
        }
    }

    std::vector<std::uint8_t> strings{0};
    std::vector<std::uint32_t> indices;
    struct Element {
        std::uint32_t name = 0;
        std::uint32_t index = 0;
        std::uint8_t start = 0;
        std::uint8_t cols = 0;
        const SigElement* element = nullptr;
    };
    std::vector<Element> elements;
    const auto add_element = [&](const SigElement& element) {
        Element entry;
        if (element.kind == 0) {
            std::vector<std::uint8_t> needle(element.name.begin(), element.name.end());
            needle.push_back(0);
            const auto found = find_bytes(strings, needle);
            if (found) {
                entry.name = static_cast<std::uint32_t>(*found);
            } else {
                strings.insert(strings.end(), needle.begin(), needle.end());
                entry.name = static_cast<std::uint32_t>(strings.size() - needle.size());
            }
        }
        const auto at = std::find(indices.begin(), indices.end(), element.index);
        if (at != indices.end()) {
            entry.index = static_cast<std::uint32_t>(std::distance(indices.begin(), at));
        } else {
            indices.push_back(element.index);
            entry.index = static_cast<std::uint32_t>(indices.size() - 1);
        }
        const bool allocated = element.row.has_value();
        entry.start = allocated ? static_cast<std::uint8_t>(*element.row) : 0;
        const std::uint8_t alloc = allocated ? static_cast<std::uint8_t>(0x40 | (element.col << 4)) : 0;
        entry.cols = static_cast<std::uint8_t>(element.cols | alloc);
        entry.element = &element;
        elements.push_back(entry);
    };
    for (const SigElement& element : p.inputs) add_element(element);
    for (const SigElement& element : p.outputs) add_element(element);
    for (const SigElement& element : p.prims) add_element(element);

    if (named_entry) {
        write_u32(out, entry_offset, static_cast<std::uint32_t>(strings.size()));
        strings.insert(strings.end(), p.entry_name.begin(), p.entry_name.end());
        strings.push_back(0);
    }
    strings.resize((strings.size() + 3) / 4 * 4, 0);
    append_u32(out, static_cast<std::uint32_t>(strings.size()));
    out.insert(out.end(), strings.begin(), strings.end());
    append_u32(out, static_cast<std::uint32_t>(indices.size()));
    for (const std::uint32_t index : indices) append_u32(out, index);
    if (!elements.empty()) {
        append_u32(out, 16);
        for (const Element& entry : elements) {
            append_u32(out, entry.name);
            append_u32(out, entry.index);
            out.push_back(1);
            out.push_back(entry.start);
            out.push_back(entry.cols);
            out.push_back(entry.element->kind);
            out.push_back(static_cast<std::uint8_t>(signal_component(entry.element->comp)));
            out.push_back(entry.element->interp);
            out.push_back(0);
            out.push_back(0);
        }
    }

    if (in_vectors > 0 && out_vectors > 0) {
        const std::uint32_t ni = scalar_count(p.inputs);
        const std::uint32_t no = scalar_count(p.outputs);
        const std::uint32_t per = (out_vectors * 4 + 31) / 32;
        for (std::uint32_t i = 0; i < in_vectors * 4; ++i) {
            for (std::uint32_t k = 0; k < per; ++k) {
                const std::uint32_t behind = 32 * k >= no ? 0 : no - 32 * k;
                const std::uint32_t bits = i < ni ? std::min(behind, 32u) : 0;
                append_u32(out, bits == 32 ? UINT32_MAX : (1u << bits) - 1u);
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> build(std::span<const ContainerPart> parts) {
    const std::size_t table = kContainerHeaderSize + 4 * parts.size();
    std::size_t total = table;
    for (const ContainerPart& part : parts) total += kPartHeaderSize + part.data.size();
    std::vector<std::uint8_t> out;
    out.reserve(total);
    out.insert(out.end(), {'D', 'X', 'B', 'C'});
    out.resize(kDigestOffset + 16, 0);
    append_u16(out, 1);
    append_u16(out, 0);
    append_u32(out, static_cast<std::uint32_t>(total));
    append_u32(out, static_cast<std::uint32_t>(parts.size()));
    std::size_t offset = table;
    for (const ContainerPart& part : parts) {
        append_u32(out, static_cast<std::uint32_t>(offset));
        offset += kPartHeaderSize + part.data.size();
    }
    for (const ContainerPart& part : parts) {
        out.insert(out.end(), part.name.begin(), part.name.end());
        append_u32(out, static_cast<std::uint32_t>(part.data.size()));
        out.insert(out.end(), part.data.begin(), part.data.end());
    }
    const std::array<std::uint8_t, 16> digest = hash(std::span<const std::uint8_t>(out).subspan(kHashedOffset));
    std::copy(digest.begin(), digest.end(), out.begin() + kDigestOffset);
    return out;
}

std::array<std::uint8_t, 16> hash(std::span<const std::uint8_t> data) {
    const std::size_t len = data.size();
    const std::size_t left = len % 64;
    std::vector<std::uint8_t> tail;
    tail.reserve(128);
    if (left < 56) {
        append_u32(tail, static_cast<std::uint32_t>(len) << 3);
        tail.insert(tail.end(), data.begin() + static_cast<std::ptrdiff_t>(len - left), data.end());
        tail.push_back(0x80);
        tail.resize(60, 0);
    } else {
        tail.insert(tail.end(), data.begin() + static_cast<std::ptrdiff_t>(len - left), data.end());
        tail.push_back(0x80);
        tail.resize(64, 0);
        append_u32(tail, static_cast<std::uint32_t>(len) << 3);
        tail.resize(124, 0);
    }
    append_u32(tail, (static_cast<std::uint32_t>(len) << 1) | 1);

    std::array<std::uint32_t, 4> state{};
    std::copy(std::begin(kMd5State), std::end(kMd5State), state.begin());
    for (std::size_t at = 0; at < len - left; at += 64) md5_block(state, data.data() + at);
    for (std::size_t at = 0; at < tail.size(); at += 64) md5_block(state, tail.data() + at);

    std::array<std::uint8_t, 16> out{};
    for (std::size_t i = 0; i < state.size(); ++i) {
        out[i * 4] = static_cast<std::uint8_t>(state[i]);
        out[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 8);
        out[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 16);
        out[i * 4 + 3] = static_cast<std::uint8_t>(state[i] >> 24);
    }
    return out;
}

}  // namespace g6shim::shader::emit
