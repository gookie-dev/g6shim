// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "g6shim/shader/emit/bitcode.hpp"

namespace {

using namespace g6shim::shader::emit;

std::uint32_t read_u32(std::span<const std::uint8_t> data, std::size_t at) {
    return static_cast<std::uint32_t>(data[at]) | static_cast<std::uint32_t>(data[at + 1]) << 8 |
           static_cast<std::uint32_t>(data[at + 2]) << 16 | static_cast<std::uint32_t>(data[at + 3]) << 24;
}

Module synthetic(std::size_t regs, std::size_t stages, std::size_t read) {
    Module m;
    const Ty i32t = m.int_type(32);
    const Ty void_type = m.void_type();
    const Ty function_type = m.func_ty(i32t, {i32t, i32t});
    const V function = m.function("dx.op.synthetic", function_type, Attr::ReadNone, false);
    m.function("main", m.func_ty(void_type, {}), std::nullopt, true);
    std::size_t block = m.new_block();
    std::vector<V> current;
    for (std::size_t r = 0; r < regs; ++r) current.push_back(m.i32(static_cast<std::uint32_t>(r)));
    for (std::size_t s = 0; s < stages; ++s) {
        const V condition = m.cmp(block, pred::kEq, current[0], current[s % regs]);
        const std::size_t taken = m.new_block();
        const std::size_t other = m.new_block();
        const std::size_t join = m.new_block();
        m.terminate(block, Term::cond_br(condition, taken, other));
        const V call = m.call(taken, function, {current[s % regs], current[(s + 1) % regs]});
        m.terminate(taken, Term::br(join));
        m.terminate(other, Term::br(join));
        std::vector<V> next;
        for (std::size_t r = 0; r < regs; ++r) {
            const V phi = m.phi(join, i32t);
            const V value = r == s % regs ? call : current[r];
            m.set_phi(phi, {{value, taken}, {current[r], other}});
            next.push_back(phi);
        }
        current = std::move(next);
        block = join;
    }
    const std::size_t head = m.new_block();
    const std::size_t body = m.new_block();
    const std::size_t latch = m.new_block();
    const std::size_t exit = m.new_block();
    m.terminate(block, Term::br(head));
    std::vector<V> phis;
    for (std::size_t r = 0; r < regs; ++r) phis.push_back(m.phi(head, i32t));
    const V one = m.i32(1);
    std::vector<V> stepped;
    for (const V phi : phis) stepped.push_back(m.bin(body, Bin::Add, phi, one));
    const V condition = m.cmp(body, pred::kIult, stepped[0], current[0]);
    m.terminate(head, Term::br(body));
    m.terminate(body, Term::cond_br(condition, latch, exit));
    m.terminate(latch, Term::br(head));
    for (std::size_t r = 0; r < regs; ++r) m.set_phi(phis[r], {{current[r], block}, {stepped[r], latch}});
    V accumulator = stepped[0];
    for (std::size_t k = 1; k < std::max<std::size_t>(read, 1); ++k)
        accumulator = m.bin(exit, Bin::Add, accumulator, stepped[k]);
    m.call(exit, function, {accumulator, current[0]});
    m.terminate(exit, Term::ret());
    return m;
}

std::uint64_t fnv(std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 0xcbf2'9ce4'8422'2325u;
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 0x0100'0000'01b3u;
    }
    return hash;
}

std::vector<bool> live_by_passes(const Module& m) {
    std::vector<bool> live(m.instructions().size(), false);
    for (const Block& block : m.blocks()) {
        for (const std::uint32_t index : block.body) {
            const auto kind = m.instructions()[index].op.kind;
            live[index] = kind == BitOp::Kind::Call || kind == BitOp::Kind::Load || kind == BitOp::Kind::Store ||
                          kind == BitOp::Kind::AtomicRmw || kind == BitOp::Kind::CmpXchg;
        }
        if (block.term && block.term->kind == Term::Kind::CondBr &&
            block.term->condition.kind == V::Kind::Inst)
            live[block.term->condition.index] = true;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 0; i < m.instructions().size(); ++i) {
            if (!live[i]) continue;
            for_each_operand(m.instructions()[i].op, [&](V value) {
                if (value.kind == V::Kind::Inst && !live[value.index]) {
                    live[value.index] = true;
                    changed = true;
                }
            });
        }
    }
    return live;
}

TEST(bitcode_vbr_and_blocks) {
    Writer writer;
    writer.vbr(5, 6);
    writer.vbr(100, 6);
    CHECK(writer.pending() == (5u | (0b10'0100u << 6) | (3u << 12)));
    Writer block;
    block.enter(8, 3);
    block.record(1, {1});
    block.exit();
    const std::vector<std::uint8_t> bytes = block.finish();
    CHECK(bytes.size() % 4 == 0);
    CHECK(read_u32(bytes, 4) == 1);
    CHECK(signed_vbr(-3) == 7);
    CHECK(signed_vbr(2) == 4);
}

TEST(bitcode_worklist_liveness_matches_passes) {
    for (const auto& [regs, stages, read] :
         {std::tuple{1, 0, 1}, std::tuple{1, 1, 1}, std::tuple{4, 3, 1}, std::tuple{8, 17, 3},
          std::tuple{16, 40, 16}}) {
        const Module m = synthetic(static_cast<std::size_t>(regs), static_cast<std::size_t>(stages),
                                   static_cast<std::size_t>(read));
        CHECK(m.live() == live_by_passes(m));
    }
}

TEST(bitcode_unread_phis_are_dropped) {
    Module m;
    const Ty i32t = m.int_type(32);
    const std::size_t entry = m.new_block();
    const std::size_t join = m.new_block();
    const V a = m.i32(1);
    const V b = m.i32(2);
    m.terminate(entry, Term::br(join));
    const V read = m.phi(join, i32t);
    const V unread = m.phi(join, i32t);
    const V cycle_a = m.phi(join, i32t);
    const V cycle_b = m.phi(join, i32t);
    m.set_phi(read, {{a, entry}});
    m.set_phi(unread, {{b, entry}});
    m.set_phi(cycle_a, {{cycle_b, entry}});
    m.set_phi(cycle_b, {{cycle_a, entry}});
    const V sum = m.bin(join, Bin::Add, read, b);
    const V output = m.function("output", m.func_ty(m.void_type(), {i32t}), Attr::NoUnwind, false);
    m.call(join, output, {sum});
    m.terminate(join, Term::ret());
    const std::vector<bool> live = m.live();
    CHECK(live[read.index] && live[sum.index]);
    CHECK(!live[unread.index] && !live[cycle_a.index] && !live[cycle_b.index]);
    CHECK(live == live_by_passes(m));
}

TEST(bitcode_successors_of_each_terminator) {
    CHECK(successors(std::nullopt).empty());
    CHECK(successors(Term::ret()).empty());
    const Successors jump = successors(Term::br(7));
    CHECK(jump.size() == 1 && jump[0] == 7);
    const Successors branch = successors(Term::cond_br(V::const_value(0), 3, 5));
    CHECK(branch.size() == 2 && branch[0] == 3 && branch[1] == 5);
}

TEST(bitcode_omits_unused_arithmetic_without_dropping_effects) {
    const auto make = [](bool add_dead_work) {
        Module m;
        const Ty integer = m.int_type(32);
        const Ty no_result = m.void_type();
        const V effect = m.function("effect", m.func_ty(no_result, {integer}), Attr::NoUnwind, false);
        m.function("main", m.func_ty(no_result, {}), std::nullopt, true);
        const auto block = m.new_block();
        const V one = m.i32(1);
        const V two = m.i32(2);
        m.int_type(1);
        if (add_dead_work) {
            const V sum = m.bin(block, Bin::Add, one, two);
            m.cmp(block, pred::kEq, sum, two);
        }
        const V argument = m.bin(block, Bin::Sub, two, one);
        m.call(block, effect, {argument});
        m.terminate(block, Term::ret());
        return m;
    };
    const Module with_dead_work = make(true);
    const auto live = with_dead_work.live();
    CHECK(live == (std::vector<bool>{false, false, true, true}));
    CHECK(with_dead_work.write() == make(false).write());
}

TEST(bitcode_written_bytes_are_deterministic) {
    struct Pin {
        std::size_t regs;
        std::size_t stages;
        std::size_t read;
        std::size_t length;
        std::uint64_t hash;
    };
    const Pin pins[] = {{64, 64, 64, 38'916, 0x9586'730c'f953'4a45u},
                        {128, 256, 32, 253'648, 0x0bd0'fe56'66b8'6f78u}};
    for (const Pin& pin : pins) {
        const std::vector<std::uint8_t> bytes = synthetic(pin.regs, pin.stages, pin.read).write();
        CHECK(bytes.size() == pin.length);
        CHECK(fnv(bytes) == pin.hash);
    }
}

}  // namespace
