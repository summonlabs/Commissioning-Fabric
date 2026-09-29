#include "cxf/types/enums.hpp"

#include <array>
#include <string>
#include <utility>

namespace cxf {
namespace {

template <typename E, std::size_t N>
[[nodiscard]] Outcome<E> parse_against(std::string_view text,
                                       const std::array<std::pair<std::string_view, E>, N>& table,
                                       std::string_view what) {
  for (const auto& entry : table) {
    if (entry.first == text) {
      return entry.second;
    }
  }
  return make_error(Code::kFieldOutOfRange, std::string("unknown ") + std::string(what),
                    std::string(text));
}

}  // namespace

std::string_view state_name(LifecycleState state) noexcept {
  switch (state) {
    case LifecycleState::kDeclared: return "declared";
    case LifecycleState::kIdentified: return "identified";
    case LifecycleState::kLocated: return "located";
    case LifecycleState::kDependencyValidated: return "dependency_validated";
    case LifecycleState::kCompatibilityValidated: return "compatibility_validated";
    case LifecycleState::kUtilitiesReady: return "utilities_ready";
    case LifecycleState::kNetworkReady: return "network_ready";
    case LifecycleState::kHealthValidated: return "health_validated";
    case LifecycleState::kActivationAuthorized: return "activation_authorized";
    case LifecycleState::kActivating: return "activating";
    case LifecycleState::kCommissioned: return "commissioned";
    case LifecycleState::kFailed: return "failed";
    case LifecycleState::kQuarantined: return "quarantined";
    case LifecycleState::kCancelled: return "cancelled";
  }
  return "invalid";
}

Outcome<LifecycleState> parse_state(std::string_view text) {
  static const std::array<std::pair<std::string_view, LifecycleState>, kLifecycleStateCount>
      kTable = {{{"declared", LifecycleState::kDeclared},
                 {"identified", LifecycleState::kIdentified},
                 {"located", LifecycleState::kLocated},
                 {"dependency_validated", LifecycleState::kDependencyValidated},
                 {"compatibility_validated", LifecycleState::kCompatibilityValidated},
                 {"utilities_ready", LifecycleState::kUtilitiesReady},
                 {"network_ready", LifecycleState::kNetworkReady},
                 {"health_validated", LifecycleState::kHealthValidated},
                 {"activation_authorized", LifecycleState::kActivationAuthorized},
                 {"activating", LifecycleState::kActivating},
                 {"commissioned", LifecycleState::kCommissioned},
                 {"failed", LifecycleState::kFailed},
                 {"quarantined", LifecycleState::kQuarantined},
                 {"cancelled", LifecycleState::kCancelled}}};
  return parse_against<LifecycleState, kLifecycleStateCount>(text, kTable, "lifecycle state");
}

std::array<LifecycleState, kMilestoneStateCount> milestone_states() noexcept {
  return {LifecycleState::kDeclared,         LifecycleState::kIdentified,
          LifecycleState::kLocated,          LifecycleState::kDependencyValidated,
          LifecycleState::kCompatibilityValidated, LifecycleState::kUtilitiesReady,
          LifecycleState::kNetworkReady,     LifecycleState::kHealthValidated,
          LifecycleState::kActivationAuthorized, LifecycleState::kActivating,
          LifecycleState::kCommissioned};
}

std::string_view dimension_name(EvidenceDimension dimension) noexcept {
  switch (dimension) {
    case EvidenceDimension::kIdentity: return "identity";
    case EvidenceDimension::kPlacement: return "placement";
    case EvidenceDimension::kDependency: return "dependency";
    case EvidenceDimension::kCompatibility: return "compatibility";
    case EvidenceDimension::kElectrical: return "electrical";
    case EvidenceDimension::kCooling: return "cooling";
    case EvidenceDimension::kNetwork: return "network";
    case EvidenceDimension::kHealth: return "health";
    case EvidenceDimension::kPolicy: return "policy";
    case EvidenceDimension::kServiceClass: return "service_class";
  }
  return "invalid";
}

Outcome<EvidenceDimension> parse_dimension(std::string_view text) {
  static const std::array<std::pair<std::string_view, EvidenceDimension>,
                          kEvidenceDimensionCount>
      kTable = {{{"identity", EvidenceDimension::kIdentity},
                 {"placement", EvidenceDimension::kPlacement},
                 {"dependency", EvidenceDimension::kDependency},
                 {"compatibility", EvidenceDimension::kCompatibility},
                 {"electrical", EvidenceDimension::kElectrical},
                 {"cooling", EvidenceDimension::kCooling},
                 {"network", EvidenceDimension::kNetwork},
                 {"health", EvidenceDimension::kHealth},
                 {"policy", EvidenceDimension::kPolicy},
                 {"service_class", EvidenceDimension::kServiceClass}}};
  return parse_against<EvidenceDimension, kEvidenceDimensionCount>(text, kTable,
                                                                   "evidence dimension");
}

std::array<EvidenceDimension, kEvidenceDimensionCount> all_dimensions() noexcept {
  return {EvidenceDimension::kIdentity,    EvidenceDimension::kPlacement,
          EvidenceDimension::kDependency,  EvidenceDimension::kCompatibility,
          EvidenceDimension::kElectrical,  EvidenceDimension::kCooling,
          EvidenceDimension::kNetwork,     EvidenceDimension::kHealth,
          EvidenceDimension::kPolicy,      EvidenceDimension::kServiceClass};
}

std::string_view verdict_name(EvidenceVerdict verdict) noexcept {
  switch (verdict) {
    case EvidenceVerdict::kUnknown: return "unknown";
    case EvidenceVerdict::kSatisfied: return "satisfied";
    case EvidenceVerdict::kUnsatisfied: return "unsatisfied";
  }
  return "invalid";
}

Outcome<EvidenceVerdict> parse_verdict(std::string_view text) {
  static const std::array<std::pair<std::string_view, EvidenceVerdict>, 3> kTable = {
      {{"unknown", EvidenceVerdict::kUnknown},
       {"satisfied", EvidenceVerdict::kSatisfied},
       {"unsatisfied", EvidenceVerdict::kUnsatisfied}}};
  return parse_against<EvidenceVerdict, 3>(text, kTable, "evidence verdict");
}

std::string_view source_name(EvidenceSource source) noexcept {
  switch (source) {
    case EvidenceSource::kDeclared: return "declared";
    case EvidenceSource::kImported: return "imported";
    case EvidenceSource::kObserved: return "observed";
  }
  return "invalid";
}

Outcome<EvidenceSource> parse_source(std::string_view text) {
  static const std::array<std::pair<std::string_view, EvidenceSource>, 3> kTable = {
      {{"declared", EvidenceSource::kDeclared},
       {"imported", EvidenceSource::kImported},
       {"observed", EvidenceSource::kObserved}}};
  return parse_against<EvidenceSource, 3>(text, kTable, "evidence source");
}

std::string_view activation_result_name(ActivationResultKind kind) noexcept {
  switch (kind) {
    case ActivationResultKind::kSucceeded: return "succeeded";
    case ActivationResultKind::kFailed: return "failed";
    case ActivationResultKind::kDeferred: return "deferred";
  }
  return "invalid";
}

Outcome<ActivationResultKind> parse_activation_result(std::string_view text) {
  static const std::array<std::pair<std::string_view, ActivationResultKind>, 3> kTable = {
      {{"succeeded", ActivationResultKind::kSucceeded},
       {"failed", ActivationResultKind::kFailed},
       {"deferred", ActivationResultKind::kDeferred}}};
  return parse_against<ActivationResultKind, 3>(text, kTable, "activation result");
}

std::string_view dependency_kind_name(DependencyKind kind) noexcept {
  switch (kind) {
    case DependencyKind::kSite: return "site";
    case DependencyKind::kRack: return "rack";
    case DependencyKind::kPowerFeed: return "power_feed";
    case DependencyKind::kCoolingZone: return "cooling_zone";
    case DependencyKind::kNetworkFabric: return "network_fabric";
    case DependencyKind::kUpstreamAsset: return "upstream_asset";
    case DependencyKind::kServiceClass: return "service_class";
    case DependencyKind::kOwnership: return "ownership";
  }
  return "invalid";
}

Outcome<DependencyKind> parse_dependency_kind(std::string_view text) {
  static const std::array<std::pair<std::string_view, DependencyKind>, kDependencyKindCount>
      kTable = {{{"site", DependencyKind::kSite},
                 {"rack", DependencyKind::kRack},
                 {"power_feed", DependencyKind::kPowerFeed},
                 {"cooling_zone", DependencyKind::kCoolingZone},
                 {"network_fabric", DependencyKind::kNetworkFabric},
                 {"upstream_asset", DependencyKind::kUpstreamAsset},
                 {"service_class", DependencyKind::kServiceClass},
                 {"ownership", DependencyKind::kOwnership}}};
  return parse_against<DependencyKind, kDependencyKindCount>(text, kTable, "dependency kind");
}

std::array<DependencyKind, kDependencyKindCount> all_dependency_kinds() noexcept {
  return {DependencyKind::kSite,        DependencyKind::kRack,
          DependencyKind::kPowerFeed,   DependencyKind::kCoolingZone,
          DependencyKind::kNetworkFabric, DependencyKind::kUpstreamAsset,
          DependencyKind::kServiceClass, DependencyKind::kOwnership};
}

std::string_view quarantine_reason_name(QuarantineReason reason) noexcept {
  switch (reason) {
    case QuarantineReason::kNone: return "none";
    case QuarantineReason::kIdentityConflict: return "identity_conflict";
    case QuarantineReason::kUnsupportedHardware: return "unsupported_hardware";
    case QuarantineReason::kFailedDiagnostics: return "failed_diagnostics";
    case QuarantineReason::kStaleReadiness: return "stale_readiness";
    case QuarantineReason::kDependencyAmbiguity: return "dependency_ambiguity";
    case QuarantineReason::kContradictoryEvidence: return "contradictory_evidence";
    case QuarantineReason::kPolicyDenied: return "policy_denied";
    case QuarantineReason::kOperatorRequest: return "operator_request";
  }
  return "invalid";
}

Outcome<QuarantineReason> parse_quarantine_reason(std::string_view text) {
  static const std::array<std::pair<std::string_view, QuarantineReason>, 9> kTable = {
      {{"none", QuarantineReason::kNone},
       {"identity_conflict", QuarantineReason::kIdentityConflict},
       {"unsupported_hardware", QuarantineReason::kUnsupportedHardware},
       {"failed_diagnostics", QuarantineReason::kFailedDiagnostics},
       {"stale_readiness", QuarantineReason::kStaleReadiness},
       {"dependency_ambiguity", QuarantineReason::kDependencyAmbiguity},
       {"contradictory_evidence", QuarantineReason::kContradictoryEvidence},
       {"policy_denied", QuarantineReason::kPolicyDenied},
       {"operator_request", QuarantineReason::kOperatorRequest}}};
  return parse_against<QuarantineReason, 9>(text, kTable, "quarantine reason");
}

std::string_view request_kind_name(RequestKind kind) noexcept {
  switch (kind) {
    case RequestKind::kAdmitCandidate: return "admit_candidate";
    case RequestKind::kBindIdentity: return "bind_identity";
    case RequestKind::kBindPlacement: return "bind_placement";
    case RequestKind::kSubmitEvidence: return "submit_evidence";
    case RequestKind::kEvaluateReadiness: return "evaluate_readiness";
    case RequestKind::kAuthorizeActivation: return "authorize_activation";
    case RequestKind::kReportActivation: return "report_activation";
    case RequestKind::kQuarantine: return "quarantine";
    case RequestKind::kReleaseQuarantine: return "release_quarantine";
    case RequestKind::kCancel: return "cancel";
    case RequestKind::kRecordFacilityChange: return "record_facility_change";
  }
  return "invalid";
}

Outcome<RequestKind> parse_request_kind(std::string_view text) {
  static const std::array<std::pair<std::string_view, RequestKind>, 11> kTable = {
      {{"admit_candidate", RequestKind::kAdmitCandidate},
       {"bind_identity", RequestKind::kBindIdentity},
       {"bind_placement", RequestKind::kBindPlacement},
       {"submit_evidence", RequestKind::kSubmitEvidence},
       {"evaluate_readiness", RequestKind::kEvaluateReadiness},
       {"authorize_activation", RequestKind::kAuthorizeActivation},
       {"report_activation", RequestKind::kReportActivation},
       {"quarantine", RequestKind::kQuarantine},
       {"release_quarantine", RequestKind::kReleaseQuarantine},
       {"cancel", RequestKind::kCancel},
       {"record_facility_change", RequestKind::kRecordFacilityChange}}};
  return parse_against<RequestKind, 11>(text, kTable, "request kind");
}

std::string_view event_kind_name(EventKind kind) noexcept {
  switch (kind) {
    case EventKind::kFabricCreated: return "fabric_created";
    case EventKind::kFabricOpened: return "fabric_opened";
    case EventKind::kRecovered: return "recovered";
    case EventKind::kCandidateAdmitted: return "candidate_admitted";
    case EventKind::kIdentityBound: return "identity_bound";
    case EventKind::kPlacementBound: return "placement_bound";
    case EventKind::kEvidenceRecorded: return "evidence_recorded";
    case EventKind::kReadinessEvaluated: return "readiness_evaluated";
    case EventKind::kActivationAuthorized: return "activation_authorized";
    case EventKind::kActivationReported: return "activation_reported";
    case EventKind::kCommissioned: return "commissioned";
    case EventKind::kQuarantined: return "quarantined";
    case EventKind::kQuarantineReleased: return "quarantine_released";
    case EventKind::kCancelled: return "cancelled";
    case EventKind::kFailed: return "failed";
    case EventKind::kAttemptOpened: return "attempt_opened";
    case EventKind::kFacilityChanged: return "facility_changed";
    case EventKind::kDependencyUnknown: return "dependency_unknown";
  }
  return "invalid";
}

Outcome<EventKind> parse_event_kind(std::string_view text) {
  static const std::array<std::pair<std::string_view, EventKind>, 18> kTable = {
      {{"fabric_created", EventKind::kFabricCreated},
       {"fabric_opened", EventKind::kFabricOpened},
       {"recovered", EventKind::kRecovered},
       {"candidate_admitted", EventKind::kCandidateAdmitted},
       {"identity_bound", EventKind::kIdentityBound},
       {"placement_bound", EventKind::kPlacementBound},
       {"evidence_recorded", EventKind::kEvidenceRecorded},
       {"readiness_evaluated", EventKind::kReadinessEvaluated},
       {"activation_authorized", EventKind::kActivationAuthorized},
       {"activation_reported", EventKind::kActivationReported},
       {"commissioned", EventKind::kCommissioned},
       {"quarantined", EventKind::kQuarantined},
       {"quarantine_released", EventKind::kQuarantineReleased},
       {"cancelled", EventKind::kCancelled},
       {"failed", EventKind::kFailed},
       {"attempt_opened", EventKind::kAttemptOpened},
       {"facility_changed", EventKind::kFacilityChanged},
       {"dependency_unknown", EventKind::kDependencyUnknown}}};
  return parse_against<EventKind, 18>(text, kTable, "event kind");
}

}  // namespace cxf
