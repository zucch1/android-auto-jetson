// SPDX-License-Identifier: GPL-3.0-or-later
// Exhaustive state-pair property (task 17): the raw StateMachine legality table
// must match an independently re-declared 28-edge oracle across all 100
// (from, to) pairs, so drift in the shipped table is caught here. The oracle is
// written out on purpose and never includes the production table.

#include <aa/session/Session.hpp>

#include <array>
#include <cstddef>

#include <gtest/gtest.h>

namespace {

namespace ss = aa::session;

struct Edge final {
    ss::State from;
    ss::State to;
};

constexpr std::array kAllStates{
    ss::State::disconnected, ss::State::discovering, ss::State::pairing,
    ss::State::connecting,   ss::State::negotiating, ss::State::active,
    ss::State::degraded,     ss::State::reconnecting, ss::State::stopping,
    ss::State::failed,
};

// Independent oracle of the 28 documented edges (self-transitions excluded).
constexpr std::array kExpectedEdges{
    Edge{ss::State::disconnected, ss::State::discovering},
    Edge{ss::State::discovering, ss::State::pairing},
    Edge{ss::State::discovering, ss::State::connecting},
    Edge{ss::State::discovering, ss::State::stopping},
    Edge{ss::State::discovering, ss::State::failed},
    Edge{ss::State::pairing, ss::State::connecting},
    Edge{ss::State::pairing, ss::State::stopping},
    Edge{ss::State::pairing, ss::State::failed},
    Edge{ss::State::connecting, ss::State::negotiating},
    Edge{ss::State::connecting, ss::State::stopping},
    Edge{ss::State::connecting, ss::State::failed},
    Edge{ss::State::negotiating, ss::State::active},
    Edge{ss::State::negotiating, ss::State::stopping},
    Edge{ss::State::negotiating, ss::State::failed},
    Edge{ss::State::active, ss::State::degraded},
    Edge{ss::State::active, ss::State::reconnecting},
    Edge{ss::State::active, ss::State::stopping},
    Edge{ss::State::active, ss::State::failed},
    Edge{ss::State::degraded, ss::State::active},
    Edge{ss::State::degraded, ss::State::reconnecting},
    Edge{ss::State::degraded, ss::State::stopping},
    Edge{ss::State::degraded, ss::State::failed},
    Edge{ss::State::reconnecting, ss::State::connecting},
    Edge{ss::State::reconnecting, ss::State::stopping},
    Edge{ss::State::reconnecting, ss::State::failed},
    Edge{ss::State::stopping, ss::State::disconnected},
    Edge{ss::State::stopping, ss::State::failed},
    Edge{ss::State::failed, ss::State::disconnected},
};

[[nodiscard]] bool expected_legal(ss::State from, ss::State to) noexcept {
    for (const Edge& edge : kExpectedEdges) {
        if (edge.from == from && edge.to == to) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(StateMachineTable, EveryStatePairMatchesIndependentEdgeList) {
    // Given: ten states and the independent 28-edge oracle.
    int legal_count = 0;
    // When: every one of the 100 (from, to) pairs is checked against the oracle.
    for (ss::State from : kAllStates) {
        for (ss::State to : kAllStates) {
            const bool actual = ss::is_legal_transition(from, to);
            const bool expected = expected_legal(from, to);
            EXPECT_EQ(actual, expected)
                << "edge " << ss::to_string(from) << " -> " << ss::to_string(to);
            if (actual) {
                ++legal_count;
            }
        }
    }
    // Then: the pair count is exactly the 28 documented edges.
    EXPECT_EQ(legal_count, 28);
    EXPECT_EQ(kExpectedEdges.size(), std::size_t{28});
}

TEST(StateMachineTable, SelfTransitionsAreNeverLegal) {
    // Given/When/Then: no state may transition to itself.
    for (ss::State state : kAllStates) {
        EXPECT_FALSE(ss::is_legal_transition(state, state)) << ss::to_string(state);
    }
}

TEST(StateMachineTable, MachineRejectsOffTableMoveWithoutMoving) {
    // Given: a machine at the disconnected entry state.
    ss::StateMachine machine;
    // When: a jump straight to active is attempted.
    const auto result = machine.transition(ss::State::active);
    // Then: the typed error names the illegal transition and the state is held.
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), aa::ErrorCode::session_illegal_transition);
    EXPECT_EQ(machine.state(), ss::State::disconnected);
}
