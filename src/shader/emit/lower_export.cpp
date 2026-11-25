// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "lower_internal.hpp"

namespace g6shim::shader::emit {

namespace {

bool usage_is_other_zero(const Usage& usage) {
    return usage.kind == Usage::Kind::Other && usage.raw == 0;
}

std::optional<std::uint8_t> packed_input_partner(const PsInput& input, const std::vector<PsInput>& layout,
                                                 const std::vector<SigElement>& signature) {
    if (!input.pack2) return std::nullopt;
    if (input.row2 != input.row) return input.row2;
    if (input.row == UINT8_MAX) return std::nullopt;
    const auto available = [&](std::uint8_t row) {
        const bool reserved = std::any_of(layout.begin(), layout.end(), [row](const PsInput& entry) {
            return entry.row == row || (entry.pack2 && entry.row2 != entry.row && entry.row2 == row);
        });
        if (reserved) return false;

        bool has_packed_row = false;
        for (const SigElement& element : signature) {
            if (element.row != row) continue;
            if (!element.half || element.kind != 0 || element.comp != 9) return false;
            has_packed_row = true;
        }
        return has_packed_row;
    };
    const auto adjacent = static_cast<std::uint8_t>(input.row + 1);
    if (available(adjacent)) return adjacent;

    std::uint32_t end_row = UINT8_MAX + 1u;
    for (const PsInput& entry : layout) {
        if (entry.fp16 && entry.pack2 && entry.row2 == entry.row && entry.row > input.row)
            end_row = std::min(end_row, std::uint32_t{entry.row});
    }
    std::optional<std::uint8_t> partner;
    for (const SigElement& element : signature) {
        if (!element.row || *element.row <= adjacent || *element.row >= end_row) continue;
        const auto row = static_cast<std::uint8_t>(*element.row);
        if (row == partner || !available(row)) continue;
        if (partner) return std::nullopt;
        partner = row;
    }
    return partner;
}

}  // namespace

void Lower::interp(const IrInst& inst, Site site) {
    const auto found = interps_.find(site);
    if (found == interps_.end()) fail("the first half of an interpolation");
    const Interp& interp_value = *found->second;
    Attribute attribute;
    std::uint8_t mode = 0;
    if (interp_value.kind == Interp::Kind::Interpolated) {
        attribute = interp_value.attribute;
        switch (interp_value.bary) {
            case Barycentric::PerspectiveCenter: mode = 2; break;
            case Barycentric::PerspectiveCentroid: mode = 3; break;
            case Barycentric::PerspectiveSample: mode = 6; break;
            case Barycentric::LinearCenter: mode = 4; break;
            case Barycentric::LinearCentroid: mode = 5; break;
            case Barycentric::LinearSample: mode = 7; break;
            case Barycentric::PerspectivePullModel: fail("pull-model interpolation");
        }
    } else if (interp_value.vertex == 2) {
        attribute = interp_value.attribute;
        mode = 1;
    } else {
        fail("a non-provoking vertex value");
    }
    Operand dst;
    if (const auto* vintrp = std::get_if<VintrpFields>(&inst.fields)) {
        dst = vintrp->dst;
    } else if (const auto* valu = std::get_if<Valu>(&inst.fields)) {
        if (!valu->dst) fail("an interpolation");
        dst = *valu->dst;
    } else {
        std::abort();
    }
    const auto* target = std::get_if<VectorOperand>(&dst);
    if (target == nullptr) fail("an interpolation");
    const std::uint8_t reg = target->reg;
    const auto channel = [&](std::uint8_t row, std::uint8_t ch) -> V {
        const auto input = find_input(row, ch);
        if (!input) fail("an input the signature lacks");
        SigElement& element = s_.inputs[input->first];
        if (element.interp != mode && element.usage != 0)
            fail("one input interpolated two ways");
        element.interp = mode;
        const Val value = load_any_input(input->first, input->second);
        if (value.kind != Val::Kind::I32) fail("an input");
        return value.value;
    };
    std::optional<std::pair<bool, std::optional<std::uint8_t>>> packing;
    if (const auto* ps = std::get_if<PsIo>(&x_->native->io)) {
        for (const PsInput& input : ps->inputs) {
            if (input.row != attribute.row || !input.fp16) continue;
            packing = std::pair<bool, std::optional<std::uint8_t>>{
                input.pack2, packed_input_partner(input, ps->inputs, s_.inputs)};
            break;
        }
    }
    const bool f16 = inst.name().ends_with("_f16");
    V x;
    if (!packing) {
        if (f16 || attribute.high) fail("a 16-bit read of a 32-bit input");
        x = channel(attribute.row, attribute.channel);
    } else {
        const bool pack2 = packing->first;
        const std::optional<std::uint8_t> partner = packing->second;
        const auto half = [&](std::uint8_t h) -> std::optional<V> {
            std::optional<std::pair<std::uint8_t, std::uint8_t>> src;
            if (pack2) {
                src = packed_half(s_.inputs, attribute.row, partner, h);
            } else if (h % 2 == 0) {
                src = std::pair<std::uint8_t, std::uint8_t>{
                    attribute.row, static_cast<std::uint8_t>(h / 2)};
            }
            if (!src) return std::nullopt;
            const V value = channel(src->first, src->second);
            const V f = as_float(value);
            const Ty i32t = s_.t.i32;
            return s_.op(cur_, "legacyF32ToF16", Attr::ReadNone, i32t, 130, {f});
        };
        const std::uint8_t h = static_cast<std::uint8_t>(2 * attribute.channel);
        const std::size_t c = cur_;
        if (f16) {
            const std::optional<V> lo =
                half(static_cast<std::uint8_t>(h + (attribute.high ? 1 : 0)));
            if (!lo) fail("a packed input half no element holds");
            x = low_half(inst, reg, *lo);
        } else if (mode == 1) {
            const V z = s_.m.i32(0);
            const std::optional<V> lo = half(h);
            const std::optional<V> hi = half(static_cast<std::uint8_t>(h + 1));
            const V k16 = s_.m.i32(16);
            const V hi_up = s_.m.bin(c, Bin::Shl, hi.value_or(z), k16);
            x = s_.m.bin(c, Bin::Or, lo.value_or(z), hi_up);
        } else {
            fail("32-bit interpolation of a 16-bit input");
        }
    }
    write(inst, Reg::v(reg), x, std::nullopt);
}

void Lower::export_(std::size_t b, std::size_t i) {
    const auto found = exports_.find(Site{b, i});
    if (found == exports_.end()) fail("an export I/O raising did not see");
    const Export& e = *found->second;
    if (e.compressed) fail("a compressed export");
    const std::optional<V> guard_value = guard(b, i);
    const bool mesh = s_.mesh.has_value();
    const std::size_t c = cur_;
    if (guard_value && !mesh) {
        if (!(e.done && s_.pixel)) fail("a conditional export");
        const V t = s_.m.boolean(true);
        const V kill = s_.m.bin(c, Bin::Xor, *guard_value, t);
        s_.op(c, "discard", Attr::NoUnwind, s_.t.void_type, 82, {kill});
    }
    if (mesh && e.target.kind == Target::Kind::Primitive) {
        emit_indices(e, guard_value);
        return;
    }
    const auto param_row = [&](const Usage& usage, std::uint8_t row) -> std::optional<std::uint8_t> {
        if (usage_is_other_zero(usage) && mesh) return unnamed_row();
        return row;
    };
    struct Store {
        std::size_t id;
        std::uint8_t col;
        Value value;
        std::optional<bool> high;
    };
    std::vector<Store> stores;
    const bool primitive = e.target.kind == Target::Kind::PrimitiveParam;
    std::vector<SigElement>& outputs = primitive ? s_.prims : s_.outputs;
    const auto by_kind = [&](std::uint8_t kind) -> std::optional<std::size_t> {
        for (std::size_t id = 0; id < outputs.size(); ++id)
            if (outputs[id].kind == kind) return id;
        return std::nullopt;
    };
    std::optional<std::pair<std::uint8_t, std::pair<bool, std::optional<std::uint8_t>>>> halves;
    if (e.target.kind == Target::Kind::Param) {
        const std::optional<std::uint8_t> row = param_row(e.target.usage, e.target.row);
        if (row) {
            const auto half_entry = half_outputs_.find(*row);
            if (half_entry != half_outputs_.end())
                halves = std::pair<std::uint8_t,
                                   std::pair<bool, std::optional<std::uint8_t>>>{
                    *row, half_entry->second};
        }
    }
    if (halves) {
        const std::uint8_t row = halves->first;
        const bool pack2 = halves->second.first;
        const std::optional<std::uint8_t> partner = halves->second.second;
        for (std::uint8_t ch = 0; ch < 4; ++ch) {
            if (!e.values[ch]) continue;
            const Value value = *e.values[ch];
            for (bool high : {false, true}) {
                const std::uint8_t h = static_cast<std::uint8_t>(2 * ch + (high ? 1 : 0));
                std::optional<std::pair<std::uint8_t, std::uint8_t>> src;
                if (pack2) {
                    src = packed_half(outputs, row, partner, h);
                } else if (!high) {
                    src = std::pair<std::uint8_t, std::uint8_t>{row, ch};
                }
                std::optional<std::size_t> id;
                if (src) {
                    for (std::size_t n = 0; n < outputs.size(); ++n) {
                        const SigElement& element = outputs[n];
                        if (!element.row || *element.row != src->first) continue;
                        if (src->second >= element.col &&
                            src->second < element.col + element.cols) {
                            id = n;
                            break;
                        }
                    }
                }
                if (id) {
                    const std::uint8_t col =
                        static_cast<std::uint8_t>(src->second - outputs[*id].col);
                    stores.push_back(Store{*id, col, value, high});
                }
            }
        }
    }
    for (std::uint8_t ch = 0; ch < 4; ++ch) {
        if (!e.values[ch]) continue;
        const Value value = *e.values[ch];
        if (halves) break;
        std::optional<std::pair<std::size_t, std::uint8_t>> entry;
        const Target& target = e.target;
        if (target.kind == Target::Kind::Color) {
            for (std::size_t id = 0; id < outputs.size(); ++id) {
                if (outputs[id].kind == 16 && outputs[id].row &&
                    *outputs[id].row == target.index) {
                    entry = std::pair<std::size_t, std::uint8_t>{id, ch};
                    break;
                }
            }
        } else if (target.kind == Target::Kind::Position && target.index == 0) {
            const std::optional<std::size_t> id = by_kind(3);
            if (id) entry = std::pair<std::size_t, std::uint8_t>{*id, ch};
        } else if (target.kind == Target::Kind::Depth) {
            std::optional<std::size_t> id;
            if (ch == 0) {
                id = by_kind(17);
                if (!id) id = by_kind(18);
                if (!id) id = by_kind(19);
            } else if (ch == 1) {
                id = by_kind(20);
            } else if (ch == 2) {
                id = by_kind(14);
            }
            if (id) entry = std::pair<std::size_t, std::uint8_t>{*id, std::uint8_t{0}};
        } else if (target.kind == Target::Kind::Param ||
                   target.kind == Target::Kind::PrimitiveParam) {
            const std::optional<std::uint8_t> row = param_row(target.usage, target.row);
            for (std::size_t id = 0; id < outputs.size(); ++id) {
                const SigElement& element = outputs[id];
                if (!row || !element.row || *element.row != *row) continue;
                if (ch >= element.col && ch < element.col + element.cols) {
                    entry = std::pair<std::size_t, std::uint8_t>{
                        id, static_cast<std::uint8_t>(ch - element.col)};
                    break;
                }
            }
        } else if (target.kind == Target::Kind::Null || target.kind == Target::Kind::Unread) {
        } else if (target.kind == Target::Kind::Position && target.index >= 1 &&
                   target.index <= 2) {
            for (const SigElement& element : outputs) {
                if (element.kind == 4 || element.kind == 5)
                    fail("render-target or viewport index exports");
            }
            std::vector<std::size_t> dists;
            for (std::size_t id = 0; id < outputs.size(); ++id)
                if (outputs[id].kind == 6 || outputs[id].kind == 7) dists.push_back(id);
            std::stable_sort(dists.begin(), dists.end(), [&](std::size_t a, std::size_t b) {
                if (outputs[a].kind != outputs[b].kind) return outputs[a].kind < outputs[b].kind;
                return outputs[a].index < outputs[b].index;
            });
            std::vector<std::pair<std::size_t, std::uint8_t>> flat;
            for (std::size_t id : dists)
                for (std::uint8_t channel = 0; channel < outputs[id].cols; ++channel)
                    flat.emplace_back(id, channel);
            const std::size_t k = static_cast<std::size_t>(target.index - 1) * 4 + ch;
            if (k < flat.size()) entry = flat[k];
        } else if (target.kind == Target::Kind::Position) {
            fail("clip, cull or point-size exports");
        } else if (target.kind == Target::Kind::Primitive) {
            fail("a primitive export");
        }
        if (entry) stores.push_back(Store{entry->first, entry->second, value, std::nullopt});
    }
    auto elements = [&]() -> std::vector<SigElement>& { return primitive ? s_.prims : s_.outputs; };
    for (const Store& store : stores) {
        if (!store.high && elements()[store.id].half)
            fail("a 16-bit output the stage data does not mark");
    }
    const auto at = mesh ? begin_guarded(guard_value)
                         : std::optional<std::pair<std::size_t, std::size_t>>{};
    std::string name = "storeOutput";
    std::uint32_t opcode = 5;
    if (mesh && !primitive) {
        name = "storeVertexOutput";
        opcode = 171;
    } else if (mesh && primitive) {
        name = "storePrimitiveOutput";
        opcode = 172;
    }
    for (const Store& store : stores) {
        V x = i32_of(store.value);
        if (store.high) {
            const std::size_t block = cur_;
            if (*store.high) {
                const V k16 = s_.m.i32(16);
                x = s_.m.bin(block, Bin::LShr, x, k16);
            }
            const V f = s_.op(block, "legacyF16ToF32", Attr::ReadNone, s_.t.f32, 131, {x});
            x = as_int(f);
        }
        SigElement& element = elements()[store.id];
        element.usage = static_cast<std::uint8_t>(element.usage | (1u << store.col));
        std::string suffix = ".i32";
        V argument = x;
        if (element.comp == 9 && store.high) {
            suffix = ".f32";
            argument = as_float(x);
        } else if (element.comp == 9) {
            suffix = ".f32";
            const auto found_float = floats_.find(store.value);
            argument = found_float != floats_.end() ? found_float->second : as_float(x);
        }
        std::vector<V> args = {s_.m.i32(static_cast<std::uint32_t>(store.id)), s_.m.i32(0),
                               s_.m.i8(store.col), argument};
        if (mesh) args.push_back(mesh_thread());
        s_.op(cur_, name + suffix, Attr::NoUnwind, s_.t.void_type, opcode, args);
    }
    end_guarded(at, {});
}

void Lower::emit_indices(const Export& e, std::optional<V> guard_value) {
    if (!e.values[0]) fail("a primitive export without its indices");
    const V x = i32_of(*e.values[0]);
    const auto at = begin_guarded(guard_value);
    const std::size_t c = cur_;
    const V z = s_.m.i32(0);
    const V culled = s_.m.cmp(c, pred::kSlt, x, z);
    std::vector<V> args = {mesh_thread()};
    for (std::uint32_t k = 0; k < 3; ++k) {
        const V shift = s_.m.i32(10 * k);
        const V mask = s_.m.i32(0x3FF);
        V i = s_.m.bin(c, Bin::LShr, x, shift);
        i = s_.m.bin(c, Bin::And, i, mask);
        args.push_back(s_.m.select(c, culled, z, i));
    }
    s_.op(c, "emitIndices", Attr::NoUnwind, s_.t.void_type, 169, args);
    end_guarded(at, {});
}

std::optional<std::uint8_t> Lower::unnamed_row() const {
    const auto* ms = std::get_if<MeshIo>(&x_->native->io);
    if (ms == nullptr) return std::nullopt;
    const auto named = [&](std::uint32_t row) {
        for (const VsOutput& output : ms->outputs) {
            if ((output.param & 0x80) != 0) continue;
            if (usage_is_other_zero(output.usage)) continue;
            if (output.row == row) return true;
        }
        return false;
    };
    std::vector<std::uint32_t> free_rows;
    for (const SigElement& element : s_.outputs) {
        if (element.kind != 0 || !element.row) continue;
        if (named(*element.row)) continue;
        free_rows.push_back(*element.row);
    }
    std::sort(free_rows.begin(), free_rows.end());
    free_rows.erase(std::unique(free_rows.begin(), free_rows.end()), free_rows.end());
    if (free_rows.size() != 1 || free_rows[0] > 255) return std::nullopt;
    return static_cast<std::uint8_t>(free_rows[0]);
}

}  // namespace g6shim::shader::emit
