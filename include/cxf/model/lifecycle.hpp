// Commissioning Fabric - the commissioning state machine.
//
// The validation states form a strict chain that is only ever entered in order
// and never backwards. A candidate is never moved to a later state because a
// later fact happens to be true: the evaluator computes the furthest state the
// satisfied dimensions support, and the transition is recorded with the
// evidence that justified it.
#ifndef CXF_MODEL_LIFECYCLE_HPP
#define CXF_MODEL_LIFECYCLE_HPP

#include <array>
#include <cstddef>
#include <string_view>

#include "cxf/support/status.hpp"
#include "cxf/types/enums.hpp"

namespace cxf {

/// True when the state machine permits the move from one state to another.
[[nodiscard]] bool transition_allowed(LifecycleState from, LifecycleState to) noexcept;

/// Transition check that returns the canonical error for a refused move.
[[nodiscard]] Status require_transition(LifecycleState from, LifecycleState to);

/// What the state asserts, and - just as importantly - what it does not assert.
[[nodiscard]] std::string_view state_assertion(LifecycleState state) noexcept;
[[nodiscard]] std::string_view state_non_assertion(LifecycleState state) noexcept;

/// The chain state that a satisfied readiness dimension unlocks. Identity and
/// placement unlock Identified and Located; electrical and cooling together
/// unlock UtilitiesReady; policy and service class unlock the prerequisites for
/// explicit activation authorization.
[[nodiscard]] LifecycleState milestone_for_dimension(EvidenceDimension dimension) noexcept;

/// Furthest chain state reachable when exactly the listed dimensions are
/// satisfied. Always a prefix of the chain: a gap is never skipped over.
[[nodiscard]] LifecycleState furthest_reachable_state(
    const std::array<bool, kEvidenceDimensionCount>& satisfied) noexcept;

/// The next chain state after the given one, or an error when there is none.
[[nodiscard]] Outcome<LifecycleState> next_milestone(LifecycleState state);

/// The state a candidate returns to when quarantine is released: identity is
/// already bound, so the new attempt restarts at Identified.
inline constexpr LifecycleState kQuarantineReleaseState = LifecycleState::kIdentified;

}  // namespace cxf

#endif  // CXF_MODEL_LIFECYCLE_HPP
