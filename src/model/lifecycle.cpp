// Commissioning Fabric - the commissioning state machine.
//
// The chain is entered in order and never backwards. A later fact being true
// does not move a candidate: the evaluator computes the furthest state the
// satisfied dimensions support, and the caller records the transition with the
// evidence that justified it. Failed, Quarantined and Cancelled are fences:
// nothing leaves them except an explicit quarantine release.
#include "cxf/model/lifecycle.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "cxf/support/status.hpp"
#include "cxf/types/enums.hpp"

namespace cxf {
namespace {

[[nodiscard]] constexpr std::size_t dimension_index(EvidenceDimension dimension) noexcept {
  return static_cast<std::size_t>(dimension);
}

/// True when the gate dimensions of one chain state are all satisfied. States
/// that are not gates of this function (ActivationAuthorized and later) are
/// never reached from readiness alone.
[[nodiscard]] bool gate_satisfied(
    LifecycleState state,
    const std::array<bool, kEvidenceDimensionCount>& satisfied) noexcept {
  switch (state) {
    case LifecycleState::kIdentified:
      return satisfied[dimension_index(EvidenceDimension::kIdentity)];
    case LifecycleState::kLocated:
      return satisfied[dimension_index(EvidenceDimension::kPlacement)];
    case LifecycleState::kDependencyValidated:
      return satisfied[dimension_index(EvidenceDimension::kDependency)];
    case LifecycleState::kCompatibilityValidated:
      return satisfied[dimension_index(EvidenceDimension::kCompatibility)];
    case LifecycleState::kUtilitiesReady:
      // Both utilities must be observed: power without cooling is not ready.
      return satisfied[dimension_index(EvidenceDimension::kElectrical)] &&
             satisfied[dimension_index(EvidenceDimension::kCooling)];
    case LifecycleState::kNetworkReady:
      return satisfied[dimension_index(EvidenceDimension::kNetwork)];
    case LifecycleState::kHealthValidated:
      return satisfied[dimension_index(EvidenceDimension::kHealth)];
    default:
      return false;
  }
}

}  // namespace

bool transition_allowed(LifecycleState from, LifecycleState to) noexcept {
  // Nothing is ever moved back to the admission state, and a transition to the
  // state a candidate is already in is not a transition.
  if (from == to || to == LifecycleState::kDeclared) {
    return false;
  }
  if (is_terminal_state(from)) {
    // Commissioned, Failed and Cancelled are closed. Quarantine is the one
    // terminal state with an exit: an explicit release, or cancellation.
    return from == LifecycleState::kQuarantined &&
           (to == LifecycleState::kIdentified || to == LifecycleState::kCancelled);
  }
  // Any live (non-terminal) state may fail, be quarantined or be cancelled.
  if (to == LifecycleState::kFailed || to == LifecycleState::kQuarantined ||
      to == LifecycleState::kCancelled) {
    return true;
  }
  // Forward moves along the milestone chain only, always to a strictly later
  // state. The evaluator computes a prefix of the chain; a caller that recorded
  // the evidence for a later milestone may name it directly, but never move
  // backwards.
  return is_milestone_state(from) && is_milestone_state(to) &&
         state_rank(to) > state_rank(from);
}

Status require_transition(LifecycleState from, LifecycleState to) {
  if (transition_allowed(from, to)) {
    return Status::success();
  }
  return make_error(Code::kTransitionNotAllowed,
                    std::string("transition from ") + std::string(state_name(from)) +
                        " to " + std::string(state_name(to)) + " is not allowed",
                    std::string("from=") + std::string(state_name(from)) +
                        " to=" + std::string(state_name(to)));
}

std::string_view state_assertion(LifecycleState state) noexcept {
  switch (state) {
    case LifecycleState::kDeclared:
      return "the candidate is admitted with a declaration only";
    case LifecycleState::kIdentified:
      return "the asset identity is bound to a registry identity observed live";
    case LifecycleState::kLocated:
      return "the placement was bound from a live placement observation";
    case LifecycleState::kDependencyValidated:
      return "every required dependency resolved at the expected generation";
    case LifecycleState::kCompatibilityValidated:
      return "compatibility evidence for this hardware and firmware is satisfied";
    case LifecycleState::kUtilitiesReady:
      return "electrical and cooling observations for the placement are satisfied";
    case LifecycleState::kNetworkReady:
      return "a live network observation for the placement is satisfied";
    case LifecycleState::kHealthValidated:
      return "live health observations are satisfied";
    case LifecycleState::kActivationAuthorized:
      return "an activation authority token was issued for this revision";
    case LifecycleState::kActivating:
      return "an activation attempt is in flight under a valid token";
    case LifecycleState::kCommissioned:
      return "an activation result reporting success was recorded";
    case LifecycleState::kFailed:
      return "an attempt ended in failure and the failure is recorded";
    case LifecycleState::kQuarantined:
      return "the candidate is fenced out of commissioning with a typed reason";
    case LifecycleState::kCancelled:
      return "the candidate was withdrawn from commissioning by an operator";
  }
  return "invalid state";
}

std::string_view state_non_assertion(LifecycleState state) noexcept {
  switch (state) {
    case LifecycleState::kDeclared:
      return "the asset exists, is identified, placed or ready";
    case LifecycleState::kIdentified:
      return "the asset is compatible, placed or ready";
    case LifecycleState::kLocated:
      return "the placed equipment is powered, cooled or reachable";
    case LifecycleState::kDependencyValidated:
      return "the dependencies will still hold at that generation";
    case LifecycleState::kCompatibilityValidated:
      return "the asset stays compatible after a hardware or firmware change";
    case LifecycleState::kUtilitiesReady:
      return "power or cooling is currently applied to the asset";
    case LifecycleState::kNetworkReady:
      return "the asset is reachable or its addresses are assigned";
    case LifecycleState::kHealthValidated:
      return "the asset will stay healthy or passed every diagnostic mode";
    case LifecycleState::kActivationAuthorized:
      return "the asset was activated or the token was used";
    case LifecycleState::kActivating:
      return "the activation has taken effect";
    case LifecycleState::kCommissioned:
      return "the asset is serving traffic or remains commissioned elsewhere";
    case LifecycleState::kFailed:
      return "the asset is unusable or the failure is understood";
    case LifecycleState::kQuarantined:
      return "the asset is defective";
    case LifecycleState::kCancelled:
      return "the asset is decommissioned or its placement was released";
  }
  return "invalid state";
}

LifecycleState milestone_for_dimension(EvidenceDimension dimension) noexcept {
  switch (dimension) {
    case EvidenceDimension::kIdentity:
      return LifecycleState::kIdentified;
    case EvidenceDimension::kPlacement:
      return LifecycleState::kLocated;
    case EvidenceDimension::kDependency:
      return LifecycleState::kDependencyValidated;
    case EvidenceDimension::kCompatibility:
      return LifecycleState::kCompatibilityValidated;
    case EvidenceDimension::kElectrical:
    case EvidenceDimension::kCooling:
      return LifecycleState::kUtilitiesReady;
    case EvidenceDimension::kNetwork:
      return LifecycleState::kNetworkReady;
    case EvidenceDimension::kHealth:
      return LifecycleState::kHealthValidated;
    case EvidenceDimension::kPolicy:
    case EvidenceDimension::kServiceClass:
      return LifecycleState::kActivationAuthorized;
  }
  return LifecycleState::kDeclared;
}

LifecycleState furthest_reachable_state(
    const std::array<bool, kEvidenceDimensionCount>& satisfied) noexcept {
  // The floor is always Declared: a candidate that satisfies nothing is still
  // admitted, never "not a candidate".
  LifecycleState furthest = LifecycleState::kDeclared;
  const std::array<LifecycleState, kMilestoneStateCount> chain = milestone_states();
  const std::size_t last = state_rank(LifecycleState::kHealthValidated);
  for (std::size_t rank = 1; rank <= last; ++rank) {
    if (!gate_satisfied(chain[rank], satisfied)) {
      break;
    }
    furthest = chain[rank];
  }
  // ActivationAuthorized is deliberately not reachable here: it requires an
  // explicit authorization token rather than a satisfied dimension.
  return furthest;
}

Outcome<LifecycleState> next_milestone(LifecycleState state) {
  if (!is_milestone_state(state)) {
    return make_error(Code::kFieldOutOfRange, "state is not part of the milestone chain",
                      std::string(state_name(state)));
  }
  const std::size_t rank = state_rank(state);
  if (rank + 1 >= kMilestoneStateCount) {
    return make_error(Code::kFieldOutOfRange,
                      "state is the last state of the milestone chain",
                      std::string(state_name(state)));
  }
  return static_cast<LifecycleState>(rank + 1);
}

}  // namespace cxf
