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

namespace {

constexpr std::array<std::string_view, 14> kFloat1 = {
    "v_rcp_f16",   "v_sqrt_f16",  "v_rsq_f16",  "v_log_f16",   "v_exp_f16",
    "v_fract_f16", "v_trunc_f16", "v_floor_f16", "v_ceil_f16", "v_rndne_f16",
    "v_sin_f16",   "v_cos_f16",   "v_cvt_f16_u16", "v_cvt_f16_i16"};
constexpr std::array<std::string_view, 6> kFloat2 = {
    "v_add_f16", "v_sub_f16", "v_subrev_f16", "v_mul_f16", "v_max_f16", "v_min_f16"};
constexpr std::array<std::string_view, 7> kFloat3 = {
    "v_fma_f16", "v_mad_f16", "v_fmac_f16", "v_mac_f16", "v_max3_f16", "v_min3_f16",
    "v_med3_f16"};
constexpr std::array<std::string_view, 15> kInt16 = {
    "v_add_nc_u16",  "v_sub_nc_u16",  "v_subrev_nc_u16",     "v_mul_lo_u16", "v_lshlrev_b16",
    "v_lshrrev_b16", "v_ashrrev_i16", "v_max_u16",           "v_max_i16",    "v_min_u16",
    "v_min_i16",     "v_cvt_u16_f16", "v_cvt_i16_f16",       "v_cvt_norm_i16_f16",
    "v_cvt_norm_u16_f16"};

constexpr std::string_view kComparePrefix = "v_cmp_";
constexpr std::string_view kCompareExecPrefix = "v_cmpx_";

bool member(std::span<const std::string_view> list, std::string_view value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

std::string_view compare_rest(std::string_view name) {
    if (name.starts_with(kCompareExecPrefix)) return name.substr(kCompareExecPrefix.size());
    return name.substr(kComparePrefix.size());
}

}  // namespace

void Lower::valu(const IrInst& inst, std::size_t b, std::size_t i) {
    const auto* fields = std::get_if<Valu>(&inst.fields);
    if (fields == nullptr) std::abort();
    const std::string_view name = inst.name();
    const auto& compares = x_->res->waterfall.compares;
    if (std::any_of(compares.begin(), compares.end(),
                    [&](const Site& site) { return site.block == b && site.index == i; })) {
        lane(inst, true);
        return;
    }
    if (name.starts_with(kComparePrefix) || name.starts_with(kCompareExecPrefix)) {
        try {
            if (fields->dpp) fail("a compare with DPP");
            compare(inst, compare_rest(name));
        } catch (const WhyError& error) {
            for (const auto& write : inst.writes) lane_fail_[write.second] = error.why;
            throw;
        }
        return;
    }
    std::optional<V> guard_value = guard(b, i);
    if (fields->dpp) {
        const auto [x, enable] = dpp(inst, *fields->dpp);
        if (guard_value && enable)
            guard_value = s_.m.bin(cur_, Bin::And, *guard_value, *enable);
        else if (!guard_value)
            guard_value = enable;
        src0_ = x;
    }
    try {
        valu_op(inst, guard_value);
    } catch (...) {
        src0_ = std::nullopt;
        throw;
    }
    src0_ = std::nullopt;
}

std::pair<V, std::optional<V>> Lower::dpp(const IrInst& inst, const Dpp& dpp_value) {
    const auto* fields = std::get_if<Valu>(&inst.fields);
    if (fields == nullptr) std::abort();
    if (fields->src.empty()) fail("a DPP operand");
    const auto* first = std::get_if<VectorOperand>(&fields->src.front());
    if (first == nullptr) fail("a DPP operand");
    const std::uint8_t reg = first->reg;
    const std::optional<Value> read = inst.read(Reg::v(reg));
    if (!read) fail("a DPP operand");
    const V x = i32_of(*read);
    bool quad = false;
    if (const auto* row = std::get_if<DppRow>(&dpp_value)) quad = row->ctrl <= 0xFF;
    const bool mesh_quad = s_.mesh.has_value() && quad;
    if (!mesh_quad) cross_lane(quad);
    const std::size_t c = cur_;
    const V lane = lane_index();
    const auto k = [&](std::uint32_t n) { return s_.m.i32(n); };
    const V sixteen_mask = k(~15u);
    const V low = k(15);
    const V row_value = s_.m.bin(c, Bin::And, lane, sixteen_mask);
    const V pos = s_.m.bin(c, Bin::And, lane, low);
    V src;
    std::optional<V> inside;
    std::optional<std::pair<std::uint8_t, std::uint8_t>> masks;
    bool zero = false;
    if (const auto* lanes = std::get_if<DppLanes>(&dpp_value)) {
        const V seven = k(7);
        const V not7 = k(~7u);
        const V three = k(3);
        const V p = s_.m.bin(c, Bin::And, lane, seven);
        const V sh = s_.m.bin(c, Bin::Mul, p, three);
        const V sel = k(lanes->sel);
        V pick = s_.m.bin(c, Bin::LShr, sel, sh);
        pick = s_.m.bin(c, Bin::And, pick, seven);
        const V base = s_.m.bin(c, Bin::And, lane, not7);
        src = s_.m.bin(c, Bin::Or, base, pick);
    } else {
        const auto& pattern = std::get<DppRow>(dpp_value);
        const std::uint32_t ctrl = pattern.ctrl;
        const std::uint32_t n = ctrl & 15;
        if (ctrl <= 0xFF) {
            const V three = k(3);
            const V one = k(1);
            const V not3 = k(~3u);
            const V p = s_.m.bin(c, Bin::And, lane, three);
            const V sh = s_.m.bin(c, Bin::Shl, p, one);
            const V perm = k(ctrl);
            V pick = s_.m.bin(c, Bin::LShr, perm, sh);
            pick = s_.m.bin(c, Bin::And, pick, three);
            const V base = s_.m.bin(c, Bin::And, lane, not3);
            src = s_.m.bin(c, Bin::Or, base, pick);
        } else if ((ctrl >= 0x101 && ctrl <= 0x10F) || (ctrl >= 0x111 && ctrl <= 0x11F)) {
            const V nn = k(n);
            const bool left = ctrl < 0x110;
            const Bin op = left ? Bin::Add : Bin::Sub;
            const std::uint8_t p = left ? pred::kIult : pred::kIuge;
            const V s = s_.m.bin(c, op, lane, nn);
            const V bound = left ? k(16 - n) : nn;
            src = s;
            inside = s_.m.cmp(c, p, pos, bound);
        } else if (ctrl >= 0x121 && ctrl <= 0x12F) {
            const V nn = k(n);
            V t = s_.m.bin(c, Bin::Sub, pos, nn);
            t = s_.m.bin(c, Bin::And, t, low);
            src = s_.m.bin(c, Bin::Or, row_value, t);
        } else if (ctrl == 0x140 || ctrl == 0x141) {
            const V m = k(ctrl == 0x140 ? 15u : 7u);
            src = s_.m.bin(c, Bin::Xor, lane, m);
        } else if (ctrl >= 0x150 && ctrl <= 0x15F) {
            const V nn = k(n);
            src = s_.m.bin(c, Bin::Or, row_value, nn);
        } else if (ctrl >= 0x160 && ctrl <= 0x16F) {
            const V nn = k(n);
            src = s_.m.bin(c, Bin::Xor, lane, nn);
        } else {
            fail("a DPP lane pattern");
        }
        if (pattern.row_mask != 0xF || pattern.bank_mask != 0xF)
            masks = std::pair<std::uint8_t, std::uint8_t>{pattern.row_mask, pattern.bank_mask};
        zero = pattern.bound_ctrl;
    }
    V r = mesh_quad ? quad_pick(x, src) : read_lane(x, src);
    std::optional<V> enable;
    if (inside) {
        if (zero) {
            r = s_.m.select(c, *inside, r, k(0));
        } else {
            enable = *inside;
        }
    }
    if (masks) {
        const V four = k(4);
        const V two = k(2);
        const V three = k(3);
        V ri = s_.m.bin(c, Bin::LShr, lane, four);
        ri = s_.m.bin(c, Bin::And, ri, three);
        V bi = s_.m.bin(c, Bin::LShr, lane, two);
        bi = s_.m.bin(c, Bin::And, bi, three);
        const V rm = k(masks->first);
        const V bm = k(masks->second);
        const V rb = s_.m.bin(c, Bin::LShr, rm, ri);
        const V bb = s_.m.bin(c, Bin::LShr, bm, bi);
        V both = s_.m.bin(c, Bin::And, rb, bb);
        const V one = k(1);
        const V z = k(0);
        both = s_.m.bin(c, Bin::And, both, one);
        const V on = s_.m.cmp(c, pred::kNe, both, z);
        enable = enable ? s_.m.bin(c, Bin::And, *enable, on) : on;
    }
    return {r, enable};
}

void Lower::valu_op(const IrInst& inst, std::optional<V> guard_value) {
    const auto* fields = std::get_if<Valu>(&inst.fields);
    if (fields == nullptr) std::abort();
    const std::string_view name = inst.name();
    const std::size_t c = cur_;
    if (valu16(inst, name, guard_value)) return;
    if (name.starts_with("v_pk_")) {
        packed16(inst, name.substr(5), guard_value);
        return;
    }
    if (name == "v_lshlrev_b64" || name == "v_lshrrev_b64" || name == "v_ashrrev_i64") {
        const V n = src(inst, 0);
        const V x = src64(inst, 1);
        const Ty i64t = s_.m.int_type(64);
        const V k63 = s_.m.i32(63);
        V shift = s_.m.bin(c, Bin::And, n, k63);
        shift = s_.m.cast(c, Cast::ZExt, shift, i64t);
        Bin op = Bin::AShr;
        if (name == "v_lshlrev_b64") op = Bin::Shl;
        else if (name == "v_lshrrev_b64") op = Bin::LShr;
        const V r = s_.m.bin(c, op, x, shift);
        if (!fields->dst) fail("a 64-bit result");
        const auto* dst = std::get_if<VectorOperand>(&*fields->dst);
        if (dst == nullptr) fail("a 64-bit result");
        const auto [lo, hi] = split64(r);
        write(inst, Reg::v(dst->reg), lo, guard_value);
        write(inst, Reg::v(static_cast<std::uint8_t>(dst->reg + 1)), hi, guard_value);
        return;
    }
    if (name == "v_cubeid_f32" || name == "v_cubesc_f32" || name == "v_cubetc_f32" ||
        name == "v_cubema_f32") {
        const V x = fsrc(inst, 0);
        const V y = fsrc(inst, 1);
        const V z = fsrc(inst, 2);
        const V ax = unary(6, x);
        const V ay = unary(6, y);
        const V az = unary(6, z);
        const V zx = s_.m.cmp(c, pred::kOge, az, ax);
        const V zy = s_.m.cmp(c, pred::kOge, az, ay);
        const V zmaj = s_.m.bin(c, Bin::And, zx, zy);
        const V ymaj = s_.m.cmp(c, pred::kOge, ay, ax);
        const V zero = s_.m.f32(0);
        const auto neg = [&](V v) { return s_.m.cmp(c, pred::kOlt, v, zero); };
        const auto minus = [&](V v) {
            const V sign = s_.m.f32(0x8000'0000u);
            return s_.m.bin(c, Bin::Sub, sign, v);
        };
        const auto pick = [&](V zv, V yv, V xv) {
            const V t = s_.m.select(c, ymaj, yv, xv);
            return s_.m.select(c, zmaj, zv, t);
        };
        V r;
        if (name == "v_cubeid_f32") {
            const auto kf = [&](float v) { return s_.m.f32(std::bit_cast<std::uint32_t>(v)); };
            const V nz = neg(z);
            const V ny = neg(y);
            const V nx = neg(x);
            const V f5 = kf(5.0f);
            const V f4 = kf(4.0f);
            const V f3 = kf(3.0f);
            const V f2 = kf(2.0f);
            const V f1 = kf(1.0f);
            const V f0 = kf(0.0f);
            const V zf = s_.m.select(c, nz, f5, f4);
            const V yf = s_.m.select(c, ny, f3, f2);
            const V xf = s_.m.select(c, nx, f1, f0);
            r = pick(zf, yf, xf);
        } else if (name == "v_cubesc_f32") {
            const V nz = neg(z);
            const V nx = neg(x);
            const V mx = minus(x);
            const V mz = minus(z);
            const V zf = s_.m.select(c, nz, mx, x);
            const V xf = s_.m.select(c, nx, z, mz);
            r = pick(zf, x, xf);
        } else if (name == "v_cubetc_f32") {
            const V ny = neg(y);
            const V my = minus(y);
            const V mz = minus(z);
            const V yf = s_.m.select(c, ny, mz, z);
            r = pick(my, yf, my);
        } else {
            const V t = pick(z, y, x);
            const V two = s_.m.f32(std::bit_cast<std::uint32_t>(2.0f));
            r = s_.m.bin(c, Bin::Mul, t, two);
        }
        write_f(inst, r, guard_value);
        return;
    }
    if (name == "v_movrels_b32" || name == "v_movreld_b32" || name == "v_movrelsd_b32") {
        const std::optional<Value> m0_read = inst.read(Reg::s(kM0));
        if (!m0_read) fail("M0");
        const V m0 = i32_of(*m0_read);
        if (fields->src.empty() || !fields->dst) fail("an M0-relative move");
        const auto* source = std::get_if<VectorOperand>(&fields->src.front());
        const auto* target = std::get_if<VectorOperand>(&*fields->dst);
        if (source == nullptr || target == nullptr) fail("an M0-relative move");
        const std::uint8_t sreg = source->reg;
        const std::uint8_t dreg = target->reg;
        const bool indexed_src = name != "v_movreld_b32";
        V value;
        if (indexed_src) {
            V r = s_.m.i32(0);
            const std::uint8_t last = static_cast<std::uint8_t>(255 - sreg);
            for (std::uint8_t k = 0; k <= last; ++k) {
                const std::optional<Value> read =
                    inst.read(Reg::v(static_cast<std::uint8_t>(sreg + k)));
                if (!read) break;
                const V x = i32_of(*read);
                const V kk = s_.m.i32(k);
                const V is = s_.m.cmp(c, pred::kEq, m0, kk);
                r = s_.m.select(c, is, x, r);
            }
            value = r;
        } else {
            value = src(inst, 0);
        }
        if (name == "v_movrels_b32") {
            write_i(inst, value, guard_value);
            return;
        }
        const std::uint8_t last = static_cast<std::uint8_t>(255 - dreg);
        for (std::uint8_t k = 0; k <= last; ++k) {
            if (!inst.write(Reg::v(static_cast<std::uint8_t>(dreg + k)))) break;
            V old = s_.m.i32(0);
            if (const std::optional<Value> read = inst.read(Reg::v(static_cast<std::uint8_t>(dreg + k))))
                old = i32_of(*read);
            const V kk = s_.m.i32(k);
            const V is = s_.m.cmp(c, pred::kEq, m0, kk);
            const V x = s_.m.select(c, is, value, old);
            write_placed(inst, Reg::v(static_cast<std::uint8_t>(dreg + k)), x, guard_value);
        }
        return;
    }
    if (name == "v_swap_b32") {
        if (fields->src.empty() || !fields->dst) fail("v_swap_b32");
        const auto* source = std::get_if<VectorOperand>(&fields->src.front());
        const auto* target = std::get_if<VectorOperand>(&*fields->dst);
        if (source == nullptr || target == nullptr) fail("v_swap_b32");
        const std::uint8_t sreg = source->reg;
        const std::uint8_t dreg = target->reg;
        const std::optional<Value> a_read = inst.read(Reg::v(dreg));
        if (!a_read) fail("v_swap_b32");
        const V a = i32_of(*a_read);
        const std::optional<Value> b_read = inst.read(Reg::v(sreg));
        if (!b_read) fail("v_swap_b32");
        const V b = i32_of(*b_read);
        write_placed(inst, Reg::v(dreg), b, guard_value);
        if (dreg != sreg) write_placed(inst, Reg::v(sreg), a, guard_value);
        return;
    }
    if (name == "v_ffbh_u32" || name == "v_ffbh_i32") {
        const V a = src(inst, 0);
        const std::uint32_t op = name == "v_ffbh_u32" ? 33 : 34;
        const V r = s_.op(c, "unaryBits.i32", Attr::ReadNone, s_.t.i32, op, {a});
        write_i(inst, r, guard_value);
        return;
    }
    if (name == "v_mov_b32") {
        if (fields->abs != 0 || fields->neg != 0 || fields->clamp || fields->omod != 0) {
            const V a = fsrc(inst, 0);
            write_f(inst, a, guard_value);
            return;
        }
        const V a = src(inst, 0);
        write_i(inst, a, guard_value);
        return;
    }
    if (name == "v_add_f32" || name == "v_sub_f32" || name == "v_subrev_f32" ||
        name == "v_mul_f32" || name == "v_mul_legacy_f32" || name == "v_max_f32" ||
        name == "v_min_f32") {
        const V a = fsrc(inst, 0);
        const V b = fsrc(inst, 1);
        V r;
        if (name == "v_add_f32") r = s_.m.bin(c, Bin::Add, a, b);
        else if (name == "v_sub_f32") r = s_.m.bin(c, Bin::Sub, a, b);
        else if (name == "v_subrev_f32") r = s_.m.bin(c, Bin::Sub, b, a);
        else if (name == "v_mul_f32") r = s_.m.bin(c, Bin::Mul, a, b);
        else if (name == "v_mul_legacy_f32") r = legacy_mul(a, b);
        else if (name == "v_max_f32") r = max_min(true, a, b);
        else r = max_min(false, a, b);
        write_f(inst, r, guard_value);
        return;
    }
    if (name == "v_fma_f32" || name == "v_fmaak_f32" || name == "v_fmamk_f32") {
        const V a = fsrc(inst, 0);
        const V b = fsrc(inst, 1);
        const V d = fsrc(inst, 2);
        write_f(inst, fmad(a, b, d), guard_value);
        return;
    }
    if (name == "v_fmac_f32" || name == "v_mac_f32" || name == "v_mac_legacy_f32") {
        const V a = fsrc(inst, 0);
        const V b = fsrc(inst, 1);
        if (!fields->dst) fail("an accumulator");
        const auto* target = std::get_if<VectorOperand>(&*fields->dst);
        if (target == nullptr) fail("an accumulator");
        const std::optional<Value> read = inst.read(Reg::v(target->reg));
        if (!read) fail("an accumulator");
        const auto found = floats_.find(*read);
        const V acc = found != floats_.end() ? found->second : as_float(i32_of(*read));
        V r;
        if (name == "v_fmac_f32") {
            r = fmad(a, b, acc);
        } else if (name == "v_mac_f32") {
            const V m = s_.m.bin(c, Bin::Mul, a, b);
            r = s_.m.bin(c, Bin::Add, m, acc);
        } else {
            const V m = legacy_mul(a, b);
            r = s_.m.bin(c, Bin::Add, m, acc);
        }
        write_f(inst, r, guard_value);
        return;
    }
    if (name == "v_mad_f32" || name == "v_madak_f32" || name == "v_madmk_f32" ||
        name == "v_mad_legacy_f32") {
        const V a = fsrc(inst, 0);
        const V b = fsrc(inst, 1);
        const V d = fsrc(inst, 2);
        const V m = name == "v_mad_legacy_f32" ? legacy_mul(a, b) : s_.m.bin(c, Bin::Mul, a, b);
        write_f(inst, s_.m.bin(c, Bin::Add, m, d), guard_value);
        return;
    }
    if (name == "v_max3_f32" || name == "v_min3_f32" || name == "v_med3_f32") {
        const V a = fsrc(inst, 0);
        const V b = fsrc(inst, 1);
        const V d = fsrc(inst, 2);
        V r;
        if (name == "v_max3_f32") {
            const V t = max_min(true, a, b);
            r = max_min(true, t, d);
        } else if (name == "v_min3_f32") {
            const V t = max_min(false, a, b);
            r = max_min(false, t, d);
        } else {
            r = med3(a, b, d);
        }
        write_f(inst, r, guard_value);
        return;
    }
    if (name == "v_rcp_f32" || name == "v_rcp_iflag_f32") {
        const V a = fsrc(inst, 0);
        const V one = s_.m.f32(std::bit_cast<std::uint32_t>(1.0f));
        write_f(inst, s_.m.bin(c, Bin::SDiv, one, a), guard_value);
        return;
    }
    if (name == "v_rsq_f32" || name == "v_sqrt_f32" || name == "v_log_f32" ||
        name == "v_exp_f32" || name == "v_fract_f32" || name == "v_trunc_f32" ||
        name == "v_floor_f32" || name == "v_ceil_f32" || name == "v_rndne_f32" ||
        name == "v_sin_f32" || name == "v_cos_f32") {
        const V a = fsrc(inst, 0);
        std::uint32_t op = 12;
        if (name == "v_rsq_f32") op = 25;
        else if (name == "v_sqrt_f32") op = 24;
        else if (name == "v_log_f32") op = 23;
        else if (name == "v_exp_f32") op = 21;
        else if (name == "v_fract_f32") op = 22;
        else if (name == "v_trunc_f32") op = 29;
        else if (name == "v_floor_f32") op = 27;
        else if (name == "v_ceil_f32") op = 28;
        else if (name == "v_rndne_f32") op = 26;
        else if (name == "v_sin_f32") op = 13;
        V r;
        if (op == 12 || op == 13) {
            r = trig_turns(a, op == 12);
        } else if (op == 22) {
            r = fract(a, 1.0f, std::bit_cast<float>(0x3F7F'FFFFu));
        } else {
            r = unary(op, a);
        }
        write_f(inst, r, guard_value);
        return;
    }
    if (name == "v_cvt_i32_f32" || name == "v_cvt_u32_f32" || name == "v_cvt_flr_i32_f32" ||
        name == "v_cvt_rpi_i32_f32") {
        V a = fsrc(inst, 0);
        if (name == "v_cvt_rpi_i32_f32") {
            const V h = s_.m.f32(std::bit_cast<std::uint32_t>(0.5f));
            a = s_.m.bin(c, Bin::Add, a, h);
        }
        if (name != "v_cvt_i32_f32" && name != "v_cvt_u32_f32") a = unary(27, a);
        const Cast op = name == "v_cvt_u32_f32" ? Cast::FpToUi : Cast::FpToSi;
        write_i(inst, s_.m.cast(c, op, a, s_.t.i32), guard_value);
        return;
    }
    if (name == "v_cvt_f32_i32" || name == "v_cvt_f32_u32" || name == "v_cvt_f32_ubyte0" ||
        name == "v_cvt_f32_ubyte1" || name == "v_cvt_f32_ubyte2" || name == "v_cvt_f32_ubyte3") {
        V a = src(inst, 0);
        if (name.starts_with("v_cvt_f32_ubyte")) {
            const char digit = name[name.size() - 1];
            const V sh = s_.m.i32(8u * static_cast<std::uint32_t>(digit - '0'));
            const V ff = s_.m.i32(0xff);
            a = s_.m.bin(c, Bin::LShr, a, sh);
            a = s_.m.bin(c, Bin::And, a, ff);
        }
        const Cast op = name == "v_cvt_f32_i32" ? Cast::SiToFp : Cast::UiToFp;
        write_f(inst, s_.m.cast(c, op, a, s_.t.f32), guard_value);
        return;
    }
    if (name == "v_fma_mix_f32" || name == "v_fma_mixlo_f16" || name == "v_fma_mixhi_f16") {
        std::vector<V> xs;
        for (std::size_t i = 0; i < 3; ++i) {
            if (i >= fields->src.size()) fail("a missing operand");
            const V raw = operand(inst, fields->src[i]);
            V x;
            if ((fields->opsel_hi >> i & 1) != 0) {
                V h = raw;
                if ((fields->opsel >> i & 1) != 0) {
                    const V k16 = s_.m.i32(16);
                    h = s_.m.bin(c, Bin::LShr, raw, k16);
                }
                x = s_.op(c, "legacyF16ToF32", Attr::ReadNone, s_.t.f32, 131, {h});
            } else {
                x = as_float(raw);
            }
            if ((fields->neg_hi >> i & 1) != 0) x = unary(6, x);
            if ((fields->neg >> i & 1) != 0) {
                const V sign = s_.m.f32(0x8000'0000u);
                x = s_.m.bin(c, Bin::Sub, sign, x);
            }
            xs.push_back(x);
        }
        V r = fmad(xs[0], xs[1], xs[2]);
        if (name == "v_fma_mix_f32") {
            write_f(inst, r, guard_value);
            return;
        }
        if (fields->clamp) r = unary(7, r);
        if (!fields->dst) fail("a mixed-precision result");
        const auto* target = std::get_if<VectorOperand>(&*fields->dst);
        if (target == nullptr) fail("a mixed-precision result");
        V old = s_.m.i32(0);
        if (const std::optional<Value> read = inst.read(Reg::v(target->reg))) old = i32_of(*read);
        const V h = s_.op(c, "legacyF32ToF16", Attr::ReadNone, s_.t.i32, 130, {r});
        std::uint32_t keep = 0xFFFF'0000u;
        V half = h;
        if (name == "v_fma_mixhi_f16") {
            const V k16 = s_.m.i32(16);
            keep = 0xFFFF;
            half = s_.m.bin(c, Bin::Shl, h, k16);
        }
        const V keep_value = s_.m.i32(keep);
        old = s_.m.bin(c, Bin::And, old, keep_value);
        const V x = s_.m.bin(c, Bin::Or, old, half);
        write(inst, Reg::v(target->reg), x, guard_value);
        return;
    }
    if (name == "v_cvt_pkrtz_f16_f32") {
        const V a = fsrc(inst, 0);
        const V b = fsrc(inst, 1);
        const V lo = f16_toward_zero(a);
        const V hi = f16_toward_zero(b);
        const V sh = s_.m.i32(16);
        const V hi_up = s_.m.bin(c, Bin::Shl, hi, sh);
        write_i(inst, s_.m.bin(c, Bin::Or, lo, hi_up), guard_value);
        return;
    }
    if (name == "v_cvt_f16_f32" && fields->opsel == 0) {
        const V a = fsrc(inst, 0);
        const V r = s_.op(c, "legacyF32ToF16", Attr::ReadNone, s_.t.i32, 130, {a});
        if (!fields->dst) fail("a 16-bit result outside a VGPR");
        const auto* target = std::get_if<VectorOperand>(&*fields->dst);
        if (target == nullptr) fail("a 16-bit result outside a VGPR");
        const V value = low_half(inst, target->reg, r);
        write_i(inst, value, guard_value);
        return;
    }
    if (name == "v_pack_b32_f16") {
        std::vector<V> halves;
        for (std::size_t k = 0; k < 2; ++k) {
            if (k >= fields->src.size()) fail("a missing operand");
            V x = operand(inst, fields->src[k]);
            if ((fields->opsel >> k & 1) != 0) {
                const V k16 = s_.m.i32(16);
                x = s_.m.bin(c, Bin::LShr, x, k16);
            }
            const std::uint32_t mask_bits = (fields->abs >> k & 1) != 0 ? 0x7FFFu : 0xFFFFu;
            const V mask = s_.m.i32(mask_bits);
            x = s_.m.bin(c, Bin::And, x, mask);
            if ((fields->neg >> k & 1) != 0) {
                const V sign = s_.m.i32(0x8000);
                x = s_.m.bin(c, Bin::Xor, x, sign);
            }
            halves.push_back(x);
        }
        const V k16 = s_.m.i32(16);
        const V hi = s_.m.bin(c, Bin::Shl, halves[1], k16);
        write_i(inst, s_.m.bin(c, Bin::Or, halves[0], hi), guard_value);
        return;
    }
    if (name == "v_cvt_f32_f16" && fields->opsel == 0) {
        const V a = src(inst, 0);
        const V r = s_.op(c, "legacyF16ToF32", Attr::ReadNone, s_.t.f32, 131, {a});
        write_f(inst, r, guard_value);
        return;
    }
    if (name == "v_cndmask_b32") {
        const auto* scalar = fields->src.size() > 2 ? std::get_if<ScalarOperand>(&fields->src[2]) : nullptr;
        const auto mask = scalar != nullptr ? inst.read(Reg::s(scalar->code)) : std::nullopt;
        const auto found = mask ? x_->st->masks.find(*mask) : x_->st->masks.end();
        V cond;
        if (found != x_->st->masks.end()) {
            cond = boolean(found->second, c);
        } else {
            if (!console_lanes_ || !mask) fail("a select on an untracked mask");
            std::optional<V> high;
            if (x_->native->wave64) {
                const auto upper = inst.read(Reg::s(static_cast<std::uint8_t>(scalar->code + 1)));
                if (!upper) fail("a select on an untracked mask");
                high = i32_of(*upper);
            }
            cross_lane(false);
            cond = lane_bit(c, i32_of(*mask), high);
        }
        if (fields->abs != 0 || fields->neg != 0) {
            const V a = fsrc(inst, 0);
            const V b = fsrc(inst, 1);
            write_f(inst, s_.m.select(c, cond, b, a), guard_value);
            return;
        }
        const V a = src(inst, 0);
        const V b = src(inst, 1);
        write_i(inst, s_.m.select(c, cond, b, a), guard_value);
        return;
    }
    if (name == "v_add_co_u32" || name == "v_sub_co_u32" || name == "v_subrev_co_u32" ||
        name == "v_add_co_ci_u32" || name == "v_sub_co_ci_u32" || name == "v_subrev_co_ci_u32") {
        V a = src(inst, 0);
        V b = src(inst, 1);
        if (name.find("subrev") != std::string_view::npos) std::swap(a, b);
        std::optional<V> cin;
        if (name.ends_with("_ci_u32")) {
            std::optional<Value> mask;
            if (fields->src.size() > 2) {
                if (const auto* scalar = std::get_if<ScalarOperand>(&fields->src[2]))
                    mask = inst.read(Reg::s(scalar->code));
            }
            if (!mask) fail("a carry-in from an untracked mask");
            const auto found = x_->st->masks.find(*mask);
            if (found == x_->st->masks.end()) fail("a carry-in from an untracked mask");
            cin = boolean(found->second, c);
        }
        const auto result = add_sub_carry(c, name.starts_with("v_add"), a, b, cin);
        write_i(inst, result.first, guard_value);
        if (fields->carry) {
            if (const auto* scalar = std::get_if<ScalarOperand>(&*fields->carry)) {
                if (const std::optional<Value> w = inst.write(Reg::s(scalar->code)))
                    lane_bits_[*w] = result.second;
            }
        }
        for (const auto& write : inst.writes)
            vals_.emplace(write.second, Val::missing("a lane mask read as data"));
        return;
    }
    if (name == "v_readlane_b32") {
        const V a = src(inst, 0);
        const V lane = src(inst, 1);
        if (mask_data_ == MaskData::Host && lane.kind != V::Kind::Const) {
            s_.flags |= feature::kWaveOps;
        } else {
            cross_lane(false);
        }
        write_i(inst, read_lane(a, lane), guard_value);
        return;
    }
    if (name == "v_writelane_b32") {
        const V a = src(inst, 0);
        const V lane = src(inst, 1);
        if (!fields->dst) fail("a lane write");
        const auto* target = std::get_if<VectorOperand>(&*fields->dst);
        if (target == nullptr) fail("a lane write");
        V old = s_.m.i32(0);
        if (const std::optional<Value> read = inst.read(Reg::v(target->reg))) old = i32_of(*read);
        cross_lane(false);
        const V me = lane_index();
        const V is = s_.m.cmp(c, pred::kEq, me, lane);
        write_i(inst, s_.m.select(c, is, a, old), guard_value);
        return;
    }
    if (name == "v_readfirstlane_b32") {
        const auto& reads = x_->res->waterfall.reads;
        if (!inst.writes.empty() &&
            std::find(reads.begin(), reads.end(), inst.writes.front().second) != reads.end()) {
            const V a = src(inst, 0);
            write_i(inst, a, guard_value);
            return;
        }
        const V a = src(inst, 0);
        s_.flags |= feature::kWaveOps;
        const V r = s_.op(c, "waveReadLaneFirst.i32", Attr::NoUnwind, s_.t.i32, 118, {a});
        write_i(inst, r, guard_value);
        return;
    }
    const V r = integer(inst, name);
    write_i(inst, r, guard_value);
}

V Lower::src64(const IrInst& inst, std::size_t k) {
    const auto* fields = std::get_if<Valu>(&inst.fields);
    if (fields == nullptr) std::abort();
    if ((fields->abs >> k & 1) != 0 || (fields->neg >> k & 1) != 0 || fields->sdwa ||
        fields->dpp)
        fail("64-bit operand modifiers");
    const std::size_t c = cur_;
    const Ty i64t = s_.m.int_type(64);
    s_.flags |= feature::kInt64;
    if (k >= fields->src.size()) fail("a missing operand");
    const Operand& op = fields->src[k];
    const auto pair_of = [&](Reg lo, Reg hi) -> V {
        const std::optional<Value> lo_value = inst.read(lo);
        if (!lo_value) fail("an unread operand");
        const std::optional<Value> hi_value = inst.read(hi);
        if (!hi_value) fail("an unread operand");
        const V lo64 = s_.m.cast(c, Cast::ZExt, i32_of(*lo_value), i64t);
        const V hi64 = s_.m.cast(c, Cast::ZExt, i32_of(*hi_value), i64t);
        const V k32 = s_.m.int_const(i64t, 32);
        const V hi_up = s_.m.bin(c, Bin::Shl, hi64, k32);
        return s_.m.bin(c, Bin::Or, lo64, hi_up);
    };
    if (const auto* vector = std::get_if<VectorOperand>(&op))
        return pair_of(Reg::v(vector->reg), Reg::v(static_cast<std::uint8_t>(vector->reg + 1)));
    if (const auto* scalar = std::get_if<ScalarOperand>(&op); scalar && scalar->code != kNull)
        return pair_of(Reg::s(scalar->code), Reg::s(static_cast<std::uint8_t>(scalar->code + 1)));
    if (const auto* constant = std::get_if<ConstantOperand>(&op);
        constant && constant->code >= 128 && constant->code <= 208) {
        const std::optional<std::uint32_t> x = inline_constant(constant->code);
        if (!x) fail("a special operand");
        return s_.m.int_const(i64t, static_cast<std::int32_t>(*x));
    }
    const V x = operand(inst, op);
    return s_.m.cast(c, Cast::ZExt, x, i64t);
}

std::pair<V, V> Lower::split64(V x) {
    const std::size_t c = cur_;
    const Ty i64t = s_.m.int_type(64);
    const V k32 = s_.m.int_const(i64t, 32);
    const V lo = s_.m.cast(c, Cast::Trunc, x, s_.t.i32);
    const V hi_full = s_.m.bin(c, Bin::LShr, x, k32);
    const V hi = s_.m.cast(c, Cast::Trunc, hi_full, s_.t.i32);
    return {lo, hi};
}

V Lower::half_src(const IrInst& inst, std::size_t k) {
    const auto* fields = std::get_if<Valu>(&inst.fields);
    if (fields == nullptr) std::abort();
    if (k >= fields->src.size()) fail("a missing operand");
    const Operand& op = fields->src[k];
    bool high = (fields->opsel >> k & 1) != 0;
    if (fields->sdwa) {
        const std::uint8_t select = k < fields->sdwa->src_sel.size() ? fields->sdwa->src_sel[k] : 6;
        if (select == 4) {
        } else if (select == 5) {
            high = true;
        } else if (select != 6) {
            fail("a byte select on a 16-bit operation");
        }
    }
    V x = (src0_ && k == 0) ? *src0_ : f16_operand(inst, op);
    const std::size_t c = cur_;
    if (high) {
        const V k16 = s_.m.i32(16);
        x = s_.m.bin(c, Bin::LShr, x, k16);
    }
    const V m = s_.m.i32(0xFFFF);
    return s_.m.bin(c, Bin::And, x, m);
}

V Lower::half_fsrc(const IrInst& inst, std::size_t k) {
    const auto* fields = std::get_if<Valu>(&inst.fields);
    if (fields == nullptr) std::abort();
    const V h = half_src(inst, k);
    V f = s_.op(cur_, "legacyF16ToF32", Attr::ReadNone, s_.t.f32, 131, {h});
    if ((fields->abs >> k & 1) != 0) f = unary(6, f);
    if ((fields->neg >> k & 1) != 0) {
        const V z = s_.m.f32(0x8000'0000u);
        f = s_.m.bin(cur_, Bin::Sub, z, f);
    }
    return f;
}

void Lower::write_half(const IrInst& inst, V x, std::optional<V> guard_value) {
    const auto* fields = std::get_if<Valu>(&inst.fields);
    if (fields == nullptr) std::abort();
    if (!fields->dst) fail("a 16-bit result outside a VGPR");
    const auto* target = std::get_if<VectorOperand>(&*fields->dst);
    if (target == nullptr) fail("a 16-bit result outside a VGPR");
    const std::uint8_t reg = target->reg;
    bool high = (fields->opsel >> 3 & 1) != 0;
    bool keep = true;
    if (fields->sdwa) {
        const std::uint8_t dst_sel = fields->sdwa->dst_sel;
        const std::uint8_t dst_unused = fields->sdwa->dst_unused;
        if (dst_sel == 4 && dst_unused == 2) {
        } else if (dst_sel == 5 && dst_unused == 2) {
            high = true;
        } else if (dst_sel == 4 || dst_sel == 6) {
            keep = false;
        } else if (dst_sel == 5) {
            high = true;
            keep = false;
        } else {
            fail("an SDWA destination of a 16-bit operation");
        }
    }
    const std::size_t c = cur_;
    V old = s_.m.i32(0);
    if (keep) {
        if (const std::optional<Value> read = inst.read(Reg::v(reg))) {
            try {
                old = i32_of(*read);
            } catch (const WhyError&) {
                old = s_.m.i32(0);
            }
        }
    }
    const V k16 = s_.m.i32(16);
    const V low = s_.m.i32(0xFFFF);
    const V masked = s_.m.bin(c, Bin::And, x, low);
    V r;
    if (high) {
        const V o = s_.m.bin(c, Bin::And, old, low);
        const V up = s_.m.bin(c, Bin::Shl, masked, k16);
        r = s_.m.bin(c, Bin::Or, o, up);
    } else {
        const V keep_mask = s_.m.i32(0xFFFF'0000u);
        const V o = s_.m.bin(c, Bin::And, old, keep_mask);
        r = s_.m.bin(c, Bin::Or, o, masked);
    }
    write_placed(inst, Reg::v(reg), r, guard_value);
}

bool Lower::valu16(const IrInst& inst, std::string_view name, std::optional<V> guard_value) {
    if (!member(kFloat1, name) && !member(kFloat2, name) && !member(kFloat3, name) &&
        !member(kInt16, name) && name != "v_cvt_f16_f32" && name != "v_cvt_f32_f16")
        return false;
    valu16_op(inst, name, guard_value, kFloat1, kFloat2, kFloat3);
    return true;
}

void Lower::valu16_op(const IrInst& inst, std::string_view name, std::optional<V> guard_value,
                      std::span<const std::string_view> float1,
                      std::span<const std::string_view> float2,
                      std::span<const std::string_view> float3) {
    const auto* fields = std::get_if<Valu>(&inst.fields);
    if (fields == nullptr) std::abort();
    const std::size_t c = cur_;
    const Ty i32t = s_.t.i32;
    const Ty f32t = s_.t.f32;
    const auto float_result = [&](V f) {
        if (fields->omod != 0) fail("an output modifier on a 16-bit operation");
        if (fields->clamp) f = unary(7, f);
        const V h = s_.op(cur_, "legacyF32ToF16", Attr::ReadNone, i32t, 130, {f});
        write_half(inst, h, guard_value);
    };
    if (name == "v_cvt_f32_f16") {
        const V f = half_fsrc(inst, 0);
        write_f(inst, f, guard_value);
        return;
    }
    if (name == "v_cvt_f16_f32") {
        const V f = fsrc(inst, 0);
        float_result(f);
        return;
    }
    if (member(float2, name)) {
        const V a = half_fsrc(inst, 0);
        const V b = half_fsrc(inst, 1);
        V r;
        if (name == "v_add_f16") r = s_.m.bin(c, Bin::Add, a, b);
        else if (name == "v_sub_f16") r = s_.m.bin(c, Bin::Sub, a, b);
        else if (name == "v_subrev_f16") r = s_.m.bin(c, Bin::Sub, b, a);
        else if (name == "v_mul_f16") r = s_.m.bin(c, Bin::Mul, a, b);
        else if (name == "v_max_f16") r = max_min(true, a, b);
        else r = max_min(false, a, b);
        float_result(r);
        return;
    }
    if (member(float3, name)) {
        const V a = half_fsrc(inst, 0);
        const V b = half_fsrc(inst, 1);
        V acc;
        if (name == "v_fmac_f16" || name == "v_mac_f16") {
            if (!fields->dst) fail("an accumulator");
            const auto* target = std::get_if<VectorOperand>(&*fields->dst);
            if (target == nullptr) fail("an accumulator");
            const std::optional<Value> read = inst.read(Reg::v(target->reg));
            if (!read) fail("an accumulator");
            V x = i32_of(*read);
            if ((fields->opsel >> 3 & 1) != 0) {
                const V k16 = s_.m.i32(16);
                x = s_.m.bin(c, Bin::LShr, x, k16);
            }
            const V m = s_.m.i32(0xFFFF);
            x = s_.m.bin(c, Bin::And, x, m);
            acc = s_.op(c, "legacyF16ToF32", Attr::ReadNone, f32t, 131, {x});
        } else {
            acc = half_fsrc(inst, 2);
        }
        V r;
        if (name == "v_fma_f16" || name == "v_fmac_f16" || name == "v_mad_f16" ||
            name == "v_mac_f16") {
            r = fmad(a, b, acc);
        } else if (name == "v_max3_f16") {
            const V t = max_min(true, a, b);
            r = max_min(true, t, acc);
        } else if (name == "v_min3_f16") {
            const V t = max_min(false, a, b);
            r = max_min(false, t, acc);
        } else {
            r = med3(a, b, acc);
        }
        float_result(r);
        return;
    }
    if (member(float1, name)) {
        V r;
        if (name == "v_cvt_f16_u16" || name == "v_cvt_f16_i16") {
            V x = half_src(inst, 0);
            Cast op = Cast::UiToFp;
            if (name == "v_cvt_f16_i16") {
                const V k16 = s_.m.i32(16);
                const V t = s_.m.bin(c, Bin::Shl, x, k16);
                x = s_.m.bin(c, Bin::AShr, t, k16);
                op = Cast::SiToFp;
            }
            r = s_.m.cast(c, op, x, f32t);
        } else if (name == "v_rcp_f16") {
            const V a = half_fsrc(inst, 0);
            const V one = s_.m.f32(std::bit_cast<std::uint32_t>(1.0f));
            r = s_.m.bin(c, Bin::SDiv, one, a);
        } else {
            const V a = half_fsrc(inst, 0);
            std::uint32_t op = 12;
            if (name == "v_sqrt_f16") op = 24;
            else if (name == "v_rsq_f16") op = 25;
            else if (name == "v_log_f16") op = 23;
            else if (name == "v_exp_f16") op = 21;
            else if (name == "v_fract_f16") op = 22;
            else if (name == "v_trunc_f16") op = 29;
            else if (name == "v_floor_f16") op = 27;
            else if (name == "v_ceil_f16") op = 28;
            else if (name == "v_rndne_f16") op = 26;
            else if (name == "v_sin_f16") op = 13;
            if (op == 12 || op == 13) {
                r = trig_turns(a, op == 12);
            } else if (op == 22) {
                r = fract(a, 1.0f - 1.0f / 4096.0f, 1.0f - 1.0f / 2048.0f);
            } else {
                r = unary(op, a);
            }
        }
        float_result(r);
        return;
    }
    if (name == "v_cvt_norm_i16_f16" || name == "v_cvt_norm_u16_f16") {
        if (fields->clamp || fields->omod != 0) fail("modifiers on a 16-bit integer operation");
        const V r = cvt_norm16(inst, name == "v_cvt_norm_i16_f16");
        write_half(inst, r, guard_value);
        return;
    }
    if (fields->clamp || fields->omod != 0 || fields->abs != 0 || fields->neg != 0)
        fail("modifiers on a 16-bit integer operation");
    V r;
    if (name == "v_cvt_u16_f16" || name == "v_cvt_i16_f16") {
        const V a = half_fsrc(inst, 0);
        const bool is_unsigned = name == "v_cvt_u16_f16";
        const V lo = s_.m.f32(std::bit_cast<std::uint32_t>(is_unsigned ? 0.0f : -32768.0f));
        const V hi = s_.m.f32(std::bit_cast<std::uint32_t>(is_unsigned ? 65535.0f : 32767.0f));
        V t = binary(35, a, lo);
        t = binary(36, t, hi);
        const V converted = s_.m.cast(c, Cast::FpToSi, t, i32t);
        const V nan = s_.m.cmp(c, pred::kUno, a, a);
        const V z = s_.m.i32(0);
        r = s_.m.select(c, nan, z, converted);
    } else {
        V a = half_src(inst, 0);
        V b = half_src(inst, 1);
        const V k16 = s_.m.i32(16);
        const auto sext = [&](V x) {
            const V t = s_.m.bin(c, Bin::Shl, x, k16);
            return s_.m.bin(c, Bin::AShr, t, k16);
        };
        if (name.ends_with("_i16")) {
            a = sext(a);
            b = sext(b);
        }
        const V k15 = s_.m.i32(15);
        if (name == "v_add_nc_u16") {
            r = s_.m.bin(c, Bin::Add, a, b);
        } else if (name == "v_sub_nc_u16") {
            r = s_.m.bin(c, Bin::Sub, a, b);
        } else if (name == "v_subrev_nc_u16") {
            r = s_.m.bin(c, Bin::Sub, b, a);
        } else if (name == "v_mul_lo_u16") {
            r = s_.m.bin(c, Bin::Mul, a, b);
        } else if (name == "v_lshlrev_b16" || name == "v_lshrrev_b16" || name == "v_ashrrev_i16") {
            const V sh = s_.m.bin(c, Bin::And, a, k15);
            const V x = name == "v_ashrrev_i16" ? sext(b) : b;
            Bin op = Bin::AShr;
            if (name == "v_lshlrev_b16") op = Bin::Shl;
            else if (name == "v_lshrrev_b16") op = Bin::LShr;
            r = s_.m.bin(c, op, x, sh);
        } else {
            std::uint8_t p = pred::kSlt;
            if (name == "v_max_u16") p = pred::kIugt;
            else if (name == "v_min_u16") p = pred::kIult;
            else if (name == "v_max_i16") p = pred::kSgt;
            const V t = s_.m.cmp(c, p, a, b);
            r = s_.m.select(c, t, a, b);
        }
    }
    write_half(inst, r, guard_value);
}

V Lower::cvt_norm16(const IrInst& inst, bool is_signed) {
    const auto* fields = std::get_if<Valu>(&inst.fields);
    if (fields == nullptr) std::abort();
    const std::size_t c = cur_;
    const auto k = [&](std::uint32_t x) { return s_.m.i32(x); };
    V h = half_src(inst, 0);
    if ((fields->abs & 1) != 0) {
        const V m = k(0x7FFFu);
        h = s_.m.bin(c, Bin::And, h, m);
    }
    if ((fields->neg & 1) != 0) {
        const V m = k(0x8000u);
        h = s_.m.bin(c, Bin::Xor, h, m);
    }
    const std::uint32_t scale = is_signed ? 0x7FFFu : 0xFFFFu;
    const V zero = k(0);
    const V one = k(1);
    const V ten = k(10);
    const V mag_mask = k(0x7FFFu);
    const V mag = s_.m.bin(c, Bin::And, h, mag_mask);
    const V fifteen = k(15);
    const V sign = s_.m.bin(c, Bin::LShr, h, fifteen);
    const V negative = s_.m.cmp(c, pred::kNe, sign, zero);
    const V exponent = s_.m.bin(c, Bin::LShr, mag, ten);
    const V man_mask = k(0x3FFu);
    const V man = s_.m.bin(c, Bin::And, mag, man_mask);
    const V implicit = k(0x400u);
    const V normal = s_.m.bin(c, Bin::Or, man, implicit);
    const V subnormal = s_.m.cmp(c, pred::kEq, exponent, zero);
    const V m = s_.m.select(c, subnormal, man, normal);
    const V twenty_five = k(25);
    const V shift_normal = s_.m.bin(c, Bin::Sub, twenty_five, exponent);
    const V twenty_four = k(24);
    const V shift = s_.m.select(c, subnormal, twenty_four, shift_normal);
    const V ks = k(scale);
    const V product = s_.m.bin(c, Bin::Mul, m, ks);
    const V q = s_.m.bin(c, Bin::LShr, product, shift);
    const V one_shifted = s_.m.bin(c, Bin::Shl, one, shift);
    const V low_mask = s_.m.bin(c, Bin::Sub, one_shifted, one);
    const V rem = s_.m.bin(c, Bin::And, product, low_mask);
    const V half = s_.m.bin(c, Bin::LShr, one_shifted, one);
    const V above = s_.m.cmp(c, pred::kIugt, rem, half);
    const V tie = s_.m.cmp(c, pred::kEq, rem, half);
    const V odd = s_.m.bin(c, Bin::And, q, one);
    const V odd_bit = s_.m.cmp(c, pred::kNe, odd, zero);
    const V tie_up = s_.m.bin(c, Bin::And, tie, odd_bit);
    const V up = s_.m.bin(c, Bin::Or, above, tie_up);
    V r = s_.m.select(c, up, one, zero);
    r = s_.m.bin(c, Bin::Add, q, r);
    const V one_half = k(0x3C00u);
    const V big = s_.m.cmp(c, pred::kIuge, mag, one_half);
    r = s_.m.select(c, big, ks, r);
    const V infinity = k(0x7C00u);
    const V nan = s_.m.cmp(c, pred::kIugt, mag, infinity);
    r = s_.m.select(c, nan, zero, r);
    if (is_signed) {
        const V negated = s_.m.bin(c, Bin::Sub, zero, r);
        r = s_.m.select(c, negative, negated, r);
    } else {
        r = s_.m.select(c, negative, zero, r);
    }
    const V mask16 = k(0xFFFFu);
    return s_.m.bin(c, Bin::And, r, mask16);
}

V Lower::packed_src(const IrInst& inst, std::size_t k, bool high, bool float16) {
    const auto* fields = std::get_if<Valu>(&inst.fields);
    if (fields == nullptr) std::abort();
    if (k >= fields->src.size()) fail("a missing operand");
    const Operand& op = fields->src[k];
    const std::uint8_t select = high ? fields->opsel_hi : fields->opsel;
    const bool choose_high = (select >> k & 1) != 0;
    V x = float16 ? f16_operand(inst, op) : operand(inst, op);
    const std::size_t c = cur_;
    if (choose_high) {
        const V k16 = s_.m.i32(16);
        x = s_.m.bin(c, Bin::LShr, x, k16);
    }
    const V m = s_.m.i32(0xFFFF);
    return s_.m.bin(c, Bin::And, x, m);
}

void Lower::packed16(const IrInst& inst, std::string_view op, std::optional<V> guard_value) {
    const auto* fields = std::get_if<Valu>(&inst.fields);
    if (fields == nullptr) std::abort();
    const bool is_float = op.ends_with("_f16");
    std::size_t n = 0;
    if (op == "fma_f16" || op == "mad_u16" || op == "mad_i16") {
        n = 3;
    } else if (op == "add_f16" || op == "mul_f16" || op == "min_f16" || op == "max_f16" ||
               op == "add_u16" || op == "sub_u16" || op == "add_i16" || op == "sub_i16" ||
               op == "mul_lo_u16" || op == "lshlrev_b16" || op == "lshrrev_b16" ||
               op == "ashrrev_i16" || op == "max_u16" || op == "min_u16" || op == "max_i16" ||
               op == "min_i16") {
        n = 2;
    } else {
        fail("a packed 16-bit operation");
    }
    if (fields->omod != 0 || (!is_float && (fields->clamp || fields->neg != 0 || fields->neg_hi != 0)))
        fail("modifiers on a packed integer operation");
    const std::size_t c = cur_;
    const Ty i32t = s_.t.i32;
    const Ty f32t = s_.t.f32;
    std::vector<V> halves;
    for (bool high : {false, true}) {
        std::vector<V> xs;
        for (std::size_t k = 0; k < n; ++k) {
            const V h = packed_src(inst, k, high, is_float);
            if (is_float) {
                V f = s_.op(c, "legacyF16ToF32", Attr::ReadNone, f32t, 131, {h});
                const std::uint8_t neg = high ? fields->neg_hi : fields->neg;
                if ((neg >> k & 1) != 0) {
                    const V z = s_.m.f32(0x8000'0000u);
                    f = s_.m.bin(c, Bin::Sub, z, f);
                }
                xs.push_back(f);
            } else {
                xs.push_back(h);
            }
        }
        V r;
        if (is_float) {
            V f;
            if (op == "add_f16") f = s_.m.bin(c, Bin::Add, xs[0], xs[1]);
            else if (op == "mul_f16") f = s_.m.bin(c, Bin::Mul, xs[0], xs[1]);
            else if (op == "min_f16") f = max_min(false, xs[0], xs[1]);
            else if (op == "max_f16") f = max_min(true, xs[0], xs[1]);
            else f = fmad(xs[0], xs[1], xs[2]);
            if (fields->clamp) f = unary(7, f);
            r = s_.op(c, "legacyF32ToF16", Attr::ReadNone, i32t, 130, {f});
        } else {
            const V k16 = s_.m.i32(16);
            const auto sext = [&](V x) {
                const V t = s_.m.bin(c, Bin::Shl, x, k16);
                return s_.m.bin(c, Bin::AShr, t, k16);
            };
            if (op.ends_with("_i16")) {
                for (V& x : xs) x = sext(x);
            }
            const V k15 = s_.m.i32(15);
            if (op == "add_u16" || op == "add_i16") {
                r = s_.m.bin(c, Bin::Add, xs[0], xs[1]);
            } else if (op == "sub_u16" || op == "sub_i16") {
                r = s_.m.bin(c, Bin::Sub, xs[0], xs[1]);
            } else if (op == "mul_lo_u16") {
                r = s_.m.bin(c, Bin::Mul, xs[0], xs[1]);
            } else if (op == "mad_u16" || op == "mad_i16") {
                const V m = s_.m.bin(c, Bin::Mul, xs[0], xs[1]);
                r = s_.m.bin(c, Bin::Add, m, xs[2]);
            } else if (op == "lshlrev_b16" || op == "lshrrev_b16" || op == "ashrrev_i16") {
                const V sh = s_.m.bin(c, Bin::And, xs[0], k15);
                Bin o = Bin::AShr;
                if (op == "lshlrev_b16") o = Bin::Shl;
                else if (op == "lshrrev_b16") o = Bin::LShr;
                const V x = op == "ashrrev_i16" ? sext(xs[1]) : xs[1];
                r = s_.m.bin(c, o, x, sh);
            } else {
                std::uint8_t p = pred::kSlt;
                if (op == "max_u16") p = pred::kIugt;
                else if (op == "min_u16") p = pred::kIult;
                else if (op == "max_i16") p = pred::kSgt;
                const V t = s_.m.cmp(c, p, xs[0], xs[1]);
                r = s_.m.select(c, t, xs[0], xs[1]);
            }
        }
        const V m = s_.m.i32(0xFFFF);
        halves.push_back(s_.m.bin(c, Bin::And, r, m));
    }
    const V k16 = s_.m.i32(16);
    const V hi = s_.m.bin(c, Bin::Shl, halves[1], k16);
    const V r = s_.m.bin(c, Bin::Or, halves[0], hi);
    if (!fields->dst) fail("a packed result outside a VGPR");
    const auto* target = std::get_if<VectorOperand>(&*fields->dst);
    if (target == nullptr) fail("a packed result outside a VGPR");
    write(inst, Reg::v(target->reg), r, guard_value);
}

V Lower::integer(const IrInst& inst, std::string_view name) {
    const auto* fields = std::get_if<Valu>(&inst.fields);
    if (fields == nullptr) std::abort();
    const std::size_t c = cur_;
    const auto k = [&](std::uint32_t x) { return s_.m.i32(x); };
    const auto m31 = [&](V x) {
        const V k31 = s_.m.i32(31);
        return s_.m.bin(c, Bin::And, x, k31);
    };
    if (name == "v_and_b32" || name == "v_or_b32" || name == "v_xor_b32" ||
        name == "v_add_nc_u32" || name == "v_sub_nc_u32" || name == "v_subrev_nc_u32" ||
        name == "v_mul_lo_u32") {
        const V a = src(inst, 0);
        const V b = src(inst, 1);
        Bin op = Bin::Mul;
        V x = a;
        V y = b;
        if (name == "v_and_b32") op = Bin::And;
        else if (name == "v_or_b32") op = Bin::Or;
        else if (name == "v_xor_b32") op = Bin::Xor;
        else if (name == "v_add_nc_u32") op = Bin::Add;
        else if (name == "v_sub_nc_u32") op = Bin::Sub;
        else if (name == "v_subrev_nc_u32") {
            op = Bin::Sub;
            x = b;
            y = a;
        }
        return s_.m.bin(c, op, x, y);
    }
    if (name == "v_xnor_b32") {
        const V a = src(inst, 0);
        const V b = src(inst, 1);
        const V x = s_.m.bin(c, Bin::Xor, a, b);
        return s_.m.bin(c, Bin::Xor, x, k(UINT32_MAX));
    }
    if (name == "v_not_b32") {
        const V a = src(inst, 0);
        return s_.m.bin(c, Bin::Xor, a, k(UINT32_MAX));
    }
    if (name == "v_lshlrev_b32" || name == "v_lshrrev_b32" || name == "v_ashrrev_i32") {
        const V sh = src(inst, 0);
        const V a = src(inst, 1);
        const V amount = m31(sh);
        Bin op = Bin::AShr;
        if (name == "v_lshlrev_b32") op = Bin::Shl;
        else if (name == "v_lshrrev_b32") op = Bin::LShr;
        return s_.m.bin(c, op, a, amount);
    }
    if (name == "v_lshl_add_u32" || name == "v_lshl_or_b32" || name == "v_add_lshl_u32" ||
        name == "v_and_or_b32" || name == "v_or3_b32" || name == "v_xad_u32" ||
        name == "v_add3_u32" || name == "v_xor3_b32") {
        const V a = src(inst, 0);
        const V b = src(inst, 1);
        const V d = src(inst, 2);
        if (name == "v_lshl_add_u32" || name == "v_lshl_or_b32") {
            const V shifted = s_.m.bin(c, Bin::Shl, a, m31(b));
            return s_.m.bin(c, name == "v_lshl_add_u32" ? Bin::Add : Bin::Or, shifted, d);
        }
        if (name == "v_add_lshl_u32") {
            const V t = s_.m.bin(c, Bin::Add, a, b);
            return s_.m.bin(c, Bin::Shl, t, m31(d));
        }
        Bin o1 = Bin::Xor;
        Bin o2 = Bin::Xor;
        if (name == "v_and_or_b32") {
            o1 = Bin::And;
            o2 = Bin::Or;
        } else if (name == "v_or3_b32") {
            o1 = Bin::Or;
            o2 = Bin::Or;
        } else if (name == "v_xad_u32") {
            o1 = Bin::Xor;
            o2 = Bin::Add;
        } else if (name == "v_add3_u32") {
            o1 = Bin::Add;
            o2 = Bin::Add;
        }
        const V t = s_.m.bin(c, o1, a, b);
        return s_.m.bin(c, o2, t, d);
    }
    if (name == "v_mul_u32_u24" || name == "v_mul_i32_i24") {
        const V a = src(inst, 0);
        const V b = src(inst, 1);
        const V sh = k(8);
        const Bin op = name == "v_mul_i32_i24" ? Bin::AShr : Bin::LShr;
        V x = s_.m.bin(c, Bin::Shl, a, sh);
        x = s_.m.bin(c, op, x, sh);
        V y = s_.m.bin(c, Bin::Shl, b, sh);
        y = s_.m.bin(c, op, y, sh);
        return s_.m.bin(c, Bin::Mul, x, y);
    }
    if (name == "v_bfe_u32" || name == "v_bfe_i32") {
        const V a = src(inst, 0);
        const V off = src(inst, 1);
        const V w = m31(src(inst, 2));
        return bfe(a, off, w, name == "v_bfe_i32");
    }
    if (name == "v_bfi_b32") {
        const V m = src(inst, 0);
        const V a = src(inst, 1);
        const V b = src(inst, 2);
        const V ones = k(UINT32_MAX);
        const V nm = s_.m.bin(c, Bin::Xor, m, ones);
        const V x = s_.m.bin(c, Bin::And, m, a);
        const V y = s_.m.bin(c, Bin::And, nm, b);
        return s_.m.bin(c, Bin::Or, x, y);
    }
    if (name == "v_max_i32" || name == "v_min_i32" || name == "v_max_u32" ||
        name == "v_min_u32") {
        const V a = src(inst, 0);
        const V b = src(inst, 1);
        std::uint8_t p = pred::kIult;
        if (name == "v_max_i32") p = pred::kSgt;
        else if (name == "v_min_i32") p = pred::kSlt;
        else if (name == "v_max_u32") p = pred::kIugt;
        const V t = s_.m.cmp(c, p, a, b);
        return s_.m.select(c, t, a, b);
    }
    if (name == "v_mul_hi_u32" || name == "v_mul_hi_i32") {
        const V a = src(inst, 0);
        const V b = src(inst, 1);
        return mul_hi(a, b, name == "v_mul_hi_i32");
    }
    if (name == "v_alignbit_b32" || name == "v_alignbyte_b32") {
        const V a = src(inst, 0);
        const V b = src(inst, 1);
        const V sh = src(inst, 2);
        V shift;
        if (name == "v_alignbyte_b32") {
            const V three = k(3);
            const V t = s_.m.bin(c, Bin::And, sh, three);
            shift = s_.m.bin(c, Bin::Shl, t, k(3));
        } else {
            shift = m31(sh);
        }
        const V k32 = k(32);
        const V lo = s_.m.bin(c, Bin::LShr, b, shift);
        V left = s_.m.bin(c, Bin::Sub, k32, shift);
        left = m31(left);
        const V hi_raw = s_.m.bin(c, Bin::Shl, a, left);
        const V zero = k(0);
        const V none = s_.m.cmp(c, pred::kEq, shift, zero);
        const V hi = s_.m.select(c, none, zero, hi_raw);
        return s_.m.bin(c, Bin::Or, lo, hi);
    }
    if (name == "v_bfrev_b32") {
        const V a = src(inst, 0);
        return s_.op(c, "unary.i32", Attr::ReadNone, s_.t.i32, 30, {a});
    }
    if (name == "v_bcnt_u32_b32") {
        const V a = src(inst, 0);
        const V b = src(inst, 1);
        const V n = s_.op(c, "unaryBits.i32", Attr::ReadNone, s_.t.i32, 31, {a});
        return s_.m.bin(c, Bin::Add, n, b);
    }
    if (name == "v_mad_u32_u24" || name == "v_mad_i32_i24") {
        const V a = src(inst, 0);
        const V b = src(inst, 1);
        const V d = src(inst, 2);
        const V sh = k(8);
        const Bin op = name == "v_mad_i32_i24" ? Bin::AShr : Bin::LShr;
        V x = s_.m.bin(c, Bin::Shl, a, sh);
        x = s_.m.bin(c, op, x, sh);
        V y = s_.m.bin(c, Bin::Shl, b, sh);
        y = s_.m.bin(c, op, y, sh);
        const V m = s_.m.bin(c, Bin::Mul, x, y);
        return s_.m.bin(c, Bin::Add, m, d);
    }
    if (name == "v_min3_i32" || name == "v_max3_i32" || name == "v_min3_u32" ||
        name == "v_max3_u32" || name == "v_med3_i32" || name == "v_med3_u32") {
        const V a = src(inst, 0);
        const V b = src(inst, 1);
        const V d = src(inst, 2);
        const bool is_signed = name.ends_with("i32");
        const std::uint8_t lt = is_signed ? pred::kSlt : pred::kIult;
        const std::uint8_t gt = is_signed ? pred::kSgt : pred::kIugt;
        const auto pick = [&](std::uint8_t p, V x, V y) {
            const V t = s_.m.cmp(c, p, x, y);
            return s_.m.select(c, t, x, y);
        };
        const std::string_view kind = name.substr(2, 4);
        if (kind == "min3") {
            const V t = pick(lt, a, b);
            return pick(lt, t, d);
        }
        if (kind == "max3") {
            const V t = pick(gt, a, b);
            return pick(gt, t, d);
        }
        const V lo = pick(lt, a, b);
        const V hi = pick(gt, a, b);
        const V t = pick(lt, hi, d);
        return pick(gt, lo, t);
    }
    if (name == "v_perm_b32") {
        const Operand* selector = fields->src.size() > 2 ? &fields->src[2] : nullptr;
        if (selector == nullptr) fail(name);
        if (const auto* literal = std::get_if<LiteralOperand>(selector)) {
            const std::uint32_t sel = literal->value;
            const V a = src(inst, 0);
            const V b = src(inst, 1);
            V r = k(0);
            for (std::uint32_t i = 0; i < 4; ++i) {
                const std::uint32_t byte_index = (sel >> (8 * i)) & 0xFF;
                std::optional<V> byte;
                if (byte_index <= 7) {
                    const V source = byte_index < 4 ? b : a;
                    const std::uint32_t n = byte_index < 4 ? byte_index : byte_index - 4;
                    const V t = s_.m.bin(c, Bin::LShr, source, k(8 * n));
                    byte = s_.m.bin(c, Bin::And, t, k(0xFF));
                } else if (byte_index <= 11) {
                    const V source = byte_index < 10 ? b : a;
                    const std::uint32_t bit = byte_index % 2 == 0 ? 15 : 31;
                    const V t = s_.m.bin(c, Bin::Shl, source, k(31 - bit));
                    const V shifted = s_.m.bin(c, Bin::AShr, t, k(31));
                    byte = s_.m.bin(c, Bin::And, shifted, k(0xFF));
                } else if (byte_index == 12) {
                    continue;
                } else {
                    byte = k(0xFF);
                }
                const V up = s_.m.bin(c, Bin::Shl, *byte, k(8 * i));
                r = s_.m.bin(c, Bin::Or, r, up);
            }
            return r;
        }
        if (const auto* constant = std::get_if<ConstantOperand>(selector)) {
            const std::optional<std::uint32_t> value = inline_constant(constant->code);
            if (!value) fail(name);
            const std::uint32_t sel = *value;
            const V a = src(inst, 0);
            const V b = src(inst, 1);
            V r = k(0);
            for (std::uint32_t i = 0; i < 4; ++i) {
                const std::uint32_t byte_index = (sel >> (8 * i)) & 0xFF;
                std::optional<V> byte;
                if (byte_index <= 7) {
                    const V source = byte_index < 4 ? b : a;
                    const std::uint32_t n = byte_index < 4 ? byte_index : byte_index - 4;
                    const V t = s_.m.bin(c, Bin::LShr, source, k(8 * n));
                    byte = s_.m.bin(c, Bin::And, t, k(0xFF));
                } else if (byte_index <= 11) {
                    const V source = byte_index < 10 ? b : a;
                    const std::uint32_t bit = byte_index % 2 == 0 ? 15 : 31;
                    const V t = s_.m.bin(c, Bin::Shl, source, k(31 - bit));
                    const V shifted = s_.m.bin(c, Bin::AShr, t, k(31));
                    byte = s_.m.bin(c, Bin::And, shifted, k(0xFF));
                } else if (byte_index == 12) {
                    continue;
                } else {
                    byte = k(0xFF);
                }
                const V up = s_.m.bin(c, Bin::Shl, *byte, k(8 * i));
                r = s_.m.bin(c, Bin::Or, r, up);
            }
            return r;
        }
        const V a = src(inst, 0);
        const V b = src(inst, 1);
        const V sv = src(inst, 2);
        V r = k(0);
        for (std::uint32_t i = 0; i < 4; ++i) {
            const V sh = k(8 * i);
            const V ff = k(0xFF);
            V selector_byte = s_.m.bin(c, Bin::LShr, sv, sh);
            selector_byte = s_.m.bin(c, Bin::And, selector_byte, ff);
            const V four = k(4);
            const V zero_bits = k(0);
            const V high_bit = s_.m.bin(c, Bin::And, selector_byte, four);
            const V high = s_.m.cmp(c, pred::kNe, high_bit, zero_bits);
            const V word = s_.m.select(c, high, a, b);
            const V three = k(3);
            const V index_bits = s_.m.bin(c, Bin::And, selector_byte, three);
            const V bits = s_.m.bin(c, Bin::Shl, index_bits, three);
            const V x = s_.m.bin(c, Bin::LShr, word, bits);
            V byte = s_.m.bin(c, Bin::And, x, ff);
            const std::array<std::tuple<std::uint32_t, V, std::uint32_t>, 4> signs = {
                std::tuple<std::uint32_t, V, std::uint32_t>{8, b, 15},
                std::tuple<std::uint32_t, V, std::uint32_t>{9, b, 31},
                std::tuple<std::uint32_t, V, std::uint32_t>{10, a, 15},
                std::tuple<std::uint32_t, V, std::uint32_t>{11, a, 31}};
            for (const auto& entry : signs) {
                const V left = k(31 - std::get<2>(entry));
                const V t = s_.m.bin(c, Bin::Shl, std::get<1>(entry), left);
                const V shifted = s_.m.bin(c, Bin::AShr, t, k(31));
                const V extended = s_.m.bin(c, Bin::And, shifted, ff);
                const V is = s_.m.cmp(c, pred::kEq, selector_byte, k(std::get<0>(entry)));
                byte = s_.m.select(c, is, extended, byte);
            }
            const V twelve = k(12);
            const V zero = k(0);
            const V is12 = s_.m.cmp(c, pred::kEq, selector_byte, twelve);
            byte = s_.m.select(c, is12, zero, byte);
            const V above = s_.m.cmp(c, pred::kIugt, selector_byte, twelve);
            byte = s_.m.select(c, above, ff, byte);
            const V up = s_.m.bin(c, Bin::Shl, byte, k(8 * i));
            r = s_.m.bin(c, Bin::Or, r, up);
        }
        return r;
    }
    if (name == "v_bfm_b32") {
        const V a = src(inst, 0);
        const V b = src(inst, 1);
        const V one = k(1);
        const V am = m31(a);
        const V bm = m31(b);
        V m = s_.m.bin(c, Bin::Shl, one, am);
        m = s_.m.bin(c, Bin::Sub, m, one);
        return s_.m.bin(c, Bin::Shl, m, bm);
    }
    if (name == "v_cvt_pk_u8_f32") {
        const V f = fsrc(inst, 0);
        const V b = src(inst, 1);
        const V d = src(inst, 2);
        const V rounded = unary(26, f);
        const V zero = s_.m.f32(0);
        const V max = s_.m.f32(std::bit_cast<std::uint32_t>(255.0f));
        V clamped = binary(35, rounded, zero);
        clamped = binary(36, clamped, max);
        const V x = s_.m.cast(c, Cast::FpToUi, clamped, s_.t.i32);
        const V three = k(3);
        V sh = s_.m.bin(c, Bin::And, b, three);
        sh = s_.m.bin(c, Bin::Shl, sh, k(3));
        const V up = s_.m.bin(c, Bin::Shl, x, sh);
        const V ff = k(0xFF);
        const V hole_bits = s_.m.bin(c, Bin::Shl, ff, sh);
        V hole = s_.m.bin(c, Bin::Xor, hole_bits, k(UINT32_MAX));
        hole = s_.m.bin(c, Bin::And, d, hole);
        return s_.m.bin(c, Bin::Or, hole, up);
    }
    if (name == "v_ffbl_b32") {
        const V a = src(inst, 0);
        return s_.op(c, "unaryBits.i32", Attr::ReadNone, s_.t.i32, 32, {a});
    }
    if (name == "v_mbcnt_lo_u32_b32" || name == "v_mbcnt_hi_u32_b32") {
        const V a = src(inst, 0);
        const V b = src(inst, 1);
        if (mask_data_ == MaskData::Host) {
            bool host = false;
            if (!fields->src.empty()) {
                if (const auto* constant = std::get_if<ConstantOperand>(&fields->src.front())) {
                    host = constant->code == 193;
                } else if (const auto* scalar =
                               std::get_if<ScalarOperand>(&fields->src.front())) {
                    const std::optional<Value> read = inst.read(Reg::s(scalar->code));
                    if (read)
                        host = x_->st->masks.contains(*read) || x_->st->highs.contains(*read);
                }
            }
            if (!host) fail("v_mbcnt of a mask that is no ballot (console lane layout)");
            s_.flags |= feature::kWaveOps;
        } else if (!s_.mesh) {
            cross_lane(false);
        }
        const V lane = lane_index();
        V below;
        if (name == "v_mbcnt_lo_u32_b32") {
            below = lane;
        } else {
            const V k32 = k(32);
            const V z = k(0);
            const V t = s_.m.bin(c, Bin::Sub, lane, k32);
            const V low = s_.m.cmp(c, pred::kIult, lane, k32);
            below = s_.m.select(c, low, z, t);
        }
        const V one = k(1);
        const V k32 = k(32);
        const V ones = k(UINT32_MAX);
        const V sh = m31(below);
        V m = s_.m.bin(c, Bin::Shl, one, sh);
        m = s_.m.bin(c, Bin::Sub, m, one);
        const V full = s_.m.cmp(c, pred::kIuge, below, k32);
        m = s_.m.select(c, full, ones, m);
        const V x = s_.m.bin(c, Bin::And, a, m);
        const V n = s_.op(c, "unaryBits.i32", Attr::ReadNone, s_.t.i32, 31, {x});
        return s_.m.bin(c, Bin::Add, n, b);
    }
    fail(name);
}

void Lower::compare(const IrInst& inst, std::string_view rest) {
    const std::size_t split = rest.rfind('_');
    if (split == std::string_view::npos) fail("a compare");
    const std::string_view op = rest.substr(0, split);
    const std::string_view ty = rest.substr(split + 1);
    const auto* fields = std::get_if<Valu>(&inst.fields);
    if (fields == nullptr) std::abort();
    const std::size_t c = cur_;
    V r;
    if ((ty == "f32" || ty == "f16") && op == "class") {
        const bool f16 = ty == "f16";
        V x;
        if (f16) {
            V h = half_src(inst, 0);
            if ((fields->abs & 1) != 0) {
                const V m = s_.m.i32(0x7FFF);
                h = s_.m.bin(c, Bin::And, h, m);
            }
            if ((fields->neg & 1) != 0) {
                const V sign = s_.m.i32(0x8000);
                h = s_.m.bin(c, Bin::Xor, h, sign);
            }
            x = h;
        } else {
            x = as_int(fsrc(inst, 0));
        }
        const V mask = src(inst, 1);
        const auto k = [&](std::uint32_t v) { return s_.m.i32(v); };
        std::uint32_t exp_sh = 23;
        std::uint32_t sign_sh = 31;
        std::uint32_t exp_mask = 0xFF;
        std::uint32_t mant_mask = 0x7F'FFFF;
        std::uint32_t quiet_bit = 0x40'0000;
        if (f16) {
            exp_sh = 10;
            sign_sh = 15;
            exp_mask = 0x1F;
            mant_mask = 0x03FF;
            quiet_bit = 0x0200;
        }
        const V k_exp = k(exp_sh);
        const V k_sign = k(sign_sh);
        const V k_emax = k(exp_mask);
        const V mant = k(mant_mask);
        const V quiet = k(quiet_bit);
        V e = s_.m.bin(c, Bin::LShr, x, k_exp);
        e = s_.m.bin(c, Bin::And, e, k_emax);
        const V m = s_.m.bin(c, Bin::And, x, mant);
        const V neg = s_.m.bin(c, Bin::LShr, x, k_sign);
        const V zero = k(0);
        const V e0 = s_.m.cmp(c, pred::kEq, e, zero);
        const V emax = s_.m.cmp(c, pred::kEq, e, k_emax);
        const V m0 = s_.m.cmp(c, pred::kEq, m, zero);
        V q = s_.m.bin(c, Bin::And, x, quiet);
        q = s_.m.cmp(c, pred::kNe, q, zero);
        const V normal = k(8);
        const V inf = k(9);
        const V zero_c = k(6);
        const V denorm = k(7);
        V t = s_.m.select(c, m0, zero_c, denorm);
        t = s_.m.select(c, e0, t, normal);
        t = s_.m.select(c, emax, inf, t);
        const V eleven = k(11);
        const V mirrored = s_.m.bin(c, Bin::Sub, eleven, t);
        const V is_neg = s_.m.cmp(c, pred::kNe, neg, zero);
        t = s_.m.select(c, is_neg, mirrored, t);
        const V one = k(1);
        const V snan = k(0);
        const V nan_class = s_.m.select(c, q, one, snan);
        const V yes = s_.m.boolean(true);
        V nan = s_.m.bin(c, Bin::Xor, m0, yes);
        nan = s_.m.bin(c, Bin::And, nan, emax);
        t = s_.m.select(c, nan, nan_class, t);
        V bit = s_.m.bin(c, Bin::LShr, mask, t);
        bit = s_.m.bin(c, Bin::And, bit, one);
        r = s_.m.cmp(c, pred::kNe, bit, zero);
    } else if (ty == "f32" || ty == "f16") {
        V a;
        V b;
        if (ty == "f16") {
            a = half_fsrc(inst, 0);
            b = half_fsrc(inst, 1);
        } else {
            a = fsrc(inst, 0);
            b = fsrc(inst, 1);
        }
        std::uint8_t p = 0;
        bool known = true;
        if (op == "lg") {
            const V lt = s_.m.cmp(c, pred::kOlt, a, b);
            const V gt = s_.m.cmp(c, pred::kOgt, a, b);
            r = s_.m.bin(c, Bin::Or, lt, gt);
            lane_bits_for(inst, r);
            return;
        }
        if (op == "f") p = pred::kFalse;
        else if (op == "lt") p = pred::kOlt;
        else if (op == "eq") p = pred::kOeq;
        else if (op == "le") p = pred::kOle;
        else if (op == "gt") p = pred::kOgt;
        else if (op == "lg") p = pred::kOne;
        else if (op == "ge") p = pred::kOge;
        else if (op == "o") p = pred::kOrd;
        else if (op == "u") p = pred::kUno;
        else if (op == "nge") p = pred::kUlt;
        else if (op == "nlg") p = pred::kUeq;
        else if (op == "ngt") p = pred::kUle;
        else if (op == "nle") p = pred::kUgt;
        else if (op == "neq") p = pred::kUne;
        else if (op == "nlt") p = pred::kUge;
        else if (op == "tru") p = pred::kTrue;
        else known = false;
        if (!known) fail(inst.name());
        r = s_.m.cmp(c, p, a, b);
    } else if (ty == "u64" || ty == "i64") {
        const V a = src64(inst, 0);
        const V b = src64(inst, 1);
        const bool is_signed = ty == "i64";
        std::uint8_t p = 0;
        bool known = true;
        if (op == "eq") p = pred::kEq;
        else if (op == "ne" || op == "lg") p = pred::kNe;
        else if (op == "lt" && is_signed) p = pred::kSlt;
        else if (op == "le" && is_signed) p = pred::kSle;
        else if (op == "gt" && is_signed) p = pred::kSgt;
        else if (op == "ge" && is_signed) p = pred::kSge;
        else if (op == "lt") p = pred::kIult;
        else if (op == "le") p = pred::kIule;
        else if (op == "gt") p = pred::kIugt;
        else if (op == "ge") p = pred::kIuge;
        else known = false;
        if (!known) fail("a 64-bit compare");
        r = s_.m.cmp(c, p, a, b);
    } else if (ty == "u32" || ty == "i32" || ty == "u16" || ty == "i16") {
        V a;
        V b;
        if (ty.ends_with("16")) {
            a = half_src(inst, 0);
            b = half_src(inst, 1);
            if (ty == "i16") {
                const V k16 = s_.m.i32(16);
                for (V* x : {&a, &b}) {
                    const V t = s_.m.bin(c, Bin::Shl, *x, k16);
                    *x = s_.m.bin(c, Bin::AShr, t, k16);
                }
            }
        } else {
            a = src(inst, 0);
            b = src(inst, 1);
        }
        const bool is_signed = ty == "i32" || ty == "i16";
        std::uint8_t p = 0;
        bool known = true;
        if (op == "eq") p = pred::kEq;
        else if (op == "ne" || op == "lg") p = pred::kNe;
        else if (op == "lt" && is_signed) p = pred::kSlt;
        else if (op == "le" && is_signed) p = pred::kSle;
        else if (op == "gt" && is_signed) p = pred::kSgt;
        else if (op == "ge" && is_signed) p = pred::kSge;
        else if (op == "lt") p = pred::kIult;
        else if (op == "le") p = pred::kIule;
        else if (op == "gt") p = pred::kIugt;
        else if (op == "ge") p = pred::kIuge;
        else if (op == "f" || op == "t") {
            lane(inst, op == "t");
            return;
        } else {
            known = false;
        }
        if (!known) fail("an integer compare");
        r = s_.m.cmp(c, p, a, b);
    } else {
        fail(inst.name());
    }
    lane_bits_for(inst, r);
}

void Lower::lane(const IrInst& inst, std::optional<bool> value) {
    const V b = s_.m.boolean(value == true);
    lane_bits_for(inst, b);
}

void Lower::lane_bits_for(const IrInst& inst, V r) {
    const auto* fields = std::get_if<Valu>(&inst.fields);
    if (fields == nullptr) return;
    std::optional<std::uint8_t> code;
    if (inst.name().starts_with("v_cmpx")) {
        code = kExec;
    } else if (fields->dst) {
        if (const auto* scalar = std::get_if<ScalarOperand>(&*fields->dst)) code = scalar->code;
    }
    if (code) {
        if (const std::optional<Value> lo = inst.write(Reg::s(*code))) lane_bits_[*lo] = r;
    }
    for (const auto& write : inst.writes)
        vals_.emplace(write.second, Val::missing("a lane mask read as data"));
}

V Lower::low_half(const IrInst& inst, std::uint8_t reg, V x) {
    const std::size_t c = cur_;
    V old = s_.m.i32(0);
    if (const std::optional<Value> read = inst.read(Reg::v(reg))) {
        try {
            old = i32_of(*read);
        } catch (const WhyError&) {
            old = s_.m.i32(0);
        }
    }
    const V high = s_.m.i32(0xFFFF'0000u);
    const V low = s_.m.i32(0xFFFF);
    const V kept = s_.m.bin(c, Bin::And, old, high);
    const V masked = s_.m.bin(c, Bin::And, x, low);
    return s_.m.bin(c, Bin::Or, kept, masked);
}

}  // namespace g6shim::shader::emit
