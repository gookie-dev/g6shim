// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lower_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "g6shim/shader/error.hpp"

namespace g6shim::shader::emit {

namespace {

constexpr float kTwoPi = 6.283185307179586476925286766559f;
constexpr std::uint16_t kGsAllocReq = 9;
constexpr std::uint32_t kGeometryMaxVertices = 1024;
constexpr std::uint32_t kGeometryScalarProduct = 1024;
constexpr std::uint32_t kTrianglePrimitive = 3;
constexpr std::uint32_t kTriangleStrip = 5;
constexpr std::uint8_t kMsgGs = 2;
constexpr std::uint8_t kMsgGsDone = 3;
constexpr std::uint8_t kGsOpCut = 1;
constexpr std::uint8_t kGsOpEmit = 2;
constexpr std::uint8_t kGsOpEmitCut = 3;
constexpr std::uint32_t kEmitStream = 97;
constexpr std::uint32_t kCutStream = 98;

Why mask_data_reason(const Bools& bools, Bool bits) {
    for (const Atom& atom : bools.support(bits)) {
        if (atom.kind == Atom::Kind::Quad) return kQuadMask;
        if (atom.kind == Atom::Kind::Opaque) return kOpaqueMask;
    }
    return kLaneMaskElsewhere;
}

constexpr Numeric kNorm[] = {Numeric::Unorm, Numeric::Snorm, Numeric::Uscaled,
                             Numeric::Sscaled, Numeric::Uint, Numeric::Sint};
constexpr Numeric kAll[] = {Numeric::Unorm, Numeric::Snorm, Numeric::Uscaled,
                            Numeric::Sscaled, Numeric::Uint, Numeric::Sint, Numeric::Float};
constexpr Numeric kWide[] = {Numeric::Uint, Numeric::Sint, Numeric::Float};

constexpr std::uint32_t kB8[] = {8};
constexpr std::uint32_t kB16[] = {16};
constexpr std::uint32_t kB8x2[] = {8, 8};
constexpr std::uint32_t kB32[] = {32};
constexpr std::uint32_t kB16x2[] = {16, 16};
constexpr std::uint32_t kB11_11_10[] = {11, 11, 10};
constexpr std::uint32_t kB10_11_11[] = {10, 11, 11};
constexpr std::uint32_t kB2_10_10_10[] = {2, 10, 10, 10};
constexpr std::uint32_t kB10_10_10_2[] = {10, 10, 10, 2};
constexpr std::uint32_t kB8x4[] = {8, 8, 8, 8};
constexpr std::uint32_t kB32x2[] = {32, 32};
constexpr std::uint32_t kB16x4[] = {16, 16, 16, 16};
constexpr std::uint32_t kB32x3[] = {32, 32, 32};
constexpr std::uint32_t kB32x4[] = {32, 32, 32, 32};

struct Family {
    std::span<const std::uint32_t> bits;
    std::span<const Numeric> numerics;
};

constexpr Family kFamilies[] = {
    {kB8, kNorm},          {kB16, kAll},      {kB8x2, kNorm},      {kB32, kWide},
    {kB16x2, kAll},        {kB11_11_10, kAll}, {kB10_11_11, kAll}, {kB2_10_10_10, kNorm},
    {kB10_10_10_2, kNorm}, {kB8x4, kNorm},   {kB32x2, kWide},     {kB16x4, kAll},
    {kB32x3, kWide},       {kB32x4, kWide},
};

}  // namespace

std::optional<BufferFormat> buffer_format(std::uint8_t format) {
    if (format == 0) return std::nullopt;
    std::size_t n = static_cast<std::size_t>(format) - 1;
    for (const Family& family : kFamilies) {
        if (n < family.numerics.size()) return BufferFormat{family.bits, family.numerics[n]};
        n -= family.numerics.size();
    }
    return std::nullopt;
}

std::uint32_t half_shift(std::uint32_t width) { return width == 16 ? 0 : 15 - width; }

bool reads_mask_as_data(const Structured& st, const IrInst& inst) {
    const auto is_mask = [&st](Value v) { return st.masks.contains(v) || st.highs.contains(v); };
    std::vector<const Operand*> ops;
    if (const auto* sop = std::get_if<SopFields>(&inst.fields)) {
        for (const auto& entry : inst.writes) {
            if (entry.first.kind == Reg::Kind::S && is_mask(entry.second)) return false;
        }
        for (const Operand& op : sop->src) ops.push_back(&op);
    } else if (const auto* valu = std::get_if<Valu>(&inst.fields)) {
        const std::string_view name = inst.name();
        if (name == "v_cndmask_b32" || name.find("_co_ci_") != std::string_view::npos)
            return false;
        for (const Operand& op : valu->src) ops.push_back(&op);
    } else {
        return false;
    }
    for (const Operand* op : ops) {
        const auto* scalar = std::get_if<ScalarOperand>(op);
        if (scalar == nullptr || scalar->code == kNull) continue;
        const std::optional<Value> read = inst.read(Reg::s(scalar->code));
        if (read && is_mask(*read)) return true;
    }
    return false;
}

std::optional<std::pair<std::uint8_t, std::uint8_t>> packed_half(
    const std::vector<SigElement>& elements, std::uint8_t a, std::optional<std::uint8_t> b,
    std::uint8_t h) {
    const auto extent = [&elements](std::uint8_t row) -> std::optional<std::uint32_t> {
        std::optional<std::uint32_t> best;
        for (const SigElement& element : elements) {
            if (!element.row || *element.row != row) continue;
            const std::uint32_t end = static_cast<std::uint32_t>(element.col + element.cols);
            best = std::max(best.value_or(0), end);
        }
        return best;
    };
    const std::optional<std::uint32_t> na = extent(a);
    if (!na) return std::nullopt;
    const std::uint32_t a_halves = (*na + 1) / 2 * 2;
    if (h < a_halves) {
        if (h < *na) return std::pair<std::uint8_t, std::uint8_t>{a, h};
        return std::nullopt;
    }
    if (!b) return std::nullopt;
    const std::uint32_t c = h - a_halves;
    const std::optional<std::uint32_t> nb = extent(*b);
    if (!nb) return std::nullopt;
    if (c < *nb) return std::pair<std::uint8_t, std::uint8_t>{*b, static_cast<std::uint8_t>(c)};
    return std::nullopt;
}

std::map<std::uint8_t, std::pair<bool, std::optional<std::uint8_t>>> half_outputs(
    const StageIo& io, const std::vector<SigElement>& outputs) {
    const std::vector<VsOutput>* entries = nullptr;
    if (const auto* vs = std::get_if<VsIo>(&io)) entries = &vs->outputs;
    else if (const auto* ms = std::get_if<MeshIo>(&io)) entries = &ms->outputs;
    else return {};
    const auto per_vertex = [entries]() {
        std::vector<const VsOutput*> out;
        for (const VsOutput& entry : *entries)
            if ((entry.param & 0x80) == 0) out.push_back(&entry);
        return out;
    };
    std::vector<std::uint8_t> free_rows;
    for (const SigElement& element : outputs) {
        if (!(element.half && element.kind == 0)) continue;
        if (!element.row || *element.row > 255) continue;
        const std::uint8_t row = static_cast<std::uint8_t>(*element.row);
        bool named = false;
        for (const VsOutput* entry : per_vertex())
            if (entry->row == row) named = true;
        if (!named) free_rows.push_back(row);
    }
    std::sort(free_rows.begin(), free_rows.end());
    free_rows.erase(std::unique(free_rows.begin(), free_rows.end()), free_rows.end());
    std::vector<const VsOutput*> halves;
    for (const VsOutput* entry : per_vertex())
        if (entry->fp16) halves.push_back(entry);
    std::stable_sort(halves.begin(), halves.end(),
                     [](const VsOutput* a, const VsOutput* b) { return a->row < b->row; });
    std::map<std::uint8_t, std::pair<bool, std::optional<std::uint8_t>>> out;
    for (const VsOutput* entry : halves) {
        std::optional<std::size_t> index;
        if (entry->pack2) {
            for (std::size_t n = 0; n < free_rows.size(); ++n) {
                if (free_rows[n] > entry->row) {
                    index = n;
                    break;
                }
            }
        }
        std::optional<std::uint8_t> partner;
        if (index) {
            partner = free_rows[*index];
            free_rows.erase(free_rows.begin() + static_cast<std::ptrdiff_t>(*index));
        }
        out[entry->row] = {entry->pack2, partner};
    }
    return out;
}

Lower::Lower(const Parts& parts, const std::unordered_map<Value, Why, ValueHash>& bad,
             const std::map<DescriptorMask, Why>& bad_masked)
    : s_(parts.native->stage == HwStage::Ps), x_(&parts), bad_(&bad), bad_masked_(&bad_masked) {
    const Program& p = *parts.p;
    for (std::size_t b = 0; b < p.blocks.size(); ++b) {
        const IrBlock& block = p.blocks[b];
        for (std::size_t i = 0; i < block.phis.size(); ++i) phi_at_[block.phis[i].value] = {b, i};
    }
    s_.inputs = parts.inputs;
    s_.outputs = parts.outputs;
    const auto insts = [&p]() {
        std::vector<const IrInst*> out;
        for (const IrBlock& block : p.blocks)
            for (const IrInst& inst : block.insts) out.push_back(&inst);
        return out;
    };
    for (const IrInst* inst : insts()) {
        if (!(std::holds_alternative<ImageFields>(inst->fields) ||
              std::holds_alternative<BufferFields>(inst->fields)))
            continue;
        const std::string_view name = inst->name();
        if (name.find("store") != std::string_view::npos ||
            name.find("atomic") != std::string_view::npos)
            writes_uavs_ = true;
    }
    if (const auto* cs = std::get_if<ComputeIo>(&parts.native->io)) {
        s_.threads = cs->threads;
        s_.lds_dwords = cs->lds_bytes() / 4;
        console_lanes_ = false;
        for (const IrInst* inst : insts()) {
            const std::string_view name = inst->name();
            if (name.starts_with("ds_swizzle") || name.find("permute") != std::string_view::npos ||
                name.starts_with("v_mbcnt") || name == "v_readlane_b32" ||
                name == "v_writelane_b32" ||
                (std::holds_alternative<Valu>(inst->fields) &&
                 std::get<Valu>(inst->fields).dpp.has_value()) ||
                reads_mask_as_data(*parts.st, *inst)) {
                console_lanes_ = true;
                break;
            }
        }
        if (!console_lanes_) {
            std::vector<Bool> all;
            for (const auto& row : parts.st->guards)
                for (const auto& entry : row)
                    if (entry) all.push_back(*entry);
            for (const LaneExit& exit : parts.st->exits)
                if (exit.kind == LaneExit::Kind::Branch) all.push_back(exit.when);
            std::sort(all.begin(), all.end());
            all.erase(std::unique(all.begin(), all.end()), all.end());
            for (Bool guard : all) {
                for (const Atom& atom : parts.st->bools.support(guard)) {
                    if (atom.kind == Atom::Kind::Opaque || atom.kind == Atom::Kind::Constant) {
                        console_lanes_ = true;
                        break;
                    }
                }
                if (console_lanes_) break;
            }
        }
        const std::uint32_t wave_size = parts.native->wave64 ? 64 : 32;
        const std::uint64_t product = static_cast<std::uint64_t>(cs->threads[0]) * cs->threads[1] *
                                      cs->threads[2];
        const bool one_wave = product <= wave_size;
        const bool host_picks = std::getenv("G6SHIM_HOST_WAVE") != nullptr;
        if (!host_picks && (console_lanes_ || (one_wave && s_.lds_dwords > 0))) s_.wave_size = wave_size;
    }
    if (const auto* ms = std::get_if<MeshIo>(&parts.native->io)) {
        s_.prims = parts.prims;
        s_.threads = std::array<std::uint32_t, 3>{ms->threads, 1, 1};
        s_.lds_dwords = (ms->lds_bytes + 3) / 4;
        s_.mesh = Mesh{ms->max_vertices, ms->max_primitives, ms->topology};
        const std::vector<std::size_t> order = reverse_postorder(p.blocks);
        std::vector<std::size_t> index(p.blocks.size(), std::numeric_limits<std::size_t>::max());
        for (std::size_t n = 0; n < order.size(); ++n) index[order[n]] = n;
        const std::vector<std::size_t> idom = idoms(p.blocks, order, index);
        std::vector<std::size_t> alloc;
        for (std::size_t b = 0; b < p.blocks.size(); ++b) {
            for (const IrInst& inst : p.blocks[b].insts) {
                const auto* sopp = std::get_if<SoppFields>(&inst.fields);
                if (inst.name() == "s_sendmsg" && sopp != nullptr &&
                    (sopp->imm & 0xF) == kGsAllocReq) {
                    alloc.push_back(b);
                    break;
                }
            }
        }
        for (std::size_t b : order) {
            std::size_t d = b;
            bool dominated = false;
            for (;;) {
                if (std::find(alloc.begin(), alloc.end(), d) != alloc.end()) {
                    dominated = true;
                    break;
                }
                if (idom[d] == d || idom[d] == std::numeric_limits<std::size_t>::max()) break;
                d = idom[d];
            }
            if (ms->lds_bytes == 0 || dominated) no_barrier_.insert(b);
        }
    }
    block_.assign(p.blocks.size(), std::nullopt);
    exported_.assign(p.blocks.size(), false);
    rpo_.assign(p.blocks.size(), std::numeric_limits<std::size_t>::max());
    tail_.assign(p.blocks.size(), std::nullopt);
    half_outputs_ = half_outputs(parts.native->io, parts.outputs);
    if (parts.native->stage == HwStage::Gs) {
        Geometry geometry;
        geometry.input_primitive = kTrianglePrimitive;
        geometry.max_vertices = kGeometryMaxVertices;
        geometry.stream_mask = 1;
        geometry.output_topology = kTriangleStrip;
        geometry.instances = 1;
        s_.geometry = geometry;
    }
    if (parts.native->stage == HwStage::Cs) mask_data_ = MaskData::Console;
    else if (parts.native->stage == HwStage::Ps || parts.native->stage == HwStage::Vs ||
             parts.native->stage == HwStage::Gs)
        mask_data_ = MaskData::Host;
    else
        mask_data_ = MaskData::None;
    for (const auto& entry : parts.res->params)
        params_[std::get<0>(entry)] = {std::get<1>(entry), std::get<2>(entry)};
    for (const auto& entry : parts.res->scalar) scalar_[entry.first] = entry.second;
    for (const Descriptor& descriptor : parts.res->descriptors)
        descriptors_[{descriptor.site, descriptor.role == Role::Sampler}] = &descriptor.source;
    for (const Export& export_value : parts.io->exports) exports_[export_value.site] = &export_value;
    for (const auto& entry : parts.io->interps) interps_[entry.first] = &entry.second;
    for (std::size_t b = 0; b < p.blocks.size(); ++b) {
        const IrBlock& block = p.blocks[b];
        for (const auto& entry : block.insts)
            for (const auto& write : entry.writes) def_block_[write.second] = b;
        for (const Phi& phi : block.phis) def_block_[phi.value] = b;
    }
}

std::vector<UserData> Lower::user_reads() const {
    std::vector<UserData> out;
    out.reserve(user_reads_.size());
    for (const auto& entry : user_reads_) out.push_back(entry.second);
    return out;
}

V Lower::code_dword(V byte) {
    V global;
    std::uint32_t base = 0;
    std::uint32_t length = 0;
    if (!code_table_) {
        std::optional<std::uint32_t> first;
        for (const auto& entry : x_->res->scalar) {
            if (entry.second.param != kCode) continue;
            const std::uint32_t offset = entry.second.offset & ~3u;
            first = std::min(first.value_or(offset), offset);
        }
        for (const Descriptor& descriptor : x_->res->descriptors) {
            for (const Source* leaf : descriptor.source.leaves()) {
                if (leaf->kind != Source::Kind::Built || leaf->address.param != kCode) continue;
                const std::uint32_t offset = leaf->address.offset & ~3u;
                first = std::min(first.value_or(offset), offset);
            }
        }
        if (!first) fail("a code table");
        const std::vector<std::uint8_t>& code = x_->native->code;
        if (*first > code.size()) fail("a code table past the code");
        std::vector<std::uint32_t> words;
        for (std::size_t at = *first; at < code.size(); at += 4) {
            const std::size_t count = std::min<std::size_t>(4, code.size() - at);
            std::array<std::uint8_t, 4> bytes{};
            for (std::size_t k = 0; k < count; ++k) bytes[k] = code[at + k];
            words.push_back(static_cast<std::uint32_t>(bytes[0]) |
                            static_cast<std::uint32_t>(bytes[1]) << 8 |
                            static_cast<std::uint32_t>(bytes[2]) << 16 |
                            static_cast<std::uint32_t>(bytes[3]) << 24);
        }
        if (words.empty()) fail("a code table past the code");
        std::vector<V> elements;
        elements.reserve(words.size());
        for (std::uint32_t word : words) elements.push_back(s_.m.i32(word));
        const Ty array = s_.m.array(static_cast<std::uint32_t>(words.size()), s_.t.i32);
        const V init = s_.m.aggregate(array, elements);
        global = s_.m.constant_global("code", init);
        code_table_ = std::tuple<V, std::uint32_t, std::uint32_t>{
            global, *first, static_cast<std::uint32_t>(words.size())};
    }
    std::tie(global, base, length) = *code_table_;
    const std::size_t c = cur_;
    const V k = s_.m.i32(base);
    const V rel = s_.m.bin(c, Bin::Sub, byte, k);
    const V two = s_.m.i32(2);
    const V index = s_.m.bin(c, Bin::LShr, rel, two);
    const V last = s_.m.i32(length - 1);
    const V over = s_.m.cmp(c, pred::kIugt, index, last);
    const V selected = s_.m.select(c, over, last, index);
    const V at = s_.m.element(c, global, selected);
    return s_.m.load(c, at);
}

void Lower::flat(const IrInst& inst, std::size_t b, std::size_t i) {
    const auto* fields = std::get_if<FlatFields>(&inst.fields);
    if (fields == nullptr) std::abort();
    const std::string_view name = inst.name();
    std::optional<std::string_view> rest = std::nullopt;
    if (name.starts_with("global_load_")) rest = name.substr(12);
    if (!rest) fail("a global memory write");
    std::size_t n = 0;
    std::optional<std::pair<std::uint32_t, bool>> sub;
    if (*rest == "dword") n = 1;
    else if (*rest == "dwordx2") n = 2;
    else if (*rest == "dwordx3") n = 3;
    else if (*rest == "dwordx4") n = 4;
    else if (*rest == "ubyte") { n = 1; sub = {{8, false}}; }
    else if (*rest == "sbyte") { n = 1; sub = {{8, true}}; }
    else if (*rest == "ushort") { n = 1; sub = {{16, false}}; }
    else if (*rest == "sshort") { n = 1; sub = {{16, true}}; }
    else fail(name);
    const auto address_at = scalar_.find(Site{b, i});
    if (address_at == scalar_.end()) fail("an untracked global address");
    const Address a = address_at->second;
    const std::optional<V> guard = this->guard(b, i);
    if (!fields->dst) fail("a global load result");
    const auto* dst = std::get_if<VectorOperand>(&*fields->dst);
    if (dst == nullptr) fail("a global load result");
    const std::uint8_t reg = dst->reg;
    const std::size_t c = cur_;
    V off = s_.m.i32(a.offset);
    if (a.dynamic) off = s_.m.bin(c, Bin::Add, off, i32_of(*a.dynamic));
    std::optional<V> h;
    if (a.param != kCode) {
        const Source src = Source::built(a, 0, std::array<std::uint32_t, 2>{0, 0});
        h = handle(Site{b, i}, src, false, {11, 0});
    }
    const auto dword = [&](V byte) -> V {
        if (!h) return code_dword(byte);
        const std::size_t at = cur_;
        const V u = s_.m.undef(s_.t.i32);
        const V mask = s_.m.i8(1);
        const V align = s_.m.i32(4);
        const V r = s_.op(at, "rawBufferLoad.f32", Attr::ReadOnly, s_.t.res_ret, 139,
                          {*h, byte, u, mask, align});
        return as_int(s_.m.extract(at, r, 0));
    };
    for (std::size_t k = 0; k < n; ++k) {
        V x;
        if (!sub) {
            const V four = s_.m.i32(static_cast<std::uint32_t>(4 * k));
            const V at = s_.m.bin(c, Bin::Add, off, four);
            x = dword(at);
        } else {
            const std::uint32_t width = sub->first;
            const bool is_signed = sub->second;
            const V not3 = s_.m.i32(~3u);
            const V three = s_.m.i32(3);
            const V at = s_.m.bin(c, Bin::And, off, not3);
            const V w = dword(at);
            const V byte = s_.m.bin(c, Bin::And, off, three);
            const V sh = s_.m.bin(c, Bin::Shl, byte, three);
            const V shifted = s_.m.bin(c, Bin::LShr, w, sh);
            const V left = s_.m.i32(32 - width);
            const V up = s_.m.bin(c, Bin::Shl, shifted, left);
            x = s_.m.bin(c, is_signed ? Bin::AShr : Bin::LShr, up, left);
        }
        write(inst, Reg::v(static_cast<std::uint8_t>(reg + k)), x, guard);
    }
}

void Lower::find_sections() {
    const Program& p = *x_->p;
    std::unordered_map<Value, std::tuple<std::size_t, std::size_t, std::size_t>, ValueHash> inside;
    for (std::size_t b = 0; b < p.blocks.size(); ++b) {
        const std::unordered_map<std::size_t, Section> found = sections(p, b);
        for (const auto& entry : found) {
            const std::size_t start = entry.first;
            const std::size_t end = entry.second.end;
            for (std::size_t i = start + 1; i < end; ++i) {
                for (const auto& write : p.blocks[b].insts[i].writes)
                    inside[write.second] = std::tuple<std::size_t, std::size_t, std::size_t>{
                        b, start, end};
            }
        }
        if (!found.empty()) sections_[b] = found;
    }
    if (inside.empty()) return;
    for (std::size_t b = 0; b < p.blocks.size(); ++b) {
        const IrBlock& block = p.blocks[b];
        for (const Phi& phi : block.phis) {
            for (Value arg : phi.args)
                if (inside.contains(arg)) section_out_.insert(arg);
        }
        for (std::size_t i = 0; i < block.insts.size(); ++i) {
            for (const auto& read : block.insts[i].reads) {
                const auto hit = inside.find(read.second);
                if (hit == inside.end()) continue;
                const std::size_t sb = std::get<0>(hit->second);
                const std::size_t start = std::get<1>(hit->second);
                const std::size_t end = std::get<2>(hit->second);
                if (!(sb == b && i > start && i < end)) section_out_.insert(read.second);
            }
        }
    }
}

void Lower::section_inst(std::size_t b, std::size_t i, std::size_t start) {
    constexpr Why unknown = "a whole-wave section step the simulation does not follow";
    const IrInst& inst = x_->p->blocks[b].insts[i];
    for (const auto& write : inst.writes) {
        const Value w = write.second;
        Val val = Val::missing(unknown);
        if (section_out_.contains(w)) {
            const auto& shapes = sections_[b][start].shapes;
            const auto found = shapes.find(w);
            if (found == shapes.end()) {
                val = Val::missing(unknown);
            } else {
                try {
                    val = Val::i32(shape(found->second));
                } catch (const WhyError& error) {
                    val = Val::missing(error.why);
                }
            }
        }
        vals_[w] = val;
    }
}

V Lower::shape(Shape shape_value) {
    const Op op = shape_value.kind == Shape::Kind::K || shape_value.kind == Shape::Kind::Value
                      ? Op::AddU
                      : shape_value.op;
    if (shape_value.kind == Shape::Kind::K) return s_.m.i32(shape_value.constant);
    if (shape_value.kind == Shape::Kind::Value) return i32_of(shape_value.value);
    const Value src = shape_value.src;
    const Value mask = shape_value.mask;
    const std::uint32_t ident = shape_value.ident;
    std::optional<bool> prefix;
    if (shape_value.kind == Shape::Kind::Masked) {
        const std::size_t c = cur_;
        const auto found = x_->st->masks.find(mask);
        if (found == x_->st->masks.end()) fail("a wave reduction's untracked mask");
        const V on = boolean(found->second, c);
        const V x = i32_of(src);
        const V id = s_.m.i32(ident);
        return s_.m.select(c, on, x, id);
    }
    if (shape_value.kind == Shape::Kind::Prefix) prefix = shape_value.inclusive;
    if (mask_data_ == MaskData::None) fail("a wave reduction in a mesh shader");
    const std::size_t c = cur_;
    const auto found = x_->st->masks.find(mask);
    if (found == x_->st->masks.end()) fail("a wave reduction's untracked mask");
    const V on = boolean(found->second, c);
    V x = i32_of(src);
    const V id = s_.m.i32(ident);
    x = s_.m.select(c, on, x, id);
    s_.flags |= feature::kWaveOps;
    const bool is_float = op == Op::AddF || op == Op::MinF || op == Op::MaxF;
    const Ty i32t = s_.t.i32;
    const Ty f32t = s_.t.f32;
    const V arg = is_float ? as_float(x) : x;
    const Ty ty = is_float ? f32t : i32t;
    const std::string sfx = is_float ? "f32" : "i32";
    V r;
    if (prefix) {
        if (op != Op::AddU && op != Op::AddF) fail("a wave prefix of an operation other than a sum");
        const V k0 = s_.m.i8(0);
        const V k1 = s_.m.i8(1);
        r = s_.op(c, "wavePrefixOp." + sfx, Attr::NoUnwind, ty, 121, {arg, k0, k1});
        if (*prefix) r = s_.m.bin(c, Bin::Add, r, arg);
    } else if (op == Op::Or || op == Op::And || op == Op::Xor) {
        const std::uint8_t code = op == Op::And ? 0 : op == Op::Or ? 1 : 2;
        const V k = s_.m.i8(code);
        r = s_.op(c, "waveActiveBit.i32", Attr::NoUnwind, i32t, 120, {arg, k});
    } else {
        std::uint8_t code = 0;
        std::uint8_t unsigned_value = 1;
        switch (op) {
            case Op::AddU: code = 0; unsigned_value = 1; break;
            case Op::AddF: code = 0; unsigned_value = 0; break;
            case Op::MinU: code = 2; unsigned_value = 1; break;
            case Op::MaxU: code = 3; unsigned_value = 1; break;
            case Op::MinI:
            case Op::MinF: code = 2; unsigned_value = 0; break;
            default: code = 3; unsigned_value = 0; break;
        }
        const V k = s_.m.i8(code);
        const V u = s_.m.i8(unsigned_value);
        r = s_.op(c, "waveActiveOp." + sfx, Attr::NoUnwind, ty, 119, {arg, k, u});
    }
    return is_float ? as_int(r) : r;
}

Lowered Lower::run() {
    const Program& p = *x_->p;
    const std::vector<std::size_t> order = reverse_postorder(p.blocks);
    for (std::size_t n = 0; n < order.size(); ++n) rpo_[order[n]] = n;
    for (std::size_t n = 0; n < order.size(); ++n) {
        const std::size_t b = order[n];
        if (n == 0) {
            block_[b] = s_.entry;
        } else {
            block_[b] = s_.m.new_block();
        }
    }
    for (std::size_t b : order) {
        bool header = false;
        for (std::size_t q : p.blocks[b].preds)
            if (block_[q] && rpo_[b] <= rpo_[q]) header = true;
        if (header) {
            const std::size_t blk = *block_[b];
            const V phi = s_.m.phi(blk, s_.t.i32);
            counters_[b] = {phi, {}};
        }
    }
    cur_ = s_.entry;
    find_sections();
    inputs();
    if (s_.mesh) {
        mesh_prologue(*s_.mesh);
        block_[order[0]] = cur_;
    }
    for (std::size_t b : order) {
        cur_ = *block_[b];
        cur_ir_ = b;
        std::vector<std::size_t> preds;
        for (std::size_t q : p.blocks[b].preds)
            if (block_[q]) preds.push_back(q);
        bool all_exported = !preds.empty();
        for (std::size_t q : preds)
            if (!exported_out(q)) all_exported = false;
        exported_[b] = all_exported;
        for (std::size_t i = 0; i < p.blocks[b].phis.size(); ++i) {
            const Phi& phi = p.blocks[b].phis[i];
            Val val;
            if (phi.reg.kind == Reg::Kind::S && (phi.reg.code == kExec || phi.reg.code == kExec + 1)) {
                val = Val::missing("a lane mask");
            } else if (bad_->contains(phi.value)) {
                val = Val::missing(bad_->at(phi.value));
            } else if (phi.reg.kind == Reg::Kind::Scc) {
                const V v = s_.m.phi(cur_, s_.t.i1);
                data_phis_.emplace_back(v, b, i);
                val = Val::bit(v);
            } else {
                const V v = s_.m.phi(cur_, s_.t.i32);
                data_phis_.emplace_back(v, b, i);
                val = Val::i32(v);
            }
            vals_[phi.value] = val;
        }
        std::size_t i = 0;
        while (i < p.blocks[b].insts.size()) {
            inst(b, i);
            std::optional<std::size_t> end;
            const auto section = sections_.find(b);
            if (section != sections_.end()) {
                const auto hit = section->second.find(i);
                if (hit != section->second.end()) end = hit->second.end;
            }
            if (end) {
                for (std::size_t j = i + 1; j < *end; ++j) section_inst(b, j, i);
                i = *end;
                continue;
            }
            ++i;
        }
        mask_ok_ = mask_data_ != MaskData::None;
        tail_[b] = cur_;
        exit(b);
    }
    fill_phis();
    fill_mask_words();
    s_.samplers = static_cast<std::uint32_t>(samplers_.size());
    if (s_.geometry) {
        std::uint32_t scalars = 1;
        for (const SigElement& element : s_.outputs) {
            if (!element.row) continue;
            scalars = std::max(scalars, *element.row * 4 + static_cast<std::uint32_t>(element.col + element.cols));
        }
        s_.geometry->max_vertices = std::min(s_.geometry->max_vertices, kGeometryScalarProduct / scalars);
    }
    Lowered out;
    out.dxil = s_.finish();
    out.cmp_samplers = s_.cmp_samplers;
    out.root_cbvs.reserve(s_.root_cbvs.size());
    for (const auto& entry : s_.root_cbvs) out.root_cbvs.push_back(entry.first);
    out.root_srvs.reserve(s_.root_srvs.size());
    for (const auto& entry : s_.root_srvs) out.root_srvs.push_back(entry.first);
    out.user_data = user_reads();
    out.samplers = samplers_;
    out.nonuniform_handles = s_.nonuniform_handles;
    out.wave_ops = (s_.flags & feature::kWaveOps) != 0;
    return out;
}

bool Lower::exported_out(std::size_t b) const { return exported_[b] || has_done_export(b); }

bool Lower::has_done_export(std::size_t b) const {
    for (std::size_t i = 0; i < x_->p->blocks[b].insts.size(); ++i) {
        const auto found = exports_.find(Site{b, i});
        if (found != exports_.end() && found->second->done) return true;
    }
    return false;
}

void Lower::inputs() {
    std::unordered_map<Value, Input, ValueHash> io_inputs;
    for (const auto& entry : x_->io->inputs) io_inputs[entry.first] = entry.second;
    for (const auto& entry : x_->p->inputs) {
        const Reg reg = entry.first;
        const Value v = entry.second;
        Val val;
        if (reg.kind == Reg::Kind::S && params_.contains(v)) continue;
        if (reg.kind == Reg::Kind::S) {
            const auto found = io_inputs.find(v);
            if (found != io_inputs.end()) {
                val = input(found->second);
            } else if (reg.code <= 105 && x_->io->initialized_sgprs &&
                       reg.code >= *x_->io->initialized_sgprs) {
                val = Val::i32(s_.m.i32(0));
            } else {
                val = Val::missing("a hardware SGPR input");
            }
        } else if (reg.kind == Reg::Kind::Scc) {
            val = Val::missing("SCC on entry");
        } else {
            const auto found = io_inputs.find(v);
            val = found == io_inputs.end() ? Val::i32(s_.m.i32(0)) : input(found->second);
        }
        vals_[v] = val;
    }
}

Val Lower::input(const Input& value) {
    std::uint8_t kind = 0;
    std::uint8_t col = 0;
    switch (value.kind) {
        case Input::Kind::VertexId: kind = 1; col = 0; break;
        case Input::Kind::InstanceId: kind = 2; col = 0; break;
        case Input::Kind::Position: kind = 3; col = value.index; break;
        case Input::Kind::Attribute: {
            const auto found = find_input(value.row, value.channel);
            if (!found) return Val::missing("a vertex input the signature lacks");
            return load_input(found->first, found->second);
        }
        case Input::Kind::Barycentric:
            return Val::missing("barycentrics read directly");
        case Input::Kind::PrimitiveId: return Val::missing("the primitive id");
        case Input::Kind::FrontFace: kind = 13; col = 0; break;
        case Input::Kind::ThreadIdInGroup: return thread_id(value.index);
        case Input::Kind::GroupId: return compute_id("groupId.i32", 94, value.index);
        case Input::Kind::GroupInfo: return Val::missing("the thread-group info SGPR");
        case Input::Kind::MeshGroup: {
            const Val group = compute_id("groupId.i32", 94, 0);
            const V n = s_.m.i32(x_->mesh_stride);
            return Val::i32(s_.m.bin(s_.entry, Bin::Mul, group.value, n));
        }
        case Input::Kind::MergedWaveInfo: {
            const V f = mesh_thread();
            const V shift = s_.m.i32(static_cast<std::uint32_t>(std::countr_zero(wave())));
            const V w = s_.m.bin(s_.entry, Bin::LShr, f, shift);
            const V k24 = s_.m.i32(24);
            return Val::i32(s_.m.bin(s_.entry, Bin::Shl, w, k24));
        }
        default: return Val::missing("a pixel system input");
    }
    std::uint32_t free_row = 0;
    for (const SigElement& element : s_.inputs)
        if (element.row) free_row = std::max(free_row, *element.row + 1);
    std::optional<std::size_t> found;
    for (std::size_t id = 0; id < s_.inputs.size(); ++id) {
        if (s_.inputs[id].kind == kind) {
            found = id;
            break;
        }
    }
    std::size_t id = 0;
    if (found) {
        id = *found;
        if (kind != 3 && !s_.inputs[id].row) s_.inputs[id].row = free_row;
    } else if (kind >= 3) {
        return Val::missing("a pixel system input the signature lacks");
    } else {
        std::uint32_t row = 0;
        for (const SigElement& element : s_.inputs)
            if (element.row) row = std::max(row, *element.row + 1);
        SigElement element;
        element.name = kind == 1 ? "SV_VertexID" : "SV_InstanceID";
        element.index = 0;
        element.kind = kind;
        element.comp = 5;
        element.interp = 0;
        element.row = row;
        element.col = 0;
        element.cols = 1;
        element.usage = 0;
        element.half = false;
        s_.inputs.push_back(std::move(element));
        id = s_.inputs.size() - 1;
    }
    return load_input(id, col);
}

Val Lower::compute_id(std::string_view name, std::uint32_t opcode, std::uint8_t c) {
    const std::size_t entry = s_.entry;
    const V index = s_.m.i32(c);
    return Val::i32(s_.op(entry, std::string(name), Attr::ReadNone, s_.t.i32, opcode, {index}));
}

std::optional<std::pair<std::size_t, std::uint8_t>> Lower::find_input(std::uint8_t row,
                                                                      std::uint8_t channel) {
    for (std::size_t id = 0; id < s_.inputs.size(); ++id) {
        const SigElement& element = s_.inputs[id];
        if (!element.row) continue;
        if (*element.row == row && channel >= element.col &&
            channel < element.col + element.cols)
            return std::pair<std::size_t, std::uint8_t>{
                id, static_cast<std::uint8_t>(channel - element.col)};
    }
    return std::nullopt;
}

Val Lower::load_input(std::size_t id, std::uint8_t col) {
    if (s_.inputs[id].half) return Val::missing("a 16-bit input");
    return load_any_input(id, col);
}

Val Lower::load_any_input(std::size_t id, std::uint8_t col, std::optional<std::uint32_t> vertex) {
    SigElement& e = s_.inputs[id];
    e.usage = static_cast<std::uint8_t>(e.usage | (1u << col));
    const bool is_int = e.comp != 9;
    const bool bit = e.comp == 1;
    Ty ty = s_.t.i32;
    if (bit) ty = s_.t.i1;
    else if (!is_int) ty = s_.t.f32;
    std::string suffix = ".i32";
    if (bit) suffix = ".i1";
    else if (!is_int) suffix = ".f32";
    const std::size_t entry = s_.entry;
    const V axis = vertex ? s_.m.i32(*vertex) : s_.m.undef(s_.t.i32);
    const std::vector<V> args = {s_.m.i32(static_cast<std::uint32_t>(id)), s_.m.i32(0), s_.m.i8(col), axis};
    const V v = s_.op(entry, "loadInput" + suffix, Attr::ReadNone, ty, 4, args);
    if (bit) return Val::i32(s_.m.cast(entry, Cast::ZExt, v, s_.t.i32));
    if (is_int) return Val::i32(v);
    return Val::i32(s_.m.cast(entry, Cast::Bitcast, v, s_.t.i32));
}

std::optional<std::pair<std::size_t, std::uint8_t>> Lower::geometry_slot(std::vector<SigElement>& elements,
                                                                         std::uint32_t dword, bool input) {
    if (dword > 255) return std::nullopt;
    const std::uint8_t row = static_cast<std::uint8_t>(dword / 4);
    const std::uint8_t col = static_cast<std::uint8_t>(dword % 4);
    for (std::size_t id = 0; id < elements.size(); ++id) {
        const SigElement& element = elements[id];
        if (!element.row || *element.row != row) continue;
        if (col >= element.col && col < element.col + element.cols)
            return std::pair<std::size_t, std::uint8_t>{id, static_cast<std::uint8_t>(col - element.col)};
    }
    if (!elements.empty()) return std::nullopt;
    SigElement element;
    element.row = row;
    element.col = 0;
    element.cols = 4;
    element.comp = 9;
    element.usage = 0;
    element.half = false;
    element.interp = row == 0 ? 4 : 2;
    if (row == 0) {
        element.name = "SV_Position";
        element.kind = 3;
        element.index = 0;
    } else {
        element.name = "TEXCOORD";
        element.kind = 0;
        element.index = static_cast<std::uint32_t>(row - 1);
    }
    if (!input && element.kind == 0) element.interp = 2;
    elements.push_back(std::move(element));
    return std::pair<std::size_t, std::uint8_t>{elements.size() - 1, col};
}

void Lower::geometry_ring(const IrInst& inst, std::size_t b, std::size_t i) {
    const Site site{b, i};
    bool matched = false;
    const std::optional<V> guard_value = guard(b, i);
    for (const RingLoad& load : x_->io->ring_loads) {
        if (load.site != site) continue;
        matched = true;
        const std::optional<std::pair<std::size_t, std::uint8_t>> slot =
            geometry_slot(s_.inputs, load.dword, true);
        if (!slot) fail("a geometry input the signature lacks");
        const Val value = load_any_input(slot->first, slot->second, load.vertex);
        if (value.kind != Val::Kind::I32) fail(value.why);
        write(inst, Reg::v(load.vgpr), value.value, guard_value);
    }
    const auto at = begin_guarded(guard_value);
    for (const RingStore& store : x_->io->ring_stores) {
        if (store.site != site) continue;
        matched = true;
        const std::optional<std::pair<std::size_t, std::uint8_t>> slot =
            geometry_slot(s_.outputs, store.dword, false);
        if (!slot) fail("a geometry output the signature lacks");
        const std::optional<Value> source = inst.read(Reg::v(store.vgpr));
        if (!source) fail("an unread geometry output");
        SigElement& element = s_.outputs[slot->first];
        element.usage = static_cast<std::uint8_t>(element.usage | (1u << slot->second));
        V argument = i32_of(*source);
        std::string suffix = ".i32";
        if (element.comp == 9) {
            suffix = ".f32";
            const auto found = floats_.find(*source);
            argument = found != floats_.end() ? found->second : as_float(argument);
        }
        const std::vector<V> args = {s_.m.i32(static_cast<std::uint32_t>(slot->first)), s_.m.i32(0),
                                     s_.m.i8(slot->second), argument};
        s_.op(cur_, "storeOutput" + suffix, Attr::NoUnwind, s_.t.void_type, 5, args);
    }
    end_guarded(at, {});
    if (!matched) fail("an LDS operation in a geometry shader");
}

void Lower::geometry_message(const IrInst& inst, std::size_t b, std::size_t i) {
    if (x_->native->stage != HwStage::Gs) return;
    const auto* fields = std::get_if<SoppFields>(&inst.fields);
    if (fields == nullptr) std::abort();
    const std::uint16_t msg = fields->imm & 0xF;
    const std::uint16_t op = (fields->imm >> 4) & 7;
    const std::uint16_t stream = (fields->imm >> 8) & 3;
    if (msg == kMsgGsDone && op == 0) return;
    if (msg != kMsgGs) fail("a geometry message");
    if (stream != 0) fail("a geometry stream other than zero");
    if (op == 0) return;
    if (op != kGsOpCut && op != kGsOpEmit && op != kGsOpEmitCut) fail("a geometry message");
    const std::optional<V> guard_value = guard(b, i);
    const auto at = begin_guarded(guard_value);
    const V id = s_.m.i8(static_cast<std::uint8_t>(stream));
    if (op == kGsOpEmit || op == kGsOpEmitCut)
        s_.op(cur_, "emitStream", Attr::NoUnwind, s_.t.void_type, kEmitStream, {id});
    if (op == kGsOpCut || op == kGsOpEmitCut)
        s_.op(cur_, "cutStream", Attr::NoUnwind, s_.t.void_type, kCutStream, {id});
    end_guarded(at, {});
}

Val Lower::get(Value v) {
    if (phi_at_.contains(v)) read_phis_.insert(v);
    const bool is_mask = x_->st->masks.contains(v) || x_->st->highs.contains(v);
    const auto found = vals_.find(v);
    const bool missing = found == vals_.end() || found->second.kind == Val::Kind::Missing;
    if (missing && mask_ok_ && is_mask) return Val::i32(mask_dword(v));
    if (found != vals_.end()) {
        if (found->second.kind == Val::Kind::Missing) fail(found->second.why);
        return found->second;
    }
    const auto param = params_.find(v);
    if (param != params_.end()) {
        const Val val = user(param->second.first, param->second.second);
        vals_[v] = val;
        return val;
    }
    const auto address = descriptor_addresses_.find(v);
    if (address != descriptor_addresses_.end()) {
        const Val val = descriptor_address(address->second);
        vals_[v] = val;
        return val;
    }
    fail("a value defined after its use");
}

V Lower::i32_of(Value v) {
    const Val val = get(v);
    if (val.kind == Val::Kind::I32) return val.value;
    if (val.kind == Val::Kind::Bit) return s_.m.cast(cur_, Cast::ZExt, val.value, s_.t.i32);
    fail(val.why);
}

Val Lower::user(std::size_t param, std::uint32_t dword) {
    const RootParam& rp = x_->native->layout[param];
    if (rp.root.kind == RootType::Kind::ResourceHeap || rp.root.kind == RootType::Kind::SamplerHeap)
        return Val::i32(s_.m.i32(0));
    if (rp.root.kind != RootType::Kind::Constants && rp.root.kind != RootType::Kind::Table)
        return Val::missing("a root descriptor");
    const std::uint32_t index = static_cast<std::uint32_t>(rp.user_data) + dword;
    user_reads_[index] = UserData{index, rp.slot, rp.root, dword};
    return user_dword(index);
}

Val Lower::descriptor_address(UserData data) {
    constexpr std::uint32_t kRootDwords = 64;
    std::uint32_t index = 0;
    for (const RootParam& param : x_->native->layout) {
        if (param.user_data >= kRootDwords || param.sgprs > kRootDwords - param.user_data)
            fail("a buffer descriptor address beyond host root constants");
        index = std::max(index, std::uint32_t{param.user_data} + param.sgprs);
    }
    for (const auto& [used_index, entry] : user_reads_) {
        if (entry.source == data.source && entry.slot == data.slot && entry.dword == data.dword)
            return user_dword(used_index);
        index = std::max(index, used_index + 1);
    }
    if (index >= kRootDwords) fail("a buffer descriptor address beyond host root constants");
    data.index = index;
    user_reads_.emplace(index, data);
    return user_dword(index);
}

Val Lower::user_dword(std::uint32_t index) {
    s_.user_data = std::max(s_.user_data, index + 1);
    V h;
    if (user_data_) {
        h = *user_data_;
    } else {
        h = s_.user_data_handle();
        user_data_ = h;
    }
    const std::size_t entry = s_.entry;
    const V row = s_.m.i32(index / 4);
    const V r = s_.op(entry, "cbufferLoadLegacy.i32", Attr::ReadOnly, s_.t.cbuf_ret, 59, {h, row});
    return Val::i32(s_.m.extract(entry, r, index % 4));
}

V Lower::mask_dword(Value v) {
    const std::pair<V, V> words = mask_pair(v);
    const bool high = x_->st->highs.contains(v);
    return high ? words.second : words.first;
}

std::pair<V, V> Lower::mask_pair(Value v) {
    Value lo = v;
    if (const auto high_found = x_->st->highs.find(v); high_found != x_->st->highs.end())
        lo = high_found->second;
    const auto mask = x_->st->masks.find(lo);
    if (mask == x_->st->masks.end()) fail(kLaneMask);
    if (const auto ballot = ballots_.find(lo); ballot != ballots_.end()) return ballot->second;
    if (!x_->st->lane_data.contains(lo)) fail(mask_data_reason(x_->st->bools, mask->second));
    if (!balloting_.insert(lo).second) fail("a lane mask whose lanes depend on its own value");
    std::pair<V, V> words;
    try {
        const std::size_t at = mask_site(lo);
        if (x_->st->active.contains(lo)) words = active_ballot(mask->second, at);
        else words = combine_mask(mask->second, at);
    } catch (...) {
        balloting_.erase(lo);
        throw;
    }
    balloting_.erase(lo);
    ballots_[lo] = words;
    return words;
}

std::size_t Lower::mask_site(Value lo) {
    std::size_t at = s_.entry;
    const auto def = def_block_.find(lo);
    if (def == def_block_.end()) return at;
    if (def->second == cur_ir_) return cur_;
    if (!tail_[def->second]) fail("a lane mask written after its use");
    return *tail_[def->second];
}

std::pair<V, V> Lower::active_ballot(Bool bits, std::size_t at) {
    const V bit = boolean(bits, at);
    const Ty i32t = s_.t.i32;
    const Ty four = s_.m.named_struct("dx.types.fouri32", {i32t, i32t, i32t, i32t});
    const V r = s_.op(at, "waveActiveBallot", Attr::NoUnwind, four, 116, {bit});
    s_.flags |= feature::kWaveOps;
    return {s_.m.extract(at, r, 0), s_.m.extract(at, r, 1)};
}

std::pair<V, V> Lower::mask_ones() {
    const V lo = s_.m.i32(0xFFFFFFFFu);
    const V hi = s_.m.i32(x_->native->wave64 ? 0xFFFFFFFFu : 0u);
    return {lo, hi};
}

std::pair<V, V> Lower::mask_bin(Bin op, std::pair<V, V> a, std::pair<V, V> b, std::size_t at) {
    return {s_.m.bin(at, op, a.first, b.first), s_.m.bin(at, op, a.second, b.second)};
}

std::pair<V, V> Lower::combine_mask(Bool bits, std::size_t at) {
    if (bits == kFalse) return {s_.m.i32(0), s_.m.i32(0)};
    if (bits == kTrue) return mask_ones();
    const auto key = std::pair<std::uint32_t, std::size_t>{bits.index, at};
    if (const auto found = mask_words_.find(key); found != mask_words_.end()) return found->second;
    const std::optional<std::tuple<Atom, Bool, Bool>> node = x_->st->bools.node(bits);
    if (!node) std::abort();
    const Atom& atom_value = std::get<0>(*node);
    const Bool low = std::get<1>(*node);
    const Bool high = std::get<2>(*node);
    const std::pair<V, V> a = atom_mask(atom_value, at);
    std::pair<V, V> words;
    if (low == kFalse && high == kTrue) {
        words = a;
    } else if (low == kTrue && high == kFalse) {
        words = mask_bin(Bin::Xor, a, mask_ones(), at);
    } else if (low == kFalse) {
        words = mask_bin(Bin::And, a, combine_mask(high, at), at);
    } else if (high == kTrue) {
        words = mask_bin(Bin::Or, a, combine_mask(low, at), at);
    } else {
        const std::pair<V, V> on_false = combine_mask(low, at);
        const std::pair<V, V> on_true = combine_mask(high, at);
        const std::pair<V, V> flipped = mask_bin(Bin::Xor, a, mask_ones(), at);
        words = mask_bin(Bin::Or, mask_bin(Bin::And, a, on_true, at),
                         mask_bin(Bin::And, flipped, on_false, at), at);
    }
    mask_words_[key] = words;
    return words;
}

std::pair<V, V> Lower::atom_mask(const Atom& atom_value, std::size_t at) {
    switch (atom_value.kind) {
        case Atom::Kind::Lane: {
            if (const auto found = lane_ballots_.find(atom_value.value); found != lane_ballots_.end())
                return found->second;
            const auto bit = lane_bits_.find(atom_value.value);
            if (bit == lane_bits_.end()) {
                const auto reason = lane_fail_.find(atom_value.value);
                if (reason != lane_fail_.end()) fail(reason->second);
                fail("a lane mask from a carry");
            }
            const std::size_t born = mask_site(atom_value.value);
            const Ty i32t = s_.t.i32;
            const Ty four = s_.m.named_struct("dx.types.fouri32", {i32t, i32t, i32t, i32t});
            const V r = s_.op(born, "waveActiveBallot", Attr::NoUnwind, four, 116, {bit->second});
            s_.flags |= feature::kWaveOps;
            const std::pair<V, V> out = {s_.m.extract(born, r, 0), s_.m.extract(born, r, 1)};
            lane_ballots_[atom_value.value] = out;
            return out;
        }
        case Atom::Kind::Constant: {
            const V lo = s_.m.i32(static_cast<std::uint32_t>(atom_value.constant));
            const V hi = s_.m.i32(static_cast<std::uint32_t>(atom_value.constant >> 32));
            return {lo, hi};
        }
        case Atom::Kind::Uniform: {
            const V bit = atom(atom_value, at);
            return {s_.m.select(at, bit, mask_ones().first, s_.m.i32(0)),
                    s_.m.select(at, bit, mask_ones().second, s_.m.i32(0))};
        }
        case Atom::Kind::Phi: {
            const Value id = atom_value.value;
            if (const auto known = mask_phi_words_.find(id); known != mask_phi_words_.end())
                return known->second;
            const auto location = phi_at_.find(id);
            if (location == phi_at_.end()) fail("a lane-mask join without a phi");
            const std::size_t block = location->second.first;
            if (!block_[block]) fail("an unreachable join");
            const V lo = s_.m.phi(*block_[block], s_.t.i32);
            const V hi = s_.m.phi(*block_[block], s_.t.i32);
            const std::pair<V, V> words = {lo, hi};
            mask_phi_words_[id] = words;
            pending_mask_words_.push_back(id);
            return words;
        }
        case Atom::Kind::Quad:
            fail(kQuadMask);
        case Atom::Kind::Opaque:
            fail(kOpaqueMask);
    }
    fail(kOpaqueMask);
}

void Lower::fill_mask_words() {
    while (!pending_mask_words_.empty()) {
        const Value id = pending_mask_words_.back();
        pending_mask_words_.pop_back();
        const std::pair<V, V> words = mask_phi_words_.at(id);
        const auto location = phi_at_.at(id);
        const std::size_t block = location.first;
        const Phi& phi = x_->p->blocks[block].phis[location.second];
        const std::size_t offset = x_->p->blocks[block].offset;
        std::vector<std::pair<V, std::size_t>> lo_in;
        std::vector<std::pair<V, std::size_t>> hi_in;
        for (std::size_t n = 0; n < x_->p->blocks[block].preds.size(); ++n) {
            const std::size_t q = x_->p->blocks[block].preds[n];
            if (!tail_[q]) continue;
            const std::size_t qb = edges_.contains({q, block}) ? edges_.at({q, block}) : *tail_[q];
            if (n >= phi.args.size()) fail("a lane-mask join of an untracked mask");
            const Value arg = phi.args[n];
            if (arg == id) fail("a lane mask whose lanes depend on its own value");
            if (!(x_->st->masks.contains(arg) || x_->st->highs.contains(arg)))
                fail("a lane-mask join of an untracked mask");
            const std::pair<V, V> incoming =
                stop_at(offset, [&]() { return mask_pair(arg); });
            lo_in.emplace_back(incoming.first, qb);
            hi_in.emplace_back(incoming.second, qb);
        }
        s_.m.set_phi(words.first, lo_in);
        s_.m.set_phi(words.second, hi_in);
    }
}

V Lower::operand(const IrInst& inst, const Operand& op) {
    if (const auto* scalar = std::get_if<ScalarOperand>(&op)) {
        if (scalar->code == kNull) return s_.m.i32(0);
        const std::optional<Value> value = inst.read(Reg::s(scalar->code));
        if (!value) fail("an unread operand");
        return i32_of(*value);
    }
    if (const auto* vector = std::get_if<VectorOperand>(&op)) {
        const std::optional<Value> value = inst.read(Reg::v(vector->reg));
        if (!value) fail("an unread operand");
        return i32_of(*value);
    }
    if (const auto* constant = std::get_if<ConstantOperand>(&op)) {
        const std::optional<std::uint32_t> value = inline_constant(constant->code);
        if (!value) fail("a special operand (vccz, execz, scc, lds_direct)");
        return s_.m.i32(*value);
    }
    if (const auto* literal = std::get_if<LiteralOperand>(&op)) return s_.m.i32(literal->value);
    fail("an attribute operand");
}

V Lower::as_float(V x) { return s_.m.cast(cur_, Cast::Bitcast, x, s_.t.f32); }

V Lower::as_int(V f) { return s_.m.cast(cur_, Cast::Bitcast, f, s_.t.i32); }

V Lower::max_min(bool max, V a, V b) {
    const std::size_t c = cur_;
    const V r = binary(max ? 35 : 36, a, b);
    const V ia = as_int(a);
    const V ib = as_int(b);
    V either = s_.m.bin(c, Bin::Or, ia, ib);
    const V magnitude = s_.m.i32(0x7FFF'FFFFu);
    either = s_.m.bin(c, Bin::And, either, magnitude);
    const V zero = s_.m.i32(0);
    const V zeros = s_.m.cmp(c, pred::kEq, either, zero);
    const V sign = s_.m.bin(c, max ? Bin::And : Bin::Or, ia, ib);
    const V signed_zero = as_float(sign);
    return s_.m.select(c, zeros, signed_zero, r);
}

V Lower::f16_operand(const IrInst& inst, const Operand& op) {
    if (const auto* constant = std::get_if<ConstantOperand>(&op);
        constant && constant->code >= 240 && constant->code <= 248)
        return s_.m.i32(kHalfConstants[constant->code - 240]);
    return operand(inst, op);
}

std::optional<std::pair<std::uint8_t, bool>> Lower::sdwa_src(const IrInst& inst, std::size_t k) {
    const auto* valu = std::get_if<Valu>(&inst.fields);
    if (valu == nullptr || !valu->sdwa) return std::nullopt;
    if (k >= valu->sdwa->src_sel.size()) return std::nullopt;
    const std::uint8_t sel = valu->sdwa->src_sel[k];
    const bool sext = ((valu->sdwa->sext >> k) & 1) != 0;
    if (sel >= 6) return std::nullopt;
    return std::pair<std::uint8_t, bool>{sel, sext};
}

V Lower::select_bits(V x, std::uint8_t sel, bool sext) {
    const std::size_t c = cur_;
    std::uint32_t width = 8;
    std::uint32_t at = 8u * sel;
    if (sel >= 4) {
        width = 16;
        at = 16u * static_cast<std::uint32_t>(sel - 4);
    }
    const V left = s_.m.i32(32 - width - at);
    const V t = s_.m.bin(c, Bin::Shl, x, left);
    const V right = s_.m.i32(32 - width);
    return s_.m.bin(c, sext ? Bin::AShr : Bin::LShr, t, right);
}

V Lower::fsrc(const IrInst& inst, std::size_t k) {
    const auto* valu = std::get_if<Valu>(&inst.fields);
    if (valu == nullptr) std::abort();
    if (k >= valu->src.size()) fail("a missing operand");
    const Operand& op = valu->src[k];
    const std::optional<std::pair<std::uint8_t, bool>> selected = sdwa_src(inst, k);
    V f;
    if (selected) {
        const V x = operand(inst, op);
        f = as_float(select_bits(x, selected->first, selected->second));
    } else if (k == 0 && src0_) {
        f = as_float(*src0_);
    } else if (const auto* vector = std::get_if<VectorOperand>(&op)) {
        const std::optional<Value> value = inst.read(Reg::v(vector->reg));
        if (!value) fail("an unread operand");
        const auto found = floats_.find(*value);
        f = found != floats_.end() ? found->second : as_float(i32_of(*value));
    } else if (const auto* scalar = std::get_if<ScalarOperand>(&op); scalar && scalar->code != kNull) {
        const std::optional<Value> value = inst.read(Reg::s(scalar->code));
        if (!value) fail("an unread operand");
        const auto found = floats_.find(*value);
        f = found != floats_.end() ? found->second : as_float(i32_of(*value));
    } else {
        f = as_float(operand(inst, op));
    }
    if ((valu->abs >> k & 1) != 0) f = unary(6, f);
    if ((valu->neg >> k & 1) != 0) {
        const V z = s_.m.f32(0x8000'0000u);
        f = s_.m.bin(cur_, Bin::Sub, z, f);
    }
    return f;
}

V Lower::src(const IrInst& inst, std::size_t k) {
    const auto* valu = std::get_if<Valu>(&inst.fields);
    if (valu == nullptr) std::abort();
    if (k >= valu->src.size()) fail("a missing operand");
    const Operand& op = valu->src[k];
    if ((valu->abs >> k & 1) != 0 || (valu->neg >> k & 1) != 0)
        fail("integer operand modifiers");
    if (src0_ && k == 0) return *src0_;
    const V x = operand(inst, op);
    const std::optional<std::pair<std::uint8_t, bool>> selected = sdwa_src(inst, k);
    if (selected) return select_bits(x, selected->first, selected->second);
    return x;
}

V Lower::unary(std::uint32_t op, V f) {
    return s_.op(cur_, "unary.f32", Attr::ReadNone, s_.t.f32, op, {f});
}

V Lower::binary(std::uint32_t op, V a, V b) {
    return s_.op(cur_, "binary.f32", Attr::ReadNone, s_.t.f32, op, {a, b});
}

V Lower::fmad(V a, V b, V c) {
    return s_.op(cur_, "tertiary.f32", Attr::ReadNone, s_.t.f32, 46, {a, b, c});
}

V Lower::f16_toward_zero(V f) {
    const std::size_t c = cur_;
    const Ty i32t = s_.t.i32;
    const V h = s_.op(c, "legacyF32ToF16", Attr::ReadNone, i32t, 130, {f});
    const V back = s_.op(c, "legacyF16ToF32", Attr::ReadNone, s_.t.f32, 131, {h});
    const V fa = unary(6, f);
    const V ba = unary(6, back);
    const V away = s_.m.cmp(c, pred::kOgt, ba, fa);
    const V one = s_.m.i32(1);
    const V stepped = s_.m.bin(c, Bin::Sub, h, one);
    return s_.m.select(c, away, stepped, h);
}

V Lower::bfe(V a, V off, V width, bool is_signed) {
    const std::size_t c = cur_;
    const V k31 = s_.m.i32(31);
    const V k32 = s_.m.i32(32);
    const V zero = s_.m.i32(0);
    const V one = s_.m.i32(1);
    const V ones = s_.m.i32(UINT32_MAX);
    off = s_.m.bin(c, Bin::And, off, k31);
    const Bin shift = is_signed ? Bin::AShr : Bin::LShr;
    const V shifted = s_.m.bin(c, shift, a, off);
    const V full = s_.m.cmp(c, pred::kIuge, width, k32);
    const V w = s_.m.bin(c, Bin::And, width, k31);
    V mask = s_.m.bin(c, Bin::Shl, one, w);
    mask = s_.m.bin(c, Bin::Sub, mask, one);
    mask = s_.m.select(c, full, ones, mask);
    const V r = s_.m.bin(c, Bin::And, shifted, mask);
    if (!is_signed) return r;
    V left = s_.m.bin(c, Bin::Sub, k32, w);
    left = s_.m.bin(c, Bin::And, left, k31);
    V t = s_.m.bin(c, Bin::Shl, r, left);
    t = s_.m.bin(c, Bin::AShr, t, left);
    const V is0 = s_.m.cmp(c, pred::kEq, w, zero);
    const V keep = s_.m.bin(c, Bin::Or, full, is0);
    return s_.m.select(c, keep, r, t);
}

V Lower::med3(V a, V b, V third) {
    const std::size_t c = cur_;
    const V lo = binary(36, a, b);
    const V hi = binary(35, a, b);
    const V t = binary(36, hi, third);
    const V median = binary(35, lo, t);
    const V least = binary(36, lo, third);
    const V ab = s_.m.cmp(c, pred::kUno, a, b);
    const V nan3 = s_.m.cmp(c, pred::kUno, third, third);
    const V nan = s_.m.bin(c, Bin::Or, ab, nan3);
    return s_.m.select(c, nan, least, median);
}

V Lower::fract(V a, float threshold, float below_one) {
    const std::size_t c = cur_;
    const V r = unary(22, a);
    const V limit = s_.m.f32(std::bit_cast<std::uint32_t>(threshold));
    const V k = s_.m.f32(std::bit_cast<std::uint32_t>(below_one));
    const V over = s_.m.cmp(c, pred::kOge, r, limit);
    return s_.m.select(c, over, k, r);
}

V Lower::trig_turns(V x, bool cosine) {
    const std::size_t c = cur_;
    const V zero = s_.m.f32(0);
    const V whole = unary(26, x);
    V part = s_.m.bin(c, Bin::Sub, x, whole);
    const V is_zero = s_.m.cmp(c, pred::kOeq, x, zero);
    part = s_.m.select(c, is_zero, x, part);
    const V k = s_.m.f32(std::bit_cast<std::uint32_t>(kTwoPi));
    const V angle = s_.m.bin(c, Bin::Mul, part, k);
    const V r = unary(cosine ? 12 : 13, angle);
    const float at_value = cosine ? 0.25f : 0.5f;
    const V at = s_.m.f32(std::bit_cast<std::uint32_t>(at_value));
    const V size = unary(6, part);
    const V exact = s_.m.cmp(c, pred::kOeq, size, at);
    return s_.m.select(c, exact, zero, r);
}

V Lower::mul_hi(V a, V b, bool is_signed) {
    const std::size_t c = cur_;
    const Ty i64t = s_.m.int_type(64);
    const Cast ext = is_signed ? Cast::SExt : Cast::ZExt;
    a = s_.m.cast(c, ext, a, i64t);
    b = s_.m.cast(c, ext, b, i64t);
    const V m = s_.m.bin(c, Bin::Mul, a, b);
    const V k32 = s_.m.int_const(i64t, 32);
    const V hi = s_.m.bin(c, Bin::LShr, m, k32);
    s_.flags |= feature::kInt64;
    return s_.m.cast(c, Cast::Trunc, hi, s_.t.i32);
}

V Lower::legacy_mul(V a, V b) {
    const std::size_t c = cur_;
    const V m = s_.m.bin(c, Bin::Mul, a, b);
    const V zero = s_.m.f32(0);
    const V az = s_.m.cmp(c, pred::kOeq, a, zero);
    const V bz = s_.m.cmp(c, pred::kOeq, b, zero);
    const V z = s_.m.bin(c, Bin::Or, az, bz);
    return s_.m.select(c, z, zero, m);
}

V Lower::boolean(Bool b, std::size_t at) {
    if (b == kTrue || b == kFalse) return s_.m.boolean(b == kTrue);
    const auto key = std::pair<std::size_t, std::uint32_t>{at, b.index};
    const auto found = bools_.find(key);
    if (found != bools_.end()) return found->second;
    const std::optional<std::tuple<Atom, Bool, Bool>> node = x_->st->bools.node(b);
    if (!node) std::abort();
    const Atom& atom_value = std::get<0>(*node);
    const Bool lo = std::get<1>(*node);
    const Bool hi = std::get<2>(*node);
    const V a = atom(atom_value, at);
    V v;
    if (lo == kFalse && hi == kTrue) {
        v = a;
    } else if (lo == kTrue && hi == kFalse) {
        const V t = s_.m.boolean(true);
        v = s_.m.bin(at, Bin::Xor, a, t);
    } else if (lo == kFalse) {
        v = s_.m.bin(at, Bin::And, a, boolean(hi, at));
    } else if (hi == kTrue) {
        v = s_.m.bin(at, Bin::Or, a, boolean(lo, at));
    } else {
        const V l = boolean(lo, at);
        const V h = boolean(hi, at);
        v = s_.m.select(at, a, h, l);
    }
    bools_[key] = v;
    return v;
}

V Lower::atom(const Atom& atom_value, std::size_t at) {
    switch (atom_value.kind) {
        case Atom::Kind::Lane: {
            const auto found = lane_bits_.find(atom_value.value);
            if (found != lane_bits_.end()) return found->second;
            const auto reason = lane_fail_.find(atom_value.value);
            if (reason != lane_fail_.end()) fail(reason->second);
            fail("a lane mask from a carry");
        }
        case Atom::Kind::Uniform: {
            const Val val = get(atom_value.value);
            if (val.kind == Val::Kind::Bit) return val.value;
            if (val.kind == Val::Kind::I32) {
                const V z = s_.m.i32(0);
                return s_.m.cmp(at, pred::kNe, val.value, z);
            }
            fail(val.why);
        }
        case Atom::Kind::Phi: {
            const Value id = atom_value.value;
            const auto known = mask_phis_.find(id);
            if (known != mask_phis_.end()) return known->second;
            const auto location = phi_at_.find(id);
            if (location == phi_at_.end()) fail("a lane-mask join without a phi");
            const std::size_t b = location->second.first;
            const std::size_t i = location->second.second;
            if (!block_[b]) fail("an unreachable join");
            const V v = s_.m.phi(*block_[b], s_.t.i1);
            mask_phis_[id] = v;
            pending_masks_.emplace_back(v, b, i);
            return v;
        }
        case Atom::Kind::Quad:
            return s_.m.boolean(true);
        case Atom::Kind::Opaque: {
            if (x_->native->stage == HwStage::Gs) return s_.m.boolean(true);
            if (!console_lanes_) fail("an opaque lane mask");
            std::optional<Val> hi;
            if (const std::optional<Value> high = pair_high(atom_value.value)) hi = get(*high);
            const Val lo = get(atom_value.value);
            const auto word = [](const Val& w) -> V {
                if (w.kind != Val::Kind::I32) fail("an opaque lane mask");
                return w.value;
            };
            const V low_word = word(lo);
            std::optional<V> high_word;
            if (hi) high_word = word(*hi);
            return lane_bit(at, low_word, high_word);
        }
        case Atom::Kind::Constant: {
            if (x_->native->stage == HwStage::Gs) return s_.m.boolean(true);
            if (!console_lanes_) fail("a constant lane mask");
            const V lo = s_.m.i32(static_cast<std::uint32_t>(atom_value.constant));
            const V hi = s_.m.i32(static_cast<std::uint32_t>(atom_value.constant >> 32));
            return lane_bit(at, lo, hi);
        }
    }
    fail("an opaque lane mask");
}

V Lower::lane_bit(std::size_t at, V lo, std::optional<V> hi) {
    const V lane = s_.op(at, "waveGetLaneIndex", Attr::ReadOnly, s_.t.i32, 111, {});
    const V k31 = s_.m.i32(31);
    const V k32 = s_.m.i32(32);
    const V one = s_.m.i32(1);
    const V z = s_.m.i32(0);
    V word = lo;
    if (hi) {
        const V low = s_.m.cmp(at, pred::kIult, lane, k32);
        word = s_.m.select(at, low, lo, *hi);
    }
    const V sh = s_.m.bin(at, Bin::And, lane, k31);
    V t = s_.m.bin(at, Bin::LShr, word, sh);
    t = s_.m.bin(at, Bin::And, t, one);
    return s_.m.cmp(at, pred::kNe, t, z);
}

std::optional<Value> Lower::pair_high(Value lo) const {
    if (!x_->native->wave64) return std::nullopt;
    const Program& p = *x_->p;
    const auto pick = [](Reg reg) -> std::optional<Reg> {
        if (reg.kind != Reg::Kind::S) return std::nullopt;
        return Reg::s(static_cast<std::uint8_t>(reg.code + 1));
    };
    for (const auto& entry : p.inputs) {
        if (!(entry.second == lo)) continue;
        const std::optional<Reg> high = pick(entry.first);
        if (!high) return std::nullopt;
        for (const auto& other : p.inputs)
            if (other.first == *high) return other.second;
        return std::nullopt;
    }
    const auto location = phi_at_.find(lo);
    if (location != phi_at_.end()) {
        const std::size_t b = location->second.first;
        const std::size_t i = location->second.second;
        const std::optional<Reg> high = pick(p.blocks[b].phis[i].reg);
        if (!high) return std::nullopt;
        for (const Phi& phi : p.blocks[b].phis)
            if (phi.reg == *high) return phi.value;
        return std::nullopt;
    }
    const auto def = def_block_.find(lo);
    if (def == def_block_.end()) return std::nullopt;
    for (const IrInst& inst : p.blocks[def->second].insts) {
        bool has = false;
        for (const auto& entry : inst.writes)
            if (entry.second == lo) has = true;
        if (!has) continue;
        Reg reg;
        bool found = false;
        for (const auto& entry : inst.writes) {
            if (!(entry.second == lo)) continue;
            reg = entry.first;
            found = true;
        }
        if (!found) return std::nullopt;
        const std::optional<Reg> high = pick(reg);
        if (!high) return std::nullopt;
        return inst.write(*high);
    }
    return std::nullopt;
}

std::optional<V> Lower::guard(std::size_t b, std::size_t i) {
    const std::optional<Bool>& entry = x_->st->guards[b][i];
    if (!entry) return std::nullopt;
    if (x_->native->stage == HwStage::Gs) {
        try {
            return boolean(*entry, cur_);
        } catch (const WhyError&) {
            return std::nullopt;
        }
    }
    return boolean(*entry, cur_);
}

void Lower::set(std::optional<Value> v, Val val) {
    if (v) vals_[*v] = val;
}

void Lower::write(const IrInst& inst, Reg reg, V x, std::optional<V> guard_value) {
    const std::optional<std::pair<std::uint8_t, std::uint8_t>> sd = sdwa_dst(inst);
    if (sd && reg.kind == Reg::Kind::V) x = place_bits(inst, reg, x, sd->first, sd->second);
    write_placed(inst, reg, x, guard_value);
}

void Lower::write_placed(const IrInst& inst, Reg reg, V x, std::optional<V> guard_value) {
    const std::optional<Value> target = inst.write(reg);
    if (!target) return;
    V value = x;
    if (guard_value && reg.kind == Reg::Kind::V) {
        V old = s_.m.i32(0);
        if (const std::optional<Value> read = inst.read(reg)) {
            try {
                old = i32_of(*read);
            } catch (const WhyError&) {
                old = s_.m.i32(0);
            }
        }
        value = s_.m.select(cur_, *guard_value, x, old);
    }
    vals_[*target] = Val::i32(value);
}

std::optional<std::pair<std::uint8_t, std::uint8_t>> Lower::sdwa_dst(const IrInst& inst) {
    const auto* valu = std::get_if<Valu>(&inst.fields);
    if (valu == nullptr || !valu->sdwa) return std::nullopt;
    if (valu->sdwa->dst_sel >= 6) return std::nullopt;
    return std::pair<std::uint8_t, std::uint8_t>{valu->sdwa->dst_sel, valu->sdwa->dst_unused};
}

V Lower::place_bits(const IrInst& inst, Reg reg, V x, std::uint8_t sel, std::uint8_t unused) {
    const std::size_t c = cur_;
    std::uint32_t width = 8;
    std::uint32_t at = 8u * sel;
    if (sel >= 4) {
        width = 16;
        at = 16u * static_cast<std::uint32_t>(sel - 4);
    }
    const V left = s_.m.i32(32 - width);
    const V t = s_.m.bin(c, Bin::Shl, x, left);
    const V right = s_.m.i32(32 - width - at);
    if (unused == 1) return s_.m.bin(c, Bin::AShr, t, right);
    const V field = s_.m.bin(c, Bin::LShr, t, right);
    if (unused != 2) return field;
    V old = s_.m.i32(0);
    if (const std::optional<Value> read = inst.read(reg)) {
        try {
            old = i32_of(*read);
        } catch (const WhyError&) {
            old = s_.m.i32(0);
        }
    }
    const std::uint32_t keep_bits =
        ~(static_cast<std::uint32_t>((std::uint64_t{1} << width) - 1) << at);
    const V keep = s_.m.i32(keep_bits);
    old = s_.m.bin(c, Bin::And, old, keep);
    return s_.m.bin(c, Bin::Or, old, field);
}

void Lower::write_f(const IrInst& inst, V f, std::optional<V> guard_value) {
    const auto* valu = std::get_if<Valu>(&inst.fields);
    if (valu == nullptr) std::abort();
    V value = f;
    if (valu->omod != 0) {
        const float factors[4] = {1.0f, 2.0f, 4.0f, 0.5f};
        const std::uint32_t k = valu->omod;
        if (k > 3) fail("an output modifier");
        const V factor = s_.m.f32(std::bit_cast<std::uint32_t>(factors[k]));
        value = s_.m.bin(cur_, Bin::Mul, value, factor);
    }
    if (valu->clamp) value = unary(7, value);
    if (!valu->dst) fail("a float result outside a VGPR");
    const auto* dst = std::get_if<VectorOperand>(&*valu->dst);
    if (dst == nullptr) fail("a float result outside a VGPR");
    const std::uint8_t reg = dst->reg;
    const V x = as_int(value);
    write(inst, Reg::v(reg), x, guard_value);
    if (!guard_value && !sdwa_dst(inst)) {
        if (const std::optional<Value> target = inst.write(Reg::v(reg))) floats_[*target] = value;
    }
}

void Lower::write_i(const IrInst& inst, V x, std::optional<V> guard_value) {
    const auto* valu = std::get_if<Valu>(&inst.fields);
    if (valu == nullptr) std::abort();
    if (valu->clamp || valu->omod != 0) fail("integer output modifiers");
    if (!valu->dst) fail("a result without a register");
    if (const auto* vector = std::get_if<VectorOperand>(&*valu->dst)) {
        write(inst, Reg::v(vector->reg), x, guard_value);
        return;
    }
    if (const auto* scalar = std::get_if<ScalarOperand>(&*valu->dst)) {
        write(inst, Reg::s(scalar->code), x, std::nullopt);
        return;
    }
    fail("a result without a register");
}

std::optional<std::pair<std::size_t, std::size_t>> Lower::begin_guarded(std::optional<V> guard_value) {
    if (!guard_value) return std::nullopt;
    const std::size_t then_block = s_.m.new_block();
    const std::size_t join = s_.m.new_block();
    s_.m.terminate(cur_, Term::cond_br(*guard_value, then_block, join));
    const std::size_t head = cur_;
    cur_ = then_block;
    return std::pair<std::size_t, std::size_t>{head, join};
}

std::vector<V> Lower::end_guarded(std::optional<std::pair<std::size_t, std::size_t>> at,
                                  const std::vector<std::pair<V, V>>& values) {
    if (!at) {
        std::vector<V> out;
        for (const auto& entry : values) out.push_back(entry.first);
        return out;
    }
    const std::size_t head = at->first;
    const std::size_t join = at->second;
    const std::size_t then_block = cur_;
    s_.m.terminate(then_block, Term::br(join));
    cur_ = join;
    std::vector<V> out;
    for (const auto& entry : values) {
        const Ty ty = s_.m.type_of(entry.first);
        const V phi = s_.m.phi(join, ty);
        s_.m.set_phi(phi, {{entry.first, then_block}, {entry.second, head}});
        out.push_back(phi);
    }
    return out;
}

void Lower::cross_lane(bool quad) {
    if (s_.mesh) fail("a cross-lane operation in a mesh shader");
    if (!s_.threads && !(quad && s_.pixel)) fail("a cross-lane operation outside compute");
    s_.flags |= feature::kWaveOps;
}

Val Lower::thread_id(std::uint8_t c) {
    if (!console_lanes_ || !s_.wave_size) return compute_id("threadIdInGroup.i32", 95, c);
    const std::array<std::uint32_t, 3> threads = *s_.threads;
    const std::uint32_t x = threads[0];
    const std::uint32_t y = threads[1];
    const std::size_t e = s_.entry;
    const Ty i32t = s_.t.i32;
    const V flat = s_.op(e, "flattenedThreadIdInGroup.i32", Attr::ReadNone, i32t, 96, {});
    const V first = s_.op(e, "waveReadLaneFirst.i32", Attr::NoUnwind, i32t, 118, {flat});
    const std::uint32_t wave_size = *s_.wave_size;
    const V mask = s_.m.i32(~(wave_size - 1));
    const V base = s_.m.bin(e, Bin::And, first, mask);
    const V lane = lane_index();
    const V f = s_.m.bin(e, Bin::Add, base, lane);
    s_.flags |= feature::kWaveOps;
    const V xk = s_.m.i32(x);
    const V yk = s_.m.i32(y);
    const V xy = s_.m.i32(x * y);
    if (c == 0) return Val::i32(s_.m.bin(e, Bin::URem, f, xk));
    if (c == 1) {
        const V t = s_.m.bin(e, Bin::UDiv, f, xk);
        return Val::i32(s_.m.bin(e, Bin::URem, t, yk));
    }
    return Val::i32(s_.m.bin(e, Bin::UDiv, f, xy));
}

void Lower::mesh_prologue(Mesh mesh) {
    const std::uint32_t threads = (*s_.threads)[0];
    if (mesh.topology != 2) fail("a mesh shader not drawing triangles");
    if (threads > 128 || mesh.max_vertices > 256 || mesh.max_primitives > 256)
        fail("a mesh shader beyond the host's limits");
    const std::size_t e = s_.entry;
    const Ty void_type = s_.t.void_type;
    const V v = s_.m.i32(mesh.max_vertices);
    const V p = s_.m.i32(mesh.max_primitives);
    s_.op(e, "setMeshOutputCounts", Attr::NoUnwind, void_type, 168, {v, p});
    const V flat = mesh_thread();
    std::optional<V> guard_value;
    if (threads > mesh.max_primitives) guard_value = s_.m.cmp(e, pred::kIult, flat, p);
    const auto at = begin_guarded(guard_value);
    const V z = s_.m.i32(0);
    s_.op(cur_, "emitIndices", Attr::NoUnwind, void_type, 169, {flat, z, z, z});
    end_guarded(at, {});
}

V Lower::mesh_thread() {
    if (flat_) return *flat_;
    const std::size_t e = s_.entry;
    const V f = s_.op(e, "flattenedThreadIdInGroup.i32", Attr::ReadNone, s_.t.i32, 96, {});
    flat_ = f;
    return f;
}

std::uint32_t Lower::wave() const { return x_->native->wave64 ? 64 : 32; }

V Lower::lane_index() {
    if (s_.mesh) {
        const V f = mesh_thread();
        const V m = s_.m.i32(wave() - 1);
        return s_.m.bin(s_.entry, Bin::And, f, m);
    }
    return s_.op(cur_, "waveGetLaneIndex", Attr::ReadOnly, s_.t.i32, 111, {});
}

V Lower::quad_pick(V x, V src) {
    s_.flags |= feature::kWaveOps;
    const std::size_t c = cur_;
    const V three = s_.m.i32(3);
    const V q = s_.m.bin(c, Bin::And, src, three);
    V r = x;
    for (std::uint32_t n = 0; n < 4; ++n) {
        const V kn = s_.m.i32(n);
        const V read = s_.op(c, "quadReadLaneAt.i32", Attr::NoUnwind, s_.t.i32, 122, {x, kn});
        const V is = s_.m.cmp(c, pred::kEq, q, kn);
        r = s_.m.select(c, is, read, r);
    }
    return r;
}

V Lower::read_lane(V x, V lane) {
    return s_.op(cur_, "waveReadLaneAt.i32", Attr::NoUnwind, s_.t.i32, 117, {x, lane});
}

V Lower::float_of(Value v) {
    const auto found = floats_.find(v);
    if (found != floats_.end()) return found->second;
    return as_float(i32_of(v));
}

std::vector<V> Lower::vgprs(const IrInst& inst, const Operand& op, std::uint8_t n) {
    const auto* vector = std::get_if<VectorOperand>(&op);
    if (vector == nullptr) fail("a vector operand");
    std::vector<V> out;
    out.reserve(n);
    for (std::uint8_t k = 0; k < n; ++k) {
        const std::optional<Value> value = inst.read(Reg::v(static_cast<std::uint8_t>(vector->reg + k)));
        if (!value) fail("an unread operand");
        out.push_back(i32_of(*value));
    }
    return out;
}

void Lower::inst(std::size_t b, std::size_t i) {
    const IrInst& inst_value = x_->p->blocks[b].insts[i];
    const std::size_t offset = inst_value.machine.offset;
    const std::string_view name = inst_value.name();
    switch (mask_data_) {
        case MaskData::Console:
        case MaskData::Host:
            mask_ok_ = true;
            break;
        case MaskData::None:
            mask_ok_ = false;
            break;
    }
    const bool holds_flat = std::holds_alternative<FlatFields>(inst_value.fields);
    const bool holds_ds = std::holds_alternative<DsFields>(inst_value.fields);
    const bool effect =
        std::holds_alternative<ExpFields>(inst_value.fields) ||
        (holds_flat && !name.starts_with("global_load_")) ||
        name.find("store") != std::string_view::npos ||
        name.find("atomic") != std::string_view::npos || name == "s_barrier" ||
        (name == "s_sendmsg" && x_->native->stage == HwStage::Gs) ||
        (holds_ds && x_->native->stage == HwStage::Gs) ||
        (holds_ds && !(name.starts_with("ds_read") || name.starts_with("ds_swizzle") ||
                       name.find("permute") != std::string_view::npos));
    try {
        if (std::holds_alternative<SopFields>(inst_value.fields)) {
            sop(inst_value);
        } else if (std::holds_alternative<SoppFields>(inst_value.fields)) {
            if (name == "s_sendmsg" || name == "s_sendmsghalt") {
                geometry_message(inst_value, b, i);
            } else if (name == "s_barrier") {
                if (no_barrier_.contains(b)) return;
                const std::uint32_t mode = writes_uavs_ ? (1u | 2u | 8u) : (1u | 8u);
                const V k = s_.m.i32(mode);
                s_.op(cur_, "barrier", Attr::NoDuplicate, s_.t.void_type, 80, {k});
            }
        } else if (std::holds_alternative<SmemFields>(inst_value.fields)) {
            smem(inst_value, Site{b, i});
        } else if (std::holds_alternative<Valu>(inst_value.fields)) {
            if (name.starts_with("v_interp_")) interp(inst_value, Site{b, i});
            else valu(inst_value, b, i);
        } else if (std::holds_alternative<VintrpFields>(inst_value.fields)) {
            interp(inst_value, Site{b, i});
        } else if (std::holds_alternative<ImageFields>(inst_value.fields)) {
            image(inst_value, b, i);
        } else if (std::holds_alternative<BufferFields>(inst_value.fields)) {
            buffer(inst_value, b, i);
        } else if (std::holds_alternative<ExpFields>(inst_value.fields)) {
            export_(b, i);
        } else if (std::holds_alternative<DsFields>(inst_value.fields)) {
            ds(inst_value, b, i);
        } else if (std::holds_alternative<FlatFields>(inst_value.fields)) {
            if (name.starts_with("global_")) flat(inst_value, b, i);
            else fail("flat memory");
        }
    } catch (const WhyError& error) {
        if (effect) throw Stop::failure(offset, error.why);
        for (const auto& entry : inst_value.writes) vals_[entry.second] = Val::missing(error.why);
    }
}

bool Lower::is_mask(std::optional<Value> v) const {
    return v && x_->st->masks.contains(*v);
}

void Lower::exit(std::size_t b) {
    const IrBlock& block = x_->p->blocks[b];
    const std::size_t offset = block.insts.empty() ? block.offset : block.insts.back().machine.offset;
    const LaneExit& lane_exit = x_->st->exits[b];
    switch (lane_exit.kind) {
        case LaneExit::Kind::Jump: {
            const std::size_t t = lane_exit.target;
            if (counters_.contains(t) && rpo_[t] <= rpo_[b]) {
                const V ok = count_iteration(t, pred::kIult);
                const std::size_t abort = abort_block();
                s_.m.terminate(cur_, Term::cond_br(ok, *block_[t], abort));
            } else {
                enter_loop(b, t, cur_);
                s_.m.terminate(cur_, Term::br(*block_[t]));
            }
            return;
        }
        case LaneExit::Kind::Branch: {
            std::size_t taken = lane_exit.target;
            std::size_t not_taken = lane_exit.not_taken;
            V cond;
            const std::optional<std::tuple<Atom, Bool, Bool>> node =
                x_->st->bools.node(lane_exit.when);
            if (node && std::get<1>(*node) == kTrue && std::get<2>(*node) == kFalse) {
                cond = stop_at(offset, [&]() { return atom(std::get<0>(*node), cur_); });
                std::swap(taken, not_taken);
            } else {
                cond = stop_at(offset, [&]() { return boolean(lane_exit.when, cur_); });
            }
            const auto back = [&](std::size_t to) {
                return counters_.contains(to) && rpo_[to] <= rpo_[b];
            };
            if (back(taken)) {
                const V ok = count_iteration(taken, pred::kIult);
                cond = s_.m.bin(cur_, Bin::And, cond, ok);
            } else if (back(not_taken)) {
                const V stop = count_iteration(not_taken, pred::kIuge);
                cond = s_.m.bin(cur_, Bin::Or, cond, stop);
            }
            const std::size_t t = edge(b, taken);
            const std::size_t f = edge(b, not_taken);
            const std::array<std::pair<std::size_t, std::size_t>, 2> targets = {
                std::pair<std::size_t, std::size_t>{taken, t},
                std::pair<std::size_t, std::size_t>{not_taken, f}};
            for (const auto& entry : targets) {
                if (back(entry.first)) continue;
                const std::size_t from = block_[entry.first] && *block_[entry.first] == entry.second
                                              ? cur_
                                              : entry.second;
                enter_loop(b, entry.first, from);
            }
            s_.m.terminate(cur_, Term::cond_br(cond, t, f));
            return;
        }
        case LaneExit::Kind::End: {
            if (s_.pixel && !exported_[b] && !has_done_export(b)) {
                const V t = s_.m.boolean(true);
                s_.op(cur_, "discard", Attr::NoUnwind, s_.t.void_type, 82, {t});
            }
            s_.m.terminate(cur_, Term::ret());
            return;
        }
        case LaneExit::Kind::Indirect:
            throw Stop::failure(offset, "an indirect jump");
    }
}

void Lower::enter_loop(std::size_t from, std::size_t to, std::size_t via) {
    (void)from;
    const auto found = counters_.find(to);
    if (found == counters_.end()) return;
    const V z = s_.m.i32(0);
    found->second.second.emplace_back(z, via);
}

V Lower::count_iteration(std::size_t to, std::uint8_t p) {
    const std::size_t c = cur_;
    const V phi = counters_.at(to).first;
    const V one = s_.m.i32(1);
    const V next = s_.m.bin(c, Bin::Add, phi, one);
    const V limit = s_.m.i32(loop_limit());
    counters_.at(to).second.emplace_back(next, c);
    return s_.m.cmp(c, p, next, limit);
}

std::size_t Lower::abort_block() {
    if (abort_) return *abort_;
    const std::size_t a = s_.m.new_block();
    if (s_.pixel) {
        const V t = s_.m.boolean(true);
        s_.op(a, "discard", Attr::NoUnwind, s_.t.void_type, 82, {t});
    }
    s_.m.terminate(a, Term::ret());
    abort_ = a;
    return a;
}

std::size_t Lower::edge(std::size_t from, std::size_t to) {
    const std::size_t target = *block_[to];
    std::size_t preds = 0;
    for (std::size_t q : x_->p->blocks[to].preds)
        if (block_[q]) ++preds;
    if (preds < 2 || rpo_[to] <= rpo_[from]) return target;
    const std::size_t e = s_.m.new_block();
    s_.m.terminate(e, Term::br(target));
    edges_[{from, to}] = e;
    return e;
}

bool Lower::has_partial_descriptor(Value value) const {
    if (!descriptor_addresses_.contains(value) && !phi_at_.contains(value)) return false;
    std::unordered_set<Value, ValueHash> visited;
    std::vector<Value> work{value};
    while (!work.empty()) {
        const Value v = work.back();
        work.pop_back();
        if (!visited.insert(v).second) continue;
        const auto field = descriptor_addresses_.find(v);
        if (field != descriptor_addresses_.end() && field->second.known_bits() != UINT32_MAX) return true;
        const auto at = phi_at_.find(v);
        if (at == phi_at_.end() || is_mask(v) || x_->st->highs.contains(v)) continue;
        const auto [b, i] = at->second;
        const Phi& phi = x_->p->blocks[b].phis[i];
        if (phi.reg.kind == Reg::Kind::S) work.insert(work.end(), phi.args.begin(), phi.args.end());
    }
    return false;
}

V Lower::masked_descriptor_value(Value value, std::uint32_t mask) {
    constexpr Why unknown_bits = "a descriptor's address or format read as data";
    const auto field = descriptor_addresses_.find(value);
    if (field != descriptor_addresses_.end()) {
        if ((mask & ~field->second.known_bits()) != 0) fail(unknown_bits);
        UserData data = field->second;
        if (data.source == UserData::Source::StructuredBufferHighWord && (mask & ~UserData::kBufferAddressHighMask) == 0)
            data.source = UserData::Source::ShaderBufferAddressHigh;
        return descriptor_address(data).value;
    }
    const auto at = phi_at_.find(value);
    if (at != phi_at_.end() && !is_mask(value) && !x_->st->highs.contains(value)) {
        const DescriptorMask key{value, mask};
        const auto failed = bad_masked_->find(key);
        if (failed != bad_masked_->end()) fail(failed->second);
        const auto [b, i] = at->second;
        if (block_[b] && x_->p->blocks[b].phis[i].reg.kind == Reg::Kind::S) {
            const auto [entry, inserted] = masked_descriptor_phis_.try_emplace(key, V{});
            if (inserted) {
                entry->second = s_.m.phi(*block_[b], s_.t.i32);
                pending_descriptor_phis_.emplace_back(value, mask, entry->second);
            }
            return entry->second;
        }
    }
    const Val val = get(value);
    if (val.kind != Val::Kind::I32) fail("an SCC joined with SGPR values");
    return val.value;
}

void Lower::fill_masked_descriptor_phis() {
    for (std::size_t i = 0; i < pending_descriptor_phis_.size(); ++i) {
        const auto [source, mask, result] = pending_descriptor_phis_[i];
        const auto [b, n] = phi_at_.at(source);
        const auto& block = x_->p->blocks[b];
        const Phi& phi = block.phis[n];
        std::vector<std::pair<V, std::size_t>> incoming;
        for (std::size_t j = 0; j < block.preds.size(); ++j) {
            const std::size_t q = block.preds[j];
            if (!tail_[q]) continue;
            const std::size_t qb = edges_.contains({q, b}) ? edges_.at({q, b}) : *tail_[q];
            try {
                incoming.emplace_back(masked_descriptor_value(phi.args[j], mask), qb);
            } catch (const WhyError& error) {
                throw Stop::retry_masked(source, mask, error.why);
            }
        }
        s_.m.set_phi(result, incoming);
    }
}

void Lower::fill_phis() {
    fill_masked_descriptor_phis();
    const Program& p = *x_->p;
    std::vector<Value> work;
    for (Value v : read_phis_) work.push_back(v);
    while (!work.empty()) {
        const Value v = work.back();
        work.pop_back();
        const auto at = phi_at_.find(v);
        if (at == phi_at_.end()) continue;
        const Phi& phi = p.blocks[at->second.first].phis[at->second.second];
        for (Value arg : phi.args)
            if (phi_at_.contains(arg) && read_phis_.insert(arg).second) work.push_back(arg);
    }
    std::vector<std::tuple<V, std::size_t, std::size_t>> data_phis = std::move(data_phis_);
    data_phis_.clear();
    for (const auto& entry : data_phis) {
        const V v = std::get<0>(entry);
        const std::size_t b = std::get<1>(entry);
        const std::size_t i = std::get<2>(entry);
        const Phi& phi = p.blocks[b].phis[i];
        const bool bit = phi.reg.kind == Reg::Kind::Scc;
        const bool read = read_phis_.contains(phi.value);
        std::vector<std::pair<V, std::size_t>> incoming;
        for (std::size_t n = 0; n < p.blocks[b].preds.size(); ++n) {
            const std::size_t q = p.blocks[b].preds[n];
            if (!tail_[q]) continue;
            const std::size_t qb = edges_.contains({q, b}) ? edges_.at({q, b}) : *tail_[q];
            const Value arg = phi.args[n];
            if (read && !bit && descriptor_addresses_.contains(arg) && !vals_.contains(arg)) {
                try {
                    (void)get(arg);
                } catch (const WhyError& error) {
                    throw Stop::retry(phi.value, error.why);
                }
            }
            const auto found = vals_.find(arg);
            V x;
            if (found != vals_.end() && found->second.kind == Val::Kind::I32 && !bit) {
                x = found->second.value;
            } else if (found != vals_.end() && found->second.kind == Val::Kind::Bit && bit) {
                x = found->second.value;
            } else if (found != vals_.end() && found->second.kind == Val::Kind::Bit && !bit &&
                       read) {
                throw Stop::retry(phi.value, "an SCC joined with SGPR values");
            } else if (!bit && read &&
                       (x_->st->masks.contains(arg) || x_->st->highs.contains(arg))) {
                x = stop_at(p.blocks[b].offset, [&]() { return mask_dword(arg); });
            } else if (found != vals_.end() && found->second.kind == Val::Kind::Missing && read) {
                throw Stop::retry(phi.value, found->second.why);
            } else if (read) {
                throw Stop::retry(phi.value, "a join of mismatched values");
            } else if (bit) {
                x = s_.m.boolean(false);
            } else {
                x = s_.m.i32(0);
            }
            incoming.emplace_back(x, qb);
        }
        s_.m.set_phi(v, incoming);
    }
    for (const auto& entry : counters_) s_.m.set_phi(entry.second.first, entry.second.second);
    counters_.clear();
    while (!pending_masks_.empty()) {
        const V v = std::get<0>(pending_masks_.back());
        const std::size_t b = std::get<1>(pending_masks_.back());
        const std::size_t i = std::get<2>(pending_masks_.back());
        pending_masks_.pop_back();
        const Phi& phi = p.blocks[b].phis[i];
        const std::size_t offset = p.blocks[b].offset;
        std::vector<std::pair<V, std::size_t>> incoming;
        for (std::size_t n = 0; n < p.blocks[b].preds.size(); ++n) {
            const std::size_t q = p.blocks[b].preds[n];
            if (!tail_[q]) continue;
            const std::size_t qb = edges_.contains({q, b}) ? edges_.at({q, b}) : *tail_[q];
            const auto mask = x_->st->masks.find(phi.args[n]);
            if (mask == x_->st->masks.end())
                throw Stop::failure(offset, "a lane-mask join of an untracked mask");
            const V x = stop_at(offset, [&]() { return boolean(mask->second, qb); });
            incoming.emplace_back(x, qb);
        }
        s_.m.set_phi(v, incoming);
    }
}

Lowered lower(const Parts& parts) {
    std::unordered_map<Value, Why, ValueHash> bad;
    std::map<DescriptorMask, Why> bad_masked;
    for (;;) {
        try {
            Lower lowering(parts, bad, bad_masked);
            return lowering.run();
        } catch (const Stop& stop) {
            if (stop.kind == Stop::Kind::Retry) {
                const bool inserted = stop.mask ? bad_masked.try_emplace(DescriptorMask{stop.value, *stop.mask}, stop.why).second
                                                : bad.try_emplace(stop.value, stop.why).second;
                if (!inserted)
                    throw ParseError(ErrorKind::Unsupported,
                                     "GFX10 code at 0x0: a phi that does not settle is not supported");
            } else {
                throw ParseError(ErrorKind::Unsupported, "GFX10 code at " + hex(stop.offset) + ": " +
                                                             std::string(stop.why) +
                                                             " is not supported");
            }
        }
    }
}

}  // namespace g6shim::shader::emit
