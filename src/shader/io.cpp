// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/io.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "g6shim/shader/error.hpp"
#include "g6shim/shader/gfx10_fields.hpp"

namespace g6shim::shader {

namespace {

struct IoError {
    const char* what;
};

struct BarycentricInput {
    std::uint8_t count = 0;
    std::optional<Barycentric> kind;
};

constexpr std::array<BarycentricInput, 16> kPsVgprs{{
    {2, Barycentric::PerspectiveSample},
    {2, Barycentric::PerspectiveCenter},
    {2, Barycentric::PerspectiveCentroid},
    {3, Barycentric::PerspectivePullModel},
    {2, Barycentric::LinearSample},
    {2, Barycentric::LinearCenter},
    {2, Barycentric::LinearCentroid},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
    {1, std::nullopt},
}};

Input ps_system(std::size_t bit) {
    if (bit == 7) return Input::simple(Input::Kind::LineStipple);
    if (bit >= 8 && bit <= 11) return Input::of_position(static_cast<std::uint8_t>(bit - 8));
    if (bit == 12) return Input::simple(Input::Kind::FrontFace);
    if (bit == 13) return Input::simple(Input::Kind::Ancillary);
    if (bit == 14) return Input::simple(Input::Kind::SampleCoverage);
    return Input::simple(Input::Kind::FixedPosition);
}

std::vector<std::pair<std::uint8_t, Input>> ps_vgprs(std::uint32_t addr) {
    std::vector<std::pair<std::uint8_t, Input>> out;
    std::uint8_t next = 0;
    for (std::size_t bit = 0; bit < kPsVgprs.size(); ++bit) {
        if (((addr >> bit) & 1) == 0) continue;
        const BarycentricInput& entry = kPsVgprs[bit];
        for (std::uint8_t index = 0; index < entry.count; ++index) {
            Input input = entry.kind ? Input::of_barycentric(*entry.kind, index) : ps_system(bit);
            out.emplace_back(static_cast<std::uint8_t>(next + index), input);
        }
        next = static_cast<std::uint8_t>(next + entry.count);
    }
    return out;
}

struct Pair {
    Barycentric bary = Barycentric::PerspectiveSample;
    std::vector<Site> first;

    Interp with(const Attribute& attribute) const { return Interp::interpolated(attribute, bary, first); }
};

class Halves {
public:
    Halves(const Program& program, const std::unordered_map<Value, Site, ValueHash>& defs,
           const std::unordered_map<Value, Site, ValueHash>& phis)
        : p_(&program), defs_(&defs), phis_(&phis) {}

    Pair pair(const Operand& attr, std::optional<Value> acc, std::optional<Value> j,
              const std::function<std::optional<std::pair<Barycentric, std::uint8_t>>(std::optional<Value>)>&
                  barycentric) const {
        if (!acc) throw IoError{"an interpolation without its first half"};
        std::vector<Value> seen;
        std::vector<Site> first;
        sites(*acc, seen, first);
        const auto known = barycentric(j);
        if (!known || known->second != 1) throw IoError{"an interpolation with computed barycentrics"};
        const Barycentric kind = known->first;
        for (const Site& site : first) {
            const Inst& inst = p_->blocks[site.block].insts[site.index];
            const Operand* operand = nullptr;
            std::optional<Value> read;
            if (const auto* intrp = std::get_if<VintrpFields>(&inst.fields)) {
                if (inst.name() == "v_interp_p1_f32") {
                    if (const auto* vec = std::get_if<VectorOperand>(&intrp->src)) {
                        operand = &intrp->attr;
                        read = inst.read(Reg::v(vec->reg));
                    }
                }
            } else if (const auto* val = std::get_if<Valu>(&inst.fields)) {
                if (inst.name() == "v_interp_p1ll_f16" && val->src.size() > 1) {
                    if (const auto* vec = std::get_if<VectorOperand>(&val->src[1])) {
                        operand = &val->src[0];
                        read = inst.read(Reg::v(vec->reg));
                    }
                }
            }
            if (operand == nullptr) throw IoError{"an interpolation without its first half"};
            if (!(*operand == attr)) throw IoError{"interpolation halves of different attributes"};
            const auto index = barycentric(read);
            if (!index || index->first != kind || index->second != 0)
                throw IoError{"an interpolation with computed barycentrics"};
        }
        return Pair{kind, std::move(first)};
    }

private:
    void sites(Value value, std::vector<Value>& seen, std::vector<Site>& out) const {
        if (const auto it = defs_->find(value); it != defs_->end()) {
            if (std::find(out.begin(), out.end(), it->second) == out.end()) out.push_back(it->second);
            return;
        }
        const auto phi = phis_->find(value);
        if (phi == phis_->end()) throw IoError{"an interpolation without its first half"};
        if (std::find(seen.begin(), seen.end(), value) != seen.end()) return;
        seen.push_back(value);
        for (Value arg : p_->blocks[phi->second.block].phis[phi->second.index].args) sites(arg, seen, out);
    }

    const Program* p_;
    const std::unordered_map<Value, Site, ValueHash>* defs_;
    const std::unordered_map<Value, Site, ValueHash>* phis_;
};

const std::vector<VsOutput>& no_params() {
    static const std::vector<VsOutput> empty;
    return empty;
}

constexpr std::uint8_t kWord0 = 4;
constexpr std::uint8_t kWord1 = 5;
constexpr std::uint8_t kDwordSelect = 6;
constexpr std::uint32_t kMaxRingDwords = 128;
constexpr std::int32_t kPackedVertexMask = 0xffff;
constexpr std::int32_t kDwordByteShift = 2;

struct Disp {
    std::optional<std::uint8_t> vertex;
    std::int32_t bytes = 0;
    bool known = false;
};

std::optional<std::int32_t> immediate(const Operand& op) {
    if (const auto* constant = std::get_if<ConstantOperand>(&op)) {
        const std::optional<std::uint32_t> value = inline_constant(constant->code);
        if (!value) return std::nullopt;
        return static_cast<std::int32_t>(*value);
    }
    if (const auto* literal = std::get_if<LiteralOperand>(&op))
        return static_cast<std::int32_t>(literal->value);
    return std::nullopt;
}

std::optional<Value> vector_value(const Inst& inst, const Operand& op) {
    const auto* vector = std::get_if<VectorOperand>(&op);
    if (vector == nullptr) return std::nullopt;
    return inst.read(Reg::v(vector->reg));
}

bool ring_address_arithmetic(const Inst& inst) {
    const std::string_view name = inst.name();
    if (name != "s_lshl_b32" && name != "s_add_i32" && name != "s_add_u32" && name != "s_mov_b32" &&
        name != "v_mad_u32_u24" && name != "v_add_nc_u32" && name != "v_mov_b32")
        return false;
    return std::none_of(inst.writes.begin(), inst.writes.end(), [](const auto& entry) {
        return entry.first.kind == Reg::Kind::S && entry.first.code >= kVcc;
    });
}

void find_ring_address_loads(const Program& program, const std::unordered_map<Value, Site, ValueHash>& defs,
                             const std::unordered_map<Value, Site, ValueHash>& phis,
                             const std::unordered_map<Site, Value, SiteHash>& ring_addresses, Io& io) {
    if (ring_addresses.empty()) return;
    const auto at = [&](Site site) -> const Inst& { return program.blocks[site.block].insts[site.index]; };
    const auto dependencies = [&](std::vector<Value> pending) {
        std::unordered_set<Value, ValueHash> seen;
        while (!pending.empty()) {
            const Value value = pending.back();
            pending.pop_back();
            if (!seen.insert(value).second) continue;
            const auto phi = phis.find(value);
            if (phi != phis.end()) {
                const auto& args = program.blocks[phi->second.block].phis[phi->second.index].args;
                pending.insert(pending.end(), args.begin(), args.end());
            }
            const auto def = defs.find(value);
            if (def == defs.end()) continue;
            const Inst& inst = at(def->second);
            if (!ring_address_arithmetic(inst)) continue;
            for (const auto& read : inst.reads) pending.push_back(read.second);
        }
        return seen;
    };

    std::vector<Value> observed;
    for (std::size_t b = 0; b < program.blocks.size(); ++b) {
        const Block& block = program.blocks[b];
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            const Inst& inst = block.insts[i];
            if (ring_address_arithmetic(inst)) continue;
            const auto address = ring_addresses.find(Site{b, i});
            for (const auto& read : inst.reads)
                if (address == ring_addresses.end() || read.second != address->second)
                    observed.push_back(read.second);
        }
    }
    std::vector<Value> addresses;
    for (const auto& entry : ring_addresses) addresses.push_back(entry.second);
    const auto live = dependencies(std::move(observed));
    for (Value value : dependencies(std::move(addresses))) {
        if (live.contains(value)) continue;
        const auto def = defs.find(value);
        if (def == defs.end()) continue;
        const Inst& inst = at(def->second);
        if (inst.name() == "s_load_dword" && inst.writes.size() == 1)
            io.ring_address_loads.push_back(def->second);
    }
    std::sort(io.ring_address_loads.begin(), io.ring_address_loads.end());
}

void raise_rings(const Program& program, const std::unordered_map<Value, Site, ValueHash>& defs,
                 const std::unordered_map<Value, Site, ValueHash>& phis, Io& io) {
    std::unordered_map<Value, std::uint8_t, ValueHash> packs;
    for (const auto& entry : program.inputs) {
        if (entry.first == Reg::v(0)) packs.emplace(entry.second, std::uint8_t{0});
        if (entry.first == Reg::v(1)) packs.emplace(entry.second, std::uint8_t{1});
    }
    std::unordered_map<Value, std::uint8_t, ValueHash> bases;
    const auto at = [&](Site site) -> const Inst& { return program.blocks[site.block].insts[site.index]; };
    const auto unmodified = [](const Valu& op) {
        return !op.sdwa && !op.dpp && op.abs == 0 && op.neg == 0 && !op.clamp && op.omod == 0;
    };
    const auto scalar_immediate = [&](const Inst& inst, const Operand& operand) -> std::optional<std::int32_t> {
        if (const auto direct = immediate(operand)) return direct;
        const auto* reg = std::get_if<ScalarOperand>(&operand);
        const auto value = reg ? inst.read(Reg::s(reg->code)) : std::nullopt;
        if (!value) return std::nullopt;
        const auto def = defs.find(*value);
        if (def == defs.end()) return std::nullopt;
        const Inst& move = at(def->second);
        const auto* sop = std::get_if<SopFields>(&move.fields);
        if (move.name() != "s_mov_b32" || sop == nullptr || sop->src.size() != 1) return std::nullopt;
        return immediate(sop->src.front());
    };
    const auto packed_low = [&](Value value, std::int32_t mask) -> std::optional<Value> {
        if (mask == kPackedVertexMask) return value;
        if (mask != kPackedVertexMask << kDwordByteShift) return std::nullopt;
        const auto def = defs.find(value);
        if (def == defs.end()) return std::nullopt;
        const Inst& shift = at(def->second);
        const auto* op = std::get_if<Valu>(&shift.fields);
        if (shift.name() != "v_lshlrev_b32" || op == nullptr || op->src.size() != 2 || !unmodified(*op) ||
            immediate(op->src[0]) != kDwordByteShift)
            return std::nullopt;
        return vector_value(shift, op->src[1]);
    };
    for (const Block& block : program.blocks) {
        for (const Inst& inst : block.insts) {
            const auto* valu = std::get_if<Valu>(&inst.fields);
            if (valu == nullptr || !valu->dst || valu->src.empty()) continue;
            const auto* dst = std::get_if<VectorOperand>(&*valu->dst);
            if (dst == nullptr) continue;
            const std::optional<Value> written = inst.write(Reg::v(dst->reg));
            if (!written) continue;
            if (valu->sdwa && valu->sdwa->dst_sel == kDwordSelect && valu->sdwa->sext == 0 &&
                valu->abs == 0 && valu->neg == 0 && !valu->clamp && valu->omod == 0) {
                const bool packed_shift = inst.name() == "v_lshlrev_b32" && valu->src.size() == 2 &&
                                          valu->sdwa->src_sel[0] == kDwordSelect &&
                                          scalar_immediate(inst, valu->src[0]) == kDwordByteShift;
                const std::size_t packed_operand = packed_shift ? 1 : 0;
                const std::optional<Value> src = vector_value(inst, valu->src[packed_operand]);
                const std::uint8_t sel = valu->sdwa->src_sel[packed_operand];
                if (src && (sel == kWord0 || sel == kWord1)) {
                    const auto pack = packs.find(*src);
                    if (pack != packs.end() && (inst.name() == "v_mov_b32" || packed_shift))
                        bases.emplace(*written, static_cast<std::uint8_t>(pack->second * 2 + (sel == kWord1 ? 1 : 0)));
                }
            }
            if (inst.name() == "v_and_b32" && valu->src.size() == 2 && unmodified(*valu)) {
                const std::size_t mask_operand = immediate(valu->src[0]) ? 0 : 1;
                const auto mask = immediate(valu->src[mask_operand]);
                const auto src = vector_value(inst, valu->src[1 - mask_operand]);
                const auto low = src && mask ? packed_low(*src, *mask) : std::nullopt;
                const auto pack = low ? packs.find(*low) : packs.end();
                if (pack != packs.end()) bases.emplace(*written, static_cast<std::uint8_t>(pack->second * 2));
            }
            if (inst.name() == "v_lshrrev_b32" && valu->src.size() > 1) {
                const std::optional<std::int32_t> shift = immediate(valu->src[0]);
                const std::optional<Value> src = vector_value(inst, valu->src[1]);
                if (shift && *shift == 16 && src) {
                    const auto pack = packs.find(*src);
                    if (pack != packs.end())
                        bases.emplace(*written, static_cast<std::uint8_t>(pack->second * 2 + 1));
                }
            }
        }
    }
    const std::function<Disp(Value, int)> displacement = [&](Value value, int depth) -> Disp {
        if (depth > 8) return {};
        const auto base = bases.find(value);
        if (base != bases.end()) return Disp{base->second, 0, true};
        const auto def = defs.find(value);
        if (def == defs.end()) return {};
        const Inst& inst = at(def->second);
        const auto* valu = std::get_if<Valu>(&inst.fields);
        if (valu == nullptr) return Disp{std::nullopt, 0, true};
        const std::string_view name = inst.name();
        if (name == "v_mov_b32" && !valu->sdwa && valu->src.size() == 1) {
            const std::optional<Value> src = vector_value(inst, valu->src[0]);
            if (!src) return Disp{std::nullopt, 0, true};
            return displacement(*src, depth + 1);
        }
        if (name == "v_add_u32" || name == "v_add_co_u32" || name == "v_add_nc_u32" || name == "v_add_i32") {
            if (valu->src.size() < 2) return {};
            const std::optional<Value> left = vector_value(inst, valu->src[0]);
            const std::optional<Value> right = vector_value(inst, valu->src[1]);
            const std::optional<std::int32_t> left_imm = immediate(valu->src[0]);
            const std::optional<std::int32_t> right_imm = immediate(valu->src[1]);
            if (left && right_imm) {
                Disp d = displacement(*left, depth + 1);
                if (d.known) d.bytes += *right_imm;
                return d;
            }
            if (right && left_imm) {
                Disp d = displacement(*right, depth + 1);
                if (d.known) d.bytes += *left_imm;
                return d;
            }
            if (left) return displacement(*left, depth + 1);
            if (right) return displacement(*right, depth + 1);
            return Disp{std::nullopt, 0, true};
        }
        if (name == "v_lshlrev_b32" && valu->src.size() > 1) {
            const std::optional<std::int32_t> shift = immediate(valu->src[0]);
            const std::optional<Value> src = vector_value(inst, valu->src[1]);
            if (shift && *shift == 2 && src) {
                Disp d = displacement(*src, depth + 1);
                if (d.known) d.bytes <<= 2;
                return d;
            }
        }
        return Disp{std::nullopt, 0, true};
    };
    struct Piece {
        std::uint32_t byte = 0;
        std::uint32_t dwords = 0;
    };
    const auto pieces = [](const Inst& inst) {
        std::vector<Piece> out;
        const auto* fields = std::get_if<DsFields>(&inst.fields);
        if (fields == nullptr) return out;
        const std::string_view name = inst.name();
        const bool pair = name.starts_with("ds_read2") || name.starts_with("ds_write2");
        std::uint32_t width = 1;
        if (name.ends_with("b64")) width = 2;
        else if (name.ends_with("b96")) width = 3;
        else if (name.ends_with("b128")) width = 4;
        else if (!name.ends_with("b32")) return out;
        if (pair) {
            std::uint32_t scale = 4 * width;
            if (name.find("st64") != std::string_view::npos) scale *= 64;
            out.push_back({static_cast<std::uint32_t>(fields->offset0) * scale, width});
            out.push_back({static_cast<std::uint32_t>(fields->offset1) * scale, width});
        } else {
            const std::uint32_t offset = static_cast<std::uint32_t>(fields->offset0) |
                                         static_cast<std::uint32_t>(fields->offset1) << 8;
            out.push_back({offset, width});
        }
        return out;
    };
    std::unordered_map<Site, Value, SiteHash> ring_addresses;
    for (std::size_t b = 0; b < program.blocks.size(); ++b) {
        const Block& block = program.blocks[b];
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            const Inst& inst = block.insts[i];
            const Site site{b, i};
            if (std::holds_alternative<ExpFields>(inst.fields))
                detail::fail_unsupported(inst.machine.offset, "an export in a geometry shader");
            if (!std::holds_alternative<DsFields>(inst.fields)) continue;
            const auto* fields = std::get_if<DsFields>(&inst.fields);
            if (fields->gds) detail::fail_unsupported(inst.machine.offset, "GDS");
            const std::string_view name = inst.name();
            const bool read = name.starts_with("ds_read");
            const bool write = name.starts_with("ds_write");
            if (!read && !write)
                detail::fail_unsupported(inst.machine.offset, "an LDS operation in a geometry shader");
            const std::vector<Piece> listed = pieces(inst);
            if (listed.empty() || !fields->addr)
                detail::fail_unsupported(inst.machine.offset, "an LDS operation in a geometry shader");
            const std::optional<Value> addr = vector_value(inst, *fields->addr);
            if (!addr) detail::fail_unsupported(inst.machine.offset, "a geometry ring address");
            const Disp disp = displacement(*addr, 0);
            if (!disp.known) detail::fail_unsupported(inst.machine.offset, "a geometry ring address");
            if (read && !disp.vertex) detail::fail_unsupported(inst.machine.offset, "a geometry ring read");
            bool address_is_data = false;
            std::uint8_t cursor = 0;
            if (read) {
                if (!fields->dst) detail::fail_unsupported(inst.machine.offset, "a geometry ring read");
                const auto* dst = std::get_if<VectorOperand>(&*fields->dst);
                if (dst == nullptr) detail::fail_unsupported(inst.machine.offset, "a geometry ring read");
                cursor = dst->reg;
            }
            for (std::size_t piece = 0; piece < listed.size(); ++piece) {
                for (std::uint32_t k = 0; k < listed[piece].dwords; ++k) {
                    const std::int64_t byte = static_cast<std::int64_t>(disp.bytes) + listed[piece].byte + 4 * k;
                    if (byte < 0 || (byte % 4) != 0)
                        detail::fail_unsupported(inst.machine.offset, "a geometry ring address");
                    const std::uint32_t dword = static_cast<std::uint32_t>(byte / 4);
                    if (dword >= kMaxRingDwords)
                        detail::fail_unsupported(inst.machine.offset, "a geometry ring slot");
                    if (read) {
                        RingLoad load;
                        load.site = site;
                        load.vgpr = cursor++;
                        load.vertex = *disp.vertex;
                        load.dword = dword;
                        io.ring_loads.push_back(load);
                    } else {
                        if (piece >= fields->data.size())
                            detail::fail_unsupported(inst.machine.offset, "a geometry ring address");
                        const auto* data = std::get_if<VectorOperand>(&fields->data[piece]);
                        if (data == nullptr)
                            detail::fail_unsupported(inst.machine.offset, "a geometry ring address");
                        RingStore store;
                        store.site = site;
                        store.vgpr = static_cast<std::uint8_t>(data->reg + k);
                        store.dword = dword;
                        address_is_data |= inst.read(Reg::v(store.vgpr)) == addr;
                        io.ring_stores.push_back(store);
                    }
                }
            }
            if (write && !address_is_data) ring_addresses.emplace(site, *addr);
        }
    }
    find_ring_address_loads(program, defs, phis, ring_addresses, io);
}

void raise_geometry_primitive_counts(const Program& program, Io& io) {
    constexpr std::uint8_t kMergedWaveInfoSgpr = 3;
    constexpr std::int32_t kPrimitiveCountExtract = (8 << 16) | 8;
    const auto input = std::find_if(program.inputs.begin(), program.inputs.end(), [](const auto& entry) {
        return entry.first == Reg::s(kMergedWaveInfoSgpr);
    });
    if (input == program.inputs.end()) return;

    std::unordered_map<Value, std::vector<const Inst*>, ValueHash> users;
    for (const Block& block : program.blocks) {
        for (const Phi& phi : block.phis)
            for (Value value : phi.args) users[value].push_back(nullptr);
        for (const Inst& inst : block.insts)
            for (const auto& read : inst.reads) users[read.second].push_back(&inst);
    }
    const auto uses = [&](Value value) -> std::span<const Inst* const> {
        const auto found = users.find(value);
        return found == users.end() ? std::span<const Inst* const>{} : std::span<const Inst* const>{found->second};
    };
    const auto scalar = [](const Inst& inst, const Operand& operand) -> std::optional<Value> {
        const auto* reg = std::get_if<ScalarOperand>(&operand);
        return reg ? inst.read(Reg::s(reg->code)) : std::nullopt;
    };
    const auto full_wave_mask_select = [&](const Inst* inst, Value count) {
        if (inst == nullptr || inst->name() != "s_cselect_b64" || !inst->write(Reg::s(kExec))) return false;
        const auto* fields = std::get_if<SopFields>(&inst->fields);
        if (fields == nullptr || fields->src.size() != 2 || immediate(fields->src[0]) != -1) return false;
        const auto* retained = std::get_if<ScalarOperand>(&fields->src[1]);
        if (retained == nullptr || retained->code != kExec || retained->dwords != 2) return false;
        const auto low = inst->read(Reg::s(kExec));
        const auto high = inst->read(Reg::s(kExec + 1));
        if (!low || !high) return false;
        return std::any_of(uses(count).begin(), uses(count).end(), [&](const Inst* mask) {
            return mask != nullptr && mask->name() == "s_bfm_b64" && mask->write(Reg::s(kExec)) == low &&
                   mask->write(Reg::s(kExec + 1)) == high;
        });
    };
    const auto dispatch_control = [&](const Inst* inst, Value count) {
        if (inst == nullptr) return false;
        const auto* fields = std::get_if<SopFields>(&inst->fields);
        if (fields == nullptr || fields->src.size() != 2) return false;
        const bool count_first = scalar(*inst, fields->src[0]) == count;
        const bool count_second = scalar(*inst, fields->src[1]) == count;
        const auto compared = count_first ? immediate(fields->src[1]) : immediate(fields->src[0]);
        const bool count_then_zero = count_first && compared == 0;
        if (inst->name() == "s_cmp_eq_u32" || inst->name() == "s_cmp_lg_u32") {
            if (!count_first && !count_second) return false;
            if (compared == 0) return true;
            if (inst->name() != "s_cmp_eq_u32" || compared != (program.wave64 ? 64 : 32)) return false;
            const auto condition = inst->write(Reg::scc());
            return condition && !uses(*condition).empty() &&
                   std::all_of(uses(*condition).begin(), uses(*condition).end(),
                               [&](const Inst* use) { return full_wave_mask_select(use, count); });
        }
        if (inst->name() != "s_bfm_b64" || !count_then_zero) return false;
        for (const auto& write : inst->writes)
            for (const Inst* mask_use : uses(write.second))
                if (mask_use == nullptr || !mask_use->write(Reg::s(kExec)) ||
                    (mask_use->name() != "s_cselect_b64" && mask_use->name() != "s_mov_b64"))
                    return false;
        return true;
    };
    for (const Inst* inst : uses(input->second)) {
        if (inst == nullptr || (inst->name() != "s_bfe_i32" && inst->name() != "s_bfe_u32")) continue;
        const auto* fields = std::get_if<SopFields>(&inst->fields);
        if (fields == nullptr || fields->src.size() != 2 || !fields->dst ||
            scalar(*inst, fields->src[0]) != input->second || immediate(fields->src[1]) != kPrimitiveCountExtract)
            continue;
        const auto* dst = std::get_if<ScalarOperand>(&*fields->dst);
        const auto count = dst && dst->code < kVcc ? inst->write(Reg::s(dst->code)) : std::nullopt;
        if (!count || uses(*count).empty()) continue;
        if (std::all_of(uses(*count).begin(), uses(*count).end(),
                        [&](const Inst* use) { return dispatch_control(use, *count); }))
            io.geometry_primitive_counts.push_back(*count);
    }
    std::sort(io.geometry_primitive_counts.begin(), io.geometry_primitive_counts.end());
}

}  // namespace

std::vector<std::pair<std::uint8_t, Input>> pixel_input_vgprs(std::uint32_t input_addr) {
    return ps_vgprs(input_addr);
}

Input Input::simple(Kind kind) {
    Input input;
    input.kind = kind;
    return input;
}

Input Input::of_attribute(std::uint8_t row, std::uint8_t channel) {
    Input input;
    input.kind = Kind::Attribute;
    input.row = row;
    input.channel = channel;
    return input;
}

Input Input::of_barycentric(Barycentric bary, std::uint8_t index) {
    Input input;
    input.kind = Kind::Barycentric;
    input.bary = bary;
    input.index = index;
    return input;
}

Input Input::of_position(std::uint8_t channel) {
    Input input;
    input.kind = Kind::Position;
    input.index = channel;
    return input;
}

Input Input::of_group_id(std::uint8_t component) {
    Input input;
    input.kind = Kind::GroupId;
    input.index = component;
    return input;
}

Input Input::of_thread_id(std::uint8_t component) {
    Input input;
    input.kind = Kind::ThreadIdInGroup;
    input.index = component;
    return input;
}

Interp Interp::interpolated(Attribute attribute, Barycentric bary, std::vector<Site> first) {
    Interp interp;
    interp.kind = Kind::Interpolated;
    interp.attribute = attribute;
    interp.bary = bary;
    interp.first = std::move(first);
    return interp;
}

Interp Interp::vertex_value(Attribute attribute, std::uint8_t vertex) {
    Interp interp;
    interp.kind = Kind::Vertex;
    interp.attribute = attribute;
    interp.vertex = vertex;
    return interp;
}

Target Target::color(std::uint8_t index) {
    Target target;
    target.kind = Kind::Color;
    target.index = index;
    return target;
}

Target Target::depth() {
    Target target;
    target.kind = Kind::Depth;
    return target;
}

Target Target::null_target() {
    Target target;
    target.kind = Kind::Null;
    return target;
}

Target Target::position(std::uint8_t channel) {
    Target target;
    target.kind = Kind::Position;
    target.index = channel;
    return target;
}

Target Target::param(Usage usage, std::uint8_t row) {
    Target target;
    target.kind = Kind::Param;
    target.usage = usage;
    target.row = row;
    return target;
}

Target Target::unread() {
    Target target;
    target.kind = Kind::Unread;
    return target;
}

Target Target::primitive() {
    Target target;
    target.kind = Kind::Primitive;
    return target;
}

Target Target::primitive_param(Usage usage, std::uint8_t row) {
    Target target;
    target.kind = Kind::PrimitiveParam;
    target.usage = usage;
    target.row = row;
    return target;
}

Io raise(const Program& program, const NativeProgram& native) {
    std::vector<std::pair<Reg, Input>> loaded;
    const std::vector<PsInput>* ps_inputs = nullptr;
    const std::vector<VsOutput>* params = &no_params();
    if (native.stage == HwStage::Ps) {
        const auto* ps = std::get_if<PsIo>(&native.io);
        if (ps == nullptr) detail::fail_unsupported(0, "I/O of this stage");
        for (const auto& entry : ps_vgprs(ps->input_addr)) loaded.emplace_back(Reg::v(entry.first), entry.second);
        ps_inputs = &ps->inputs;
    } else if (native.stage == HwStage::Vs) {
        const auto* vs = std::get_if<VsIo>(&native.io);
        if (vs == nullptr) detail::fail_unsupported(0, "I/O of this stage");
        loaded.emplace_back(Reg::v(0), Input::simple(Input::Kind::VertexId));
        loaded.emplace_back(Reg::v(2), Input::simple(Input::Kind::PrimitiveId));
        loaded.emplace_back(Reg::v(3), Input::simple(Input::Kind::InstanceId));
        for (const VsInput& input : vs->inputs)
            for (std::uint8_t channel = 0; channel < 4; ++channel)
                if (((input.mask >> channel) & 1) != 0)
                    loaded.emplace_back(Reg::v(static_cast<std::uint8_t>(input.vgpr + channel)),
                                        Input::of_attribute(input.row, channel));
        params = &vs->outputs;
    } else if (native.stage == HwStage::Cs) {
        const auto* cs = std::get_if<ComputeIo>(&native.io);
        if (cs == nullptr) detail::fail_unsupported(0, "I/O of this stage");
        const auto [ids, info] = cs->system_sgprs();
        for (std::uint8_t c = 0; c < cs->thread_id_vgprs(); ++c)
            loaded.emplace_back(Reg::v(c), Input::of_thread_id(c));
        for (std::size_t c = 0; c < ids.size(); ++c)
            if (ids[c])
                loaded.emplace_back(Reg::s(*ids[c]), Input::of_group_id(static_cast<std::uint8_t>(c)));
        if (info) loaded.emplace_back(Reg::s(*info), Input::simple(Input::Kind::GroupInfo));
    } else if (native.stage == HwStage::Ms) {
        const auto* ms = std::get_if<MeshIo>(&native.io);
        if (ms == nullptr) detail::fail_unsupported(0, "I/O of this stage");
        std::optional<Value> group;
        for (const auto& entry : program.inputs)
            if (entry.first == Reg::v(5)) group = entry.second;
        for (const Block& block : program.blocks) {
            for (const Inst& inst : block.insts) {
                if (!group) continue;
                bool reads_group = false;
                for (const auto& entry : inst.reads)
                    if (entry.first == Reg::v(5) && entry.second == *group) reads_group = true;
                if (reads_group && inst.name() != "v_readfirstlane_b32" && !inst.write(Reg::v(5)))
                    detail::fail_unsupported(inst.machine.offset, "the mesh group input read per lane");
            }
        }
        loaded.emplace_back(Reg::v(5), Input::simple(Input::Kind::MeshGroup));
        loaded.emplace_back(Reg::s(3), Input::simple(Input::Kind::MergedWaveInfo));
        params = &ms->outputs;
    } else if (native.stage == HwStage::Gs) {
        params = &no_params();
    } else {
        detail::fail_unsupported(0, "I/O of this stage");
    }

    std::vector<std::pair<Value, Input>> inputs;
    for (const auto& entry : program.inputs)
        for (const auto& candidate : loaded)
            if (candidate.first == entry.first) {
                inputs.emplace_back(entry.second, candidate.second);
                break;
            }
    const auto input_of = [&](Value value) -> std::optional<Input> {
        for (const auto& entry : inputs)
            if (entry.first == value) return entry.second;
        return std::nullopt;
    };
    const auto attribute = [&](const Inst& inst, const Operand& attr) -> Attribute {
        const auto* a = std::get_if<AttributeOperand>(&attr);
        if (a == nullptr) detail::fail_unsupported(inst.machine.offset, "an interpolation operand");
        if (ps_inputs != nullptr)
            for (const PsInput& input : *ps_inputs)
                if (input.attr == a->index) return Attribute{input.usage, input.row, a->chan, a->high};
        detail::fail_unsupported(inst.machine.offset, "an interpolated attribute the stage data does not list");
    };
    const auto barycentric = [&](std::optional<Value> value)
        -> std::optional<std::pair<Barycentric, std::uint8_t>> {
        if (!value) return std::nullopt;
        const auto input = input_of(*value);
        if (!input || input->kind != Input::Kind::Barycentric) return std::nullopt;
        return std::pair<Barycentric, std::uint8_t>{input->bary, input->index};
    };

    Io io;
    io.inputs = inputs;
    if (const auto* cs = std::get_if<ComputeIo>(&native.io)) io.initialized_sgprs = cs->initialized_sgprs();
    std::unordered_map<Value, Site, ValueHash> defs;
    std::unordered_map<Value, Site, ValueHash> phis;
    for (std::size_t b = 0; b < program.blocks.size(); ++b) {
        const Block& block = program.blocks[b];
        for (std::size_t i = 0; i < block.insts.size(); ++i)
            for (const auto& entry : block.insts[i].writes) defs[entry.second] = Site{b, i};
        for (std::size_t i = 0; i < block.phis.size(); ++i) phis[block.phis[i].value] = Site{b, i};
    }
    if (native.stage == HwStage::Gs) {
        raise_rings(program, defs, phis, io);
        raise_geometry_primitive_counts(program, io);
    }
    const Halves halves(program, defs, phis);

    for (std::size_t b = 0; b < program.blocks.size(); ++b) {
        const Block& block = program.blocks[b];
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            const Inst& inst = block.insts[i];
            const Site site{b, i};
            const std::size_t at = inst.machine.offset;
            const std::string_view name = inst.name();
            if (const auto* intrp = std::get_if<VintrpFields>(&inst.fields)) {
                const Attribute attr = attribute(inst, intrp->attr);
                if (name == "v_interp_mov_f32") {
                    const auto* c = std::get_if<ConstantOperand>(&intrp->src);
                    if (c == nullptr) detail::fail_unsupported(at, "an interpolation form");
                    io.interps.emplace_back(site, Interp::vertex_value(attr, c->code));
                } else if (name == "v_interp_p1_f32") {
                } else if (name == "v_interp_p2_f32") {
                    const auto* src = std::get_if<VectorOperand>(&intrp->src);
                    if (src == nullptr) detail::fail_unsupported(at, "an interpolation form");
                    const auto* dst = std::get_if<VectorOperand>(&intrp->dst);
                    if (dst == nullptr) detail::fail_unsupported(at, "an interpolation operand");
                    try {
                        const Pair pair = halves.pair(intrp->attr, inst.read(Reg::v(dst->reg)),
                                                      inst.read(Reg::v(src->reg)), barycentric);
                        io.interps.emplace_back(site, pair.with(attr));
                    } catch (const IoError& error) {
                        detail::fail_unsupported(at, error.what);
                    }
                } else {
                    detail::fail_unsupported(at, "an interpolation form");
                }
            } else if (const auto* val = std::get_if<Valu>(&inst.fields); val != nullptr && name.starts_with("v_interp_")) {
                if (name == "v_interp_p1ll_f16") {
                } else if (name == "v_interp_p2_f16") {
                    if (val->src.empty()) detail::fail_unsupported(at, "an interpolation form");
                    const Attribute attr = attribute(inst, val->src[0]);
                    const auto read = [&](std::size_t k) -> std::optional<Value> {
                        if (k >= val->src.size()) return std::nullopt;
                        const auto* vec = std::get_if<VectorOperand>(&val->src[k]);
                        if (vec == nullptr) return std::nullopt;
                        return inst.read(Reg::v(vec->reg));
                    };
                    try {
                        const Pair pair = halves.pair(val->src[0], read(2), read(1), barycentric);
                        io.interps.emplace_back(site, pair.with(attr));
                    } catch (const IoError& error) {
                        detail::fail_unsupported(at, error.what);
                    }
                } else {
                    detail::fail_unsupported(at, "an interpolation form");
                }
            } else if (const auto* exp = std::get_if<ExpFields>(&inst.fields)) {
                const std::uint8_t t = exp->target;
                Target target;
                if (t <= 7) {
                    target = Target::color(t);
                } else if (t == 8) {
                    target = Target::depth();
                } else if (t == 9) {
                    target = Target::null_target();
                } else if (t >= 12 && t <= 16) {
                    target = Target::position(static_cast<std::uint8_t>(t - 12));
                } else if (t == 20) {
                    target = Target::primitive();
                } else if (t == 32 && params->empty()) {
                    target = Target::unread();
                } else if (t >= 32 && t <= 63) {
                    const VsOutput* output = nullptr;
                    for (const VsOutput& candidate : *params)
                        if ((candidate.param & 0x7F) == static_cast<std::uint8_t>(t - 32)) {
                            output = &candidate;
                            break;
                        }
                    if (output == nullptr)
                        detail::fail_unsupported(at, "a parameter export the stage data does not list");
                    if ((output->param & 0x80) != 0)
                        target = Target::primitive_param(output->usage, output->row);
                    else
                        target = Target::param(output->usage, output->row);
                } else {
                    detail::fail_unsupported(at, "an export target");
                }
                std::array<std::optional<Value>, 4> values;
                for (std::size_t k = 0; k < exp->src.size(); ++k) {
                    if (!exp->src[k]) continue;
                    if (const auto* vec = std::get_if<VectorOperand>(&*exp->src[k]))
                        values[k] = inst.read(Reg::v(vec->reg));
                }
                Export export_value;
                export_value.site = site;
                export_value.target = target;
                export_value.values = values;
                export_value.compressed = exp->compr;
                export_value.done = exp->done;
                io.exports.push_back(export_value);
            }
        }
    }
    return io;
}

}  // namespace g6shim::shader
