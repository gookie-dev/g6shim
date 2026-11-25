// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "g6shim/shader/emit/lower.hpp"
#include "g6shim/shader/emit/wave.hpp"

namespace g6shim::shader::emit {

using Why = std::string_view;

using IrInst = ::g6shim::shader::Inst;
using IrBlock = ::g6shim::shader::Block;

struct WhyError {
    Why why;
};

[[noreturn]] inline void fail(Why why) { throw WhyError{why}; }

struct Val {
    enum class Kind : std::uint8_t { I32, Bit, Missing };

    Kind kind = Kind::Missing;
    V value{};
    Why why{};

    static Val i32(V v) {
        Val out;
        out.kind = Kind::I32;
        out.value = v;
        return out;
    }
    static Val bit(V v) {
        Val out;
        out.kind = Kind::Bit;
        out.value = v;
        return out;
    }
    static Val missing(Why why) {
        Val out;
        out.kind = Kind::Missing;
        out.why = why;
        return out;
    }
};

struct Stop {
    enum class Kind : std::uint8_t { Retry, Fail };

    Kind kind = Kind::Fail;
    Value value{};
    Why why{};
    std::size_t offset = 0;

    static Stop retry(Value value, Why why) { return {Kind::Retry, value, why, 0}; }
    static Stop failure(std::size_t offset, Why what) { return {Kind::Fail, Value{}, what, offset}; }
};

enum class MaskData : std::uint8_t { Console, Host, None };

inline constexpr Why kLaneMask = "a lane mask read as data";
inline constexpr Why kLaneMaskElsewhere = "a lane mask holding lanes elsewhere read as data";
inline constexpr Why kQuadMask = "a whole-quad lane mask read as data";
inline constexpr Why kOpaqueMask = "an opaque lane mask read as data";

inline constexpr std::array<std::uint32_t, 9> kHalfConstants = {0x3800, 0xB800, 0x3C00, 0xBC00, 0x4000,
                                                               0xC000, 0x4400, 0xC400, 0x3118};

bool reads_mask_as_data(const Structured& st, const IrInst& inst);

std::optional<std::pair<std::uint8_t, std::uint8_t>> packed_half(
    const std::vector<SigElement>& elements, std::uint8_t a, std::optional<std::uint8_t> b,
    std::uint8_t h);

std::map<std::uint8_t, std::pair<bool, std::optional<std::uint8_t>>> half_outputs(
    const StageIo& io, const std::vector<SigElement>& outputs);

class Lower {
public:
    Lower(const Parts& parts, const std::unordered_map<Value, Why, ValueHash>& bad);

    Lowered run();

    std::vector<UserData> user_reads() const;

private:
    V code_dword(V byte);
    void flat(const IrInst& inst, std::size_t b, std::size_t i);
    void find_sections();
    void section_inst(std::size_t b, std::size_t i, std::size_t start);
    V shape(Shape shape);
    bool exported_out(std::size_t b) const;
    bool has_done_export(std::size_t b) const;
    void inputs();
    Val input(const Input& input);
    Val compute_id(std::string_view name, std::uint32_t opcode, std::uint8_t c);
    std::optional<std::pair<std::size_t, std::uint8_t>> find_input(std::uint8_t row,
                                                                   std::uint8_t channel);
    Val load_input(std::size_t id, std::uint8_t col);
    Val load_any_input(std::size_t id, std::uint8_t col,
                       std::optional<std::uint32_t> vertex = std::nullopt);
    std::optional<std::pair<std::size_t, std::uint8_t>> geometry_slot(std::vector<SigElement>& elements,
                                                                      std::uint32_t dword, bool input);
    void geometry_ring(const IrInst& inst, std::size_t b, std::size_t i);
    void geometry_message(const IrInst& inst, std::size_t b, std::size_t i);
    Val get(Value v);
    V i32_of(Value v);
    Val user(std::size_t param, std::uint32_t dword);
    V mask_dword(Value v);
    std::pair<V, V> mask_pair(Value v);
    std::size_t mask_site(Value lo);
    std::pair<V, V> active_ballot(Bool bits, std::size_t at);
    std::pair<V, V> combine_mask(Bool bits, std::size_t at);
    std::pair<V, V> atom_mask(const Atom& atom, std::size_t at);
    std::pair<V, V> mask_ones();
    std::pair<V, V> mask_bin(Bin op, std::pair<V, V> a, std::pair<V, V> b, std::size_t at);
    void fill_mask_words();
    V operand(const IrInst& inst, const Operand& op);
    V as_float(V x);
    V as_int(V f);
    V max_min(bool max, V a, V b);
    V f16_operand(const IrInst& inst, const Operand& op);
    static std::optional<std::pair<std::uint8_t, bool>> sdwa_src(const IrInst& inst, std::size_t k);
    V select_bits(V x, std::uint8_t sel, bool sext);
    V fsrc(const IrInst& inst, std::size_t k);
    V src(const IrInst& inst, std::size_t k);
    V unary(std::uint32_t op, V f);
    V binary(std::uint32_t op, V a, V b);
    V fmad(V a, V b, V c);
    V f16_toward_zero(V f);
    V bfe(V a, V off, V width, bool is_signed);
    V med3(V a, V b, V third);
    V fract(V a, float threshold, float below_one);
    V trig_turns(V x, bool cosine);
    V mul_hi(V a, V b, bool is_signed);
    V legacy_mul(V a, V b);
    V boolean(Bool b, std::size_t at);
    V atom(const Atom& atom, std::size_t at);
    V lane_bit(std::size_t at, V lo, std::optional<V> hi);
    std::optional<Value> pair_high(Value lo) const;
    std::optional<V> guard(std::size_t b, std::size_t i);
    void set(std::optional<Value> v, Val val);
    void write(const IrInst& inst, Reg reg, V x, std::optional<V> guard);
    void write_placed(const IrInst& inst, Reg reg, V x, std::optional<V> guard);
    static std::optional<std::pair<std::uint8_t, std::uint8_t>> sdwa_dst(const IrInst& inst);
    V place_bits(const IrInst& inst, Reg reg, V x, std::uint8_t sel, std::uint8_t unused);
    void write_f(const IrInst& inst, V f, std::optional<V> guard);
    void write_i(const IrInst& inst, V x, std::optional<V> guard);
    std::optional<std::pair<std::size_t, std::size_t>> begin_guarded(std::optional<V> guard);
    std::vector<V> end_guarded(std::optional<std::pair<std::size_t, std::size_t>> at,
                               const std::vector<std::pair<V, V>>& values);
    void cross_lane(bool quad);
    Val thread_id(std::uint8_t c);
    void mesh_prologue(Mesh mesh);
    V mesh_thread();
    std::uint32_t wave() const;
    V lane_index();
    V quad_pick(V x, V src);
    V read_lane(V x, V lane);
    V float_of(Value v);
    std::vector<V> vgprs(const IrInst& inst, const Operand& op, std::uint8_t n);
    void ds(const IrInst& inst, std::size_t b, std::size_t i);
    void inst(std::size_t b, std::size_t i);
    bool is_mask(std::optional<Value> v) const;
    void sop(const IrInst& inst);
    std::pair<V, V> add_sub_carry(std::size_t c, bool add, V a, V b, std::optional<V> cin);
    V scc_in(const IrInst& inst);
    void smem(const IrInst& inst, Site site);
    void descriptor_data(const IrInst& inst, Site site);
    bool defer_descriptor_address(Value value, const Source& src, const UsedResource& used,
                                  UserData::Source source);
    Val descriptor_address(UserData data);
    bool has_partial_descriptor(Value value) const;
    V masked_descriptor_value(Value value, std::uint32_t mask);
    void fill_masked_descriptor_phis();
    Val user_dword(std::uint32_t index);
    void code_smem(const IrInst& inst, Address a);
    V cbuffer_dword(V h, V a);
    std::vector<V> format_load(V h, V off, std::uint8_t format, std::uint32_t channels);
    std::pair<std::uint32_t, std::optional<V>> built_offset(const Source& src);
    const UsedResource* used_kind(const Source& src) const;
    V handle(Site site, const Source& src, bool sampler, std::pair<std::uint32_t, std::uint32_t> props);
    V lane_heap_index(const Source& src, bool sampler);
    V fetched_index(Site load, std::uint8_t first, bool sampler);
    V heap_index(Site load, std::uint8_t first, bool sampler);
    void valu(const IrInst& inst, std::size_t b, std::size_t i);
    std::pair<V, std::optional<V>> dpp(const IrInst& inst, const Dpp& dpp);
    void valu_op(const IrInst& inst, std::optional<V> guard);
    V src64(const IrInst& inst, std::size_t k);
    std::pair<V, V> split64(V x);
    V half_src(const IrInst& inst, std::size_t k);
    V half_fsrc(const IrInst& inst, std::size_t k);
    void write_half(const IrInst& inst, V x, std::optional<V> guard);
    bool valu16(const IrInst& inst, std::string_view name, std::optional<V> guard);
    void valu16_op(const IrInst& inst, std::string_view name, std::optional<V> guard,
                   std::span<const std::string_view> float1,
                   std::span<const std::string_view> float2,
                   std::span<const std::string_view> float3);
    V packed_src(const IrInst& inst, std::size_t k, bool high, bool float16);
    V cvt_norm16(const IrInst& inst, bool is_signed);
    void packed16(const IrInst& inst, std::string_view op, std::optional<V> guard);
    V integer(const IrInst& inst, std::string_view name);
    void compare(const IrInst& inst, std::string_view rest);
    void lane(const IrInst& inst, std::optional<bool> value);
    void lane_bits_for(const IrInst& inst, V r);
    void interp(const IrInst& inst, Site site);
    V low_half(const IrInst& inst, std::uint8_t reg, V x);
    void image(const IrInst& inst, std::size_t b, std::size_t i);
    std::array<V, 3> cube_direction(V s, V t, V face);
    void buffer(const IrInst& inst, std::size_t b, std::size_t i);
    void export_(std::size_t b, std::size_t i);
    void emit_indices(const Export& e, std::optional<V> guard);
    std::optional<std::uint8_t> unnamed_row() const;
    void exit(std::size_t b);
    void enter_loop(std::size_t from, std::size_t to, std::size_t via);
    V count_iteration(std::size_t to, std::uint8_t p);
    std::size_t abort_block();
    std::size_t edge(std::size_t from, std::size_t to);
    void fill_phis();

    Shader s_;
    const Parts* x_ = nullptr;
    const std::unordered_map<Value, Why, ValueHash>* bad_ = nullptr;
    std::unordered_map<Value, Why, ValueHash> lane_fail_;
    std::unordered_map<Value, Val, ValueHash> vals_;
    std::unordered_map<Value, V, ValueHash> floats_;
    std::vector<std::optional<std::size_t>> block_;
    std::size_t cur_ = 0;
    std::map<std::pair<std::size_t, std::uint32_t>, V> bools_;
    std::unordered_map<Value, V, ValueHash> lane_bits_;
    std::unordered_map<Value, std::pair<std::size_t, std::size_t>, ValueHash> phi_at_;
    std::unordered_map<Value, V, ValueHash> mask_phis_;
    std::vector<std::tuple<V, std::size_t, std::size_t>> pending_masks_;
    std::vector<std::tuple<V, std::size_t, std::size_t>> data_phis_;
    std::unordered_set<Value, ValueHash> read_phis_;
    std::unordered_map<Value, std::pair<std::size_t, std::uint32_t>, ValueHash> params_;
    std::unordered_map<Value, UserData, ValueHash> descriptor_addresses_;
    std::map<std::pair<Value, std::uint32_t>, V> masked_descriptor_phis_;
    std::vector<std::tuple<Value, std::uint32_t, V>> pending_descriptor_phis_;
    std::optional<V> user_data_;
    std::map<std::uint32_t, UserData> user_reads_;
    std::vector<std::array<std::uint32_t, 4>> samplers_;
    std::map<std::pair<Site, bool>, const Source*> descriptors_;
    std::unordered_map<Site, Address, SiteHash> scalar_;
    std::map<Site, const Export*> exports_;
    std::map<Site, const Interp*> interps_;
    std::map<std::tuple<std::size_t, Site, std::uint8_t, bool, std::uint32_t>, V> handles_;
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> edges_;
    std::vector<bool> exported_;
    std::vector<std::size_t> rpo_;
    std::vector<std::optional<std::size_t>> tail_;
    bool writes_uavs_ = false;
    std::optional<V> src0_;
    bool console_lanes_ = false;
    std::optional<V> flat_;
    std::unordered_set<std::size_t> no_barrier_;
    std::map<std::uint8_t, std::pair<bool, std::optional<std::uint8_t>>> half_outputs_;
    MaskData mask_data_ = MaskData::None;
    bool mask_ok_ = false;
    std::unordered_map<Value, std::pair<V, V>, ValueHash> ballots_;
    std::unordered_set<Value, ValueHash> balloting_;
    std::map<std::pair<std::uint32_t, std::size_t>, std::pair<V, V>> mask_words_;
    std::unordered_map<Value, std::pair<V, V>, ValueHash> mask_phi_words_;
    std::vector<Value> pending_mask_words_;
    std::unordered_map<Value, std::pair<V, V>, ValueHash> lane_ballots_;
    std::unordered_map<Value, std::size_t, ValueHash> def_block_;
    std::size_t cur_ir_ = 0;
    std::map<std::size_t, std::unordered_map<std::size_t, Section>> sections_;
    std::unordered_set<Value, ValueHash> section_out_;
    std::optional<std::tuple<V, std::uint32_t, std::uint32_t>> code_table_;
    std::map<std::size_t, std::pair<V, std::vector<std::pair<V, std::size_t>>>> counters_;
    std::optional<std::size_t> abort_;
};

template <typename F>
auto stop_at(std::size_t offset, F&& f) -> decltype(f()) {
    try {
        return f();
    } catch (const WhyError& error) {
        throw Stop::failure(offset, error.why);
    }
}

}  // namespace g6shim::shader::emit
