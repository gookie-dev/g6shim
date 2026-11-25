// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/emit/bitcode.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace g6shim::shader::emit {

namespace {

constexpr std::uint64_t kModuleBlock = 8;
constexpr std::uint64_t kParamAttrBlock = 9;
constexpr std::uint64_t kParamAttrGroupBlock = 10;
constexpr std::uint64_t kConstantsBlock = 11;
constexpr std::uint64_t kFunctionBlock = 12;
constexpr std::uint64_t kValueSymtabBlock = 14;
constexpr std::uint64_t kMetadataBlock = 15;
constexpr std::uint64_t kTypeBlock = 17;

constexpr std::uint64_t kAttributeGroupIds[][3] = {
    {1, 18, 20},
    {2, 18, 0},
    {3, 18, 21},
    {4, 12, 18},
};

constexpr std::string_view kTriple =
    "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64";

constexpr std::string_view kMetadataKinds[] = {
    "dbg",        "tbaa",          "prof",                     "fpmath",
    "range",      "tbaa.struct",   "invariant.load",           "alias.scope",
    "noalias",    "nontemporal",   "llvm.mem.parallel_loop_access", "nonnull",
    "dereferenceable", "dereferenceable_or_null",
};

std::size_t mix(std::size_t seed, std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15u + static_cast<std::uint64_t>(seed) + (value << 6) + (value >> 2);
    value ^= value >> 33;
    value *= 0xFF51AFD7ED558CCDu;
    value ^= value >> 33;
    return static_cast<std::size_t>(value);
}

std::vector<std::uint64_t> chars(std::string_view text) {
    std::vector<std::uint64_t> out;
    out.reserve(text.size());
    for (const char c : text) out.push_back(static_cast<std::uint8_t>(c));
    return out;
}

}  // namespace

std::uint64_t signed_vbr(std::int64_t value) {
    if (value >= 0) return static_cast<std::uint64_t>(value) << 1;
    const std::uint64_t magnitude = 0u - static_cast<std::uint64_t>(value);
    return magnitude << 1 | 1u;
}

std::size_t TypeDefHash::operator()(const TypeDef& def) const noexcept {
    std::size_t hash = 0;
    hash = mix(hash, static_cast<std::uint64_t>(def.kind));
    hash = mix(hash, def.bits);
    hash = mix(hash, def.pointee.index);
    hash = mix(hash, def.space);
    if (def.name) hash = mix(hash, std::hash<std::string>{}(*def.name));
    for (const Ty field : def.fields) hash = mix(hash, field.index);
    hash = mix(hash, def.result.index);
    return hash;
}

std::size_t ConstantKeyHash::operator()(const ConstantKey& key) const noexcept {
    std::size_t hash = mix(0, key.type.index);
    hash = mix(hash, static_cast<std::uint64_t>(key.constant.kind));
    hash = mix(hash, static_cast<std::uint64_t>(key.constant.value));
    hash = mix(hash, key.constant.bits);
    for (const std::uint32_t id : key.constant.ids) hash = mix(hash, id);
    return hash;
}

bool Successors::contains(std::size_t value) const noexcept {
    for (std::size_t i = 0; i < count; ++i)
        if (to[i] == value) return true;
    return false;
}

Successors successors(const std::optional<Term>& term) {
    if (!term) return {};
    switch (term->kind) {
        case Term::Kind::Br:
            return Successors{{term->target, 0}, 1};
        case Term::Kind::CondBr:
            return Successors{{term->target, term->not_taken}, 2};
        case Term::Kind::Ret:
            return {};
    }
    return {};
}

BitOp BitOp::bin(Bin op, V a, V b) {
    BitOp out;
    out.kind = Kind::Bin;
    out.code = static_cast<std::uint8_t>(op);
    out.a = a;
    out.b = b;
    return out;
}

BitOp BitOp::cast(Cast op, V v, Ty to) {
    BitOp out;
    out.kind = Kind::Cast;
    out.code = static_cast<std::uint8_t>(op);
    out.a = v;
    out.type = to;
    return out;
}

BitOp BitOp::cmp(std::uint8_t predicate, V a, V b) {
    BitOp out;
    out.kind = Kind::Cmp;
    out.code = predicate;
    out.a = a;
    out.b = b;
    return out;
}

BitOp BitOp::select(V condition, V on_true, V on_false) {
    BitOp out;
    out.kind = Kind::Select;
    out.a = condition;
    out.b = on_true;
    out.c = on_false;
    return out;
}

BitOp BitOp::extract(V aggregate, std::uint32_t index) {
    BitOp out;
    out.kind = Kind::Extract;
    out.a = aggregate;
    out.index = index;
    return out;
}

BitOp BitOp::call(std::uint32_t function, std::vector<V> args) {
    BitOp out;
    out.kind = Kind::Call;
    out.function = function;
    out.args = std::move(args);
    return out;
}

BitOp BitOp::phi(std::vector<std::pair<V, std::size_t>> incoming) {
    BitOp out;
    out.kind = Kind::Phi;
    out.incoming = std::move(incoming);
    return out;
}

BitOp BitOp::gep(Ty type, V pointer, std::vector<V> indices) {
    BitOp out;
    out.kind = Kind::Gep;
    out.type = type;
    out.a = pointer;
    out.args = std::move(indices);
    return out;
}

BitOp BitOp::load(V pointer) {
    BitOp out;
    out.kind = Kind::Load;
    out.a = pointer;
    return out;
}

BitOp BitOp::store(V pointer, V value) {
    BitOp out;
    out.kind = Kind::Store;
    out.a = pointer;
    out.b = value;
    return out;
}

BitOp BitOp::atomic_rmw(Rmw op, V pointer, V value) {
    BitOp out;
    out.kind = Kind::AtomicRmw;
    out.code = static_cast<std::uint8_t>(op);
    out.a = pointer;
    out.b = value;
    return out;
}

BitOp BitOp::cmp_xchg(V pointer, V expected, V new_value) {
    BitOp out;
    out.kind = Kind::CmpXchg;
    out.a = pointer;
    out.b = expected;
    out.c = new_value;
    return out;
}

std::uint32_t Writer::width() const noexcept {
    return stack_.empty() ? 2u : stack_.back().first;
}

void Writer::emit(std::uint64_t value, std::uint32_t width_bits) {
    current_ |= value << bits_;
    bits_ += width_bits;
    while (bits_ >= 32) {
        words_.push_back(static_cast<std::uint32_t>(current_));
        current_ >>= 32;
        bits_ -= 32;
    }
}

void Writer::vbr(std::uint64_t value, std::uint32_t width_bits) {
    const std::uint64_t high = std::uint64_t{1} << (width_bits - 1);
    while (value >= high) {
        emit((value & (high - 1)) | high, width_bits);
        value >>= width_bits - 1;
    }
    emit(value, width_bits);
}

void Writer::align() {
    if (bits_ > 0) {
        words_.push_back(static_cast<std::uint32_t>(current_));
        current_ = 0;
        bits_ = 0;
    }
}

void Writer::bytes(std::string_view text) {
    for (const char c : text) emit(static_cast<std::uint8_t>(c), 8);
}

void Writer::enter(std::uint64_t id, std::uint32_t width_bits) {
    emit(1, width());
    vbr(id, 8);
    vbr(width_bits, 4);
    align();
    words_.push_back(0);
    stack_.push_back({width_bits, words_.size()});
}

void Writer::exit() {
    emit(0, width());
    align();
    const std::pair<std::uint32_t, std::size_t> top = stack_.back();
    stack_.pop_back();
    words_[top.second - 1] = static_cast<std::uint32_t>(words_.size() - top.second);
}

void Writer::record_operands(std::uint64_t code, std::span<const std::uint64_t> operands) {
    emit(3, width());
    vbr(code, 6);
    vbr(operands.size(), 6);
    for (const std::uint64_t operand : operands) vbr(operand, 6);
}

void Writer::record(std::uint64_t code, std::initializer_list<std::uint64_t> operands) {
    record_operands(code, std::span<const std::uint64_t>(operands.begin(), operands.size()));
}

void Writer::record(std::uint64_t code, const std::vector<std::uint64_t>& operands) {
    record_operands(code, operands);
}

std::vector<std::uint8_t> Writer::finish() {
    align();
    std::vector<std::uint8_t> out;
    out.reserve(words_.size() * 4);
    for (const std::uint32_t word : words_) {
        out.push_back(static_cast<std::uint8_t>(word));
        out.push_back(static_cast<std::uint8_t>(word >> 8));
        out.push_back(static_cast<std::uint8_t>(word >> 16));
        out.push_back(static_cast<std::uint8_t>(word >> 24));
    }
    return out;
}

Ty Module::ty(TypeDef def) {
    const auto found = type_ids_.find(def);
    if (found != type_ids_.end()) return Ty{found->second};
    const std::uint32_t id = static_cast<std::uint32_t>(types_.size());
    type_ids_.emplace(def, id);
    types_.push_back(std::move(def));
    return Ty{id};
}

Ty Module::ptr(Ty to) { return ptr_in(to, 0); }

Ty Module::ptr_in(Ty to, std::uint32_t space) {
    TypeDef def;
    def.kind = TypeDef::Kind::Ptr;
    def.pointee = to;
    def.space = space;
    return ty(std::move(def));
}

Ty Module::array(std::uint32_t count, Ty of) {
    TypeDef def;
    def.kind = TypeDef::Kind::Array;
    def.bits = count;
    def.pointee = of;
    return ty(std::move(def));
}

Ty Module::named_struct(const std::string& name, std::vector<Ty> fields) {
    TypeDef def;
    def.kind = TypeDef::Kind::Struct;
    def.name = name;
    def.fields = std::move(fields);
    return ty(std::move(def));
}

Ty Module::func_ty(Ty result, std::vector<Ty> params) {
    TypeDef def;
    def.kind = TypeDef::Kind::Func;
    def.result = result;
    def.fields = std::move(params);
    return ty(std::move(def));
}

bool Module::is_void(Ty type) const { return types_[type.index].kind == TypeDef::Kind::Void; }

V Module::constant(Ty type, Constant value) {
    ConstantKey key{type, value};
    const auto found = const_ids_.find(key);
    if (found != const_ids_.end()) return V::const_value(found->second);
    const std::uint32_t id = static_cast<std::uint32_t>(consts_.size());
    const_ids_.emplace(std::move(key), id);
    consts_.push_back(ConstantKey{type, std::move(value)});
    return V::const_value(id);
}

V Module::int_const(Ty type, std::int64_t value) {
    return constant(type, Constant{Constant::Kind::Int, value, 0, {}});
}

V Module::i32(std::uint32_t value) {
    return int_const(int_type(32),
                     static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}

V Module::i8(std::uint8_t value) {
    return int_const(int_type(8), static_cast<std::int64_t>(static_cast<std::int8_t>(value)));
}

V Module::boolean(bool value) { return int_const(int_type(1), value ? -1 : 0); }

V Module::f32(std::uint32_t bits) {
    return constant(float_type(), Constant{Constant::Kind::Float, 0, bits, {}});
}

V Module::undef(Ty type) { return constant(type, Constant{Constant::Kind::Undef, 0, 0, {}}); }

V Module::aggregate(Ty type, std::span<const V> fields) {
    std::vector<std::uint32_t> ids;
    ids.reserve(fields.size());
    for (const V field : fields) {
        if (field.kind != V::Kind::Const) throw std::logic_error("aggregate of a non-constant");
        ids.push_back(field.index);
    }
    return constant(type, Constant{Constant::Kind::Aggregate, 0, 0, std::move(ids)});
}

void Module::replace_const(V old_value, V new_value) {
    if (old_value.kind != V::Kind::Const || new_value.kind != V::Kind::Const) return;
    consts_[old_value.index].constant = consts_[new_value.index].constant;
}

std::optional<std::int64_t> Module::int_value(V value) const {
    if (value.kind != V::Kind::Const || value.index >= consts_.size()) return std::nullopt;
    const Constant& constant = consts_[value.index].constant;
    if (constant.kind != Constant::Kind::Int) return std::nullopt;
    return constant.value;
}

V Module::function(const std::string& name, Ty type, std::optional<Attr> attr, bool body) {
    const auto found = func_ids_.find(name);
    if (found != func_ids_.end()) return V::func(found->second);
    const std::uint32_t id = static_cast<std::uint32_t>(funcs_.size());
    funcs_.push_back(Func{name, type, attr, body});
    func_ids_.emplace(name, id);
    return V::func(id);
}

V Module::global(const std::string& name, Ty type, std::uint32_t space) {
    const Ty pointer = ptr_in(type, space);
    globals_.push_back(Global{name, type, pointer, space, std::nullopt});
    return V::global(static_cast<std::uint32_t>(globals_.size() - 1));
}

V Module::constant_global(const std::string& name, V init) {
    if (init.kind != V::Kind::Const) throw std::logic_error("a global initialised with a non-constant");
    const Ty type = consts_[init.index].type;
    const V value = global(name, type, 0);
    globals_[value.index].init = init.index;
    return value;
}

std::size_t Module::new_block() {
    blocks_.push_back(Block{});
    return blocks_.size() - 1;
}

Ty Module::type_of(V value) const {
    switch (value.kind) {
        case V::Kind::Global: return globals_[value.index].ptr;
        case V::Kind::Const: return consts_[value.index].type;
        case V::Kind::Inst: return insts_[value.index].type;
        case V::Kind::Func: {
            TypeDef def;
            def.kind = TypeDef::Kind::Ptr;
            def.pointee = funcs_[value.index].type;
            const auto found = type_ids_.find(def);
            if (found == type_ids_.end()) throw std::logic_error("function pointer type");
            return Ty{found->second};
        }
    }
    return Ty{};
}

V Module::push(std::size_t block, Ty type, BitOp op) {
    const std::uint32_t id = static_cast<std::uint32_t>(insts_.size());
    insts_.push_back(Inst{type, std::move(op)});
    blocks_[block].body.push_back(id);
    return V::inst(id);
}

V Module::bin(std::size_t block, Bin op, V a, V b) {
    return push(block, type_of(a), BitOp::bin(op, a, b));
}

V Module::cast(std::size_t block, Cast op, V value, Ty to) {
    return push(block, to, BitOp::cast(op, value, to));
}

V Module::cmp(std::size_t block, std::uint8_t predicate, V a, V b) {
    return push(block, int_type(1), BitOp::cmp(predicate, a, b));
}

V Module::select(std::size_t block, V condition, V on_true, V on_false) {
    return push(block, type_of(on_true), BitOp::select(condition, on_true, on_false));
}

V Module::extract(std::size_t block, V aggregate, std::uint32_t index) {
    const TypeDef& type = types_[type_of(aggregate).index];
    Ty result;
    if (type.kind == TypeDef::Kind::Struct) {
        if (index >= type.fields.size()) throw std::logic_error("extractvalue out of range");
        result = type.fields[index];
    } else if (type.kind == TypeDef::Kind::Array) {
        result = type.pointee;
    } else {
        throw std::logic_error("extractvalue of a non-aggregate");
    }
    return push(block, result, BitOp::extract(aggregate, index));
}

V Module::call(std::size_t block, V function, std::vector<V> args) {
    if (function.kind != V::Kind::Func) throw std::logic_error("call of a non-function");
    const Func& callee = funcs_[function.index];
    const TypeDef& type = types_[callee.type.index];
    if (type.kind != TypeDef::Kind::Func) throw std::logic_error("call of a non-function");
    return push(block, type.result, BitOp::call(function.index, std::move(args)));
}

V Module::phi(std::size_t block, Ty type) {
    const std::uint32_t id = static_cast<std::uint32_t>(insts_.size());
    insts_.push_back(Inst{type, BitOp::phi({})});
    blocks_[block].phis.push_back(id);
    return V::inst(id);
}

V Module::element(std::size_t block, V pointer, V index) {
    const TypeDef& pointer_type = types_[type_of(pointer).index];
    if (pointer_type.kind != TypeDef::Kind::Ptr) throw std::logic_error("element of a non-pointer");
    const Ty array = pointer_type.pointee;
    const std::uint32_t space = pointer_type.space;
    const TypeDef& array_type = types_[array.index];
    if (array_type.kind != TypeDef::Kind::Array) throw std::logic_error("element of a non-array");
    const Ty element_type = array_type.pointee;
    const Ty result = ptr_in(element_type, space);
    const V zero = i32(0);
    return push(block, result, BitOp::gep(array, pointer, {zero, index}));
}

V Module::load(std::size_t block, V pointer) {
    const TypeDef& type = types_[type_of(pointer).index];
    if (type.kind != TypeDef::Kind::Ptr) throw std::logic_error("load from a non-pointer");
    return push(block, type.pointee, BitOp::load(pointer));
}

void Module::store(std::size_t block, V pointer, V value) {
    push(block, void_type(), BitOp::store(pointer, value));
}

V Module::atomic(std::size_t block, Rmw op, V pointer, V value) {
    return push(block, type_of(value), BitOp::atomic_rmw(op, pointer, value));
}

V Module::cmpxchg(std::size_t block, V pointer, V expected, V new_value) {
    const Ty value_type = type_of(new_value);
    TypeDef pair_def;
    pair_def.kind = TypeDef::Kind::Struct;
    pair_def.fields = {value_type, int_type(1)};
    const Ty pair = ty(std::move(pair_def));
    const V result = push(block, pair, BitOp::cmp_xchg(pointer, expected, new_value));
    return extract(block, result, 0);
}

void Module::set_phi(V phi, std::vector<std::pair<V, std::size_t>> incoming) {
    if (phi.kind != V::Kind::Inst) throw std::logic_error("phi of a non-instruction");
    insts_[phi.index].op = BitOp::phi(std::move(incoming));
}

void Module::terminate(std::size_t block, Term term) {
    blocks_[block].term = std::move(term);
}

Md Module::md_str(const std::string& text) {
    MdDef def;
    def.kind = MdDef::Kind::Str;
    def.text = text;
    mds_.push_back(std::move(def));
    return Md{static_cast<std::uint32_t>(mds_.size() - 1)};
}

Md Module::md_value(V value) {
    MdDef def;
    def.kind = MdDef::Kind::Value;
    def.type = type_of(value);
    def.value = value;
    mds_.push_back(std::move(def));
    return Md{static_cast<std::uint32_t>(mds_.size() - 1)};
}

Md Module::md_node(std::vector<std::optional<Md>> operands) {
    MdDef def;
    def.kind = MdDef::Kind::Node;
    def.operands = std::move(operands);
    mds_.push_back(std::move(def));
    return Md{static_cast<std::uint32_t>(mds_.size() - 1)};
}

void Module::named_md(const std::string& name, std::vector<Md> nodes) {
    named_.emplace_back(name, std::move(nodes));
}

std::vector<bool> Module::live() const {
    std::vector<bool> live(insts_.size(), false);
    std::vector<std::uint32_t> work;
    const auto mark = [&live, &work](V value) {
        if (value.kind == V::Kind::Inst && !live[value.index]) {
            live[value.index] = true;
            work.push_back(value.index);
        }
    };
    for (const Block& block : blocks_) {
        for (const std::uint32_t index : block.body) {
            const auto kind = insts_[index].op.kind;
            if (kind == BitOp::Kind::Call || kind == BitOp::Kind::Load || kind == BitOp::Kind::Store ||
                kind == BitOp::Kind::AtomicRmw || kind == BitOp::Kind::CmpXchg)
                mark(V::inst(index));
        }
        if (block.term && block.term->kind == Term::Kind::CondBr) mark(block.term->condition);
    }
    for (const MdDef& def : mds_)
        if (def.kind == MdDef::Kind::Value) mark(def.value);
    while (!work.empty()) {
        const std::uint32_t index = work.back();
        work.pop_back();
        for_each_operand(insts_[index].op, mark);
    }
    return live;
}

std::vector<std::uint8_t> Module::write() const {
    Writer w;
    w.bytes("BC\xC0\xDE");
    w.enter(kModuleBlock, 3);
    w.record(1, {1});

    w.enter(kParamAttrGroupBlock, 3);
    for (const auto& group : kAttributeGroupIds) {
        std::vector<std::uint64_t> r = {group[0], 0xFFFF'FFFFu};
        for (std::size_t i = 1; i < 3; ++i) {
            if (group[i] == 0) break;
            r.push_back(0);
            r.push_back(group[i]);
        }
        w.record(3, r);
    }
    w.exit();
    w.enter(kParamAttrBlock, 3);
    for (std::uint64_t id = 1; id <= 4; ++id) w.record(2, {id});
    w.exit();

    w.enter(kTypeBlock, 4);
    w.record(1, {types_.size()});
    for (const TypeDef& type : types_) {
        switch (type.kind) {
            case TypeDef::Kind::Void: w.record(2, {}); break;
            case TypeDef::Kind::Float: w.record(3, {}); break;
            case TypeDef::Kind::Int: w.record(7, {type.bits}); break;
            case TypeDef::Kind::Ptr: w.record(8, {type.pointee.index, type.space}); break;
            case TypeDef::Kind::Array: w.record(11, {type.bits, type.pointee.index}); break;
            case TypeDef::Kind::Struct: {
                std::vector<std::uint64_t> r = {0};
                for (const Ty field : type.fields) r.push_back(field.index);
                if (type.name) {
                    w.record(19, chars(*type.name));
                    w.record(20, r);
                } else {
                    w.record(18, r);
                }
                break;
            }
            case TypeDef::Kind::Func: {
                std::vector<std::uint64_t> r = {0, type.result.index};
                for (const Ty param : type.fields) r.push_back(param.index);
                w.record(21, r);
                break;
            }
        }
    }
    w.exit();

    w.record(2, chars("dxil-ms-dx"));
    w.record(3, chars(kTriple));
    const std::uint64_t first_const = globals_.size() + funcs_.size();
    for (const Global& g : globals_) {
        std::uint64_t constant = 0;
        std::uint64_t init = 0;
        std::uint64_t linkage = 0;
        if (g.init) {
            constant = 1;
            init = first_const + *g.init + 1;
            linkage = 3;
        }
        w.record(7, {g.type.index, static_cast<std::uint64_t>(g.space) << 2 | 2 | constant, init,
                     linkage, 3, 0});
    }
    for (const Func& f : funcs_) {
        const std::uint64_t attr = f.attr ? static_cast<std::uint64_t>(*f.attr) : 0;
        w.record(8, {f.type.index, 0, static_cast<std::uint64_t>(!f.body), 0, attr, 0, 0, 0, 0, 0, 0,
                     0, 0, 0});
    }

    const std::uint32_t globals = static_cast<std::uint32_t>(globals_.size());
    const std::uint32_t funcs = globals + static_cast<std::uint32_t>(funcs_.size());
    const auto const_id = [funcs](std::uint32_t index) {
        return static_cast<std::uint64_t>(funcs + index);
    };
    w.enter(kConstantsBlock, 4);
    std::optional<Ty> current;
    for (const ConstantKey& entry : consts_) {
        if (!current || !(*current == entry.type)) {
            w.record(1, {entry.type.index});
            current = entry.type;
        }
        const Constant& value = entry.constant;
        switch (value.kind) {
            case Constant::Kind::Undef: w.record(3, {}); break;
            case Constant::Kind::Int: w.record(4, {signed_vbr(value.value)}); break;
            case Constant::Kind::Float: w.record(6, {value.bits}); break;
            case Constant::Kind::Aggregate: {
                std::vector<std::uint64_t> r;
                r.reserve(value.ids.size());
                for (const std::uint32_t id : value.ids) r.push_back(const_id(id));
                w.record(7, r);
                break;
            }
        }
    }
    w.exit();

    const std::vector<bool> live = this->live();

    std::vector<std::size_t> post;
    std::vector<bool> seen(blocks_.size(), false);
    if (!blocks_.empty()) {
        std::vector<std::pair<std::size_t, std::size_t>> stack{{0, 0}};
        seen[0] = true;
        while (!stack.empty()) {
            std::pair<std::size_t, std::size_t>& top = stack.back();
            const std::size_t b = top.first;
            const std::size_t k = top.second;
            ++top.second;
            const Successors next = successors(blocks_[b].term);
            if (k < next.size()) {
                const std::size_t s = next[k];
                if (!seen[s]) {
                    seen[s] = true;
                    stack.push_back({s, 0});
                }
            } else {
                post.push_back(b);
                stack.pop_back();
            }
        }
    }
    std::reverse(post.begin(), post.end());
    std::vector<std::uint64_t> pos(blocks_.size(), UINT64_MAX);
    for (std::size_t n = 0; n < post.size(); ++n) pos[post[n]] = n;

    std::vector<std::vector<std::size_t>> preds(blocks_.size());
    for (const std::size_t b : post) {
        for (const std::size_t s : successors(blocks_[b].term)) preds[s].push_back(b);
    }
    std::vector<std::optional<std::pair<std::size_t, std::size_t>>> fold(blocks_.size());
    for (const std::size_t b : post) {
        const Block& block = blocks_[b];
        if (!block.term || block.term->kind != Term::Kind::Br) continue;
        if (!block.phis.empty() || !block.body.empty()) continue;
        if (preds[b].size() != 1) continue;
        const std::size_t h = block.term->target;
        const std::size_t p = preds[b][0];
        if (pos[h] < pos[b] && !successors(blocks_[p].term).contains(h)) fold[b] = std::pair(p, h);
    }
    std::vector<std::size_t> layout;
    layout.reserve(post.size());
    for (const std::size_t b : post)
        if (!fold[b]) layout.push_back(b);
    std::fill(pos.begin(), pos.end(), UINT64_MAX);
    for (std::size_t n = 0; n < layout.size(); ++n) pos[layout[n]] = n;
    const auto target = [&fold, &pos](std::size_t t) {
        return fold[t] ? pos[fold[t]->second] : pos[t];
    };
    const auto from = [&fold, &pos](std::size_t b) {
        return fold[b] ? pos[fold[b]->first] : pos[b];
    };

    const std::uint32_t base = funcs + static_cast<std::uint32_t>(consts_.size());
    std::vector<std::uint32_t> ids(insts_.size(), UINT32_MAX);
    std::uint32_t next = base;
    for (const std::size_t b : layout) {
        const Block& block = blocks_[b];
        for (const std::uint32_t i : block.phis)
            if (live[i] && !is_void(insts_[i].type)) ids[i] = next++;
        for (const std::uint32_t i : block.body)
            if (live[i] && !is_void(insts_[i].type)) ids[i] = next++;
    }
    const auto absolute = [&](V value) -> std::uint32_t {
        switch (value.kind) {
            case V::Kind::Global: return value.index;
            case V::Kind::Func: return globals + value.index;
            case V::Kind::Const: return funcs + value.index;
            case V::Kind::Inst:
                if (ids[value.index] == UINT32_MAX) throw std::logic_error("use of a void value");
                return ids[value.index];
        }
        return 0;
    };

    w.enter(kMetadataBlock, 3);
    for (const MdDef& def : mds_) {
        switch (def.kind) {
            case MdDef::Kind::Str: w.record(1, chars(def.text)); break;
            case MdDef::Kind::Value: w.record(2, {def.type.index, absolute(def.value)}); break;
            case MdDef::Kind::Node: {
                std::vector<std::uint64_t> r;
                r.reserve(def.operands.size());
                for (const std::optional<Md>& operand : def.operands)
                    r.push_back(operand ? static_cast<std::uint64_t>(operand->index) + 1 : 0);
                w.record(3, r);
                break;
            }
        }
    }
    for (const auto& [name, nodes] : named_) {
        w.record(4, chars(name));
        std::vector<std::uint64_t> r;
        r.reserve(nodes.size());
        for (const Md node : nodes) r.push_back(node.index);
        w.record(10, r);
    }
    w.exit();
    w.enter(kMetadataBlock, 3);
    for (std::size_t id = 0; id < std::size(kMetadataKinds); ++id) {
        std::vector<std::uint64_t> r = {id};
        for (const std::uint64_t c : chars(kMetadataKinds[id])) r.push_back(c);
        w.record(6, r);
    }
    w.exit();

    w.enter(kValueSymtabBlock, 4);
    for (std::size_t i = 0; i < globals_.size(); ++i) {
        std::vector<std::uint64_t> r = {i};
        for (const std::uint64_t c : chars(globals_[i].name)) r.push_back(c);
        w.record(1, r);
    }
    for (std::size_t i = 0; i < funcs_.size(); ++i) {
        std::vector<std::uint64_t> r = {globals_.size() + i};
        for (const std::uint64_t c : chars(funcs_[i].name)) r.push_back(c);
        w.record(1, r);
    }
    w.exit();

    w.enter(kFunctionBlock, 4);
    w.record(1, {layout.size()});
    std::uint32_t inst_id = base;
    std::vector<std::uint64_t> r;
    const auto relative = [&](V value) -> std::uint64_t {
        const std::uint32_t at = absolute(value);
        if (at >= inst_id) throw std::logic_error("forward reference outside a phi");
        return static_cast<std::uint64_t>(inst_id - at);
    };
    const auto instruction = [&](std::uint32_t i) {
        const Inst& inst = insts_[i];
        switch (inst.op.kind) {
            case BitOp::Kind::Bin: w.record(2, {relative(inst.op.a), relative(inst.op.b), inst.op.code}); break;
            case BitOp::Kind::Cast:
                w.record(3, {relative(inst.op.a), inst.type.index, inst.op.code});
                break;
            case BitOp::Kind::Cmp:
                w.record(28, {relative(inst.op.a), relative(inst.op.b), inst.op.code});
                break;
            case BitOp::Kind::Select:
                w.record(29, {relative(inst.op.b), relative(inst.op.c), relative(inst.op.a)});
                break;
            case BitOp::Kind::Extract: w.record(26, {relative(inst.op.a), inst.op.index}); break;
            case BitOp::Kind::Call: {
                r.clear();
                r.insert(r.end(), {0, std::uint64_t{1} << 15, funcs_[inst.op.function].type.index});
                r.push_back(relative(V::func(inst.op.function)));
                for (const V arg : inst.op.args) r.push_back(relative(arg));
                w.record(34, r);
                break;
            }
            case BitOp::Kind::Phi: {
                r.clear();
                r.push_back(inst.type.index);
                for (const auto& entry : inst.op.incoming) {
                    const std::int64_t delta = static_cast<std::int64_t>(inst_id) -
                                               static_cast<std::int64_t>(absolute(entry.first));
                    r.push_back(signed_vbr(delta));
                    r.push_back(from(entry.second));
                }
                w.record(16, r);
                break;
            }
            case BitOp::Kind::Gep: {
                r.clear();
                r.insert(r.end(), {0, inst.op.type.index, relative(inst.op.a)});
                for (const V index : inst.op.args) r.push_back(relative(index));
                w.record(43, r);
                break;
            }
            case BitOp::Kind::Load: w.record(20, {relative(inst.op.a), inst.type.index, 3, 0}); break;
            case BitOp::Kind::Store: w.record(44, {relative(inst.op.a), relative(inst.op.b), 3, 0}); break;
            case BitOp::Kind::AtomicRmw:
                w.record(38, {relative(inst.op.a), relative(inst.op.b), inst.op.code, 0, 6, 1});
                break;
            case BitOp::Kind::CmpXchg:
                w.record(46, {relative(inst.op.a), relative(inst.op.b), relative(inst.op.c), 0, 6, 1, 6, 0});
                break;
        }
        if (!is_void(inst.type)) ++inst_id;
    };
    for (const std::size_t b : layout) {
        const Block& block = blocks_[b];
        for (const std::uint32_t i : block.phis)
            if (live[i]) instruction(i);
        for (const std::uint32_t i : block.body)
            if (live[i]) instruction(i);
        if (!block.term) throw std::logic_error("unterminated block");
        const Term& term = *block.term;
        switch (term.kind) {
            case Term::Kind::Ret: w.record(10, {}); break;
            case Term::Kind::Br: w.record(11, {target(term.target)}); break;
            case Term::Kind::CondBr: {
                const std::uint32_t condition = absolute(term.condition);
                if (condition >= inst_id) throw std::logic_error("forward branch condition");
                w.record(11, {target(term.target), target(term.not_taken),
                              static_cast<std::uint64_t>(inst_id - condition)});
                break;
            }
        }
    }
    w.exit();
    w.exit();
    return w.finish();
}

}  // namespace g6shim::shader::emit
