// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/resource.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "g6shim/shader/error.hpp"
#include "g6shim/shader/gfx10_fields.hpp"
#include "g6shim/shader/structure.hpp"

namespace g6shim::shader {

namespace {

constexpr std::size_t kMaxRounds = 64;
constexpr std::size_t kMaxJoins = 8;
constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

struct TraceError {
    const char* what;
};

using Known = std::vector<std::pair<Bool, bool>>;

struct Term {
    enum class Kind : std::uint8_t { Const, Value };

    Kind kind = Kind::Value;
    std::uint32_t constant = 0;
    Value value;

    static Term constant_value(std::uint32_t constant) { return {Kind::Const, constant, Value{}}; }
    static Term origin(Value value) { return {Kind::Value, 0, value}; }

    friend bool operator==(const Term&, const Term&) = default;
};

struct Sum {
    std::optional<Value> base;
    std::uint32_t offset = 0;
    std::optional<Value> dynamic;
};

struct Moved {
    enum class Kind : std::uint8_t { Const, Value };

    Kind kind = Kind::Const;
    std::uint32_t constant = 0;
    Value value;

    static Moved constant_value(std::uint32_t constant) { return {Kind::Const, constant, Value{}}; }
    static Moved origin(Value value) { return {Kind::Value, 0, value}; }

    friend bool operator==(const Moved&, const Moved&) = default;
};

std::optional<std::uint32_t> lane_of(const Operand& op) {
    if (const auto* c = std::get_if<ConstantOperand>(&op)) return inline_constant(c->code);
    if (const auto* l = std::get_if<LiteralOperand>(&op)) return l->value;
    return std::nullopt;
}

std::uint32_t reverse_bits(std::uint32_t value) {
    value = ((value & 0x55555555u) << 1) | ((value >> 1) & 0x55555555u);
    value = ((value & 0x33333333u) << 2) | ((value >> 2) & 0x33333333u);
    value = ((value & 0x0F0F0F0Fu) << 4) | ((value >> 4) & 0x0F0F0F0Fu);
    value = ((value & 0x00FF00FFu) << 8) | ((value >> 8) & 0x00FF00FFu);
    return (value << 16) | (value >> 16);
}

std::optional<Moved> copy_source(const Inst& inst, std::size_t k) {
    const auto* fields = std::get_if<SopFields>(&inst.fields);
    if (fields == nullptr) return std::nullopt;
    const std::string_view name = inst.name();
    if (name == "s_movk_i32") {
        if (!fields->imm) return std::nullopt;
        return Moved::constant_value(static_cast<std::uint32_t>(
            static_cast<std::int32_t>(static_cast<std::int16_t>(*fields->imm))));
    }
    if (fields->src.empty()) return std::nullopt;
    const Operand& src = fields->src.front();
    if (name == "s_mov_b32" || name == "s_mov_b64") {
        if (const auto* scalar = std::get_if<ScalarOperand>(&src)) {
            if (scalar->code == kNull) return Moved::constant_value(0);
            const auto value = inst.read(Reg::s(static_cast<std::uint8_t>(scalar->code + k)));
            if (!value) return std::nullopt;
            return Moved::origin(*value);
        }
        if (name == "s_mov_b32") {
            if (const auto* literal = std::get_if<LiteralOperand>(&src))
                return Moved::constant_value(literal->value);
        }
        if (const auto* c = std::get_if<ConstantOperand>(&src)) {
            const auto low = inline_constant(c->code);
            if (!low) return std::nullopt;
            if (name == "s_mov_b32") return Moved::constant_value(*low);
            if (c->code >= 128 && c->code <= 208)
                return Moved::constant_value(k == 0 ? *low
                                                    : static_cast<std::uint32_t>(static_cast<std::int32_t>(*low) >> 31));
        }
        return std::nullopt;
    }
    const auto* c = std::get_if<ConstantOperand>(&src);
    if (c == nullptr) return std::nullopt;
    const auto value = inline_constant(c->code);
    if (!value) return std::nullopt;
    if (name == "s_brev_b32") return Moved::constant_value(reverse_bits(*value));
    if (name == "s_not_b32") return Moved::constant_value(~*value);
    return std::nullopt;
}

class Tracer {
public:
    Tracer(const Program& program, const std::vector<RootParam>& layout, HwStage stage,
           const Structured* lanes)
        : p_(&program), layout_(&layout), stage_(stage), lanes_(lanes) {
        std::size_t n = 0;
        for (const auto& entry : program.inputs)
            n = std::max(n, static_cast<std::size_t>(entry.second.index) + 1);
        for (const Block& block : program.blocks) {
            for (const Phi& phi : block.phis)
                n = std::max(n, static_cast<std::size_t>(phi.value.index) + 1);
            for (const Inst& inst : block.insts)
                for (const auto& entry : inst.writes)
                    n = std::max(n, static_cast<std::size_t>(entry.second.index) + 1);
        }
        defs_.assign(n, std::nullopt);
        phis_.assign(n, std::nullopt);
        inputs_.assign(n, std::nullopt);
        for (const auto& entry : program.inputs) inputs_[entry.second.index] = entry.first;
        for (std::size_t b = 0; b < program.blocks.size(); ++b) {
            const Block& block = program.blocks[b];
            for (std::size_t i = 0; i < block.phis.size(); ++i) phis_[block.phis[i].value.index] = Site{b, i};
            for (std::size_t i = 0; i < block.insts.size(); ++i)
                for (const auto& entry : block.insts[i].writes) defs_[entry.second.index] = Site{b, i};
        }
        rpo_.assign(program.blocks.size(), kNoIndex);
        const std::vector<std::size_t> order = reverse_postorder(program.blocks);
        for (std::size_t position = 0; position < order.size(); ++position) rpo_[order[position]] = position;
        origin_ = propagate_copies(n);
    }

    const Inst& inst(Site site) const { return p_->blocks[site.block].insts[site.index]; }

    Term origin(Value value) const {
        return value.index < origin_.size() ? origin_[value.index] : Term::origin(value);
    }

    std::optional<std::pair<std::size_t, std::uint32_t>> param_of(Reg reg) const {
        if (reg.kind != Reg::Kind::S) return std::nullopt;
        for (std::size_t i = 0; i < layout_->size(); ++i) {
            const std::uint8_t first = user_sgpr(stage_, (*layout_)[i].user_data);
            if (reg.code < first) continue;
            const std::uint32_t dword = static_cast<std::uint32_t>(reg.code) - first;
            if (dword < (*layout_)[i].sgprs) return std::pair<std::size_t, std::uint32_t>{i, dword};
        }
        return std::nullopt;
    }

    Address flat_address(const Inst& inst, const Operand* addr, const Operand* saddr, std::int16_t offset) const {
        const auto* vec = addr == nullptr ? nullptr : std::get_if<VectorOperand>(addr);
        if (vec == nullptr) throw TraceError{"a global address not in VGPRs"};
        const std::uint32_t off = static_cast<std::uint32_t>(static_cast<std::int32_t>(offset));
        if (const auto* scalar = saddr == nullptr ? nullptr : std::get_if<ScalarOperand>(saddr);
            scalar != nullptr && scalar->code != kNull) {
            const auto word = [&](std::uint8_t k) -> std::optional<Term> {
                const auto value = inst.read(Reg::s(static_cast<std::uint8_t>(scalar->code + k)));
                if (!value) return std::nullopt;
                return origin(*value);
            };
            const auto low = word(0);
            const auto high = word(1);
            if (!low || !high) throw TraceError{"an address not in SGPRs"};
            const auto value = inst.read(Reg::v(vec->reg));
            if (!value) throw TraceError{"a global address"};
            Sum extra = sum(vorigin(*value));
            if (extra.base) throw TraceError{"an offset from a root parameter"};
            extra.offset += off;
            return address(*low, *high, extra);
        }
        const auto word = [&](std::uint8_t k) -> std::optional<Term> {
            const auto value = inst.read(Reg::v(static_cast<std::uint8_t>(vec->reg + k)));
            if (!value) return std::nullopt;
            return vorigin(*value);
        };
        const auto low = word(0);
        const auto high = word(1);
        if (!low || !high) throw TraceError{"a global address"};
        return address(*low, *high, Sum{std::nullopt, off, std::nullopt});
    }

    Address smem_address(const Inst& inst, const Operand& base, std::int32_t offset, const Operand& soffset) const {
        const auto* scalar = std::get_if<ScalarOperand>(&base);
        if (scalar == nullptr) throw TraceError{"an address not in SGPRs"};
        const auto word = [&](std::uint8_t k) -> std::optional<Term> {
            const auto value = inst.read(Reg::s(static_cast<std::uint8_t>(scalar->code + k)));
            if (!value) return std::nullopt;
            return origin(*value);
        };
        const auto low = word(0);
        const auto high = word(1);
        if (!low || !high) throw TraceError{"an address not in SGPRs"};
        const auto so = term(inst, soffset);
        if (!so) throw TraceError{"an unknown offset operand"};
        Sum extra = sum(*so);
        if (extra.base) throw TraceError{"an offset from a root parameter"};
        extra.offset += static_cast<std::uint32_t>(offset);
        return address(*low, *high, extra);
    }

    Source descriptor(const Inst& inst, const Operand& op, std::size_t at) const {
        const auto* scalar = std::get_if<ScalarOperand>(&op);
        if (scalar == nullptr) throw TraceError{"a descriptor not in SGPRs"};
        std::vector<Term> words;
        words.reserve(scalar->dwords);
        for (std::uint8_t k = 0; k < scalar->dwords; ++k) {
            const auto value = inst.read(Reg::s(static_cast<std::uint8_t>(scalar->code + k)));
            if (!value) throw TraceError{"a descriptor not in SGPRs"};
            words.push_back(origin(*value));
        }
        Known known;
        if (lanes_ != nullptr && at < lanes_->reach.size()) known.emplace_back(lanes_->reach[at], true);
        std::vector<std::size_t> joins;
        return classify(words, joins, known);
    }

private:
    struct JoinGuard {
        std::vector<std::size_t>& joins;
        ~JoinGuard() { joins.pop_back(); }
    };

    const Inst& inst_at(Site site) const { return p_->blocks[site.block].insts[site.index]; }

    std::optional<const Inst*> def(Value value) const {
        if (value.index >= defs_.size()) return std::nullopt;
        const auto& site = defs_[value.index];
        if (!site) return std::nullopt;
        return &inst_at(*site);
    }

    std::optional<Site> def_site(Value value) const {
        if (value.index >= defs_.size()) return std::nullopt;
        return defs_[value.index];
    }

    std::optional<Term> term(const Inst& inst, const Operand& op) const {
        if (const auto* scalar = std::get_if<ScalarOperand>(&op)) {
            if (scalar->code == kNull) return Term::constant_value(0);
            const auto value = inst.read(Reg::s(scalar->code));
            if (!value) return std::nullopt;
            return origin(*value);
        }
        if (const auto* c = std::get_if<ConstantOperand>(&op)) {
            if (const auto value = inline_constant(c->code)) return Term::constant_value(*value);
            return std::nullopt;
        }
        if (const auto* literal = std::get_if<LiteralOperand>(&op)) return Term::constant_value(literal->value);
        return std::nullopt;
    }

    std::optional<std::pair<const Inst*, std::array<Term, 2>>> sop2(Value value,
                                                                   std::initializer_list<std::string_view> names) const {
        const auto inst = def(value);
        if (!inst) return std::nullopt;
        const std::string_view name = (*inst)->name();
        if (std::find(names.begin(), names.end(), name) == names.end()) return std::nullopt;
        if ((*inst)->writes.empty() || (*inst)->writes.front().second != value) return std::nullopt;
        const auto* fields = std::get_if<SopFields>(&(*inst)->fields);
        if (fields == nullptr || fields->src.empty() || fields->src.size() < 2) return std::nullopt;
        const auto first = term(**inst, fields->src[0]);
        const auto second = term(**inst, fields->src[1]);
        if (!first || !second) return std::nullopt;
        return std::pair<const Inst*, std::array<Term, 2>>{*inst, {*first, *second}};
    }

    Sum sum(const Term& value) const {
        if (value.kind == Term::Kind::Const) return Sum{std::nullopt, value.constant, std::nullopt};
        const Value v = value.value;
        const Sum dynamic{std::nullopt, 0, v};
        if (v.index < inputs_.size() && inputs_[v.index]) {
            const auto found = param_of(*inputs_[v.index]);
            if (found && found->second == 0) return Sum{v, 0, std::nullopt};
            return dynamic;
        }
        if (getpc(v)) return Sum{v, 0, std::nullopt};
        std::optional<std::array<Term, 2>> add;
        if (const auto s = sop2(v, {"s_add_u32", "s_add_i32"})) add = s->second;
        if (!add) add = vadd(v);
        if (!add) return dynamic;
        const Sum x = sum((*add)[0]);
        const Sum y = sum((*add)[1]);
        if ((x.base && y.base) || (x.dynamic && y.dynamic)) return dynamic;
        return Sum{x.base ? x.base : y.base, x.offset + y.offset, x.dynamic ? x.dynamic : y.dynamic};
    }

    std::optional<const Inst*> getpc(Value low) const {
        const auto inst = def(low);
        if (!inst) return std::nullopt;
        if ((*inst)->name() != "s_getpc_b64") return std::nullopt;
        if ((*inst)->writes.empty() || (*inst)->writes.front().second != low) return std::nullopt;
        return inst;
    }

    std::optional<std::array<Term, 2>> vadd(Value value) const {
        const auto inst = def(value);
        if (!inst) return std::nullopt;
        const auto* fields = std::get_if<Valu>(&(*inst)->fields);
        if (fields == nullptr) return std::nullopt;
        const std::string_view name = (*inst)->name();
        if (name != "v_add_co_u32" && name != "v_add_nc_u32") return std::nullopt;
        if (!fields->dst) return std::nullopt;
        const auto* dst = std::get_if<VectorOperand>(&*fields->dst);
        if (dst == nullptr) return std::nullopt;
        if ((*inst)->write(Reg::v(dst->reg)) != std::optional<Value>{value}) return std::nullopt;
        if (fields->abs != 0 || fields->neg != 0 || fields->sdwa || fields->dpp) return std::nullopt;
        if (fields->src.size() < 2) return std::nullopt;
        const auto first = vterm(**inst, fields->src[0]);
        const auto second = vterm(**inst, fields->src[1]);
        if (!first || !second) return std::nullopt;
        return std::array<Term, 2>{*first, *second};
    }

    std::optional<Term> vterm(const Inst& inst, const Operand& op) const {
        if (const auto* vec = std::get_if<VectorOperand>(&op)) {
            const auto value = inst.read(Reg::v(vec->reg));
            if (!value) return std::nullopt;
            return vorigin(*value);
        }
        return term(inst, op);
    }

    Term vorigin(Value value) const {
        const auto inst = def(value);
        if (inst && (*inst)->name() == "v_mov_b32") {
            const auto* fields = std::get_if<Valu>(&(*inst)->fields);
            if (fields != nullptr && fields->abs == 0 && fields->neg == 0 && !fields->sdwa && !fields->dpp &&
                !fields->src.empty()) {
                if (const auto found = vterm(**inst, fields->src.front())) return *found;
            }
        }
        return origin(value);
    }

    Address address(const Term& low, const Term& high, const Sum& extra) const {
        const Sum s = sum(low);
        if (s.dynamic && extra.dynamic) throw TraceError{"an address with two computed terms"};
        if (!s.base) throw TraceError{why_no_base(s.dynamic)};
        const Value base = *s.base;
        if (const auto pc = getpc(base)) {
            std::optional<Value> high_half_value;
            if ((*pc)->writes.size() > 1) high_half_value = (*pc)->writes[1].second;
            if (!high_half(high, high_half_value) &&
                !(high_half_value && carried(high, *high_half_value)))
                throw TraceError{"an address with an untracked high half"};
            return Address{kCode,
                           static_cast<std::uint32_t>((*pc)->machine.offset) + 4u + s.offset + extra.offset,
                           s.dynamic ? s.dynamic : extra.dynamic};
        }
        if (base.index >= inputs_.size() || !inputs_[base.index])
            throw TraceError{"an address not from a root parameter"};
        const auto found = param_of(*inputs_[base.index]);
        if (!found) throw TraceError{"an address not from a root parameter"};
        const std::size_t param = found->first;
        std::optional<Value> high_value;
        switch ((*layout_)[param].root.kind) {
            case RootType::Kind::Table:
            case RootType::Kind::ResourceHeap:
            case RootType::Kind::SamplerHeap: break;
            case RootType::Kind::Descriptor: {
                const Reg reg = Reg::s(static_cast<std::uint8_t>(
                    user_sgpr(stage_, (*layout_)[param].user_data) + 1));
                std::optional<Value> input;
                for (const auto& entry : p_->inputs)
                    if (entry.first == reg) input = entry.second;
                if (!input) throw TraceError{"a root descriptor without its high half"};
                high_value = *input;
                break;
            }
            default: throw TraceError{"an address from a root parameter that is no address"};
        }
        const std::optional<Value> dynamic = s.dynamic ? s.dynamic : extra.dynamic;
        if (!high_half(high, high_value) && !(dynamic && high_value && carried(high, *high_value)))
            throw TraceError{"an address with an untracked high half"};
        return Address{param, s.offset + extra.offset, dynamic};
    }

    bool high_half(const Term& high, std::optional<Value> value) const {
        const Term want = value ? Term::origin(*value) : Term::constant_value(0);
        if (high == want) return true;
        if (high.kind != Term::Kind::Value) return false;
        const auto add = sop2(high.value, {"s_addc_u32"});
        if (!add) return false;
        const Term& a = add->second[0];
        const Term& b = add->second[1];
        return (a == want && b == Term::constant_value(0)) || (b == want && a == Term::constant_value(0));
    }

    bool carried(const Term& high, Value value) const {
        if (high.kind != Term::Kind::Value) return false;
        const Value v = high.value;
        if (v == value) return true;
        if (const auto add = sop2(v, {"s_addc_u32"}))
            return carried(add->second[0], value) || carried(add->second[1], value);
        const auto inst = def(v);
        if (!inst) return false;
        const auto* fields = std::get_if<Valu>(&(*inst)->fields);
        if (fields == nullptr) return false;
        if ((*inst)->name() != "v_add_co_ci_u32") return false;
        const std::size_t count = std::min<std::size_t>(2, fields->src.size());
        for (std::size_t i = 0; i < count; ++i) {
            const auto t = vterm(**inst, fields->src[i]);
            if (t && carried(*t, value)) return true;
        }
        return false;
    }

    const char* why_no_base(std::optional<Value> dynamic) const {
        if (!dynamic) return "a constant address";
        const Value v = *dynamic;
        if (v.index < inputs_.size() && inputs_[v.index]) return "an address in an SGPR that is no root parameter";
        for (Value leaf : leaves(v)) {
            const auto inst = def(leaf);
            if (inst && (*inst)->name() == "s_getpc_b64") return "an address relative to the program counter";
        }
        const auto inst = def(v);
        if (!inst) return "an address from a phi";
        if ((*inst)->name().starts_with("s_load_")) return "an address loaded from memory";
        return "an address not from a root parameter";
    }

    std::vector<Value> leaves(Value value) const {
        std::vector<Value> out;
        collect_leaves(value, out);
        return out;
    }

    void collect_leaves(Value value, std::vector<Value>& out) const {
        const auto add = sop2(value, {"s_add_u32", "s_add_i32"});
        if (!add) {
            out.push_back(value);
            return;
        }
        for (const Term& t : add->second)
            if (t.kind == Term::Kind::Value) collect_leaves(t.value, out);
    }

    Source classify(const std::vector<Term>& words, std::vector<std::size_t>& joins, Known& known) const {
        if (const auto source = phi(words, joins, known)) return *source;
        if (const auto source = select(words, joins, known)) return *source;
        const auto read = [&](const Term& w) -> std::optional<Value> {
            if (w.kind != Term::Kind::Value) return std::nullopt;
            return first_lane_source(w.value);
        };
        std::vector<Value> first_lanes;
        bool every = true;
        for (const Term& word : words) {
            const auto value = read(word);
            if (!value) {
                every = false;
                break;
            }
            first_lanes.push_back(*value);
        }
        if (every) {
            std::vector<Term> lanes;
            lanes.reserve(first_lanes.size());
            for (Value value : first_lanes) lanes.push_back(Term::origin(value));
            return per_lane(lanes, joins, known);
        }
        for (const Term& word : words)
            if (read(word)) throw TraceError{"a descriptor partly read from the first lane"};
        std::vector<std::uint32_t> constants;
        bool all_constant = true;
        for (const Term& word : words) {
            if (word.kind != Term::Kind::Const) {
                all_constant = false;
                break;
            }
            constants.push_back(word.constant);
        }
        if (all_constant) return Source::constant(constants);
        if (const auto source = loaded(words)) return *source;
        if (words.size() == 4 && words[2].kind == Term::Kind::Const && words[3].kind == Term::Kind::Const) {
            Term high = words[1];
            std::uint32_t bits = 0;
            while (high.kind == Term::Kind::Value) {
                const auto more = set_bits(high.value);
                if (!more) break;
                high = more->first;
                bits |= more->second;
            }
            const Address built = address(words[0], high, Sum{std::nullopt, 0, std::nullopt});
            return Source::built(built, bits, std::array<std::uint32_t, 2>{words[2].constant, words[3].constant});
        }
        const auto is_loaded = [&](const Term& word) {
            if (word.kind != Term::Kind::Value) return false;
            const auto inst = def(word.value);
            return inst && (*inst)->name().starts_with("s_load_");
        };
        for (const Term& word : words)
            if (is_loaded(word)) throw TraceError{"a descriptor changed after loading"};
        throw TraceError{"a descriptor neither loaded nor built"};
    }

    std::optional<Source> phi(const std::vector<Term>& words, std::vector<std::size_t>& joins,
                              Known& known) const {
        const auto phi_of = [&](const Term& word) -> std::optional<Site> {
            if (word.kind != Term::Kind::Value) return std::nullopt;
            if (word.value.index >= phis_.size()) return std::nullopt;
            return phis_[word.value.index];
        };
        std::optional<std::size_t> block;
        for (const Term& word : words) {
            const auto site = phi_of(word);
            if (!site) continue;
            if (block && *block != site->block) throw TraceError{"a descriptor joined in several blocks"};
            block = site->block;
        }
        if (!block) return std::nullopt;
        if (std::find(joins.begin(), joins.end(), *block) != joins.end())
            throw TraceError{"a descriptor that changes in a loop"};
        if (joins.size() >= kMaxJoins) throw TraceError{"a descriptor joined too often"};
        joins.push_back(*block);
        const JoinGuard guard{joins};
        const std::vector<Phi>& phis = p_->blocks[*block].phis;
        const std::size_t preds = p_->blocks[*block].preds.size();
        std::vector<Source> args;
        args.reserve(preds);
        for (std::size_t j = 0; j < preds; ++j) {
            std::vector<Term> edge;
            edge.reserve(words.size());
            for (const Term& word : words) {
                const auto site = phi_of(word);
                edge.push_back(site ? origin(phis[site->index].args[j]) : word);
            }
            args.push_back(classify(edge, joins, known));
        }
        return Source::phi(*block, std::move(args));
    }

    std::optional<Source> select(const std::vector<Term>& words, std::vector<std::size_t>& joins,
                                 Known& known) const {
        std::vector<std::optional<std::tuple<Value, Term, Term>>> picks;
        picks.reserve(words.size());
        for (const Term& word : words) {
            if (word.kind != Term::Kind::Value) {
                picks.push_back(std::nullopt);
                continue;
            }
            picks.push_back(picked(word.value));
        }
        std::optional<Value> scc;
        for (const auto& pick : picks)
            if (pick) {
                scc = std::get<0>(*pick);
                break;
            }
        if (!scc) return std::nullopt;
        for (const auto& pick : picks)
            if (pick && !(std::get<0>(*pick) == *scc))
                throw TraceError{"a descriptor chosen by several conditions"};
        if (joins.size() >= kMaxJoins) throw TraceError{"a descriptor joined too often"};
        joins.push_back(kNoIndex);
        const JoinGuard guard{joins};
        std::vector<Source> args;
        for (bool set : {true, false}) {
            std::vector<Term> side;
            side.reserve(words.size());
            for (std::size_t k = 0; k < words.size(); ++k) {
                if (picks[k]) {
                    side.push_back(set ? std::get<1>(*picks[k]) : std::get<2>(*picks[k]));
                } else {
                    side.push_back(words[k]);
                }
            }
            args.push_back(classify(side, joins, known));
        }
        return Source::select(*scc, std::move(args));
    }

    std::optional<std::tuple<Value, Term, Term>> picked(Value v) const {
        const auto inst = def(v);
        if (!inst) return std::nullopt;
        const auto* fields = std::get_if<SopFields>(&(*inst)->fields);
        if (fields == nullptr) return std::nullopt;
        std::size_t k = kNoIndex;
        for (std::size_t n = 0; n < (*inst)->writes.size(); ++n)
            if ((*inst)->writes[n].second == v) {
                k = n;
                break;
            }
        if (k == kNoIndex || k > 1) return std::nullopt;
        const std::string_view name = (*inst)->name();
        const bool wide = name == "s_cselect_b64" || name == "s_cmov_b64";
        if (!wide && name != "s_cselect_b32" && name != "s_cmov_b32") return std::nullopt;
        const std::uint8_t dword_index = static_cast<std::uint8_t>(k);
        const auto dword = [&](const Operand& op) -> std::optional<Term> {
            if (const auto* scalar = std::get_if<ScalarOperand>(&op)) {
                if (scalar->code == kNull) return Term::constant_value(0);
                const auto value = (*inst)->read(Reg::s(static_cast<std::uint8_t>(scalar->code + dword_index)));
                if (!value) return std::nullopt;
                return origin(*value);
            }
            if (const auto* c = std::get_if<ConstantOperand>(&op)) {
                if (!wide) {
                    const auto value = inline_constant(c->code);
                    if (!value) return std::nullopt;
                    return Term::constant_value(*value);
                }
                if (c->code >= 128 && c->code <= 208) {
                    const auto low = inline_constant(c->code);
                    if (!low) return std::nullopt;
                    return Term::constant_value(dword_index == 0
                                                    ? *low
                                                    : static_cast<std::uint32_t>(static_cast<std::int32_t>(*low) >> 31));
                }
                return std::nullopt;
            }
            if (const auto* literal = std::get_if<LiteralOperand>(&op)) {
                if (wide) return std::nullopt;
                return Term::constant_value(literal->value);
            }
            return std::nullopt;
        };
        if (fields->src.empty()) return std::nullopt;
        const auto set = dword(fields->src.front());
        if (!set) return std::nullopt;
        std::optional<Term> clear;
        if (name.starts_with("s_cmov")) {
            if (!fields->dst) return std::nullopt;
            clear = dword(*fields->dst);
        } else {
            if (fields->src.size() < 2) return std::nullopt;
            clear = dword(fields->src[1]);
        }
        if (!clear) return std::nullopt;
        const auto scc = (*inst)->read(Reg::scc());
        if (!scc) return std::nullopt;
        return std::tuple<Value, Term, Term>{*scc, *set, *clear};
    }

public:
    std::optional<Value> first_lane_source(Value v) const {
        const auto inst = def(v);
        if (!inst) return std::nullopt;
        const auto* fields = std::get_if<Valu>(&(*inst)->fields);
        if (fields == nullptr) return std::nullopt;
        if ((*inst)->name() != "v_readfirstlane_b32" || fields->sdwa || fields->dpp) return std::nullopt;
        if (fields->src.empty()) return std::nullopt;
        const auto* vec = std::get_if<VectorOperand>(&fields->src.front());
        if (vec == nullptr) return std::nullopt;
        return (*inst)->read(Reg::v(vec->reg));
    }

    std::optional<std::vector<Value>> first_lane_words(const Inst& inst, const Operand& op) const {
        const auto* scalar = std::get_if<ScalarOperand>(&op);
        if (scalar == nullptr) return std::nullopt;
        std::vector<Value> out;
        out.reserve(scalar->dwords);
        for (std::uint8_t k = 0; k < scalar->dwords; ++k) {
            const auto value = inst.read(Reg::s(static_cast<std::uint8_t>(scalar->code + k)));
            if (!value) return std::nullopt;
            const Term t = origin(*value);
            if (t.kind != Term::Kind::Value) return std::nullopt;
            if (!first_lane_source(t.value)) return std::nullopt;
            out.push_back(t.value);
        }
        return out;
    }

    Term lane_term(Term t) const {
        for (std::size_t i = 0; i < kMaxJoins * 2; ++i) {
            if (t.kind != Term::Kind::Value) break;
            const Term next = vorigin(t.value);
            if (next == t) break;
            t = next;
        }
        return t;
    }

    bool varies(const Term& t) const {
        if (t.kind != Term::Kind::Value) return false;
        const Value v = t.value;
        std::optional<Reg> reg;
        if (v.index < inputs_.size() && inputs_[v.index]) {
            reg = inputs_[v.index];
        } else if (v.index < phis_.size() && phis_[v.index]) {
            const Site site = *phis_[v.index];
            reg = p_->blocks[site.block].phis[site.index].reg;
        } else if (const auto inst = def(v)) {
            for (const auto& entry : (*inst)->writes)
                if (entry.second == v) {
                    reg = entry.first;
                    break;
                }
        }
        return reg && reg->kind == Reg::Kind::V;
    }

    Term lane_value(Term t, const Known& known) const {
        for (std::size_t i = 0; i < kMaxJoins * 2; ++i) {
            if (t.kind != Term::Kind::Value) break;
            const Term next = vstep(t.value);
            if (next == t) break;
            if (varies(t)) {
                if (const auto site = def_site(t.value)) runs(*site, known);
            }
            t = next;
        }
        return t;
    }

    Term vstep(Value v) const {
        if (const auto inst = def(v)) {
            if ((*inst)->name() == "v_mov_b32") {
                const auto* fields = std::get_if<Valu>(&(*inst)->fields);
                if (fields != nullptr && fields->abs == 0 && fields->neg == 0 && !fields->sdwa &&
                    !fields->dpp && !fields->src.empty()) {
                    if (const auto t = raw(**inst, fields->src.front())) return *t;
                }
            }
        }
        return origin(v);
    }

    std::optional<Term> raw(const Inst& inst, const Operand& op) const {
        if (const auto* vec = std::get_if<VectorOperand>(&op)) {
            const auto value = inst.read(Reg::v(vec->reg));
            if (!value) return std::nullopt;
            return Term::origin(*value);
        }
        return term(inst, op);
    }

    void runs(Site site, const Known& known) const {
        if (lanes_ == nullptr) return;
        if (site.block >= lanes_->guards.size()) return;
        if (site.index >= lanes_->guards[site.block].size()) return;
        const std::optional<Bool>& guard = lanes_->guards[site.block][site.index];
        if (!guard) return;
        Known skips = known;
        skips.emplace_back(lanes_->reach[site.block], true);
        skips.emplace_back(*guard, false);
        if (lanes_->bools.meet(skips))
            throw TraceError{"a per-lane descriptor written where some lanes may keep the old one"};
    }

    Source per_lane(const std::vector<Term>& words, std::vector<std::size_t>& joins,
                    Known& known) const {
        std::vector<Term> stepped;
        stepped.reserve(words.size());
        for (const Term& word : words) stepped.push_back(lane_value(word, known));
        std::optional<Term> lane;
        for (const Term& word : stepped)
            if (varies(word)) {
                lane = word;
                break;
            }
        if (!lane) return classify(stepped, joins, known);
        if (const auto source = fetched(stepped, known)) return *source;
        if (const auto source = lane_select(stepped, joins, known)) return *source;
        if (const auto source = lane_phi(stepped, joins, known)) return *source;
        const Value v = lane->value;
        if (v.index < inputs_.size() && inputs_[v.index])
            throw TraceError{"a descriptor read from the first lane of an input VGPR"};
        const auto inst = def(v);
        if (!inst) throw TraceError{"a descriptor read from the first lane of a phi"};
        const std::string_view name = (*inst)->name();
        if (name.starts_with("ds_read"))
            throw TraceError{"a descriptor read from the first lane of an LDS read"};
        if (name.starts_with("buffer_load"))
            throw TraceError{"a descriptor read from the first lane of a buffer load"};
        if (name.starts_with("global_load") || name.starts_with("flat_load"))
            throw TraceError{"a descriptor read from the first lane of a partial or mixed load"};
        throw TraceError{"a descriptor read from the first lane of computed VGPRs"};
    }

    std::optional<Source> fetched(const std::vector<Term>& words, const Known& known) const {
        const auto at = [&](const Term& w) -> std::optional<std::pair<Site, std::size_t>> {
            if (w.kind != Term::Kind::Value) return std::nullopt;
            const auto site = def_site(w.value);
            if (!site) return std::nullopt;
            const Inst& inst = inst_at(*site);
            if (!inst.name().starts_with("global_load_dword")) return std::nullopt;
            std::size_t dword = 0;
            bool found = false;
            for (const auto& entry : inst.writes) {
                if (entry.first.kind != Reg::Kind::V) continue;
                if (entry.second == w.value) {
                    found = true;
                    break;
                }
                ++dword;
            }
            if (!found) return std::nullopt;
            return std::pair<Site, std::size_t>{*site, dword};
        };
        std::vector<std::pair<Site, std::size_t>> sites;
        for (const Term& word : words) {
            const auto site = at(word);
            if (!site) return std::nullopt;
            sites.push_back(*site);
        }
        const auto address = [&](Site site) {
            const Inst& inst = inst_at(site);
            const auto* fields = std::get_if<FlatFields>(&inst.fields);
            if (fields == nullptr) throw TraceError{"a global load"};
            return flat_address(inst, fields->addr ? &*fields->addr : nullptr,
                                fields->saddr ? &*fields->saddr : nullptr, fields->offset);
        };
        const Site load = sites[0].first;
        const std::size_t first = sites[0].second;
        const Address a0 = address(load);
        for (std::size_t k = 0; k < sites.size(); ++k) {
            runs(sites[k].first, known);
            const Address a = address(sites[k].first);
            const std::uint32_t want = a0.offset + 4u * static_cast<std::uint32_t>(first + k);
            if (a.param != a0.param || a.dynamic != a0.dynamic ||
                a.offset + 4u * static_cast<std::uint32_t>(sites[k].second) != want)
                throw TraceError{"a descriptor read from the first lane of loads that do not line up"};
        }
        return Source::fetched(load, static_cast<std::uint8_t>(first));
    }

    std::optional<Source> lane_select(const std::vector<Term>& words, std::vector<std::size_t>& joins,
                                      Known& known) const {
        std::vector<std::optional<std::tuple<Value, Term, Term>>> picks;
        picks.reserve(words.size());
        for (const Term& word : words) {
            if (word.kind != Term::Kind::Value) {
                picks.push_back(std::nullopt);
                continue;
            }
            picks.push_back(lane_picked(word.value));
        }
        std::optional<Value> mask;
        for (const auto& pick : picks)
            if (pick) {
                mask = std::get<0>(*pick);
                break;
            }
        if (!mask) return std::nullopt;
        for (const auto& pick : picks)
            if (pick && !(std::get<0>(*pick) == *mask))
                throw TraceError{"a descriptor chosen per lane by several masks"};
        for (std::size_t k = 0; k < words.size(); ++k) {
            if (words[k].kind != Term::Kind::Value || !picks[k]) continue;
            if (const auto site = def_site(words[k].value)) runs(*site, known);
        }
        if (joins.size() >= kMaxJoins) throw TraceError{"a descriptor joined too often"};
        joins.push_back(kNoIndex);
        const JoinGuard guard{joins};
        std::optional<Bool> bit;
        if (lanes_ != nullptr) {
            const auto found = lanes_->masks.find(*mask);
            if (found != lanes_->masks.end()) bit = found->second;
        }
        std::vector<Source> args;
        for (bool set : {true, false}) {
            std::vector<Term> side;
            side.reserve(words.size());
            for (std::size_t k = 0; k < words.size(); ++k) {
                if (picks[k]) {
                    side.push_back(set ? std::get<1>(*picks[k]) : std::get<2>(*picks[k]));
                } else {
                    side.push_back(words[k]);
                }
            }
            const std::size_t n = known.size();
            if (bit) known.emplace_back(*bit, set);
            std::optional<Source> arg;
            try {
                arg = per_lane(side, joins, known);
            } catch (...) {
                known.resize(n);
                throw;
            }
            known.resize(n);
            args.push_back(*arg);
        }
        return Source::lanes(*mask, std::move(args));
    }

    std::optional<std::tuple<Value, Term, Term>> lane_picked(Value v) const {
        const auto inst = def(v);
        if (!inst) return std::nullopt;
        const auto* fields = std::get_if<Valu>(&(*inst)->fields);
        if (fields == nullptr || !fields->dst) return std::nullopt;
        const auto* target = std::get_if<VectorOperand>(&*fields->dst);
        if (target == nullptr) return std::nullopt;
        if ((*inst)->name() != "v_cndmask_b32" ||
            (*inst)->write(Reg::v(target->reg)) != std::optional<Value>{v} || fields->abs != 0 ||
            fields->neg != 0 || fields->clamp || fields->omod != 0 || fields->sdwa || fields->dpp)
            return std::nullopt;
        if (fields->src.size() != 3) return std::nullopt;
        const auto* mask_op = std::get_if<ScalarOperand>(&fields->src[2]);
        if (mask_op == nullptr) return std::nullopt;
        const auto mask = (*inst)->read(Reg::s(mask_op->code));
        if (!mask) return std::nullopt;
        const auto set = raw(**inst, fields->src[1]);
        if (!set) return std::nullopt;
        const auto clear = raw(**inst, fields->src[0]);
        if (!clear) return std::nullopt;
        return std::tuple<Value, Term, Term>{*mask, *set, *clear};
    }

    std::optional<Source> lane_phi(const std::vector<Term>& words, std::vector<std::size_t>& joins,
                                   Known& known) const {
        const auto phi_of = [&](const Term& word) -> std::optional<Site> {
            if (word.kind != Term::Kind::Value) return std::nullopt;
            if (word.value.index >= phis_.size()) return std::nullopt;
            return phis_[word.value.index];
        };
        std::optional<std::size_t> block;
        for (const Term& word : words) {
            const auto site = phi_of(word);
            if (!site) continue;
            if (block && *block != site->block) throw TraceError{"a descriptor joined in several blocks"};
            block = site->block;
        }
        if (!block) return std::nullopt;
        if (std::find(joins.begin(), joins.end(), *block) != joins.end())
            throw TraceError{"a descriptor that changes in a loop"};
        const std::vector<std::size_t>& preds = p_->blocks[*block].preds;
        for (std::size_t q : preds)
            if (rpo_[q] >= rpo_[*block])
                throw TraceError{"a descriptor read from the first lane of a phi at a loop header"};
        if (lanes_ == nullptr)
            throw TraceError{"a descriptor read from the first lane of a phi, without the lanes' paths"};
        const Structured& lanes = *lanes_;
        for (const Term& word : words) {
            if (word.kind != Term::Kind::Value || phi_of(word)) continue;
            const auto site = def_site(word.value);
            if (site && (site->block == *block || reaches(*block, site->block)))
                throw TraceError{"a descriptor partly joined by control flow"};
        }
        if (joins.size() >= kMaxJoins) throw TraceError{"a descriptor joined too often"};
        joins.push_back(*block);
        const std::vector<Phi>& phis = p_->blocks[*block].phis;
        std::vector<std::optional<Source>> args;
        args.reserve(preds.size());
        for (std::size_t j = 0; j < preds.size(); ++j) {
            const std::size_t q = preds[j];
            const std::size_t n = known.size();
            known.emplace_back(lanes.reach[q], true);
            const LaneExit& exit = lanes.exits[q];
            if (exit.kind == LaneExit::Kind::Branch && exit.target != exit.not_taken)
                known.emplace_back(exit.when, exit.target == *block);
            std::optional<Source> arg;
            if (lanes.bools.meet(known)) {
                std::vector<Term> edge;
                edge.reserve(words.size());
                for (const Term& word : words) {
                    const auto site = phi_of(word);
                    edge.push_back(site ? Term::origin(phis[site->index].args[j]) : word);
                }
                try {
                    arg = per_lane(edge, joins, known);
                } catch (...) {
                    known.resize(n);
                    joins.pop_back();
                    throw;
                }
            }
            known.resize(n);
            args.push_back(arg);
        }
        joins.pop_back();
        return Source::lane_phi(*block, std::move(args));
    }

    bool reaches(std::size_t from, std::size_t to) const {
        std::vector<bool> seen(p_->blocks.size(), false);
        std::vector<std::size_t> stack = p_->blocks[from].exit.successors();
        while (!stack.empty()) {
            const std::size_t x = stack.back();
            stack.pop_back();
            if (x == to) return true;
            if (seen[x]) continue;
            seen[x] = true;
            const std::vector<std::size_t> more = p_->blocks[x].exit.successors();
            stack.insert(stack.end(), more.begin(), more.end());
        }
        return false;
    }

    Waterfall waterfall(std::size_t b, const std::vector<std::size_t>& accesses,
                        const std::vector<Value>& reads) const {
        constexpr const char* kLoop = "a descriptor read from the first lane outside a waterfall loop";
        constexpr const char* kSplit = "a waterfall loop nested in another or split over blocks";
        constexpr const char* kSelect = "a waterfall loop whose compares do not select the access's lanes";
        constexpr const char* kRetry = "a waterfall loop whose retry does not keep the lanes still waiting";
        if (accesses.empty() || reads.empty()) throw TraceError{kLoop};
        const std::size_t access = accesses[0];
        const Block& block = p_->blocks[b];
        if (lanes_ == nullptr) throw TraceError{kLoop};
        const Structured& lanes = *lanes_;
        if (block.insts.empty()) throw TraceError{kLoop};
        const Inst& exit = block.insts.back();
        if (block.exit.kind != Exit::Kind::Branch || block.exit.target != b ||
            block.exit.not_taken == b || exit.name() != "s_cbranch_execnz")
            throw TraceError{kLoop};
        const auto mask_of = [&](std::optional<Value> low, std::optional<Value> high) -> std::optional<Bool> {
            std::optional<Value> pair = high;
            if (!p_->wave64) pair = std::nullopt;
            if (!low) return std::nullopt;
            if (p_->wave64 && !pair) return std::nullopt;
            return lanes.mask_bits(*low, pair);
        };
        const auto exec = [&](const Inst& inst) {
            return mask_of(inst.read(Reg::s(kExec)), inst.read(Reg::s(static_cast<std::uint8_t>(kExec + 1))));
        };
        for (Value v : reads) {
            const auto site = def_site(v);
            if (!site || site->block != b) throw TraceError{kSplit};
        }
        if (access >= block.insts.size()) throw TraceError{kLoop};
        const std::optional<Bool> selected = exec(block.insts[access]);
        if (!selected) throw TraceError{kSelect};
        for (std::size_t i : accesses)
            if (i >= block.insts.size() || !(exec(block.insts[i]) == selected)) throw TraceError{kSelect};
        const std::optional<Bool> retry = exec(exit);
        if (!retry) throw TraceError{kRetry};
        const auto first_site = def_site(reads[0]);
        if (!first_site) throw TraceError{kLoop};
        const std::optional<Bool> remaining = exec(inst_at(*first_site));
        if (!remaining) throw TraceError{kSelect};
        for (Value v : reads) {
            const auto site = def_site(v);
            if (!site || site->index >= access) throw TraceError{kSelect};
            if (!(exec(inst_at(*site)) == remaining)) throw TraceError{kSelect};
        }
        const std::pair<Bool, bool> reach{lanes.reach[b], true};
        const auto meet = [&](const Known& conditions) { return lanes.bools.meet(conditions); };
        Waterfall out;
        Known required{{*remaining, true}};
        for (std::size_t i = 0; i < access; ++i) {
            const Inst& inst = block.insts[i];
            const auto* fields = std::get_if<Valu>(&inst.fields);
            if (fields == nullptr) continue;
            const std::string_view name = inst.name();
            std::string_view ty;
            if (name.starts_with("v_cmp_eq_")) {
                ty = name.substr(9);
            } else if (name.starts_with("v_cmpx_eq_")) {
                ty = name.substr(10);
            } else {
                continue;
            }
            std::size_t dwords = 0;
            if (ty == "u32" || ty == "i32") dwords = 1;
            else if (ty == "u64" || ty == "i64") dwords = 2;
            else continue;
            if (fields->abs != 0 || fields->neg != 0 || fields->sdwa || fields->dpp) continue;
            const auto pair = [&](const Operand& first, const Operand& own) -> std::optional<std::vector<Value>> {
                const auto* scalar = std::get_if<ScalarOperand>(&first);
                const auto* vec = std::get_if<VectorOperand>(&own);
                if (scalar == nullptr || vec == nullptr) return std::nullopt;
                std::vector<Value> xs;
                xs.reserve(dwords);
                for (std::size_t k = 0; k < dwords; ++k) {
                    const auto read = inst.read(Reg::s(static_cast<std::uint8_t>(scalar->code + k)));
                    if (!read) return std::nullopt;
                    const Term term_value = origin(*read);
                    if (term_value.kind != Term::Kind::Value) return std::nullopt;
                    const Value x = term_value.value;
                    const auto lane = inst.read(Reg::v(static_cast<std::uint8_t>(vec->reg + k)));
                    if (!lane) return std::nullopt;
                    const auto site_def = def_site(x);
                    if (!site_def) return std::nullopt;
                    const auto source = first_lane_source(x);
                    if (!source) return std::nullopt;
                    const bool same =
                        std::find(reads.begin(), reads.end(), x) != reads.end() && site_def->index < i &&
                        lane_term(Term::origin(*source)) == lane_term(Term::origin(*lane));
                    if (!same) return std::nullopt;
                    xs.push_back(x);
                }
                return xs;
            };
            if (fields->src.size() < 2) continue;
            std::optional<std::vector<Value>> xs = pair(fields->src[0], fields->src[1]);
            if (!xs) xs = pair(fields->src[1], fields->src[0]);
            if (!xs) continue;
            std::optional<std::uint8_t> code;
            if (name.starts_with("v_cmpx_")) {
                code = kExec;
            } else if (fields->dst) {
                if (const auto* scalar = std::get_if<ScalarOperand>(&*fields->dst)) code = scalar->code;
            }
            if (!code) continue;
            const std::optional<Bool> matched =
                mask_of(inst.write(Reg::s(*code)),
                        inst.write(Reg::s(static_cast<std::uint8_t>(*code + 1))));
            if (!matched) continue;
            if (meet({reach, {*selected, true}, {*matched, false}})) continue;
            required.emplace_back(*matched, true);
            out.compares.push_back(Site{b, i});
            for (Value x : *xs)
                if (std::find(out.reads.begin(), out.reads.end(), x) == out.reads.end())
                    out.reads.push_back(x);
        }
        for (Value v : reads)
            if (std::find(out.reads.begin(), out.reads.end(), v) == out.reads.end())
                throw TraceError{kSelect};
        if (meet({reach, {*selected, true}, {*remaining, false}})) throw TraceError{kSelect};
        required.push_back(reach);
        required.emplace_back(*selected, false);
        if (meet(required)) throw TraceError{kSelect};
        if (meet({reach, {*retry, true}, {*remaining, false}}) ||
            meet({reach, {*retry, true}, {*selected, true}}) ||
            meet({reach, {*remaining, true}, {*selected, false}, {*retry, false}}))
            throw TraceError{kRetry};
        return out;
    }

private:
    std::optional<std::pair<Term, std::uint32_t>> set_bits(Value value) const {
        if (const auto or_ = sop2(value, {"s_or_b32"})) {
            const Term& a = or_->second[0];
            const Term& b = or_->second[1];
            if (b.kind == Term::Kind::Const) return std::pair<Term, std::uint32_t>{a, b.constant};
            if (a.kind == Term::Kind::Const) return std::pair<Term, std::uint32_t>{b, a.constant};
        }
        const auto inst = def(value);
        if (!inst) return std::nullopt;
        const auto* fields = std::get_if<SopFields>(&(*inst)->fields);
        if (fields == nullptr || !fields->dst || fields->src.empty()) return std::nullopt;
        const auto* dst = std::get_if<ScalarOperand>(&*fields->dst);
        if (dst == nullptr) return std::nullopt;
        if ((*inst)->name() != "s_bitset1_b32") return std::nullopt;
        const auto bit = term(**inst, fields->src.front());
        if (!bit || bit->kind != Term::Kind::Const) return std::nullopt;
        const auto self = (*inst)->read(Reg::s(dst->code));
        if (!self) return std::nullopt;
        return std::pair<Term, std::uint32_t>{origin(*self),
                                              std::uint32_t{1} << (bit->constant & 31)};
    }

    std::optional<Source> loaded(const std::vector<Term>& words) const {
        if (words.empty() || words[0].kind != Term::Kind::Value) return std::nullopt;
        const Value first_value = words[0].value;
        const auto site = def_site(first_value);
        if (!site) return std::nullopt;
        const Inst& inst = inst_at(*site);
        if (!inst.name().starts_with("s_load_")) return std::nullopt;
        std::size_t first = kNoIndex;
        for (std::size_t i = 0; i < inst.writes.size(); ++i)
            if (inst.writes[i].second == first_value) {
                first = i;
                break;
            }
        if (first == kNoIndex) return std::nullopt;
        for (std::size_t k = 0; k < words.size(); ++k) {
            const std::size_t at = first + k;
            if (at >= inst.writes.size()) return std::nullopt;
            if (words[k] != Term::origin(inst.writes[at].second)) return std::nullopt;
        }
        return Source::loaded(*site, static_cast<std::uint8_t>(first));
    }

    std::optional<Moved> restored(const Inst& inst) const {
        const auto* fields = std::get_if<Valu>(&inst.fields);
        if (fields == nullptr || inst.name() != "v_readlane_b32") return std::nullopt;
        if (fields->src.size() != 2) return std::nullopt;
        const auto* vec = std::get_if<VectorOperand>(&fields->src[0]);
        const auto lane = lane_of(fields->src[1]);
        if (vec == nullptr || !lane) return std::nullopt;
        const auto value = inst.read(Reg::v(vec->reg));
        if (!value) return std::nullopt;
        std::vector<Value> seen;
        try {
            return spilled(*value, *lane, seen);
        } catch (const TraceError&) {
            return std::nullopt;
        }
    }

    std::optional<Moved> spilled(Value value, std::uint32_t lane, std::vector<Value>& seen) const {
        if (value.index < phis_.size() && phis_[value.index]) {
            if (std::find(seen.begin(), seen.end(), value) != seen.end()) return std::nullopt;
            const Site site = *phis_[value.index];
            seen.push_back(value);
            std::optional<Moved> found;
            for (Value arg : p_->blocks[site.block].phis[site.index].args) {
                const auto moved = spilled(arg, lane, seen);
                if (moved) {
                    if (found && !(*found == *moved)) throw TraceError{"spilled lanes disagree"};
                    found = *moved;
                }
            }
            return found;
        }
        const auto inst = def(value);
        if (!inst) throw TraceError{"a spilled value without a definition"};
        const auto* fields = std::get_if<Valu>(&(*inst)->fields);
        if (fields == nullptr) throw TraceError{"a spilled value that is no VGPR write"};
        if ((*inst)->name() != "v_writelane_b32") throw TraceError{"a spilled value that is no lane write"};
        if (!fields->dst || fields->src.size() != 2) throw TraceError{"a lane write with other operands"};
        const auto* dst = std::get_if<VectorOperand>(&*fields->dst);
        if (dst == nullptr) throw TraceError{"a lane write outside VGPRs"};
        const auto at = lane_of(fields->src[1]);
        if (!at) throw TraceError{"a lane write to a computed lane"};
        if (*at != lane) {
            const auto previous = (*inst)->read(Reg::v(dst->reg));
            if (!previous) throw TraceError{"a lane write without its old value"};
            return spilled(*previous, lane, seen);
        }
        const Operand& operand = fields->src[0];
        std::optional<Moved> moved;
        if (const auto* scalar = std::get_if<ScalarOperand>(&operand)) {
            if (scalar->code != kNull) {
                const auto read = (*inst)->read(Reg::s(scalar->code));
                if (read) moved = Moved::origin(*read);
            }
        } else if (const auto* literal = std::get_if<LiteralOperand>(&operand)) {
            moved = Moved::constant_value(literal->value);
        } else if (const auto* c = std::get_if<ConstantOperand>(&operand)) {
            if (const auto value_at = inline_constant(c->code)) moved = Moved::constant_value(*value_at);
        }
        if (!moved) throw TraceError{"a spilled value that is no lane value"};
        return moved;
    }

    std::vector<Term> propagate_copies(std::size_t n) const {
        std::vector<std::optional<Term>> origins(n);
        for (const auto& entry : p_->inputs) origins[entry.second.index] = Term::origin(entry.second);
        const std::vector<std::size_t> order = reverse_postorder(p_->blocks);
        for (std::size_t round = 0; round < kMaxRounds; ++round) {
            bool changed = false;
            for (std::size_t b : order) {
                const Block& block = p_->blocks[b];
                for (const Phi& phi : block.phis) {
                    const Term own = Term::origin(phi.value);
                    std::optional<Term> first;
                    bool differ = false;
                    for (Value arg : phi.args) {
                        if (arg.index >= origins.size() || !origins[arg.index]) continue;
                        const Term& t = *origins[arg.index];
                        if (t == own) continue;
                        if (!first)
                            first = t;
                        else if (!(*first == t))
                            differ = true;
                    }
                    if (!first) continue;
                    const Term next = differ ? own : *first;
                    if (!origins[phi.value.index] || !(*origins[phi.value.index] == next)) {
                        origins[phi.value.index] = next;
                        changed = true;
                    }
                }
                for (const Inst& inst : block.insts) {
                    for (std::size_t k = 0; k < inst.writes.size(); ++k) {
                        const Value v = inst.writes[k].second;
                        std::optional<Moved> moved = copy_source(inst, k);
                        if (!moved) moved = restored(inst);
                        std::optional<Term> next;
                        if (moved) {
                            if (moved->kind == Moved::Kind::Const)
                                next = Term::constant_value(moved->constant);
                            else if (moved->value.index < origins.size())
                                next = origins[moved->value.index];
                        } else {
                            next = Term::origin(v);
                        }
                        if (v.index >= origins.size()) continue;
                        if (next && (!origins[v.index] || !(*origins[v.index] == *next))) {
                            origins[v.index] = next;
                            changed = true;
                        }
                    }
                }
            }
            if (!changed) {
                std::vector<Term> out;
                out.reserve(n);
                for (std::size_t v = 0; v < n; ++v)
                    out.push_back(origins[v] ? *origins[v] : Term::origin(Value{static_cast<std::uint32_t>(v)}));
                return out;
            }
        }
        detail::fail_unsupported(0, "copies that do not settle");
    }

    const Program* p_;
    const std::vector<RootParam>* layout_;
    HwStage stage_;
    std::vector<std::optional<Site>> defs_;
    std::vector<std::optional<Site>> phis_;
    std::vector<std::optional<Reg>> inputs_;
    std::vector<Term> origin_;
    const Structured* lanes_;
    std::vector<std::size_t> rpo_;
};

}  // namespace

std::optional<std::uint32_t> inline_constant(std::uint8_t code) {
    const auto bits = [](float value) { return std::bit_cast<std::uint32_t>(value); };
    if (code >= 128 && code <= 192) return static_cast<std::uint32_t>(code) - 128;
    if (code >= 193 && code <= 208) return static_cast<std::uint32_t>(192 - static_cast<std::int32_t>(code));
    switch (code) {
        case 240: return bits(0.5f);
        case 241: return bits(-0.5f);
        case 242: return bits(1.0f);
        case 243: return bits(-1.0f);
        case 244: return bits(2.0f);
        case 245: return bits(-2.0f);
        case 246: return bits(4.0f);
        case 247: return bits(-4.0f);
        case 248: return 0x3E22F983u;
        default: return std::nullopt;
    }
}

Source Source::loaded(Site load, std::uint8_t first) {
    Source source;
    source.kind = Kind::Loaded;
    source.load = load;
    source.first = first;
    return source;
}

Source Source::built(Address address, std::uint32_t bits, std::array<std::uint32_t, 2> words) {
    Source source;
    source.kind = Kind::Built;
    source.address = address;
    source.bits = bits;
    source.words = words;
    return source;
}

Source Source::constant(std::vector<std::uint32_t> constants) {
    Source source;
    source.kind = Kind::Constant;
    source.constants = std::move(constants);
    return source;
}

Source Source::phi(std::size_t block, std::vector<Source> args) {
    Source source;
    source.kind = Kind::Phi;
    source.block = block;
    source.args = std::move(args);
    return source;
}

Source Source::select(Value scc, std::vector<Source> args) {
    Source source;
    source.kind = Kind::Select;
    source.key = scc;
    source.args = std::move(args);
    return source;
}

Source Source::fetched(Site load, std::uint8_t first) {
    Source source;
    source.kind = Kind::Fetched;
    source.load = load;
    source.first = first;
    return source;
}

Source Source::lanes(Value mask, std::vector<Source> args) {
    Source source;
    source.kind = Kind::Lanes;
    source.key = mask;
    source.args = std::move(args);
    return source;
}

Source Source::lane_phi(std::size_t block, std::vector<std::optional<Source>> options) {
    Source source;
    source.kind = Kind::LanePhi;
    source.block = block;
    source.options = std::move(options);
    return source;
}

std::vector<const Source*> Source::leaves() const {
    if (kind == Kind::Phi || kind == Kind::Select || kind == Kind::Lanes) {
        std::vector<const Source*> out;
        for (const Source& arg : args)
            for (const Source* leaf : arg.leaves()) out.push_back(leaf);
        return out;
    }
    if (kind == Kind::LanePhi) {
        std::vector<const Source*> out;
        for (const std::optional<Source>& arg : options)
            if (arg)
                for (const Source* leaf : arg->leaves()) out.push_back(leaf);
        return out;
    }
    return {this};
}

bool operator==(const Source& a, const Source& b) {
    return a.kind == b.kind && a.load == b.load && a.first == b.first && a.address == b.address &&
           a.bits == b.bits && a.words == b.words && a.constants == b.constants && a.block == b.block &&
           a.key == b.key && a.args == b.args && a.options == b.options;
}

Resources track(const Program& program, const std::vector<RootParam>& layout, HwStage stage,
                const Structured* lanes, std::span<const Site> ring_address_loads) {
    const Tracer tracer(program, layout, stage, lanes);
    Resources out;
    for (const auto& entry : program.inputs) {
        const auto found = tracer.param_of(entry.first);
        if (found) out.params.emplace_back(entry.second, found->first, found->second);
    }
    std::map<std::size_t, std::pair<std::vector<std::size_t>, std::vector<Value>>> first_lane_at;
    std::map<std::size_t, std::tuple<std::vector<std::size_t>, std::vector<Value>, std::vector<Site>>> offset_at;
    for (std::size_t b = 0; b < program.blocks.size(); ++b) {
        const Block& block = program.blocks[b];
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            const Inst& inst = block.insts[i];
            const Site site{b, i};
            const std::string_view name = inst.name();
            std::vector<Value> first_lane;
            std::vector<std::pair<Value, Site>> offset;
            const auto descriptor = [&](const Operand& op, Role role) {
                const Source source = tracer.descriptor(inst, op, b);
                const auto words = tracer.first_lane_words(inst, op);
                if (words) first_lane.insert(first_lane.end(), words->begin(), words->end());
                if (source.kind == Source::Kind::Loaded) {
                    const auto entry = std::find_if(out.scalar.begin(), out.scalar.end(),
                                                    [&](const std::pair<Site, Address>& s) {
                                                        return s.first == source.load;
                                                    });
                    if (entry != out.scalar.end() && entry->second.dynamic) {
                        const Term t = tracer.origin(*entry->second.dynamic);
                        if (t.kind == Term::Kind::Value && tracer.first_lane_source(t.value))
                            offset.emplace_back(t.value, source.load);
                    }
                }
                out.descriptors.push_back(Descriptor{site, role, source});
            };
            try {
                if (const auto* image = std::get_if<ImageFields>(&inst.fields)) {
                    descriptor(image->rsrc, Role::Resource);
                    if (image->sampler) descriptor(*image->sampler, Role::Sampler);
                } else if (const auto* buffer = std::get_if<BufferFields>(&inst.fields)) {
                    descriptor(buffer->rsrc, Role::Resource);
                } else if (const auto* smem = std::get_if<SmemFields>(&inst.fields)) {
                    if (name.starts_with("s_buffer_")) {
                        descriptor(smem->base, Role::Resource);
                    } else if (name.find("_load_") != std::string_view::npos ||
                               name.find("_store_") != std::string_view::npos) {
                        if (stage == HwStage::Gs && name == "s_load_dword" &&
                            std::binary_search(ring_address_loads.begin(), ring_address_loads.end(), site))
                            continue;
                        out.scalar.emplace_back(
                            site, tracer.smem_address(inst, smem->base, smem->offset, smem->soffset));
                    }
                } else if (const auto* flat = std::get_if<FlatFields>(&inst.fields)) {
                    if (name.starts_with("global_"))
                        out.scalar.emplace_back(site, tracer.flat_address(inst, flat->addr ? &*flat->addr : nullptr,
                                                                          flat->saddr ? &*flat->saddr : nullptr,
                                                                          flat->offset));
                }
            } catch (const TraceError& error) {
                detail::fail_unsupported(inst.machine.offset, error.what);
            }
            if (!first_lane.empty()) {
                auto& entry = first_lane_at[b];
                entry.first.push_back(i);
                entry.second.insert(entry.second.end(), first_lane.begin(), first_lane.end());
            }
            if (!offset.empty()) {
                auto& entry = offset_at[b];
                std::get<0>(entry).push_back(i);
                for (const auto& pair : offset) {
                    std::get<1>(entry).push_back(pair.first);
                    std::get<2>(entry).push_back(pair.second);
                }
            }
        }
    }
    if (lanes == nullptr) return out;
    for (auto& entry : first_lane_at) {
        const std::size_t block = entry.first;
        std::vector<std::size_t>& accesses = entry.second.first;
        std::vector<Value>& reads = entry.second.second;
        std::sort(reads.begin(), reads.end());
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
        try {
            const Waterfall proof = tracer.waterfall(block, accesses, reads);
            out.waterfall.compares.insert(out.waterfall.compares.end(), proof.compares.begin(),
                                          proof.compares.end());
            out.waterfall.reads.insert(out.waterfall.reads.end(), proof.reads.begin(), proof.reads.end());
        } catch (const TraceError& error) {
            detail::fail_unsupported(tracer.inst(Site{block, accesses[0]}).machine.offset, error.what);
        }
    }
    for (auto& entry : offset_at) {
        const std::size_t block = entry.first;
        std::vector<std::size_t>& accesses = std::get<0>(entry.second);
        std::vector<Value>& reads = std::get<1>(entry.second);
        const std::vector<Site>& loads = std::get<2>(entry.second);
        std::sort(reads.begin(), reads.end());
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
        try {
            const Waterfall proof = tracer.waterfall(block, accesses, reads);
            out.waterfall.compares.insert(out.waterfall.compares.end(), proof.compares.begin(),
                                          proof.compares.end());
            out.waterfall.reads.insert(out.waterfall.reads.end(), proof.reads.begin(), proof.reads.end());
            out.waterfall.loads.insert(out.waterfall.loads.end(), loads.begin(), loads.end());
        } catch (const TraceError&) {
        }
    }
    return out;
}

}  // namespace g6shim::shader
