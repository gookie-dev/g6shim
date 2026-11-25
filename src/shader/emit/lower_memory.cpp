// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lower_internal.hpp"

namespace g6shim::shader::emit {

namespace {

std::string_view last_segment(std::string_view name) {
    const std::size_t at = name.rfind('_');
    return at == std::string_view::npos ? name : name.substr(at + 1);
}

std::uint32_t width_of(std::string_view name) {
    const std::string_view rest = last_segment(name);
    if (rest == "b64") return 2;
    if (rest == "b96") return 3;
    if (rest == "b128") return 4;
    return 1;
}

}  // namespace

void Lower::ds(const IrInst& inst, std::size_t b, std::size_t i) {
    if (x_->native->stage == HwStage::Gs) {
        geometry_ring(inst, b, i);
        return;
    }
    const auto* fields = std::get_if<DsFields>(&inst.fields);
    if (fields == nullptr) std::abort();
    if (fields->gds) fail("GDS");
    const std::string_view name = inst.name();
    const std::optional<V> guard_value = guard(b, i);
    const std::size_t c = cur_;
    const std::uint32_t offset =
        static_cast<std::uint32_t>(fields->offset0) | static_cast<std::uint32_t>(fields->offset1) << 8;
    std::optional<std::uint8_t> dst_reg;
    if (fields->dst) {
        if (const auto* vector = std::get_if<VectorOperand>(&*fields->dst)) dst_reg = vector->reg;
    }
    if (name == "ds_swizzle_b32") {
        const bool quad = (offset & 0x8000) != 0 ||
                          ((offset & 0x1C) == 0x1C && (offset & 0x380) == 0 && (offset >> 10 & 0x1F) < 4);
        const bool mesh_quad = s_.mesh.has_value() && quad;
        if (!mesh_quad) cross_lane(quad);
        if (!fields->addr) fail("a swizzle source");
        const V x = vgprs(inst, *fields->addr, 1)[0];
        const V lane = lane_index();
        const auto k = [&](std::uint32_t v) { return s_.m.i32(v); };
        V src;
        if ((offset & 0x8000) != 0) {
            const V three = k(3);
            const V one = k(1);
            const V sel = s_.m.bin(c, Bin::And, lane, three);
            const V sh = s_.m.bin(c, Bin::Shl, sel, one);
            const V pattern = k(offset & 0xFF);
            V pick = s_.m.bin(c, Bin::LShr, pattern, sh);
            pick = s_.m.bin(c, Bin::And, pick, three);
            const V not3 = k(~3u);
            const V base = s_.m.bin(c, Bin::And, lane, not3);
            src = s_.m.bin(c, Bin::Or, base, pick);
        } else {
            const V and_value = k((offset & 0x1F) | ~0x1Fu);
            const V or_value = k((offset >> 5) & 0x1F);
            const V xor_value = k((offset >> 10) & 0x1F);
            V t = s_.m.bin(c, Bin::And, lane, and_value);
            t = s_.m.bin(c, Bin::Or, t, or_value);
            src = s_.m.bin(c, Bin::Xor, t, xor_value);
        }
        const V r = mesh_quad ? quad_pick(x, src) : read_lane(x, src);
        if (!dst_reg) fail("a swizzle result");
        write(inst, Reg::v(*dst_reg), r, guard_value);
        return;
    }
    if (s_.lds_dwords == 0) fail("LDS the program does not allocate");
    if (!fields->addr) fail("an LDS address");
    const V a = vgprs(inst, *fields->addr, 1)[0];
    const auto dword = [&](std::uint32_t bytes) {
        const V o = s_.m.i32(bytes);
        const V at = s_.m.bin(c, Bin::Add, a, o);
        const V two = s_.m.i32(2);
        const V index = s_.m.bin(c, Bin::LShr, at, two);
        return s_.m.element(c, s_.lds(), index);
    };
    const auto pair_offsets = [&](const std::string_view n) {
        const std::uint32_t w = width_of(n);
        const std::uint32_t scale = 4 * w * (n.find("st64") != std::string_view::npos ? 64 : 1);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
        out.push_back({static_cast<std::uint32_t>(fields->offset0) * scale, w});
        out.push_back({static_cast<std::uint32_t>(fields->offset1) * scale, w});
        return out;
    };
    if (name.starts_with("ds_read")) {
        const std::string_view rest = name.substr(7);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> elements;
        if (rest.starts_with('2')) {
            elements = pair_offsets(name);
        } else if (rest == "_b32" || rest == "_b64" || rest == "_b96" || rest == "_b128") {
            elements.push_back({offset, width_of(name)});
        } else {
            fail("an LDS read of bytes or halves");
        }
        if (!dst_reg) fail("an LDS read result");
        std::uint8_t n = 0;
        for (const auto& element : elements) {
            for (std::uint32_t k = 0; k < element.second; ++k) {
                const V p = dword(element.first + 4 * k);
                const V x = s_.m.load(cur_, p);
                write(inst, Reg::v(static_cast<std::uint8_t>(*dst_reg + n)), x, guard_value);
                ++n;
            }
        }
        return;
    }
    if (name.starts_with("ds_write")) {
        const std::string_view rest = name.substr(8);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> elements;
        if (rest.starts_with('2')) {
            elements = pair_offsets(name);
        } else if (rest == "_b32" || rest == "_b64" || rest == "_b96" || rest == "_b128") {
            elements.push_back({offset, width_of(name)});
        } else {
            fail("an LDS write of bytes or halves");
        }
        std::vector<std::pair<std::uint32_t, V>> stores;
        for (std::size_t e = 0; e < elements.size(); ++e) {
            if (e >= fields->data.size()) fail("an LDS write");
            const std::vector<V> vals =
                vgprs(inst, fields->data[e], static_cast<std::uint8_t>(elements[e].second));
            for (std::size_t k = 0; k < vals.size(); ++k)
                stores.emplace_back(elements[e].first + static_cast<std::uint32_t>(4 * k), vals[k]);
        }
        const auto at = begin_guarded(guard_value);
        for (const auto& entry : stores) {
            const V p = dword(entry.first);
            s_.m.store(cur_, p, entry.second);
        }
        end_guarded(at, {});
        return;
    }
    std::string op;
    if (!name.starts_with("ds_")) fail("an LDS operation");
    op.assign(name.substr(3));
    if (const std::size_t at = op.find("_rtn"); at != std::string::npos) op.erase(at, 4);
    std::optional<Rmw> rmw;
    bool compare_store = false;
    if (op == "add_u32") rmw = Rmw::Add;
    else if (op == "sub_u32") rmw = Rmw::Sub;
    else if (op == "and_b32") rmw = Rmw::And;
    else if (op == "or_b32") rmw = Rmw::Or;
    else if (op == "xor_b32") rmw = Rmw::Xor;
    else if (op == "min_i32") rmw = Rmw::Min;
    else if (op == "max_i32") rmw = Rmw::Max;
    else if (op == "min_u32") rmw = Rmw::UMin;
    else if (op == "max_u32") rmw = Rmw::UMax;
    else if (op == "wrxchg_b32") rmw = Rmw::Xchg;
    else if (op == "cmpst_b32") compare_store = true;
    else fail("an LDS operation");
    if (fields->data.empty()) fail("an LDS atomic");
    const V v0 = vgprs(inst, fields->data[0], 1)[0];
    std::optional<V> v1;
    if (fields->data.size() > 1) v1 = vgprs(inst, fields->data[1], 1)[0];
    std::optional<std::pair<std::uint8_t, Value>> old_reg;
    if (dst_reg) {
        if (const std::optional<Value> read = inst.read(Reg::v(*dst_reg)))
            old_reg = std::pair<std::uint8_t, Value>{*dst_reg, *read};
    }
    V outside = s_.m.i32(0);
    if (old_reg) {
        try {
            outside = i32_of(old_reg->second);
        } catch (const WhyError&) {
            outside = s_.m.i32(0);
        }
    }
    const V p = dword(offset);
    const auto at = begin_guarded(guard_value);
    V r;
    if (rmw) {
        r = s_.m.atomic(cur_, *rmw, p, v0);
    } else {
        if (!compare_store || !v1) fail("an LDS compare");
        r = s_.m.cmpxchg(cur_, p, v0, *v1);
    }
    const V result = end_guarded(at, {{r, outside}})[0];
    if (dst_reg) write(inst, Reg::v(*dst_reg), result, std::nullopt);
}

std::vector<V> Lower::format_load(V h, V off, std::uint8_t format, std::uint32_t channels) {
    const std::optional<BufferFormat> found = buffer_format(format);
    if (!found) fail("a buffer format");
    const std::span<const std::uint32_t> bits = found->bits;
    const Numeric numeric = found->numeric;
    const std::size_t c = cur_;
    std::uint32_t total = 0;
    for (std::uint32_t bit : bits) total += bit;
    const V u = s_.m.undef(s_.t.i32);
    const V align = s_.m.i32(4);
    const auto load = [&](V at, std::uint32_t n) {
        const V mask = s_.m.i8(static_cast<std::uint8_t>((1u << n) - 1));
        const V r =
            s_.op(c, "rawBufferLoad.f32", Attr::ReadOnly, s_.t.res_ret, 139, {h, at, u, mask, align});
        std::vector<V> out;
        for (std::uint32_t k = 0; k < n; ++k) out.push_back(as_int(s_.m.extract(c, r, k)));
        return out;
    };
    std::vector<V> words;
    if (total < 32) {
        const V not3 = s_.m.i32(~3u);
        const V three = s_.m.i32(3);
        const V at = s_.m.bin(c, Bin::And, off, not3);
        const V w = load(at, 1)[0];
        const V byte = s_.m.bin(c, Bin::And, off, three);
        const V sh = s_.m.bin(c, Bin::Shl, byte, three);
        words.push_back(s_.m.bin(c, Bin::LShr, w, sh));
    } else {
        words = load(off, total / 32);
    }
    std::vector<V> out;
    std::uint32_t at = 0;
    for (std::uint32_t w : bits) {
        const V word = words[at / 32];
        V field;
        if (w == 32) {
            field = word;
        } else {
            const V sh = s_.m.i32(at % 32);
            const V t = s_.m.bin(c, Bin::LShr, word, sh);
            const V m = s_.m.i32(static_cast<std::uint32_t>((std::uint64_t{1} << w) - 1));
            field = s_.m.bin(c, Bin::And, t, m);
        }
        at += w;
        const auto signed_field = [&]() {
            if (w == 32) return field;
            const V k = s_.m.i32(32 - w);
            const V t = s_.m.bin(c, Bin::Shl, field, k);
            return s_.m.bin(c, Bin::AShr, t, k);
        };
        const Ty f32t = s_.t.f32;
        V x;
        switch (numeric) {
            case Numeric::Unorm: {
                const V f = s_.m.cast(c, Cast::UiToFp, field, f32t);
                const V max = s_.m.f32(std::bit_cast<std::uint32_t>(
                    static_cast<float>((std::uint64_t{1} << w) - 1)));
                const V q = s_.m.bin(c, Bin::SDiv, f, max);
                x = as_int(q);
                break;
            }
            case Numeric::Snorm: {
                const V s = signed_field();
                const V f = s_.m.cast(c, Cast::SiToFp, s, f32t);
                const V max = s_.m.f32(std::bit_cast<std::uint32_t>(
                    static_cast<float>((std::uint64_t{1} << (w - 1)) - 1)));
                const V q = s_.m.bin(c, Bin::SDiv, f, max);
                const V lo = s_.m.f32(std::bit_cast<std::uint32_t>(-1.0f));
                x = as_int(binary(35, q, lo));
                break;
            }
            case Numeric::Uscaled: {
                const V f = s_.m.cast(c, Cast::UiToFp, field, f32t);
                x = as_int(f);
                break;
            }
            case Numeric::Sscaled: {
                const V s = signed_field();
                x = as_int(s_.m.cast(c, Cast::SiToFp, s, f32t));
                break;
            }
            case Numeric::Uint: x = field; break;
            case Numeric::Sint: x = signed_field(); break;
            case Numeric::Float:
                if (w == 32) {
                    x = field;
                } else {
                    V half_bits = field;
                    const std::uint32_t shift = half_shift(w);
                    if (shift != 0) {
                        const V sh = s_.m.i32(shift);
                        half_bits = s_.m.bin(c, Bin::Shl, field, sh);
                    }
                    x = as_int(s_.op(c, "legacyF16ToF32", Attr::ReadNone, f32t, 131, {half_bits}));
                }
                break;
        }
        out.push_back(x);
    }
    std::uint32_t one = 0;
    if (numeric == Numeric::Uint || numeric == Numeric::Sint)
        one = 1;
    else
        one = std::bit_cast<std::uint32_t>(1.0f);
    for (std::uint32_t k = static_cast<std::uint32_t>(out.size()); k < channels; ++k)
        out.push_back(s_.m.i32(k == 3 ? one : 0));
    out.resize(channels);
    return out;
}

void Lower::buffer(const IrInst& inst, std::size_t b, std::size_t i) {
    const auto* fields = std::get_if<BufferFields>(&inst.fields);
    if (fields == nullptr) std::abort();
    const std::string_view name = inst.name();
    if (fields->lds || fields->tfe) fail("a buffer access to LDS or with tfe");
    if (fields->format && !name.starts_with("tbuffer_load_format_")) fail(name);
    const Site site{b, i};
    const std::optional<V> guard_value = guard(b, i);
    const std::size_t c = cur_;
    const bool store = name.starts_with("buffer_store_");
    std::optional<std::uint32_t> atomic;
    if (name.starts_with("buffer_atomic_")) {
        const std::string_view op = name.substr(14);
        if (op == "add") atomic = 0;
        else if (op == "and") atomic = 1;
        else if (op == "or") atomic = 2;
        else if (op == "xor") atomic = 3;
        else if (op == "smin") atomic = 4;
        else if (op == "smax") atomic = 5;
        else if (op == "umin") atomic = 6;
        else if (op == "umax") atomic = 7;
        else if (op == "swap") atomic = 8;
        else if (op == "cmpswap") atomic = UINT32_MAX;
        else fail(name);
    }
    const bool d16 =
        name.starts_with("buffer_") && name.find("_format_d16_") != std::string_view::npos;
    std::string plain(name);
    if (const std::size_t at = plain.find("_format_d16_"); at != std::string::npos)
        plain.replace(at, 12, "_format_");
    const std::string_view plain_view(plain);
    bool typed = false;
    std::uint32_t count = 0;
    if (atomic) {
        typed = false;
        count = 1;
    } else {
        std::optional<std::string_view> rest;
        if (plain_view.starts_with("buffer_load_")) rest = plain_view.substr(12);
        else if (plain_view.starts_with("buffer_store_")) rest = plain_view.substr(13);
        else if (plain_view.starts_with("tbuffer_load_")) rest = plain_view.substr(13);
        if (!rest) fail(name);
        if (*rest == "dword") { typed = false; count = 1; }
        else if (*rest == "dwordx2") { typed = false; count = 2; }
        else if (*rest == "dwordx3") { typed = false; count = 3; }
        else if (*rest == "dwordx4") { typed = false; count = 4; }
        else if (*rest == "format_x") { typed = true; count = 1; }
        else if (*rest == "format_xy") { typed = true; count = 2; }
        else if (*rest == "format_xyz") { typed = true; count = 3; }
        else if (*rest == "format_xyzw") { typed = true; count = 4; }
        else fail(name);
    }
    const auto* addr = std::get_if<VectorOperand>(&fields->addr);
    if (addr == nullptr) fail("a buffer address");
    const std::uint8_t areg = addr->reg;
    const auto vaddr = [&](std::uint8_t k) -> V {
        const std::optional<Value> read =
            inst.read(Reg::v(static_cast<std::uint8_t>(areg + k)));
        if (!read) fail("a buffer address");
        return i32_of(*read);
    };
    std::optional<V> index;
    if (fields->idxen) index = vaddr(0);
    V off = s_.m.i32(fields->offset);
    std::optional<V> vector_offset;
    if (fields->offen) {
        vector_offset = vaddr(fields->idxen ? 1 : 0);
        off = s_.m.bin(c, Bin::Add, off, *vector_offset);
    }
    const V before_scalar_offset = off;
    bool soffset_null = false;
    if (const auto* scalar = std::get_if<ScalarOperand>(&fields->soffset))
        soffset_null = scalar->code == kNull;
    if (const auto* constant = std::get_if<ConstantOperand>(&fields->soffset))
        if (constant->code == 128) soffset_null = true;
    std::optional<V> scalar_offset;
    if (!soffset_null) {
        scalar_offset = operand(inst, fields->soffset);
        off = s_.m.bin(c, Bin::Add, off, *scalar_offset);
    }
    const V after_scalar_offset = off;
    const auto descriptor = descriptors_.find({site, false});
    if (descriptor == descriptors_.end()) fail("an untracked buffer");
    const Source& src = *descriptor->second;
    const UsedResource* used = used_kind(src);
    const auto [base, extra] = built_offset(src);
    if (base != 0) off = s_.m.bin(c, Bin::Add, off, s_.m.i32(base));
    if (extra) off = s_.m.bin(c, Bin::Add, off, *extra);
    if (used != nullptr && used->kind.kind == ResourceType::Kind::Cbv && !typed) {
        const V h = handle(site, src, false, {13, static_cast<std::uint32_t>(0x1'0000)});
        V byte = off;
        if (index) {
            const V sixteen = s_.m.i32(16);
            const V rows = s_.m.bin(c, Bin::Mul, *index, sixteen);
            byte = s_.m.bin(c, Bin::Add, rows, off);
        }
        if (!fields->data) fail("a buffer result");
        const auto* data = std::get_if<VectorOperand>(&*fields->data);
        if (data == nullptr) fail("a buffer result");
        for (std::uint32_t k = 0; k < count; ++k) {
            const V four = s_.m.i32(4 * k);
            const V at = s_.m.bin(c, Bin::Add, byte, four);
            const V x = cbuffer_dword(h, at);
            write(inst, Reg::v(static_cast<std::uint8_t>(data->reg + k)), x, guard_value);
        }
        return;
    }
    const bool uav = store || atomic.has_value() ||
                     (used != nullptr && used->kind.kind == ResourceType::Kind::Uav);
    const std::uint32_t u = uav ? 0x1000 : 0;
    const V iu = s_.m.undef(s_.t.i32);
    if (atomic) {
        const std::uint32_t op = *atomic;
        if (!fields->data) fail("buffer atomic data");
        const auto* data = std::get_if<VectorOperand>(&*fields->data);
        if (data == nullptr) fail("buffer atomic data");
        const std::vector<V> vals = vgprs(inst, *fields->data, op == UINT32_MAX ? 2 : 1);
        V h;
        std::array<V, 3> coords{};
        if (index) {
            const std::uint32_t stride = used != nullptr ? used->stride : 0;
            if (stride < 4 || stride % 4 != 0) fail("a structured buffer of unknown stride");
            h = handle(site, src, false, {12 | u, stride});
            coords = {*index, off, iu};
        } else {
            h = handle(site, src, false, {11 | u, 0});
            coords = {off, iu, iu};
        }
        bool returns = false;
        if (const auto* cache_fields = std::get_if<BufferFields>(&inst.fields))
            returns = cache_fields->cache.glc;
        V old = s_.m.i32(0);
        if (returns) {
            if (const std::optional<Value> read = inst.read(Reg::v(data->reg))) old = i32_of(*read);
        }
        const auto at = begin_guarded(guard_value);
        const Ty i32t = s_.t.i32;
        V r;
        if (op == UINT32_MAX) {
            const std::vector<V> args = {h, coords[0], coords[1], coords[2], vals[1], vals[0]};
            r = s_.op(cur_, "atomicCompareExchange.i32", Attr::NoUnwind, i32t, 79, args);
        } else {
            const std::vector<V> args = {h, s_.m.i32(op), coords[0], coords[1], coords[2], vals[0]};
            r = s_.op(cur_, "atomicBinOp.i32", Attr::NoUnwind, i32t, 78, args);
        }
        const V result = end_guarded(at, {{r, old}})[0];
        if (returns) write(inst, Reg::v(data->reg), result, std::nullopt);
        return;
    }
    if (store) {
        if (!fields->data) fail("buffer store data");
        const std::uint32_t regs = d16 ? (count + 1) / 2 : count;
        std::vector<V> vals = vgprs(inst, *fields->data, static_cast<std::uint8_t>(regs));
        if (d16) {
            std::vector<V> wide;
            for (std::uint32_t k = 0; k < count; ++k) {
                V x = vals[k / 2];
                if (k % 2 == 1) {
                    const V k16 = s_.m.i32(16);
                    x = s_.m.bin(c, Bin::LShr, x, k16);
                }
                const Ty f32t = s_.t.f32;
                const V f = s_.op(c, "legacyF16ToF32", Attr::ReadNone, f32t, 131, {x});
                wide.push_back(as_int(f));
            }
            vals = wide;
        }
        std::vector<V> args;
        std::uint32_t opcode = 0;
        std::string op_name;
        if (typed) {
            if (count != 4 || fields->offen || fields->offset != 0)
                fail("a typed buffer store of some channels or at an offset");
            args.push_back(handle(site, src, false, {10 | u, 0x409}));
            args.push_back(index.value_or(s_.m.i32(0)));
            args.push_back(iu);
            for (V x : vals) args.push_back(as_float(x));
            opcode = 69;
            op_name = "bufferStore.f32";
        } else {
            if (index) {
                const std::uint32_t stride = used != nullptr ? used->stride : 0;
                if (stride < 4 || stride % 4 != 0) fail("a structured buffer of unknown stride");
                args.push_back(handle(site, src, false, {12 | u, stride}));
                args.push_back(*index);
                args.push_back(off);
            } else {
                args.push_back(handle(site, src, false, {11 | u, 0}));
                args.push_back(off);
                args.push_back(iu);
            }
            args.insert(args.end(), vals.begin(), vals.end());
            args.resize(3 + 4, iu);
            opcode = 140;
            op_name = "rawBufferStore.i32";
        }
        args.push_back(
            s_.m.i8(static_cast<std::uint8_t>(typed ? 0xF : (1u << count) - 1u)));
        if (!typed) args.push_back(s_.m.i32(4));
        const auto at = begin_guarded(guard_value);
        s_.op(cur_, op_name, Attr::NoUnwind, s_.t.void_type, opcode, args);
        end_guarded(at, {});
        return;
    }
    if (fields->format) {
        if (index) fail("a typed-format load by index");
        const V h = handle(site, src, false, {11 | u, 0});
        if (!fields->data) fail("a buffer result");
        const auto* data = std::get_if<VectorOperand>(&*fields->data);
        if (data == nullptr) fail("a buffer result");
        const std::vector<V> loaded = format_load(h, off, *fields->format, count);
        for (std::size_t k = 0; k < loaded.size(); ++k)
            write(inst, Reg::v(static_cast<std::uint8_t>(data->reg + k)), loaded[k], guard_value);
        return;
    }
    const std::uint32_t stride = used != nullptr ? used->stride : 0;
    if (index && !typed && !uav && !d16 && (stride < 4 || stride % 4 != 0)) {
        if (!fields->data) fail("a buffer result");
        const auto* data = std::get_if<VectorOperand>(&*fields->data);
        if (data == nullptr) fail("a buffer result");
        if (vector_offset || scalar_offset) {
            V valid = s_.m.boolean(true);
            if (vector_offset) valid = s_.m.cmp(c, pred::kIuge, before_scalar_offset, *vector_offset);
            if (scalar_offset) {
                const V scalar_fits = s_.m.cmp(c, pred::kIuge, after_scalar_offset, *scalar_offset);
                valid = s_.m.bin(c, Bin::And, valid, scalar_fits);
            }
            off = s_.m.select(c, valid, off, s_.m.i32(UINT32_MAX));
        }
        const auto loaded = load_indexed_heap_buffer(src, *index, off, count, guard_value);
        for (std::uint32_t k = 0; k < count; ++k)
            write(inst, Reg::v(static_cast<std::uint8_t>(data->reg + k)), loaded[k], guard_value);
        return;
    }
    V r;
    if (typed) {
        if (fields->offen || off.kind == V::Kind::Inst || fields->offset != 0)
            fail("a typed buffer load at an offset");
        if (uav) s_.flags |= feature::kTypedUavLoads;
        const V h = handle(site, src, false, {10 | u, 0x409});
        const V at = index.value_or(s_.m.i32(0));
        r = s_.op(c, "bufferLoad.f32", Attr::ReadOnly, s_.t.res_ret, 68, {h, at, iu});
    } else {
        const V mask = s_.m.i8(static_cast<std::uint8_t>((1u << count) - 1));
        const V align = s_.m.i32(4);
        if (index) {
            if (stride < 4 || stride % 4 != 0) fail("a structured buffer of unknown stride");
            const V h = handle(site, src, false, {12 | u, stride});
            r = s_.op(c, "rawBufferLoad.f32", Attr::ReadOnly, s_.t.res_ret, 139,
                      {h, *index, off, mask, align});
        } else {
            const V h = handle(site, src, false, {11 | u, 0});
            r = s_.op(c, "rawBufferLoad.f32", Attr::ReadOnly, s_.t.res_ret, 139,
                      {h, off, iu, mask, align});
        }
    }
    if (!fields->data) fail("a buffer result");
    const auto* data = std::get_if<VectorOperand>(&*fields->data);
    if (data == nullptr) fail("a buffer result");
    if (d16) {
        const V k16 = s_.m.i32(16);
        const Ty i32t = s_.t.i32;
        for (std::uint32_t k = 0; k < (count + 1) / 2; ++k) {
            std::optional<V> x;
            const std::uint32_t worst = std::min(2 * k + 2, count);
            for (std::uint32_t ch = worst; ch-- > 2 * k;) {
                const V f = s_.m.extract(c, r, ch);
                const V h = s_.op(c, "legacyF32ToF16", Attr::ReadNone, i32t, 130, {f});
                if (x) {
                    const V up = s_.m.bin(c, Bin::Shl, *x, k16);
                    x = s_.m.bin(c, Bin::Or, h, up);
                } else {
                    x = h;
                }
            }
            write(inst, Reg::v(static_cast<std::uint8_t>(data->reg + k)), *x, guard_value);
        }
        return;
    }
    for (std::uint32_t k = 0; k < count; ++k) {
        const V x = as_int(s_.m.extract(c, r, k));
        write(inst, Reg::v(static_cast<std::uint8_t>(data->reg + k)), x, guard_value);
    }
}

std::vector<V> Lower::load_indexed_heap_buffer(const Source& src, V index, V offset, std::uint32_t count,
                                               std::optional<V> guard_value) {
    constexpr Why unknown = "a structured buffer of unknown stride";
    if (src.kind != Source::Kind::Loaded) fail(unknown);
    const auto address = scalar_.find(src.load);
    if (address == scalar_.end() || address->second.param >= x_->native->layout.size()) fail(unknown);
    const RootParam& root = x_->native->layout[address->second.param];
    if (root.root.kind != RootType::Kind::ResourceHeap) fail(unknown);

    using Data = BufferHeapData;
    const V byte_address = heap_byte_address(src.load, src.first);
    const std::size_t c = cur_;
    const UserData binding{0, root.slot, root.root, 0, UserData::Source::BufferHeapData};
    const auto [record, valid_slot] = read_heap_record(c, byte_address, binding);
    const V zero = s_.m.i32(0);
    const V descriptor = s_.m.extract(c, record, Data::kDescriptor);
    const V stride = s_.m.extract(c, record, Data::kStride);
    const V prefix = s_.m.extract(c, record, Data::kPrefix);
    const V records = s_.m.extract(c, record, Data::kRecords);
    const V raw = s_.m.cmp(c, pred::kEq, stride, zero);
    const V in_records = s_.m.cmp(c, pred::kIult, index, records);
    const V valid_index = s_.m.bin(c, Bin::Or, raw, in_records);
    const V row = s_.m.bin(c, Bin::Mul, index, stride);
    const V bytes = s_.m.select(c, raw, records, s_.m.bin(c, Bin::Mul, records, stride));
    const V remaining = s_.m.bin(c, Bin::Sub, bytes, row);
    constexpr std::uint32_t kWordBytes = sizeof(std::uint32_t);
    const V payload = s_.m.i32(count * kWordBytes);
    const V fits = s_.m.cmp(c, pred::kIuge, remaining, payload);
    const V max_offset = s_.m.bin(c, Bin::Sub, remaining, payload);
    const V in_bytes = s_.m.cmp(c, pred::kIule, offset, max_offset);
    const V unaligned = s_.m.bin(c, Bin::And, offset, s_.m.i32(kWordBytes - 1));
    const V aligned = s_.m.cmp(c, pred::kEq, unaligned, zero);
    const V present = s_.m.cmp(c, pred::kNe, descriptor, zero);
    V valid = s_.m.bin(c, Bin::And, valid_slot, present);
    for (const V test : {valid_index, fits, in_bytes, aligned}) valid = s_.m.bin(c, Bin::And, valid, test);
    if (guard_value) valid = s_.m.bin(c, Bin::And, valid, *guard_value);

    const auto guarded = begin_guarded(valid);
    const V slot = s_.m.bin(cur_, Bin::Sub, descriptor, s_.m.i32(1));
    const V h = s_.heap_handle(cur_, slot, false, true, {11, 0});
    const V start = s_.m.bin(cur_, Bin::Add, prefix, row);
    const V at = s_.m.bin(cur_, Bin::Add, start, offset);
    const V mask = s_.m.i8(static_cast<std::uint8_t>((1u << count) - 1));
    const V r = s_.op(cur_, "rawBufferLoad.f32", Attr::ReadOnly, s_.t.res_ret, 139,
                      {h, at, s_.m.undef(s_.t.i32), mask, s_.m.i32(kWordBytes)});
    std::vector<std::pair<V, V>> values;
    values.reserve(count);
    for (std::uint32_t k = 0; k < count; ++k) values.emplace_back(as_int(s_.m.extract(cur_, r, k)), zero);
    return end_guarded(guarded, values);
}

}  // namespace g6shim::shader::emit
