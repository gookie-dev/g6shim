// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "lower_internal.hpp"

namespace g6shim::shader::emit {

void Lower::image(const IrInst& inst, std::size_t b, std::size_t i) {
    const auto* fields = std::get_if<ImageFields>(&inst.fields);
    if (fields == nullptr) std::abort();
    if (fields->tfe || fields->lwe) fail("an image access with tfe or lwe");
    const std::string_view name = inst.name();
    const bool a16 = fields->a16;
    const bool d16 = fields->d16;
    if (a16 && name.starts_with("image_sample_b") && name != "image_sample_b")
        fail("a biased image sample with 16-bit coordinates");
    const std::uint8_t dmask = fields->dmask == 0 ? 1 : fields->dmask;
    const Site site{b, i};
    const std::optional<V> guard_value = guard(b, i);
    const std::size_t c = cur_;
    std::uint32_t kind = 0;
    std::size_t coords = 0;
    switch (fields->dim) {
        case 0: kind = 1; coords = 1; break;
        case 1: kind = 2; coords = 2; break;
        case 2: kind = 4; coords = 3; break;
        case 3:
            if (name.starts_with("image_sample") || name.starts_with("image_gather") ||
                name == "image_get_resinfo") {
                kind = 5;
                coords = 3;
            } else {
                fail("an image dimension (cube loads or MSAA)");
            }
            break;
        case 4: kind = 6; coords = 2; break;
        case 5: kind = 7; coords = 3; break;
        default: fail("an image dimension (cube loads or MSAA)");
    }
    std::vector<std::uint8_t> regs;
    for (const Operand& a : fields->addr) {
        if (const auto* vector = std::get_if<VectorOperand>(&a)) {
            for (std::uint8_t k = 0; k < vector->dwords; ++k)
                regs.push_back(static_cast<std::uint8_t>(vector->reg + k));
        }
    }
    const auto addr_val = [&](std::size_t k, std::size_t prefix_words = 0) -> V {
        const std::size_t index = prefix_words + (a16 ? k / 2 : k);
        if (index >= regs.size()) fail("an image address");
        const std::optional<Value> read = inst.read(Reg::v(regs[index]));
        if (!read) fail("an image address");
        const V x = i32_of(*read);
        if (!a16) return x;
        const std::size_t at = cur_;
        const V sh = s_.m.i32(16 * static_cast<std::uint32_t>(k % 2));
        const V mask = s_.m.i32(0xFFFF);
        V shifted = s_.m.bin(at, Bin::LShr, x, sh);
        return s_.m.bin(at, Bin::And, shifted, mask);
    };
    const auto descriptor = descriptors_.find({site, false});
    if (descriptor == descriptors_.end()) fail("an untracked image");
    const Source& src = *descriptor->second;
    const bool store = name.starts_with("image_store");
    const UsedResource* used = used_kind(src);
    const bool uav = store || (used != nullptr && used->kind.kind == ResourceType::Kind::Uav);
    const std::pair<std::uint32_t, std::uint32_t> props{kind | (uav ? 0x1000u : 0u), 0x409};
    if (name.starts_with("image_atomic_")) {
        const std::string_view op = name.substr(13);
        std::uint32_t code = 0;
        if (op == "add" || op == "sub") code = 0;
        else if (op == "and") code = 1;
        else if (op == "or") code = 2;
        else if (op == "xor") code = 3;
        else if (op == "smin") code = 4;
        else if (op == "smax") code = 5;
        else if (op == "umin") code = 6;
        else if (op == "umax") code = 7;
        else if (op == "swap") code = 8;
        else if (op == "cmpswap") code = UINT32_MAX;
        else fail(name);
        const bool cmpswap = code == UINT32_MAX;
        constexpr std::uint8_t kNarrowMask = 1;
        constexpr std::uint8_t kNarrowCmpMask = 3;
        constexpr std::uint8_t kWideMask = 3;
        constexpr std::uint8_t kWideCmpMask = 0xF;
        constexpr std::uint32_t kTypedU32 = 0x105;
        constexpr std::uint8_t kWideDwords = 2;
        constexpr std::uint8_t kWideCmpDwords = 4;
        const std::uint8_t narrow = cmpswap ? kNarrowCmpMask : kNarrowMask;
        const bool wide = dmask != narrow;
        const bool glc = fields->cache.glc;
        if (d16 || a16) fail("a 16-bit image atomic");
        if (wide && dmask != (cmpswap ? kWideCmpMask : kWideMask)) fail("a 64-bit image atomic");
        const V iu = s_.m.undef(s_.t.i32);
        std::array<V, 3> cs{iu, iu, iu};
        for (std::size_t k = 0; k < coords; ++k) cs[k] = addr_val(k);
        const auto* data = std::get_if<VectorOperand>(&fields->data);
        if (data == nullptr) fail("image atomic data");
        const std::uint8_t dwords =
            cmpswap ? (wide ? kWideCmpDwords : kNarrowCmpMask) : (wide ? kWideDwords : kNarrowMask);
        const std::vector<V> vals = vgprs(inst, fields->data, dwords);
        Ty i64t = s_.t.i32;
        if (wide) i64t = s_.m.int_type(64);
        const auto pack64 = [&](V lo, V hi) {
            const V lo64 = s_.m.cast(c, Cast::ZExt, lo, i64t);
            const V hi64 = s_.m.cast(c, Cast::ZExt, hi, i64t);
            const V shifted = s_.m.int_const(i64t, 32);
            return s_.m.bin(c, Bin::Or, lo64, s_.m.bin(c, Bin::Shl, hi64, shifted));
        };
        V value = vals[0];
        V compare = vals.size() > 1 ? vals[1] : s_.m.i32(0);
        if (wide) {
            value = pack64(vals[0], vals[1]);
            compare = cmpswap ? pack64(vals[2], vals[3]) : s_.m.int_const(i64t, 0);
        }
        if (op == "sub") {
            value = wide ? s_.m.bin(c, Bin::Sub, s_.m.int_const(i64t, 0), value)
                         : s_.m.bin(c, Bin::Sub, s_.m.i32(0), value);
        }
        V old = wide ? s_.m.int_const(i64t, 0) : s_.m.i32(0);
        if (glc) {
            const std::optional<Value> lo = inst.read(Reg::v(data->reg));
            if (wide) {
                const std::optional<Value> hi = inst.read(Reg::v(static_cast<std::uint8_t>(data->reg + 1)));
                if (lo && hi) old = pack64(i32_of(*lo), i32_of(*hi));
            } else if (lo) {
                old = i32_of(*lo);
            }
        }
        if (wide) {
            s_.flags |= feature::kInt64 | feature::kAtomicInt64OnTypedResource |
                        feature::kAtomicInt64OnHeapResource;
        }
        const V h = handle(site, src, false, {kind | 0x1000u, kTypedU32});
        const auto at = begin_guarded(guard_value);
        const Ty result_type = wide ? i64t : s_.t.i32;
        V r;
        if (cmpswap) {
            const std::vector<V> args = {h, cs[0], cs[1], cs[2], compare, wide ? value : vals[0]};
            r = s_.op(cur_, wide ? "atomicCompareExchange.i64" : "atomicCompareExchange.i32",
                      Attr::NoUnwind, result_type, 79, args);
        } else {
            const std::vector<V> args = {h, s_.m.i32(code), cs[0], cs[1], cs[2], value};
            r = s_.op(cur_, wide ? "atomicBinOp.i64" : "atomicBinOp.i32", Attr::NoUnwind, result_type,
                      78, args);
        }
        const V result = end_guarded(at, {{r, old}})[0];
        if (glc) {
            if (wide) {
                const auto [lo, hi] = split64(result);
                write(inst, Reg::v(data->reg), lo, std::nullopt);
                write(inst, Reg::v(static_cast<std::uint8_t>(data->reg + 1)), hi, std::nullopt);
            } else {
                write(inst, Reg::v(data->reg), result, std::nullopt);
            }
        }
        return;
    }
    if (store) {
        if (name != "image_store") fail("an image store to a mip level");
        if (dmask != 0xF) fail("an image store of some channels");
        const V iu = s_.m.undef(s_.t.i32);
        std::array<V, 3> cs{iu, iu, iu};
        for (std::size_t k = 0; k < coords; ++k) cs[k] = addr_val(k);
            const auto* data = std::get_if<VectorOperand>(&fields->data);
        if (data == nullptr) fail("image store data");
        std::vector<V> vals;
        for (std::uint8_t k = 0; k < 4; ++k) {
            const std::uint8_t r = d16 ? static_cast<std::uint8_t>(k / 2) : k;
            const std::optional<Value> read = inst.read(Reg::v(static_cast<std::uint8_t>(data->reg + r)));
            if (!read) fail("image store data");
            if (d16) {
                V x = i32_of(*read);
                if (k % 2 == 1) {
                    const V k16 = s_.m.i32(16);
                    x = s_.m.bin(cur_, Bin::LShr, x, k16);
                }
                vals.push_back(s_.op(cur_, "legacyF16ToF32", Attr::ReadNone, s_.t.f32, 131, {x}));
            } else {
                vals.push_back(float_of(*read));
            }
        }
        const V h = handle(site, src, false, props);
        const auto at = begin_guarded(guard_value);
        std::vector<V> args = {h, cs[0], cs[1], cs[2], vals[0], vals[1], vals[2], vals[3],
                               s_.m.i8(0xF)};
        s_.op(cur_, "textureStore.f32", Attr::NoUnwind, s_.t.void_type, 67, args);
        end_guarded(at, {});
        return;
    }
    std::uint32_t op = 0;
    bool sample = false;
    if (name == "image_sample" || name == "image_sample_o") {
        op = 60;
        sample = true;
    } else if (name == "image_sample_lz" || name == "image_sample_l") {
        op = 62;
        sample = true;
    } else if (name == "image_sample_b") {
        op = 61;
        sample = true;
    } else if (name == "image_sample_c_lz") {
        op = 65;
        sample = true;
    } else if (name == "image_sample_d") {
        op = 63;
        sample = true;
    } else if (name == "image_get_lod" && s_.pixel) {
        op = 81;
        sample = true;
    } else if (name == "image_sample_c" && s_.pixel) {
        op = 64;
        sample = true;
    } else if (name == "image_gather4" || name == "image_gather4_lz" ||
               name == "image_gather4_lz_o") {
        op = 73;
        sample = true;
    } else if (name == "image_load" || name == "image_load_mip") {
        op = 66;
    } else if (name == "image_get_resinfo") {
        op = 72;
    } else {
        fail(name);
    }
    if (op == 72) {
        const V h = handle(site, src, false, props);
        const V mip = uav ? s_.m.undef(s_.t.i32) : addr_val(0);
        const Ty i32t = s_.t.i32;
        const Ty dims = s_.m.named_struct("dx.types.Dimensions", {i32t, i32t, i32t, i32t});
        const V d = s_.op(c, "getDimensions", Attr::ReadOnly, dims, 72, {h, mip});
        const V one = s_.m.i32(1);
        std::array<bool, 4> present{};
        if (kind == 1) {
            present = {true, false, false, true};
        } else if (kind == 2 || kind == 5 || kind == 6) {
            present = {true, true, false, true};
        } else {
            present = {true, true, true, true};
        }
            const auto* data = std::get_if<VectorOperand>(&fields->data);
        if (data == nullptr) fail("an image result");
        std::uint8_t k = 0;
        for (std::uint32_t ch = 0; ch < 4; ++ch) {
            if ((dmask >> ch & 1) == 0) continue;
            const V x = (ch == 3 && uav) || !present[ch]
                            ? one
                            : s_.m.extract(c, d, ch);
            write(inst, Reg::v(static_cast<std::uint8_t>(data->reg + k)), x, guard_value);
            ++k;
        }
        return;
    }
    if (uav && op != 66) fail("sampling a UAV");
    const V h = handle(site, src, false, props);
    const V fu = s_.m.undef(s_.t.f32);
    const V iu = s_.m.undef(s_.t.i32);
    V r;
    if (sample) {
        const auto sampler_entry = descriptors_.find({site, true});
        if (sampler_entry == descriptors_.end()) fail("an untracked sampler");
        const Source& ss = *sampler_entry->second;
        const bool cmp = op == 64 || op == 65;
        if (cmp && (a16 || std::popcount(static_cast<unsigned int>(dmask)) != 1))
            fail("a comparison sample of 16-bit coordinates or several channels");
        const V sh = handle(site, ss, true, {14 | (cmp ? 0x8000u : 0u), 0});
        const bool offset_word = name.ends_with("_o");
        const std::size_t first = (offset_word ? 1u : 0u) + (cmp ? 1u : 0u);
        const std::size_t bias_words = op == 61 ? 1u : 0u;
        const std::size_t spatial = coords - ((kind == 6 || kind == 7) ? 1u : 0u);
        const std::size_t grads = op == 63 ? 2 * spatial : 0;
        std::vector<V> cs(4, fu);
        const auto to_float = [&](V x) {
            if (a16) return s_.op(cur_, "legacyF16ToF32", Attr::ReadNone, s_.t.f32, 131, {x});
            return as_float(x);
        };
        for (std::size_t k = 0; k < coords; ++k) {
            const V x = addr_val(first + grads + k, bias_words);
            cs[k] = to_float(x);
        }
        if (kind == 5) {
            if (fields->unorm || op == 63 || op == 81)
                fail("a cube sample with texel coordinates, gradients or a level query");
            const std::array<V, 3> dir = cube_direction(cs[0], cs[1], cs[2]);
            cs[0] = dir[0];
            cs[1] = dir[1];
            cs[2] = dir[2];
        }
        if (fields->unorm) {
            const Ty i32t = s_.t.i32;
            const Ty dims = s_.m.named_struct("dx.types.Dimensions", {i32t, i32t, i32t, i32t});
            const V z = s_.m.i32(0);
            const V d = s_.op(c, "getDimensions", Attr::ReadOnly, dims, 72, {h, z});
            for (std::size_t k = 0; k < spatial; ++k) {
                const V n = s_.m.extract(c, d, static_cast<std::uint32_t>(k));
                const V f = s_.m.cast(c, Cast::UiToFp, n, s_.t.f32);
                cs[k] = s_.m.bin(c, Bin::SDiv, cs[k], f);
            }
        }
        if (op == 81) {
                    const auto* data = std::get_if<VectorOperand>(&fields->data);
            if (data == nullptr) fail("an image result");
            const Ty f32t = s_.t.f32;
            std::uint8_t k = 0;
            for (std::uint32_t ch = 0; ch < 2; ++ch) {
                if ((dmask >> ch & 1) == 0) continue;
                const V clamped = s_.m.boolean(ch == 0);
                const V lod = s_.op(c, "calculateLOD.f32", Attr::ReadOnly, f32t, 81,
                                    {h, sh, cs[0], cs[1], cs[2], clamped});
                const V x = as_int(lod);
                write(inst, Reg::v(static_cast<std::uint8_t>(data->reg + k)), x, guard_value);
                ++k;
            }
            return;
        }
        const std::size_t offsets =
            kind == 5 ? 0 : std::min<std::size_t>(coords - ((kind == 6 || kind == 7) ? 1u : 0u), 3);
        const V zero = s_.m.i32(0);
        std::vector<V> os(3, iu);
        for (std::size_t k = 0; k < offsets; ++k) os[k] = zero;
        std::vector<V> args = {h, sh};
        args.insert(args.end(), cs.begin(), cs.end());
        if (op == 60) {
            if (offset_word) {
                const V w = addr_val(0);
                const std::optional<std::int64_t> constant = s_.m.int_value(w);
                if (!constant) fail("a sample offset that is not constant");
                const std::uint32_t bits = static_cast<std::uint32_t>(*constant);
                for (std::size_t k = 0; k < offsets; ++k) {
                    const std::int32_t x = static_cast<std::int32_t>((bits >> (8 * k)) & 0x3F);
                    const std::int32_t signed_offset = x >= 32 ? x - 64 : x;
                    if (signed_offset < -8 || signed_offset > 7) fail("a sample offset past -8..7");
                    os[k] = s_.m.i32(static_cast<std::uint32_t>(signed_offset));
                }
            }
            args.insert(args.end(), os.begin(), os.end());
            args.push_back(fu);
            r = s_.op(c, "sample.f32", Attr::ReadOnly, s_.t.res_ret, 60, args);
        } else if (op == 61) {
            const V bias = to_float(addr_val(0));
            args.insert(args.end(), os.begin(), os.end());
            args.push_back(bias);
            args.push_back(fu);
            r = s_.op(c, "sampleBias.f32", Attr::ReadOnly, s_.t.res_ret, 61, args);
        } else if (op == 63) {
            std::vector<V> g(6, fu);
            for (std::size_t k = 0; k < spatial; ++k) {
                const V x = addr_val(first + k);
                g[k] = to_float(x);
                const V y = addr_val(first + spatial + k);
                g[3 + k] = to_float(y);
            }
            args.insert(args.end(), os.begin(), os.end());
            args.insert(args.end(), g.begin(), g.end());
            args.push_back(fu);
            r = s_.op(c, "sampleGrad.f32", Attr::ReadOnly, s_.t.res_ret, 63, args);
        } else if (op == 64 || op == 65) {
            const V reference = as_float(addr_val(first - 1));
            args.insert(args.end(), os.begin(), os.end());
            args.push_back(reference);
            std::string op_name = "sampleCmpLevelZero.f32";
            if (op == 64) {
                args.push_back(fu);
                op_name = "sampleCmp.f32";
            }
            r = s_.op(c, op_name, Attr::ReadOnly, s_.t.res_ret, op, args);
        } else if (op == 62) {
            V lod;
            if (name == "image_sample_l") {
                lod = to_float(addr_val(coords));
            } else {
                lod = s_.m.f32(0);
            }
            args.insert(args.end(), os.begin(), os.end());
            args.push_back(lod);
            r = s_.op(c, "sampleLevel.f32", Attr::ReadOnly, s_.t.res_ret, 62, args);
        } else {
            if (name == "image_gather4") fail("a gather with implicit level");
            if (std::popcount(static_cast<unsigned int>(dmask)) != 1)
                fail("a gather of several channels");
            if (offset_word) {
                const V w = addr_val(0);
                const std::array<std::tuple<std::size_t, std::uint32_t>, 2> bits = {
                    std::tuple<std::size_t, std::uint32_t>{0, 26},
                    std::tuple<std::size_t, std::uint32_t>{1, 18}};
                for (const auto& entry : bits) {
                    const V left = s_.m.i32(std::get<1>(entry));
                    const V t = s_.m.bin(c, Bin::Shl, w, left);
                    const V k26 = s_.m.i32(26);
                    os[std::get<0>(entry)] = s_.m.bin(c, Bin::AShr, t, k26);
                }
            }
            args.push_back(os[0]);
            args.push_back(os[1]);
            args.push_back(s_.m.i32(std::countr_zero(static_cast<unsigned int>(dmask))));
            r = s_.op(c, "textureGather.f32", Attr::ReadOnly, s_.t.res_ret, 73, args);
        }
    } else {
        std::vector<V> cs(3, iu);
        for (std::size_t k = 0; k < coords; ++k) cs[k] = addr_val(k);
        V mip;
        if (name == "image_load_mip") {
            mip = addr_val(coords);
        } else if (uav) {
            s_.flags |= feature::kTypedUavLoads;
            mip = iu;
        } else {
            mip = s_.m.i32(0);
        }
        std::vector<V> args = {h, mip, cs[0], cs[1], cs[2], iu, iu, iu};
        r = s_.op(c, "textureLoad.f32", Attr::ReadOnly, s_.t.res_ret, 66, args);
    }
    const auto* data = std::get_if<VectorOperand>(&fields->data);
    if (data == nullptr) fail("an image result");
    std::vector<std::uint32_t> channels;
    if (op == 73) {
        channels = {0, 1, 2, 3};
    } else {
        for (std::uint32_t ch = 0; ch < 4; ++ch)
            if ((dmask >> ch & 1) != 0) channels.push_back(ch);
    }
    std::vector<V> xs;
    for (std::uint32_t ch : channels) {
        const V x = s_.m.extract(c, r, ch);
        if (d16) {
            xs.push_back(s_.op(c, "legacyF32ToF16", Attr::ReadNone, s_.t.i32, 130, {x}));
        } else {
            xs.push_back(as_int(x));
        }
    }
    if (d16) {
        const V k16 = s_.m.i32(16);
        std::vector<V> packed;
        for (std::size_t k = 0; k < xs.size(); k += 2) {
            if (k + 1 < xs.size()) {
                const V hi = s_.m.bin(c, Bin::Shl, xs[k + 1], k16);
                packed.push_back(s_.m.bin(c, Bin::Or, xs[k], hi));
            } else {
                packed.push_back(xs[k]);
            }
        }
        xs = packed;
    }
    for (std::size_t k = 0; k < xs.size(); ++k)
        write(inst, Reg::v(static_cast<std::uint8_t>(data->reg + k)), xs[k], guard_value);
}

std::array<V, 3> Lower::cube_direction(V s, V t, V face) {
    const std::size_t c = cur_;
    const auto k = [&](float v) { return s_.m.f32(std::bit_cast<std::uint32_t>(v)); };
    const V two = k(2.0f);
    const V three = k(3.0f);
    V u = s_.m.bin(c, Bin::Mul, s, two);
    u = s_.m.bin(c, Bin::Sub, u, three);
    V v = s_.m.bin(c, Bin::Mul, t, two);
    v = s_.m.bin(c, Bin::Sub, v, three);
    const V z = s_.m.f32(0x8000'0000u);
    const V nu = s_.m.bin(c, Bin::Sub, z, u);
    const V nv = s_.m.bin(c, Bin::Sub, z, v);
    const V one = k(1.0f);
    const V mone = k(-1.0f);
    const V f = s_.m.cast(c, Cast::FpToUi, face, s_.t.i32);
    const std::array<std::array<V, 6>, 3> table = {{
        {one, mone, u, u, u, nu},
        {nv, nv, one, mone, nv, nv},
        {nu, u, v, nv, one, mone},
    }};
    std::array<V, 3> out{};
    for (std::size_t row = 0; row < 3; ++row) {
        V r = table[row][0];
        for (std::size_t n = 1; n < 6; ++n) {
            const V kn = s_.m.i32(static_cast<std::uint32_t>(n));
            const V is = s_.m.cmp(c, pred::kEq, f, kn);
            r = s_.m.select(c, is, table[row][n], r);
        }
        out[row] = r;
    }
    return out;
}

}  // namespace g6shim::shader::emit
