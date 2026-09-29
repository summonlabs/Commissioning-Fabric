// Commissioning Fabric - closed domain enumerations.
//
// Every enumeration has exactly one canonical lowercase spelling. name() and
// parse() are exact inverses for every value, and the numeric values are part
// of the durable format: they are assigned explicitly and never renumbered.
#ifndef CXF_TYPES_ENUMS_HPP
#define CXF_TYPES_ENUMS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "cxf/support/status.hpp"

namespace cxf {

/// Ordered lifecycle states. The eight validation states form a strict chain;
/// Failed, Quarantined and Cancelled are exits from anywhere.
enum class LifecycleState : std::uint8_t {
  kDeclared = 0,
  kIdentified = 1,
  kLocated = 2,
  kDependencyValidated = 3,
  kCompatibilityValidated = 4,
  kUtilitiesReady = 5,
  kNetworkReady = 6,
  kHealthValidated = 7,
  kActivationAuthorized = 8,
  kActivating = 9,
  kCommissioned = 10,
  kFailed = 11,
  kQuarantined = 12,
  kCancelled = 13,
};

inline constexpr std::size_t kLifecycleStateCount = 14;
inline constexpr std::size_t kMilestoneStateCount = 11;  // Declared..Commissioned

[[nodiscard]] std::string_view state_name(LifecycleState state) noexcept;
[[nodiscard]] Outcome<LifecycleState> parse_state(std::string_view text);
[[nodiscard]] constexpr bool is_terminal_state(LifecycleState state) noexcept {
  return state == LifecycleState::kFailed || state == LifecycleState::kQuarantined ||
         state == LifecycleState::kCancelled || state == LifecycleState::kCommissioned;
}
/// True when state is one of Declared..Commissioned (the ordered chain).
[[nodiscard]] constexpr bool is_milestone_state(LifecycleState state) noexcept {
  return static_cast<std::uint8_t>(state) < kMilestoneStateCount;
}
/// Position in the ordered chain; non-chain states return kMilestoneStateCount.
[[nodiscard]] constexpr std::size_t state_rank(LifecycleState state) noexcept {
  return is_milestone_state(state) ? static_cast<std::size_t>(state) : kMilestoneStateCount;
}
[[nodiscard]] std::array<LifecycleState, kMilestoneStateCount> milestone_states() noexcept;

/// Readiness dimensions, in the fixed order the evaluator always reports them.
enum class EvidenceDimension : std::uint8_t {
  kIdentity = 0,
  kPlacement = 1,
  kDependency = 2,
  kCompatibility = 3,
  kElectrical = 4,
  kCooling = 5,
  kNetwork = 6,
  kHealth = 7,
  kPolicy = 8,
  kServiceClass = 9,
};

inline constexpr std::size_t kEvidenceDimensionCount = 10;

[[nodiscard]] std::string_view dimension_name(EvidenceDimension dimension) noexcept;
[[nodiscard]] Outcome<EvidenceDimension> parse_dimension(std::string_view text);
[[nodiscard]] constexpr std::size_t dimension_rank(EvidenceDimension dimension) noexcept {
  return static_cast<std::size_t>(dimension);
}
[[nodiscard]] std::array<EvidenceDimension, kEvidenceDimensionCount> all_dimensions() noexcept;

/// Tri-state verdict. Unknown is a first-class value: it is never converted to
/// satisfied or to unsatisfied by the runtime.
enum class EvidenceVerdict : std::uint8_t {
  kUnknown = 0,
  kSatisfied = 1,
  kUnsatisfied = 2,
};

[[nodiscard]] std::string_view verdict_name(EvidenceVerdict verdict) noexcept;
[[nodiscard]] Outcome<EvidenceVerdict> parse_verdict(std::string_view text);

/// Provenance of an observation. Authority rises from declared (an assertion)
/// to observed (an owning system of record) to imported (a signed bundle).
enum class EvidenceSource : std::uint8_t {
  kDeclared = 0,
  kImported = 1,
  kObserved = 2,
};

[[nodiscard]] std::string_view source_name(EvidenceSource source) noexcept;
[[nodiscard]] Outcome<EvidenceSource> parse_source(std::string_view text);
[[nodiscard]] constexpr std::uint8_t source_authority(EvidenceSource source) noexcept {
  return static_cast<std::uint8_t>(source);
}

/// Result reported for an activation attempt.
enum class ActivationResultKind : std::uint8_t {
  kSucceeded = 0,
  kFailed = 1,
  kDeferred = 2,
};

[[nodiscard]] std::string_view activation_result_name(ActivationResultKind kind) noexcept;
[[nodiscard]] Outcome<ActivationResultKind> parse_activation_result(std::string_view text);

/// Kinds of facility entity a candidate can depend on.
enum class DependencyKind : std::uint8_t {
  kSite = 0,
  kRack = 1,
  kPowerFeed = 2,
  kCoolingZone = 3,
  kNetworkFabric = 4,
  kUpstreamAsset = 5,
  kServiceClass = 6,
  kOwnership = 7,
};

inline constexpr std::size_t kDependencyKindCount = 8;

[[nodiscard]] std::string_view dependency_kind_name(DependencyKind kind) noexcept;
[[nodiscard]] Outcome<DependencyKind> parse_dependency_kind(std::string_view text);
[[nodiscard]] std::array<DependencyKind, kDependencyKindCount> all_dependency_kinds() noexcept;

/// Why a candidate was quarantined. Kept typed so the reason is durable and
/// reportable rather than a free-text note.
enum class QuarantineReason : std::uint8_t {
  kNone = 0,
  kIdentityConflict = 1,
  kUnsupportedHardware = 2,
  kFailedDiagnostics = 3,
  kStaleReadiness = 4,
  kDependencyAmbiguity = 5,
  kContradictoryEvidence = 6,
  kPolicyDenied = 7,
  kOperatorRequest = 8,
};

[[nodiscard]] std::string_view quarantine_reason_name(QuarantineReason reason) noexcept;
[[nodiscard]] Outcome<QuarantineReason> parse_quarantine_reason(std::string_view text);

/// Kinds of externally meaningful mutation. Each has one idempotency namespace
/// so a replayed request can be recognised as a replay of the same operation.
enum class RequestKind : std::uint8_t {
  kAdmitCandidate = 0,
  kBindIdentity = 1,
  kBindPlacement = 2,
  kSubmitEvidence = 3,
  kEvaluateReadiness = 4,
  kAuthorizeActivation = 5,
  kReportActivation = 6,
  kQuarantine = 7,
  kReleaseQuarantine = 8,
  kCancel = 9,
  kRecordFacilityChange = 10,
};

[[nodiscard]] std::string_view request_kind_name(RequestKind kind) noexcept;
[[nodiscard]] Outcome<RequestKind> parse_request_kind(std::string_view text);

/// Audit event kinds, in the order they can first occur.
enum class EventKind : std::uint8_t {
  kFabricCreated = 0,
  kFabricOpened = 1,
  kRecovered = 2,
  kCandidateAdmitted = 3,
  kIdentityBound = 4,
  kPlacementBound = 5,
  kEvidenceRecorded = 6,
  kReadinessEvaluated = 7,
  kActivationAuthorized = 8,
  kActivationReported = 9,
  kCommissioned = 10,
  kQuarantined = 11,
  kQuarantineReleased = 12,
  kCancelled = 13,
  kFailed = 14,
  kAttemptOpened = 15,
  kFacilityChanged = 16,
  kDependencyUnknown = 17,
};

[[nodiscard]] std::string_view event_kind_name(EventKind kind) noexcept;
[[nodiscard]] Outcome<EventKind> parse_event_kind(std::string_view text);

/// Domain check used by the durable decoder: a value outside the declared set is
/// rejected rather than mapped onto a neighbouring enumerator.
[[nodiscard]] constexpr bool enum_value_valid(LifecycleState v) noexcept {
  return static_cast<std::uint8_t>(v) < kLifecycleStateCount;
}
[[nodiscard]] constexpr bool enum_value_valid(EvidenceDimension v) noexcept {
  return static_cast<std::uint8_t>(v) < kEvidenceDimensionCount;
}
[[nodiscard]] constexpr bool enum_value_valid(EvidenceVerdict v) noexcept {
  return static_cast<std::uint8_t>(v) <= static_cast<std::uint8_t>(EvidenceVerdict::kUnsatisfied);
}
[[nodiscard]] constexpr bool enum_value_valid(EvidenceSource v) noexcept {
  return static_cast<std::uint8_t>(v) <= static_cast<std::uint8_t>(EvidenceSource::kObserved);
}
[[nodiscard]] constexpr bool enum_value_valid(ActivationResultKind v) noexcept {
  return static_cast<std::uint8_t>(v) <=
         static_cast<std::uint8_t>(ActivationResultKind::kDeferred);
}
[[nodiscard]] constexpr bool enum_value_valid(DependencyKind v) noexcept {
  return static_cast<std::uint8_t>(v) < kDependencyKindCount;
}
[[nodiscard]] constexpr bool enum_value_valid(QuarantineReason v) noexcept {
  return static_cast<std::uint8_t>(v) <=
         static_cast<std::uint8_t>(QuarantineReason::kOperatorRequest);
}
[[nodiscard]] constexpr bool enum_value_valid(RequestKind v) noexcept {
  return static_cast<std::uint8_t>(v) <=
         static_cast<std::uint8_t>(RequestKind::kRecordFacilityChange);
}
[[nodiscard]] constexpr bool enum_value_valid(EventKind v) noexcept {
  return static_cast<std::uint8_t>(v) <= static_cast<std::uint8_t>(EventKind::kDependencyUnknown);
}

}  // namespace cxf

#endif  // CXF_TYPES_ENUMS_HPP
