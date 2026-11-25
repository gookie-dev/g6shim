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

constexpr std::uint64_t kCodeCbvSize = 0x1'0000;
constexpr std::pair<std::uint32_t, std::uint32_t> kRawBufferSrv{11, 0};
constexpr std::uint32_t kResourceDescriptorBytes = 32;
constexpr std::uint32_t kBufferRecordCountWord = 2;
constexpr std::uint32_t kImageWidthLowWord = 1;
constexpr std::uint32_t kImageDimensionsWord = 2;
constexpr std::uint32_t kImageBaseMipWord = 3;
constexpr std::uint32_t kSamplerDescriptorBytes = 16;
constexpr std::uint32_t kSamplerLodWord = 1;
constexpr std::uint32_t kImageWord5ReservedMask = 0xff00'0000;
constexpr Why kDescriptorData = "a descriptor's address or format read as data";
constexpr Why kStandaloneDescriptorData = "a descriptor read only as data";

std::optional<std::uint32_t> operand_constant(const Operand& value) {
    if (const auto* literal = std::get_if<LiteralOperand>(&value)) return literal->value;
    if (const auto* encoded = std::get_if<ConstantOperand>(&value)) return inline_constant(encoded->code);
    return std::nullopt;
}

std::uint32_t extract_mask(std::uint32_t control, bool is_signed) {
    constexpr std::uint32_t kWordBits = 32;
    const std::uint32_t offset = control & (kWordBits - 1);
    const std::uint32_t width = (control >> 16) & 0x7f;
    const std::uint32_t count = std::min(width, kWordBits - offset);
    const std::uint32_t bits = count == kWordBits ? UINT32_MAX : (1u << count) - 1;
    const std::uint32_t sign = is_signed && width > count ? 1u << (kWordBits - 1) : 0;
    return (bits << offset) | sign;
}

bool strip(std::string_view text, std::string_view prefix, std::string_view* rest) {
    if (!text.starts_with(prefix)) return false;
    *rest = text.substr(prefix.size());
    return true;
}

std::optional<std::uint32_t> dword_count(std::string_view rest) {
    if (rest.empty()) return 1;
    if (rest == "x2") return 2;
    if (rest == "x4") return 4;
    if (rest == "x8") return 8;
    if (rest == "x16") return 16;
    return std::nullopt;
}

}  // namespace

void Lower::sop(const IrInst& inst) {
    const auto* fields = std::get_if<SopFields>(&inst.fields);
    if (fields == nullptr) std::abort();
    const std::optional<Operand>& dst = fields->dst;
    const std::vector<Operand>& src = fields->src;
    const std::optional<std::uint16_t>& imm = fields->imm;
    const std::string_view name = inst.name();
    const std::optional<Value> scc = inst.write(Reg::scc());
    std::optional<std::uint8_t> dst_code;
    if (dst) {
        if (const auto* scalar = std::get_if<ScalarOperand>(&*dst)) dst_code = scalar->code;
    }
    std::optional<Value> lo;
    if (dst_code) lo = inst.write(Reg::s(*dst_code));
    if (inst.write(Reg::s(kExec)).has_value() || is_mask(lo)) fail(kLaneMask);
    if (lo && std::binary_search(x_->io->geometry_primitive_counts.begin(),
                                 x_->io->geometry_primitive_counts.end(), *lo)) {
        constexpr std::uint32_t kPrimitivesPerInvocation = 1;
        set(lo, Val::i32(s_.m.i32(kPrimitivesPerInvocation)));
        set(scc, Val::bit(s_.m.boolean(true)));
        return;
    }
    if (name == "s_setreg_imm32_b32" || name == "s_setreg_b32" || name == "s_swappc_b64" ||
        name == "s_version" || name.starts_with("s_waitcnt"))
        fail("a register the emitter does not model");
    const auto scalar_value = [&](const Operand& op) -> std::optional<Value> {
        const auto* reg = std::get_if<ScalarOperand>(&op);
        if (reg == nullptr) return std::nullopt;
        return inst.read(Reg::s(reg->code));
    };
    if (name == "s_mov_b32" && lo && src.size() == 1) {
        const auto value = scalar_value(src[0]);
        if (value) {
            const auto indexed = image_heap_words_.find(*value);
            if (indexed != image_heap_words_.end()) {
                image_heap_words_[*lo] = indexed->second;
                vals_[*lo] = Val::missing(kDescriptorData);
                return;
            }
            const auto zero_bits = descriptor_zero_bits_.find(*value);
            const auto field = descriptor_addresses_.find(*value);
            if (field != descriptor_addresses_.end() && field->second.accepts_mask()) {
                const UserData data = field->second;
                descriptor_addresses_[*lo] = data;
                if (data.known_bits() != UINT32_MAX) vals_[*lo] = Val::missing(kDescriptorData);
                if (zero_bits != descriptor_zero_bits_.end()) descriptor_zero_bits_[*lo] = zero_bits->second;
                return;
            }
            if (zero_bits != descriptor_zero_bits_.end()) {
                descriptor_zero_bits_[*lo] = zero_bits->second;
                vals_[*lo] = Val::missing(kDescriptorData);
                return;
            }
        }
    }
    const std::size_t c = cur_;
    const auto src32 = [&](std::size_t k, std::optional<std::uint32_t> mask = std::nullopt) -> V {
        if (k == 0 && (name == "s_addk_i32" || name == "s_mulk_i32")) {
            if (!dst) fail("a missing operand");
            return operand(inst, *dst);
        }
        if (k >= src.size()) fail("a missing operand");
        if (mask) {
            const auto value = scalar_value(src[k]);
            if (value && has_partial_descriptor(*value)) return masked_descriptor_value(*value, *mask);
        }
        return operand(inst, src[k]);
    };
    const auto sext16 = [&]() -> V {
        const std::uint16_t value = imm.value_or(0);
        return s_.m.i32(static_cast<std::uint32_t>(static_cast<std::int32_t>(
            static_cast<std::int16_t>(value))));
    };
    const auto half = [&](std::size_t k, std::uint8_t h) -> V {
        if (k < src.size()) {
            const Operand& op = src[k];
            if (const auto* scalar = std::get_if<ScalarOperand>(&op);
                scalar != nullptr && scalar->code != kNull) {
                const std::optional<Value> value =
                    inst.read(Reg::s(static_cast<std::uint8_t>(scalar->code + h)));
                if (!value) fail("an unread operand");
                return i32_of(*value);
            }
            if (h == 0) return operand(inst, op);
            if (const auto* constant = std::get_if<ConstantOperand>(&op)) {
                const std::optional<std::uint32_t> x = inline_constant(constant->code);
                if (!x) fail("a special operand");
                return s_.m.i32(static_cast<std::int32_t>(*x) < 0 ? UINT32_MAX : 0);
            }
        }
        return s_.m.i32(0);
    };
    if (name.starts_with("s_bitcmp")) {
        const std::string_view op = name.substr(8);
        const V bit = src32(1);
        const V k31 = s_.m.i32(31);
        const V idx = s_.m.bin(c, Bin::And, bit, k31);
        V word;
        if (op.ends_with("_b64")) {
            const V a = half(0, 0);
            const V b = half(0, 1);
            const V k32 = s_.m.i32(32);
            const V upper = s_.m.bin(c, Bin::And, bit, k32);
            const V z = s_.m.i32(0);
            const V is_upper = s_.m.cmp(c, pred::kNe, upper, z);
            word = s_.m.select(c, is_upper, b, a);
        } else {
            word = src32(0);
        }
        const V shifted = s_.m.bin(c, Bin::LShr, word, idx);
        const V one = s_.m.i32(1);
        const V tested = s_.m.bin(c, Bin::And, shifted, one);
        const V want = s_.m.i32(op.starts_with('1') ? 1u : 0u);
        const V r = s_.m.cmp(c, pred::kEq, tested, want);
        set(scc, Val::bit(r));
        return;
    }
    if (name.starts_with("s_cmp_") && name.ends_with("_u64")) {
        const std::string_view op = name.substr(6);
        std::optional<V> r;
        for (std::uint8_t h = 0; h < 2; ++h) {
            const V a = half(0, h);
            const V b = half(1, h);
            const V e = s_.m.cmp(c, pred::kEq, a, b);
            r = r ? s_.m.bin(c, Bin::And, *r, e) : e;
        }
        V result = *r;
        if (op.starts_with("lg")) {
            const V t = s_.m.boolean(true);
            result = s_.m.bin(c, Bin::Xor, result, t);
        }
        set(scc, Val::bit(result));
        return;
    }
    std::string_view cmp_op;
    bool scalar_cmp = false;
    bool cmpk = false;
    if (strip(name, "s_cmp_", &cmp_op)) {
        scalar_cmp = true;
    } else if (strip(name, "s_cmpk_", &cmp_op)) {
        scalar_cmp = true;
        cmpk = true;
    }
    if (scalar_cmp) {
        V a;
        V b;
        if (cmpk) {
            b = cmp_op.ends_with("_i32") ? sext16() : s_.m.i32(imm.value_or(0));
            a = src32(0);
        } else {
            if (cmp_op.ends_with("64")) fail("a 64-bit scalar compare");
            const bool zero_test = cmp_op == "eq_u32" || cmp_op == "lg_u32";
            const auto compared_mask = [&](std::size_t index) -> std::optional<std::uint32_t> {
                if (src.size() != 2) return std::nullopt;
                const auto constant = operand_constant(src[1 - index]);
                if (zero_test && constant == 0u) return UserData::kStructuredBufferHighMask;
                if (index == 0 && (cmp_op == "lt_u32" || cmp_op == "ge_u32") && constant &&
                    std::has_single_bit(*constant))
                    return ~(*constant - 1u);
                return std::nullopt;
            };
            a = src32(0, compared_mask(0));
            b = src32(1, compared_mask(1));
        }
        const bool is_signed = cmp_op.ends_with("_i32");
        const std::string_view kind = cmp_op.substr(0, std::min<std::size_t>(2, cmp_op.size()));
        std::uint8_t p = 0;
        bool known = true;
        if (kind == "eq") p = pred::kEq;
        else if (kind == "lg") p = pred::kNe;
        else if (kind == "gt" && is_signed) p = pred::kSgt;
        else if (kind == "ge" && is_signed) p = pred::kSge;
        else if (kind == "lt" && is_signed) p = pred::kSlt;
        else if (kind == "le" && is_signed) p = pred::kSle;
        else if (kind == "gt") p = pred::kIugt;
        else if (kind == "ge") p = pred::kIuge;
        else if (kind == "lt") p = pred::kIult;
        else if (kind == "le") p = pred::kIule;
        else known = false;
        if (!known) fail("a scalar compare");
        const V r = s_.m.cmp(c, p, a, b);
        set(scc, Val::bit(r));
        return;
    }
    const bool wide =
        name.ends_with("_b64") || name.ends_with("_u64") || name.ends_with("_i64");
    if (wide) {
        std::optional<Value> hi;
        if (dst_code)
            hi = inst.write(Reg::s(static_cast<std::uint8_t>(*dst_code + 1)));
        const auto nonzero = [&](V a, V b) {
            const V o = s_.m.bin(c, Bin::Or, a, b);
            const V z = s_.m.i32(0);
            return s_.m.cmp(c, pred::kNe, o, z);
        };
        if (name == "s_bcnt1_i32_b64") {
            const V a = half(0, 0);
            const V b = half(0, 1);
            const Ty i32t = s_.t.i32;
            const V na = s_.op(c, "unaryBits.i32", Attr::ReadNone, i32t, 31, {a});
            const V nb = s_.op(c, "unaryBits.i32", Attr::ReadNone, i32t, 31, {b});
            const V r = s_.m.bin(c, Bin::Add, na, nb);
            set(lo, Val::i32(r));
            const V z = s_.m.i32(0);
            const V s = s_.m.cmp(c, pred::kNe, r, z);
            set(scc, Val::bit(s));
            return;
        }
        if (name == "s_ff1_i32_b64") {
            const V a = half(0, 0);
            const V b = half(0, 1);
            const Ty i32t = s_.t.i32;
            const V fa = s_.op(c, "unaryBits.i32", Attr::ReadNone, i32t, 32, {a});
            const V fb = s_.op(c, "unaryBits.i32", Attr::ReadNone, i32t, 32, {b});
            const V z = s_.m.i32(0);
            const V k32 = s_.m.i32(32);
            const V fb32 = s_.m.bin(c, Bin::Add, fb, k32);
            const V none = s_.m.cmp(c, pred::kEq, b, z);
            const V high = s_.m.select(c, none, fb, fb32);
            const V low = s_.m.cmp(c, pred::kNe, a, z);
            const V r = s_.m.select(c, low, fa, high);
            set(lo, Val::i32(r));
            return;
        }
        if (name == "s_and_b64" || name == "s_or_b64" || name == "s_xor_b64" ||
            name == "s_andn2_b64" || name == "s_orn2_b64" || name == "s_nand_b64" ||
            name == "s_nor_b64" || name == "s_xnor_b64" || name == "s_not_b64") {
            const V ones = s_.m.i32(UINT32_MAX);
            std::vector<V> r;
            for (std::uint8_t h = 0; h < 2; ++h) {
                const V a = half(0, h);
                V x;
                if (name == "s_not_b64") {
                    x = s_.m.bin(c, Bin::Xor, a, ones);
                } else {
                    V b = half(1, h);
                    if (name.find("n2") != std::string_view::npos)
                        b = s_.m.bin(c, Bin::Xor, b, ones);
                    const std::string_view op = name.substr(2, 3);
                    Bin bin = Bin::Xor;
                    if (op == "and" || op == "nan") bin = Bin::And;
                    else if (op == "or_" || op == "orn" || op == "nor") bin = Bin::Or;
                    const V combined = s_.m.bin(c, bin, a, b);
                    if (name == "s_nand_b64" || name == "s_nor_b64" || name == "s_xnor_b64")
                        x = s_.m.bin(c, Bin::Xor, combined, ones);
                    else
                        x = combined;
                }
                r.push_back(x);
            }
            set(lo, Val::i32(r[0]));
            set(hi, Val::i32(r[1]));
            const V s = nonzero(r[0], r[1]);
            set(scc, Val::bit(s));
            return;
        }
        if (name == "s_lshl_b64" || name == "s_lshr_b64" || name == "s_ashr_i64") {
            const V a = half(0, 0);
            const V b = half(0, 1);
            const V n = src32(1);
            const Ty i64t = s_.m.int_type(64);
            s_.flags |= feature::kInt64;
            const V a64 = s_.m.cast(c, Cast::ZExt, a, i64t);
            const V b64 = s_.m.cast(c, Cast::ZExt, b, i64t);
            const V k32 = s_.m.int_const(i64t, 32);
            const V b_up = s_.m.bin(c, Bin::Shl, b64, k32);
            const V x = s_.m.bin(c, Bin::Or, a64, b_up);
            const V k63 = s_.m.i32(63);
            const V shift = s_.m.bin(c, Bin::And, n, k63);
            const V shift64 = s_.m.cast(c, Cast::ZExt, shift, i64t);
            Bin op = Bin::AShr;
            if (name == "s_lshl_b64") op = Bin::Shl;
            else if (name == "s_lshr_b64") op = Bin::LShr;
            const V r = s_.m.bin(c, op, x, shift64);
            const V rl = s_.m.cast(c, Cast::Trunc, r, s_.t.i32);
            const V rh_full = s_.m.bin(c, Bin::LShr, r, k32);
            const V rh = s_.m.cast(c, Cast::Trunc, rh_full, s_.t.i32);
            set(lo, Val::i32(rl));
            set(hi, Val::i32(rh));
            const V s = nonzero(rl, rh);
            set(scc, Val::bit(s));
            return;
        }
        if (name == "s_mov_b64") {
            const V a = half(0, 0);
            const V b = half(0, 1);
            set(lo, Val::i32(a));
            set(hi, Val::i32(b));
            return;
        }
        if (name == "s_cselect_b64") {
            const V cond = scc_in(inst);
            for (std::uint8_t h = 0; h < 2; ++h) {
                const V a = half(0, h);
                const V b = half(1, h);
                const V r = s_.m.select(c, cond, a, b);
                set(h == 0 ? lo : hi, Val::i32(r));
            }
            return;
        }
        fail(name);
    }
    V r;
    std::optional<V> carry;
    if (name == "s_mov_b32") {
        r = src32(0);
    } else if (name == "s_movk_i32") {
        r = sext16();
    } else if (name == "s_cselect_b32") {
        const V cond = scc_in(inst);
        const V a = src32(0);
        const V b = src32(1);
        r = s_.m.select(c, cond, a, b);
    } else if (name == "s_brev_b32") {
        const V a = src32(0);
        r = s_.op(c, "unary.i32", Attr::ReadNone, s_.t.i32, 30, {a});
    } else if (name == "s_not_b32") {
        const V a = src32(0);
        const V ones = s_.m.i32(UINT32_MAX);
        r = s_.m.bin(c, Bin::Xor, a, ones);
    } else if (name == "s_add_u32" || name == "s_add_i32" || name == "s_addk_i32") {
        const V a = src32(0);
        const V b = name == "s_addk_i32" ? sext16() : src32(1);
        r = s_.m.bin(c, Bin::Add, a, b);
        if (name == "s_add_u32") carry = s_.m.cmp(c, pred::kIult, r, a);
    } else if (name == "s_sub_u32" || name == "s_sub_i32") {
        const V a = src32(0);
        const V b = src32(1);
        r = s_.m.bin(c, Bin::Sub, a, b);
        if (name == "s_sub_u32") carry = s_.m.cmp(c, pred::kIult, a, b);
    } else if (name == "s_addc_u32" || name == "s_subb_u32") {
        const V a = src32(0);
        const V b = src32(1);
        const V cin = scc_in(inst);
        const auto result = add_sub_carry(c, name == "s_addc_u32", a, b, cin);
        r = result.first;
        carry = result.second;
    } else if (name == "s_mul_i32" || name == "s_mulk_i32") {
        const V a = src32(0);
        const V b = name == "s_mulk_i32" ? sext16() : src32(1);
        r = s_.m.bin(c, Bin::Mul, a, b);
    } else if (name == "s_mul_hi_u32" || name == "s_mul_hi_i32") {
        const V a = src32(0);
        const V b = src32(1);
        r = mul_hi(a, b, name.ends_with("i32"));
    } else if (name == "s_flbit_i32_b32" || name == "s_flbit_i32") {
        const V a = src32(0);
        const std::uint32_t op = name == "s_flbit_i32_b32" ? 33 : 34;
        r = s_.op(c, "unaryBits.i32", Attr::ReadNone, s_.t.i32, op, {a});
    } else if (name == "s_ff1_i32_b32" || name == "s_bcnt1_i32_b32") {
        const V a = src32(0);
        const std::uint32_t op = name == "s_ff1_i32_b32" ? 32 : 31;
        r = s_.op(c, "unaryBits.i32", Attr::ReadNone, s_.t.i32, op, {a});
    } else if (name == "s_bfe_u32" || name == "s_bfe_i32") {
        const bool is_signed = name == "s_bfe_i32";
        const auto control = src.size() == 2 ? operand_constant(src[1]) : std::nullopt;
        const auto mask = control ? std::optional{extract_mask(*control, is_signed)} : std::nullopt;
        const V a = src32(0, mask);
        const V b = src32(1);
        const V k16 = s_.m.i32(16);
        const V k127 = s_.m.i32(0x7F);
        V w = s_.m.bin(c, Bin::LShr, b, k16);
        w = s_.m.bin(c, Bin::And, w, k127);
        r = bfe(a, b, w, is_signed);
    } else if (name == "s_and_b32" || name == "s_or_b32" || name == "s_xor_b32" ||
               name == "s_andn2_b32" || name == "s_orn2_b32" || name == "s_nand_b32" ||
               name == "s_nor_b32" || name == "s_xnor_b32") {
        const bool and_mask = name == "s_and_b32" && src.size() == 2;
        const V a = src32(0, and_mask ? operand_constant(src[1]) : std::nullopt);
        V b = src32(1, and_mask ? operand_constant(src[0]) : std::nullopt);
        const V ones = s_.m.i32(UINT32_MAX);
        if (name.find("n2") != std::string_view::npos) b = s_.m.bin(c, Bin::Xor, b, ones);
        const std::string_view op = name.substr(2, 3);
        Bin bin = Bin::Xor;
        if (op == "and" || op == "nan") bin = Bin::And;
        else if (op == "or_" || op == "orn" || op == "nor") bin = Bin::Or;
        r = s_.m.bin(c, bin, a, b);
        if (name == "s_nand_b32" || name == "s_nor_b32" || name == "s_xnor_b32")
            r = s_.m.bin(c, Bin::Xor, r, ones);
    } else if (name == "s_lshl_b32" || name == "s_lshr_b32" || name == "s_ashr_i32") {
        const auto shift = src.size() > 1 ? operand_constant(src[1]) : std::nullopt;
        const bool left = name == "s_lshl_b32";
        std::optional<std::uint32_t> mask;
        if (shift) {
            const std::uint32_t bits = *shift & 31u;
            mask = left ? UINT32_MAX >> bits : UINT32_MAX << bits;
        }
        const V a = src32(0, mask);
        V b = src32(1);
        const V k31 = s_.m.i32(31);
        b = s_.m.bin(c, Bin::And, b, k31);
        Bin op = Bin::AShr;
        if (left) op = Bin::Shl;
        else if (name == "s_lshr_b32") op = Bin::LShr;
        r = s_.m.bin(c, op, a, b);
    } else if (name == "s_lshl1_add_u32" || name == "s_lshl2_add_u32" ||
               name == "s_lshl3_add_u32" || name == "s_lshl4_add_u32") {
        const V a = src32(0);
        const V b = src32(1);
        const V k = s_.m.i32(static_cast<std::uint32_t>(name[6] - '0'));
        const V t = s_.m.bin(c, Bin::Shl, a, k);
        r = s_.m.bin(c, Bin::Add, t, b);
    } else if (name == "s_min_u32" || name == "s_max_u32" || name == "s_min_i32" ||
               name == "s_max_i32") {
        const V a = src32(0);
        const V b = src32(1);
        std::uint8_t p = pred::kSgt;
        if (name == "s_min_u32") p = pred::kIult;
        else if (name == "s_max_u32") p = pred::kIugt;
        else if (name == "s_min_i32") p = pred::kSlt;
        const V t = s_.m.cmp(c, p, a, b);
        r = s_.m.select(c, t, a, b);
    } else if (name == "s_abs_i32") {
        const V a = src32(0);
        const V z = s_.m.i32(0);
        const V neg = s_.m.cmp(c, pred::kSlt, a, z);
        const V n = s_.m.bin(c, Bin::Sub, z, a);
        r = s_.m.select(c, neg, n, a);
    } else if (name == "s_bitset0_b32" || name == "s_bitset1_b32") {
        if (!dst) fail("a missing operand");
        const V old = operand(inst, *dst);
        const V a = src32(0);
        const V k31 = s_.m.i32(31);
        const V one = s_.m.i32(1);
        const V bit = s_.m.bin(c, Bin::And, a, k31);
        const V shifted = s_.m.bin(c, Bin::Shl, one, bit);
        if (name == "s_bitset1_b32") {
            r = s_.m.bin(c, Bin::Or, old, shifted);
        } else {
            const V ones = s_.m.i32(UINT32_MAX);
            const V cleared = s_.m.bin(c, Bin::Xor, shifted, ones);
            r = s_.m.bin(c, Bin::And, old, cleared);
        }
    } else if (name == "s_bfm_b32") {
        const V a = src32(0);
        const V b = src32(1);
        const V k31 = s_.m.i32(31);
        const V one = s_.m.i32(1);
        const V a_masked = s_.m.bin(c, Bin::And, a, k31);
        const V b_masked = s_.m.bin(c, Bin::And, b, k31);
        V m = s_.m.bin(c, Bin::Shl, one, a_masked);
        m = s_.m.bin(c, Bin::Sub, m, one);
        r = s_.m.bin(c, Bin::Shl, m, b_masked);
    } else {
        fail(name);
        return;
    }
    set(lo, Val::i32(r));
    if (scc) {
        Val val;
        if (carry) {
            val = Val::bit(*carry);
        } else if (name == "s_mov_b32" || name == "s_movk_i32" || name == "s_cselect_b32" ||
                   name == "s_brev_b32" || name == "s_mul_i32" || name == "s_mulk_i32") {
            return;
        } else if (name.starts_with("s_add") || name.starts_with("s_sub")) {
            val = Val::missing("the carry of a scalar operation");
        } else {
            const V z = s_.m.i32(0);
            val = Val::bit(s_.m.cmp(c, pred::kNe, r, z));
        }
        vals_[*scc] = val;
    }
}

std::pair<V, V> Lower::add_sub_carry(std::size_t c, bool add, V a, V b, std::optional<V> cin) {
    const Bin op = add ? Bin::Add : Bin::Sub;
    V r = s_.m.bin(c, op, a, b);
    V carry = add ? s_.m.cmp(c, pred::kIult, r, a) : s_.m.cmp(c, pred::kIult, a, b);
    if (cin) {
        const V one = s_.m.cast(c, Cast::ZExt, *cin, s_.t.i32);
        const V t = r;
        r = s_.m.bin(c, op, t, one);
        const V edge = s_.m.i32(add ? UINT32_MAX : 0u);
        const V at_edge = s_.m.cmp(c, pred::kEq, t, edge);
        const V more = s_.m.bin(c, Bin::And, *cin, at_edge);
        carry = s_.m.bin(c, Bin::Or, carry, more);
    }
    return {r, carry};
}

V Lower::scc_in(const IrInst& inst) {
    const std::optional<Value> value = inst.read(Reg::scc());
    if (!value) fail("an unread SCC");
    const Val val = get(*value);
    if (val.kind == Val::Kind::Bit) return val.value;
    fail("SCC without a value");
}

void Lower::smem(const IrInst& inst, Site site) {
    const auto& spilled = x_->res->spilled_root_loads;
    if (std::binary_search(spilled.begin(), spilled.end(), site)) {
        for (const auto& write : inst.writes) {
            const auto root = params_.find(write.second);
            if (root == params_.end()) {
                vals_[write.second] = Val::missing("an undeclared spilled root word");
            } else if (root->second.second != 0 &&
                       x_->native->layout[root->second.first].root.kind == RootType::Kind::Table) {
                vals_[write.second] = Val::missing("a spilled table address high word read as data");
            }
        }
        return;
    }
    const auto* fields = std::get_if<SmemFields>(&inst.fields);
    if (fields == nullptr) std::abort();
    const std::string_view name = inst.name();
    const auto tracked = scalar_.find(site);
    if (tracked != scalar_.end() && tracked->second.param == kCode) {
        code_smem(inst, tracked->second);
        return;
    }
    std::string_view rest;
    if (!strip(name, "s_buffer_load_dword", &rest)) {
        descriptor_data(inst, site);
        return;
    }
    const std::optional<std::uint32_t> found_count = dword_count(rest);
    if (!found_count) fail("a scalar buffer load");
    const std::uint32_t count = *found_count;
    if (!fields->data) fail("a scalar buffer load");
    const auto* data = std::get_if<ScalarOperand>(&*fields->data);
    if (data == nullptr) fail("a scalar buffer load");
    const std::uint8_t code = data->code;
    const auto descriptor = descriptors_.find({site, false});
    if (descriptor == descriptors_.end()) fail("an untracked descriptor");
    const Source& src = *descriptor->second;
    const UsedResource* kind = used_kind(src);
    const std::size_t c = cur_;
    const auto [base, extra] = built_offset(src);
    const std::uint32_t total = static_cast<std::uint32_t>(fields->offset) + base;
    V addr = s_.m.i32(total);
    if (extra) addr = s_.m.bin(c, Bin::Add, addr, *extra);
    auto is_null = [](const Operand& op) {
        const auto* scalar = std::get_if<ScalarOperand>(&op);
        return scalar != nullptr && scalar->code == kNull;
    };
    if (!is_null(fields->soffset)) {
        const V so = operand(inst, fields->soffset);
        addr = s_.m.bin(c, Bin::Add, addr, so);
    }
    if (src.kind == Source::Kind::Built && src.address.param == kCode) {
        for (std::uint32_t k = 0; k < count; ++k) {
            const V off = s_.m.i32(4 * k);
            const V at = s_.m.bin(c, Bin::Add, addr, off);
            const V x = code_dword(at);
            write(inst, Reg::s(static_cast<std::uint8_t>(code + k)), x, std::nullopt);
        }
        return;
    }
    if (kind != nullptr && kind->kind.kind == ResourceType::Kind::Srv) {
        const V h = handle(site, src, false, kRawBufferSrv);
        for (std::uint32_t k = 0; k < count; ++k) {
            const V off = s_.m.i32(4 * k);
            const V at = s_.m.bin(c, Bin::Add, addr, off);
            const V mask = s_.m.i8(1);
            const V undef = s_.m.undef(s_.t.i32);
            const V align = s_.m.i32(4);
            const V r = s_.op(c, "rawBufferLoad.f32", Attr::ReadOnly, s_.t.res_ret, 139,
                              {h, at, undef, mask, align});
            const V x = as_int(s_.m.extract(c, r, 0));
            write(inst, Reg::s(static_cast<std::uint8_t>(code + k)), x, std::nullopt);
        }
        return;
    }
    const V h = handle(site, src, false, {13, static_cast<std::uint32_t>(kCodeCbvSize)});
    std::optional<std::uint32_t> constant;
    if (addr.kind == V::Kind::Const && is_null(fields->soffset) && !extra) constant = total;
    std::map<std::uint32_t, V> rows;
    for (std::uint32_t k = 0; k < count; ++k) {
        V x;
        if (constant) {
            const std::uint32_t byte = *constant + 4 * k;
            const std::uint32_t row_index = byte >> 4;
            V row;
            const auto found = rows.find(row_index);
            if (found != rows.end()) {
                row = found->second;
            } else {
                const V ri = s_.m.i32(row_index);
                row = s_.op(c, "cbufferLoadLegacy.i32", Attr::ReadOnly, s_.t.cbuf_ret, 59, {h, ri});
                rows[row_index] = row;
            }
            x = s_.m.extract(c, row, (byte >> 2) & 3);
        } else {
            const V off = s_.m.i32(4 * k);
            const V at = s_.m.bin(c, Bin::Add, addr, off);
            x = cbuffer_dword(h, at);
        }
        write(inst, Reg::s(static_cast<std::uint8_t>(code + k)), x, std::nullopt);
    }
}

bool Lower::defer_descriptor_address(Value value, const Source& src, const UsedResource& used,
                                    UserData::Source source) {
    const auto tracked = scalar_.find(src.load);
    if (tracked == scalar_.end()) return false;
    const Address& address = tracked->second;
    if (address.dynamic || address.param >= x_->native->layout.size()) return false;
    const RootParam& root = x_->native->layout[address.param];
    const std::uint64_t byte = address.offset + sizeof(std::uint32_t) * std::uint64_t{src.first};
    if (root.root.kind != RootType::Kind::Table || byte % kResourceDescriptorBytes != 0 ||
        byte / sizeof(std::uint32_t) != used.table_offset_dwords)
        return false;
    descriptor_addresses_[value] = {0, root.slot, root.root, used.table_offset_dwords, source};
    return true;
}

void Lower::descriptor_data(const IrInst& inst, Site site) {
    const auto* fields = std::get_if<SmemFields>(&inst.fields);
    if (fields == nullptr || !fields->data) fail("a scalar memory read as data");
    const auto* data = std::get_if<ScalarOperand>(&*fields->data);
    if (data == nullptr) fail("a scalar memory read as data");
    const std::uint8_t code = data->code;
    std::string_view rest;
    if (!strip(inst.name(), "s_load_dword", &rest)) fail("a scalar memory read as data");
    const std::optional<std::uint32_t> found_count = dword_count(rest);
    if (!found_count) fail("a scalar memory read as data");
    const std::uint32_t count = *found_count;
    std::optional<std::pair<Site, std::uint8_t>> use_at;
    for (const Descriptor& descriptor : x_->res->descriptors) {
        if (descriptor.role != Role::Resource) continue;
        for (const Source* leaf : descriptor.source.leaves()) {
            if (leaf->kind == Source::Kind::Loaded && leaf->load == site) {
                use_at = std::pair<Site, std::uint8_t>{descriptor.site, leaf->first};
                break;
            }
        }
        if (use_at) break;
    }
    if (!use_at) {
        standalone_descriptor_data(inst, site, code, count);
        return;
    }
    const Site at = use_at->first;
    const std::uint8_t first = use_at->second;
    const Source src = Source::loaded(site, first);
    const UsedResource* used = used_kind(src);
    const IrInst& ui = x_->p->blocks[at.block].insts[at.index];
    const std::string_view un = ui.name();
    const bool uav = un.find("store") != std::string_view::npos ||
                     un.find("atomic") != std::string_view::npos ||
                     (used != nullptr && used->kind.kind == ResourceType::Kind::Uav);
    const std::uint32_t u = uav ? 0x1000 : 0;
    std::pair<std::uint32_t, std::uint32_t> props;
    std::optional<std::uint32_t> image;
    const bool scalar_buffer = std::holds_alternative<SmemFields>(ui.fields) && un.starts_with("s_buffer_load_dword");
    const bool scalar_srv = scalar_buffer && used != nullptr && used->kind.kind == ResourceType::Kind::Srv;
    if (scalar_buffer && used != nullptr && used->kind.kind == ResourceType::Kind::Cbv) {
        using SourceKind = UserData::Source;
        constexpr std::array kWords{SourceKind::ConstantBufferAddressLow, SourceKind::ConstantBufferHighWord,
                                    SourceKind::ConstantBufferRecordCount, SourceKind::ConstantBufferFormatWord};
        for (std::uint32_t k = 0; k < count; ++k) {
            const auto v = inst.write(Reg::s(static_cast<std::uint8_t>(code + k)));
            if (!v) continue;
            const std::uint32_t word = k - first;
            if (word < kWords.size() && defer_descriptor_address(*v, src, *used, kWords[word]))
                continue;
            vals_[*v] = Val::missing("a scalar memory read as data");
        }
        return;
    }
    const auto* image_fields = std::get_if<ImageFields>(&ui.fields);
    if (image_fields != nullptr) {
        const std::uint8_t dim = image_fields->dim;
        std::uint32_t kind = 0;
        if (dim == 0) kind = 1;
        else if (dim == 1) kind = 2;
        else if (dim == 2) kind = 4;
        else if (dim == 4) kind = 6;
        else if (dim == 5) kind = 7;
        else fail("an image dimension (cube or MSAA)");
        props = {kind | u, 0x409};
        image = kind;
    } else if (const auto* buffer_fields = std::get_if<BufferFields>(&ui.fields)) {
        const std::uint32_t stride = used != nullptr ? used->stride : 0;
        if (un.starts_with("buffer_load_format") || un.starts_with("buffer_store_format")) {
            props = {10 | u, 0x409};
        } else if (buffer_fields->idxen && !buffer_fields->format && stride >= 4 &&
                   stride % 4 == 0) {
            props = {12 | u, stride};
        } else {
            props = {11 | u, 0};
        }
        if (used != nullptr && used->kind.kind == ResourceType::Kind::Cbv)
            fail("a constant buffer's descriptor read as data");
    } else if (scalar_srv) {
        props = kRawBufferSrv;
    } else {
        fail("a scalar memory read as data");
    }
    using SourceKind = UserData::Source;
    const auto* buffer_fields = std::get_if<BufferFields>(&ui.fields);
    const bool byte_addressed = buffer_fields != nullptr && !buffer_fields->idxen && buffer_fields->offen &&
                                !buffer_fields->format;
    const bool linear_raw = used != nullptr && used->kind.kind == ResourceType::Kind::Srv && used->stride <= 1 &&
                            props == kRawBufferSrv && (scalar_srv || byte_addressed);
    const bool structured = used != nullptr && used->kind.kind == ResourceType::Kind::Srv &&
                            (scalar_srv || props.first == 12) && used->stride >= 4 && used->stride % 4 == 0 &&
                            used->stride <= UserData::kBufferStrideMask;
    const bool typed = used != nullptr && used->kind.kind == ResourceType::Kind::Srv && props.first == 10;
    const auto tracked = scalar_.find(src.load);
    const RootType::Kind root = tracked != scalar_.end() && tracked->second.param < x_->native->layout.size()
                                   ? x_->native->layout[tracked->second.param].root.kind
                                   : RootType::Kind::Other;
    const bool image_has_no_metadata_plane = image_fields != nullptr && !image_fields->r128 && !uav &&
                                             (root == RootType::Kind::Table || root == RootType::Kind::ResourceHeap);
    const V h = handle(site, src, false, props);
    const std::size_t c = cur_;
    const Ty i32t = s_.t.i32;
    const Ty dims = s_.m.named_struct("dx.types.Dimensions", {i32t, i32t, i32t, i32t});
    const V mip = image && !uav ? s_.m.i32(0) : s_.m.undef(i32t);
    const V d = s_.op(c, "getDimensions", Attr::ReadOnly, dims, 72, {h, mip});
    const auto field = [&](std::uint32_t k) { return s_.m.extract(c, d, k); };
    std::vector<std::pair<std::uint32_t, V>> words;
    if (image) {
        const std::uint32_t kind = *image;
        const V one = s_.m.i32(1);
        const V w = field(0);
        V height;
        V depth;
        if (kind == 1) {
            height = one;
            depth = one;
        } else if (kind == 2) {
            height = field(1);
            depth = one;
        } else if (kind == 6) {
            height = one;
            depth = field(1);
        } else {
            height = field(1);
            depth = field(2);
        }
        const V levels = uav ? one : field(3);
        const auto less = [&](V x) { return s_.m.bin(c, Bin::Sub, x, one); };
        const V w1 = less(w);
        const V h1 = less(height);
        const V d1 = less(depth);
        const V l1 = less(levels);
        const auto k = [&](std::uint32_t v) { return s_.m.i32(v); };
        const V k30 = k(30);
        const V k2 = k(2);
        const V k14 = k(14);
        const V k16 = k(16);
        const V mask = k(0x3FFF);
        const V word1 = s_.m.bin(c, Bin::Shl, w1, k30);
        V hi = s_.m.bin(c, Bin::LShr, w1, k2);
        hi = s_.m.bin(c, Bin::And, hi, mask);
        const V ht = s_.m.bin(c, Bin::Shl, h1, k14);
        const V word2 = s_.m.bin(c, Bin::Or, hi, ht);
        const std::uint32_t types[8] = {0, 8, 9, 0, 10, 0, 12, 13};
        const V base = k(0xFAC | (types[kind] << 28));
        const V last = s_.m.bin(c, Bin::Shl, l1, k16);
        const V word3 = s_.m.bin(c, Bin::Or, base, last);
        words = {{1, word1}, {2, word2}, {3, word3}, {4, d1}};
    } else {
        words = {{2, field(0)}};
    }
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::uint32_t rel = k - static_cast<std::uint32_t>(first);
        if (rel == 7 && image_has_no_metadata_plane) {
            write(inst, Reg::s(static_cast<std::uint8_t>(code + k)), s_.m.i32(0), std::nullopt);
            continue;
        }
        const auto found = std::find_if(words.begin(), words.end(),
                                        [rel](const auto& word) { return word.first == rel; });
        if (found != words.end()) {
            write(inst, Reg::s(static_cast<std::uint8_t>(code + k)), found->second, std::nullopt);
        } else {
            const auto v = inst.write(Reg::s(static_cast<std::uint8_t>(code + k)));
            if (!v) continue;
            if (rel == 0 && (scalar_srv || linear_raw || structured || typed) &&
                defer_descriptor_address(*v, src, *used, SourceKind::ShaderBufferAddressLow))
                continue;
            if (rel == 0 && image && used != nullptr &&
                defer_descriptor_address(*v, src, *used, UserData::Source::ImageBaseAddressLow))
                continue;
            if (rel == 5 && image_fields != nullptr && !image_fields->r128) {
                descriptor_zero_bits_[*v] = kImageWord5ReservedMask;
                if (!uav && image_fields->dim == 1 && used != nullptr && used->kind.kind == ResourceType::Kind::Srv)
                    defer_descriptor_address(*v, src, *used, SourceKind::ImageSamplingFields);
            }
            if (linear_raw || structured) {
                if (rel == 1 && defer_descriptor_address(*v, src, *used, structured ? SourceKind::StructuredBufferHighWord
                                                                                  : SourceKind::LinearRawBufferHighWord))
                    continue;
                if (rel == 3 && defer_descriptor_address(*v, src, *used, structured ? SourceKind::StructuredBufferFormatWord
                                                                                  : SourceKind::LinearRawBufferFormatWord))
                    continue;
            }
            if (scalar_srv && rel == 1)
                defer_descriptor_address(*v, src, *used, SourceKind::ShaderBufferAddressHigh);
            if (scalar_srv && rel == 3)
                defer_descriptor_address(*v, src, *used, SourceKind::ShaderBufferComponentMapping);
            if (typed && (rel == 1 || rel == 3) && defer_descriptor_address(
                    *v, src, *used, rel == 1 ? SourceKind::TypedBufferHighWord : SourceKind::TypedBufferFormatWord))
                continue;
            vals_[*v] = Val::missing(kDescriptorData);
        }
    }
}

void Lower::standalone_descriptor_data(const IrInst& inst, Site site, std::uint8_t code, std::uint32_t count) {
    constexpr std::uint32_t kDwordBytes = sizeof(std::uint32_t);
    constexpr std::uint32_t kSlotDwords = kResourceDescriptorBytes / kDwordBytes;
    constexpr std::uint32_t kSamplerSlotDwords = kSamplerDescriptorBytes / kDwordBytes;
    const auto tracked = scalar_.find(site);
    if (tracked == scalar_.end() ||
        tracked->second.param >= x_->native->layout.size() || tracked->second.offset % kDwordBytes != 0)
        fail(kStandaloneDescriptorData);
    const Address& address = tracked->second;
    const RootParam& root = x_->native->layout[address.param];
    if (root.root.kind == RootType::Kind::ResourceHeap && count == kSlotDwords &&
        address.offset % kResourceDescriptorBytes == 0) {
        defer_image_heap_read(inst, site, code, root);
        return;
    }
    if (address.dynamic) fail(kStandaloneDescriptorData);
    const bool sampler = root.root.kind == RootType::Kind::SamplerHeap;
    const std::uint32_t dword = address.offset / kDwordBytes;
    const std::uint32_t first = dword % kSlotDwords;
    const std::uint32_t slot_dword = dword - first;
    const UsedResource* used = sampler ? nullptr : used_kind(Source::loaded(site, 0));
    if (!sampler && (used == nullptr || used->kind.kind != ResourceType::Kind::Srv ||
                     used->table_offset_dwords != slot_dword || root.root.kind != RootType::Kind::Table))
        fail(kStandaloneDescriptorData);
    const std::uint8_t resource_words = used == nullptr ? 0 : used->dwords;
    using SourceKind = UserData::Source;
    for (std::uint32_t k = 0; k < count; ++k) {
        const auto value = inst.write(Reg::s(static_cast<std::uint8_t>(code + k)));
        if (!value) continue;
        const std::uint32_t word = first + k;
        std::optional<SourceKind> source;
        std::uint32_t descriptor_dword = slot_dword;
        if (sampler && (dword + k) % kSamplerSlotDwords == kSamplerLodWord) {
            source = SourceKind::SamplerLodWord;
            descriptor_dword = dword + k - kSamplerLodWord;
        } else if (resource_words == 4 && word == kBufferRecordCountWord)
            source = SourceKind::ShaderBufferRecordCount;
        else if (resource_words == 8 && word == kImageWidthLowWord)
            source = SourceKind::ImageWidthLowBits;
        else if (resource_words == 8 && word == kImageDimensionsWord)
            source = SourceKind::ImageDimensions;
        else if (resource_words == 8 && word == kImageBaseMipWord)
            source = SourceKind::ImageBaseMip;
        if (source) {
            const UserData data{0, root.slot, root.root, descriptor_dword, *source};
            descriptor_addresses_[*value] = data;
            if (data.known_bits() != UINT32_MAX) vals_[*value] = Val::missing(kDescriptorData);
        } else {
            vals_[*value] = Val::missing(kStandaloneDescriptorData);
        }
    }
}

void Lower::defer_image_heap_read(const IrInst& inst, Site site, std::uint8_t code, const RootParam& root) {
    if (!image_heap_allowed_) {
        const HwStage stage = x_->native->stage;
        image_heap_allowed_ = stage == HwStage::Ps || stage == HwStage::Vs || stage == HwStage::Cs;
        if (stage == HwStage::Cs) {
            for (const IrBlock& block : x_->p->blocks) {
                for (const IrInst& candidate : block.insts)
                    if (candidate.name() == "s_barrier") image_heap_allowed_ = false;
            }
        }
    }
    if (!*image_heap_allowed_) fail(kStandaloneDescriptorData);
    const V byte_address = heap_byte_address(site, 0);
    const std::size_t read_index = image_heap_reads_.size();
    const std::size_t read = s_.m.new_block();
    const std::size_t next = s_.m.new_block();
    const UserData binding{0, root.slot, root.root, 0, UserData::Source::ImageHeapData};
    image_heap_reads_.push_back({read, next, byte_address, binding, std::nullopt});
    s_.m.terminate(cur_, Term::br(read));
    cur_ = next;
    using Data = ImageHeapData;
    constexpr std::array masks{UINT32_MAX << UserData::kImageWidthLowShift, UserData::kImageDimensionsMask,
                               UserData::kImageBaseMipMask};
    for (std::uint32_t k = 0; k < kResourceDescriptorBytes / sizeof(std::uint32_t); ++k) {
        const auto value = inst.write(Reg::s(static_cast<std::uint8_t>(code + k)));
        if (!value) continue;
        if (k >= kImageWidthLowWord && k <= kImageBaseMipWord) {
            const std::uint32_t channel = k - kImageWidthLowWord;
            image_heap_words_[*value] = {read_index, channel, masks[channel]};
        }
        vals_[*value] = Val::missing(kStandaloneDescriptorData);
    }
    static_assert(Data::kWidthLow == 0 && Data::kDimensions == 1 && Data::kBaseMip == 2);
}

V Lower::read_image_heap_word(const ImageHeapWord& word) {
    ImageHeapRead& read = image_heap_reads_[word.read];
    const std::size_t c = read.block;
    if (!read.shape) {
        using Data = ImageHeapData;
        const auto [data, valid_slot] = read_heap_record(c, read.byte_address, read.binding);
        const V present = s_.m.cmp(c, pred::kNe, s_.m.extract(c, data, Data::kPresent), s_.m.i32(0));
        const V valid = s_.m.bin(c, Bin::And, valid_slot, present);
        const std::size_t abort = abort_block();
        read.shape = data;
        s_.m.terminate(c, Term::cond_br(valid, read.next, abort));
    }
    return s_.m.extract(c, *read.shape, word.channel);
}

std::pair<V, V> Lower::read_heap_record(std::size_t c, V byte_address, const UserData& binding) {
    const V base = descriptor_address(binding).value;
    const V h = s_.heap_handle(c, base, false, false, kRawBufferSrv);
    constexpr std::uint32_t kDescriptorShift = 5;
    constexpr std::uint32_t kRecordShift = 4;
    static_assert(kResourceDescriptorBytes == (1u << kDescriptorShift));
    static_assert(ImageHeapData::kBytes == (1u << kRecordShift) && ImageHeapData::kBytes == BufferHeapData::kBytes);
    const V slot = s_.m.bin(c, Bin::LShr, byte_address, s_.m.i32(kDescriptorShift));
    const Ty i32 = s_.t.i32;
    const Ty dimensions = s_.m.named_struct("dx.types.Dimensions", {i32, i32, i32, i32});
    const V dims = s_.op(c, "getDimensions", Attr::ReadOnly, dimensions, 72, {h, s_.m.undef(i32)});
    const V count = s_.m.bin(c, Bin::LShr, s_.m.extract(c, dims, 0), s_.m.i32(kRecordShift));
    const V in_range = s_.m.cmp(c, pred::kIult, slot, count);
    const V low_bits = s_.m.bin(c, Bin::And, byte_address, s_.m.i32(kResourceDescriptorBytes - 1));
    const V aligned = s_.m.cmp(c, pred::kEq, low_bits, s_.m.i32(0));
    const V byte = s_.m.bin(c, Bin::Shl, slot, s_.m.i32(kRecordShift));
    const V safe_byte = s_.m.select(c, in_range, byte, s_.m.i32(0));
    const Ty result = s_.m.named_struct("dx.types.ResRet.i32", {i32, i32, i32, i32, i32});
    const V data = s_.op(c, "rawBufferLoad.i32", Attr::ReadOnly, result, 139,
                         {h, safe_byte, s_.m.undef(i32), s_.m.i8(0xf), s_.m.i32(sizeof(std::uint32_t))});
    return {data, s_.m.bin(c, Bin::And, in_range, aligned)};
}

void Lower::code_smem(const IrInst& inst, Address a) {
    const auto* fields = std::get_if<SmemFields>(&inst.fields);
    if (fields == nullptr || !fields->data) fail("a scalar load");
    const auto* data = std::get_if<ScalarOperand>(&*fields->data);
    if (data == nullptr) fail("a scalar load");
    const std::uint8_t code = data->code;
    std::string_view rest;
    if (!strip(inst.name(), "s_load_dword", &rest)) fail("a scalar load from the code");
    const std::optional<std::uint32_t> found_count = dword_count(rest);
    if (!found_count) fail("a scalar load from the code");
    const std::uint32_t count = *found_count;
    const std::size_t c = cur_;
    V addr = s_.m.i32(a.offset);
    if (a.dynamic) addr = s_.m.bin(c, Bin::Add, addr, i32_of(*a.dynamic));
    for (std::uint32_t k = 0; k < count; ++k) {
        const V off = s_.m.i32(4 * k);
        const V at = s_.m.bin(c, Bin::Add, addr, off);
        const V x = code_dword(at);
        write(inst, Reg::s(static_cast<std::uint8_t>(code + k)), x, std::nullopt);
    }
}

V Lower::cbuffer_dword(V h, V a) {
    const std::size_t c = cur_;
    const V four = s_.m.i32(4);
    const V ri = s_.m.bin(c, Bin::LShr, a, four);
    const V r = s_.op(c, "cbufferLoadLegacy.i32", Attr::ReadOnly, s_.t.cbuf_ret, 59, {h, ri});
    const V two = s_.m.i32(2);
    const V three = s_.m.i32(3);
    V comp = s_.m.bin(c, Bin::LShr, a, two);
    comp = s_.m.bin(c, Bin::And, comp, three);
    V x = s_.m.extract(c, r, 0);
    for (std::uint32_t j = 1; j < 4; ++j) {
        const V e = s_.m.extract(c, r, j);
        const V jc = s_.m.i32(j);
        const V is = s_.m.cmp(c, pred::kEq, comp, jc);
        x = s_.m.select(c, is, e, x);
    }
    return x;
}

std::pair<std::uint32_t, std::optional<V>> Lower::built_offset(const Source& src) {
    if (src.kind == Source::Kind::Built) {
        std::optional<V> x;
        if (src.address.dynamic) x = i32_of(*src.address.dynamic);
        return {src.address.offset, x};
    }
    return {0, std::nullopt};
}

const UsedResource* Lower::used_kind(const Source& src) const {
    if (src.kind == Source::Kind::Phi || src.kind == Source::Kind::Select ||
        src.kind == Source::Kind::Lanes) {
        if (src.args.empty()) return nullptr;
        return used_kind(src.args.front());
    }
    if (src.kind == Source::Kind::LanePhi) {
        for (const std::optional<Source>& arg : src.options)
            if (arg)
                if (const UsedResource* found = used_kind(*arg)) return found;
        return nullptr;
    }
    if (src.kind == Source::Kind::Built) {
        if (src.address.param >= x_->native->layout.size()) return nullptr;
        const RootParam& rp = x_->native->layout[src.address.param];
        for (const UsedResource& used : x_->native->used) {
            if (used.root.kind == RootType::Kind::Descriptor && used.user_data == rp.user_data)
                return &used;
        }
        return nullptr;
    }
    if (src.kind != Source::Kind::Loaded && src.kind != Source::Kind::Fetched) return nullptr;
    const auto scalar = scalar_.find(src.load);
    if (scalar == scalar_.end()) return nullptr;
    if (scalar->second.param >= x_->native->layout.size()) return nullptr;
    const RootParam& rp = x_->native->layout[scalar->second.param];
    if (rp.root.kind != RootType::Kind::Table) return nullptr;
    const std::uint32_t dword = (scalar->second.offset / 4) + src.first;
    const UsedResource* best = nullptr;
    for (const UsedResource& used : x_->native->used) {
        if (used.root.kind != RootType::Kind::Table || used.user_data != rp.user_data) continue;
        if (used.table_offset_dwords > dword) continue;
        if (best == nullptr || used.table_offset_dwords > best->table_offset_dwords) best = &used;
    }
    return best;
}

V Lower::handle(Site site, const Source& src, bool sampler,
                std::pair<std::uint32_t, std::uint32_t> props) {
    (void)site;
    if (src.kind == Source::Kind::Loaded) {
        const auto key =
            std::tuple<std::size_t, Site, std::uint8_t, bool, std::uint32_t>{cur_, src.load,
                                                                             src.first, sampler,
                                                                             props.first};
        const auto found = handles_.find(key);
        if (found != handles_.end()) return found->second;
        const V index = heap_index(src.load, src.first, sampler);
        const std::vector<Site>& loads = x_->res->waterfall.loads;
        const bool per_lane = std::find(loads.begin(), loads.end(), src.load) != loads.end();
        const V h = s_.heap_handle(cur_, index, sampler, per_lane, props);
        handles_[key] = h;
        return h;
    }
    if (src.kind == Source::Kind::Constant) {
        if (!sampler) fail("a constant resource descriptor");
        if (src.constants.size() < 4) fail("a sampler constant");
        const std::array<std::uint32_t, 4> words = {src.constants[0], src.constants[1],
                                                    src.constants[2], src.constants[3]};
        std::size_t n = samplers_.size();
        for (std::size_t k = 0; k < samplers_.size(); ++k) {
            if (samplers_[k] == words) {
                n = k;
                break;
            }
        }
        if (n == samplers_.size()) {
            samplers_.push_back(words);
            n = samplers_.size() - 1;
        }
        if ((props.first & 0x8000) != 0) s_.cmp_samplers |= std::uint64_t{1} << n;
        return s_.sampler_handle(cur_, static_cast<std::uint32_t>(n));
    }
    if (src.kind == Source::Kind::Built) {
        if (src.address.param >= x_->native->layout.size())
            fail("a descriptor built around a table in the code");
        const RootParam rp = x_->native->layout[src.address.param];
        std::optional<ResourceType> kind;
        for (const UsedResource& used : x_->native->used) {
            if (used.user_data == rp.user_data) {
                kind = used.kind;
                break;
            }
        }
        if (rp.root.kind == RootType::Kind::Descriptor &&
            (kind == std::nullopt || kind->kind == ResourceType::Kind::Cbv) && props.first == 13)
            return s_.root_cbv_handle(rp.slot);
        if (rp.root.kind == RootType::Kind::Descriptor && kind != std::nullopt &&
            kind->kind == ResourceType::Kind::Srv && props.first == 11)
            return s_.root_srv_handle(rp.slot);
        fail("a root UAV or typed root view");
    }
    if (src.kind == Source::Kind::Phi) {
        const std::size_t block = src.block;
        std::vector<std::pair<V, std::size_t>> incoming;
        const std::vector<std::size_t>& preds = x_->p->blocks[block].preds;
        const std::size_t count = std::min(preds.size(), src.args.size());
        for (std::size_t n = 0; n < count; ++n) {
            const std::size_t q = preds[n];
            const Source& arg = src.args[n];
            if (!block_[q]) continue;
            if (!tail_[q] || arg.kind != Source::Kind::Loaded)
                fail("a descriptor chosen by control flow");
            const std::size_t at =
                edges_.contains({q, block}) ? edges_.at({q, block}) : *tail_[q];
            const std::size_t here = cur_;
            cur_ = at;
            V index;
            try {
                index = heap_index(arg.load, arg.first, sampler);
            } catch (...) {
                cur_ = here;
                throw;
            }
            cur_ = here;
            incoming.emplace_back(index, at);
        }
        if (!block_[block]) fail("a descriptor chosen by control flow");
        const V phi = s_.m.phi(*block_[block], s_.t.i32);
        s_.m.set_phi(phi, incoming);
        return s_.heap_handle(cur_, phi, sampler, false, props);
    }
    if (src.kind == Source::Kind::Select) {
        if (src.args.size() != 2 || src.args[0].kind != Source::Kind::Loaded ||
            src.args[1].kind != Source::Kind::Loaded)
            fail("a descriptor chosen by a condition");
        const Val cond = get(src.key);
        if (cond.kind != Val::Kind::Bit) fail("SCC without a value");
        const V set = heap_index(src.args[0].load, src.args[0].first, sampler);
        const V clear = heap_index(src.args[1].load, src.args[1].first, sampler);
        const V index = s_.m.select(cur_, cond.value, set, clear);
        return s_.heap_handle(cur_, index, sampler, false, props);
    }
    const V index = lane_heap_index(src, sampler);
    return s_.heap_handle(cur_, index, sampler, true, props);
}

V Lower::lane_heap_index(const Source& src, bool sampler) {
    if (src.kind == Source::Kind::Loaded) return heap_index(src.load, src.first, sampler);
    if (src.kind == Source::Kind::Fetched) return fetched_index(src.load, src.first, sampler);
    if (src.kind == Source::Kind::Lanes || src.kind == Source::Kind::Select) {
        if (src.args.size() != 2) fail("a descriptor chosen per lane");
        V cond;
        if (src.kind == Source::Kind::Lanes) {
            const auto found = x_->st->masks.find(src.key);
            if (found == x_->st->masks.end()) fail("a descriptor chosen per lane by an untracked mask");
            cond = boolean(found->second, cur_);
        } else {
            const Val value = get(src.key);
            if (value.kind != Val::Kind::Bit) fail("SCC without a value");
            cond = value.value;
        }
        const V set = lane_heap_index(src.args[0], sampler);
        const V clear = lane_heap_index(src.args[1], sampler);
        return s_.m.select(cur_, cond, set, clear);
    }
    if (src.kind == Source::Kind::LanePhi) {
        if (!block_[src.block]) fail("a per-lane descriptor chosen by control flow");
        std::vector<std::pair<V, std::size_t>> incoming;
        const std::vector<std::size_t>& preds = x_->p->blocks[src.block].preds;
        const std::size_t count = std::min(preds.size(), src.options.size());
        for (std::size_t n = 0; n < count; ++n) {
            const std::size_t q = preds[n];
            if (!block_[q]) continue;
            if (!tail_[q]) fail("a per-lane descriptor joined from a later block");
            const std::size_t at =
                edges_.contains({q, src.block}) ? edges_.at({q, src.block}) : *tail_[q];
            V index;
            if (src.options[n]) {
                const std::size_t here = cur_;
                cur_ = at;
                try {
                    index = lane_heap_index(*src.options[n], sampler);
                } catch (...) {
                    cur_ = here;
                    throw;
                }
                cur_ = here;
            } else {
                index = s_.m.i32(0);
            }
            incoming.emplace_back(index, at);
        }
        const V phi = s_.m.phi(*block_[src.block], s_.t.i32);
        s_.m.set_phi(phi, incoming);
        return phi;
    }
    fail("a per-lane descriptor from neither a heap load nor a choice of them");
}

V Lower::fetched_index(Site load, std::uint8_t first, bool sampler) {
    const auto tracked = scalar_.find(load);
    if (tracked == scalar_.end()) fail("an untracked descriptor load");
    const Address a = tracked->second;
    if (a.param >= x_->native->layout.size()) fail("a descriptor outside the heaps");
    const RootType::Kind root = x_->native->layout[a.param].root.kind;
    if (root != RootType::Kind::Table && root != RootType::Kind::ResourceHeap &&
        root != RootType::Kind::SamplerHeap)
        fail("a descriptor outside the heaps");
    const IrInst& li = x_->p->blocks[load.block].insts[load.index];
    const auto* fields = std::get_if<FlatFields>(&li.fields);
    if (fields == nullptr || !fields->addr) fail("a per-lane descriptor load");
    const auto* vec = std::get_if<VectorOperand>(&*fields->addr);
    if (vec == nullptr) fail("a per-lane descriptor load");
    const std::size_t c = cur_;
    const std::optional<Value> low = li.read(Reg::v(vec->reg));
    if (!low) fail("a per-lane descriptor load");
    V at = i32_of(*low);
    if (fields->saddr) {
        const auto* scalar = std::get_if<ScalarOperand>(&*fields->saddr);
        if (scalar != nullptr && scalar->code != kNull) {
            const std::optional<Value> base = li.read(Reg::s(scalar->code));
            if (!base) fail("a per-lane descriptor load");
            at = s_.m.bin(c, Bin::Add, i32_of(*base), at);
        }
    }
    const V k = s_.m.i32(static_cast<std::uint32_t>(fields->offset) +
                         4u * static_cast<std::uint32_t>(first));
    at = s_.m.bin(c, Bin::Add, at, k);
    const V shift = s_.m.i32(sampler ? 4 : 5);
    return s_.m.bin(c, Bin::LShr, at, shift);
}

V Lower::heap_byte_address(Site load, std::uint8_t first) {
    const auto tracked = scalar_.find(load);
    if (tracked == scalar_.end()) fail("an untracked descriptor load");
    const Address a = tracked->second;
    if (a.param >= x_->native->layout.size()) fail("a descriptor outside the heaps");
    const RootType::Kind root = x_->native->layout[a.param].root.kind;
    if (root != RootType::Kind::Table && root != RootType::Kind::ResourceHeap &&
        root != RootType::Kind::SamplerHeap)
        fail("a descriptor outside the heaps");
    const IrInst& li = x_->p->blocks[load.block].insts[load.index];
    const auto* fields = std::get_if<SmemFields>(&li.fields);
    if (fields == nullptr) fail("a descriptor load");
    const auto* base = std::get_if<ScalarOperand>(&fields->base);
    if (base == nullptr) fail("a descriptor load");
    const std::size_t c = cur_;
    const std::optional<Value> read = li.read(Reg::s(base->code));
    if (!read) fail("a descriptor load");
    V addr = i32_of(*read);
    const V k = s_.m.i32(static_cast<std::uint32_t>(fields->offset) +
                         4u * static_cast<std::uint32_t>(first));
    addr = s_.m.bin(c, Bin::Add, addr, k);
    const auto* soffset = std::get_if<ScalarOperand>(&fields->soffset);
    if (!(soffset != nullptr && soffset->code == kNull)) {
        const V so = operand(li, fields->soffset);
        addr = s_.m.bin(c, Bin::Add, addr, so);
    }
    return addr;
}

V Lower::heap_index(Site load, std::uint8_t first, bool sampler) {
    const V address = heap_byte_address(load, first);
    return s_.m.bin(cur_, Bin::LShr, address, s_.m.i32(sampler ? 4 : 5));
}

}  // namespace g6shim::shader::emit
