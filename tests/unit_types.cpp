// Proof obligations for the closed domain types: every enumeration, the
// monotone generations, typed identities, the RFC 3339 timestamp, freshness
// windows and the generation-binding description.
//
// The vocabulary asserted here is the durable vocabulary: if a spelling, a
// numeric value or a bound moves, stored records stop decoding, so each of
// these checks is a compatibility guard as well as a behavioural one.

#include "support/test_harness.hpp"

#include "cxf/model/evidence.hpp"
#include "cxf/persist/record_io.hpp"
#include "cxf/runtime/fabric.hpp"
#include "cxf/support/clock.hpp"
#include "cxf/support/status.hpp"
#include "cxf/support/text.hpp"
#include "cxf/types/enums.hpp"
#include "cxf/types/generations.hpp"
#include "cxf/types/ids.hpp"
#include "cxf/types/timepoint.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cxf::ActivationResultKind;
using cxf::Code;
using cxf::CrashPoint;
using cxf::DependencyKind;
using cxf::Duration;
using cxf::EvidenceDimension;
using cxf::EvidenceSource;
using cxf::EvidenceVerdict;
using cxf::EventKind;
using cxf::FacilityGenerations;
using cxf::FrameKind;
using cxf::FreshnessWindow;
using cxf::LifecycleState;
using cxf::Liveness;
using cxf::QuarantineReason;
using cxf::RecordKind;
using cxf::RequestKind;
using cxf::Timestamp;

/// Every enumeration promises the same contract: one canonical lowercase
/// spelling per value, name() and parse() are exact inverses, and no other
/// spelling is accepted. This helper proves it for a whole table at once.
template <typename E, std::size_t N>
void round_trip_all(const std::array<std::pair<E, std::string_view>, N>& table,
                    std::string_view (*name_of)(E) noexcept,
                    cxf::Outcome<E> (*parse_of)(std::string_view)) {
  std::vector<std::string_view> seen;
  for (const std::pair<E, std::string_view>& entry : table) {
    const std::string_view spelling = name_of(entry.first);
    // Non-empty, canonical and free of uppercase: a second spelling would make
    // two requests name the same durable value differently.
    CHECK(!spelling.empty());
    CHECK_EQ(spelling, entry.second);
    CHECK(std::none_of(spelling.begin(), spelling.end(), [](char c) {
      return c >= 'A' && c <= 'Z';
    }));
    CHECK(std::none_of(seen.begin(), seen.end(),
                       [spelling](std::string_view other) { return other == spelling; }));
    seen.push_back(spelling);

    const cxf::Outcome<E> parsed = parse_of(spelling);
    REQUIRE(parsed.ok());
    CHECK_EQ(parsed.value(), entry.first);

    // Unknown, empty, padded and near-miss spellings never resolve to a value.
    CHECK(!parse_of("").ok());
    CHECK(!parse_of(" ").ok());
    const std::string extended(spelling);
    CHECK(!parse_of(extended + "x").ok());
    CHECK(!parse_of(cxf::upper_ascii(spelling)).ok());
  }
}

// ---------------------------------------------------------------------------
// Enumerations.
// ---------------------------------------------------------------------------

CXF_TEST(unit, enum_lifecycle_states_round_trip) {
  static const std::array<std::pair<LifecycleState, std::string_view>,
                          cxf::kLifecycleStateCount>
      table = {{{LifecycleState::kDeclared, "declared"},
                {LifecycleState::kIdentified, "identified"},
                {LifecycleState::kLocated, "located"},
                {LifecycleState::kDependencyValidated, "dependency_validated"},
                {LifecycleState::kCompatibilityValidated, "compatibility_validated"},
                {LifecycleState::kUtilitiesReady, "utilities_ready"},
                {LifecycleState::kNetworkReady, "network_ready"},
                {LifecycleState::kHealthValidated, "health_validated"},
                {LifecycleState::kActivationAuthorized, "activation_authorized"},
                {LifecycleState::kActivating, "activating"},
                {LifecycleState::kCommissioned, "commissioned"},
                {LifecycleState::kFailed, "failed"},
                {LifecycleState::kQuarantined, "quarantined"},
                {LifecycleState::kCancelled, "cancelled"}}};
  round_trip_all<LifecycleState, cxf::kLifecycleStateCount>(table, cxf::state_name,
                                                            cxf::parse_state);

  // The numeric values are part of the durable format, so they are pinned.
  for (std::size_t i = 0; i < table.size(); ++i) {
    CHECK_EQ(static_cast<std::uint8_t>(table[i].first), static_cast<std::uint8_t>(i));
  }
  // A cross-family spelling must not resolve.
  CHECK(!cxf::parse_state("identity").ok());
  CHECK(!cxf::parse_state("Commissioned").ok());
}

CXF_TEST(unit, enum_dimensions_round_trip) {
  static const std::array<std::pair<EvidenceDimension, std::string_view>,
                          cxf::kEvidenceDimensionCount>
      table = {{{EvidenceDimension::kIdentity, "identity"},
                {EvidenceDimension::kPlacement, "placement"},
                {EvidenceDimension::kDependency, "dependency"},
                {EvidenceDimension::kCompatibility, "compatibility"},
                {EvidenceDimension::kElectrical, "electrical"},
                {EvidenceDimension::kCooling, "cooling"},
                {EvidenceDimension::kNetwork, "network"},
                {EvidenceDimension::kHealth, "health"},
                {EvidenceDimension::kPolicy, "policy"},
                {EvidenceDimension::kServiceClass, "service_class"}}};
  round_trip_all<EvidenceDimension, cxf::kEvidenceDimensionCount>(
      table, cxf::dimension_name, cxf::parse_dimension);
  for (std::size_t i = 0; i < table.size(); ++i) {
    CHECK_EQ(static_cast<std::uint8_t>(table[i].first), static_cast<std::uint8_t>(i));
  }
  // The evaluator reports dimensions in this fixed order.
  const std::array<EvidenceDimension, cxf::kEvidenceDimensionCount> expected =
      cxf::all_dimensions();
  for (std::size_t i = 0; i < table.size(); ++i) {
    CHECK_EQ(expected[i], table[i].first);
    CHECK_EQ(cxf::dimension_rank(expected[i]), i);
  }
}

CXF_TEST(unit, enum_verdict_source_and_result_round_trip) {
  static const std::array<std::pair<EvidenceVerdict, std::string_view>, 3> verdicts = {
      {{EvidenceVerdict::kUnknown, "unknown"},
       {EvidenceVerdict::kSatisfied, "satisfied"},
       {EvidenceVerdict::kUnsatisfied, "unsatisfied"}}};
  round_trip_all<EvidenceVerdict, 3>(verdicts, cxf::verdict_name, cxf::parse_verdict);

  static const std::array<std::pair<EvidenceSource, std::string_view>, 3> sources = {
      {{EvidenceSource::kDeclared, "declared"},
       {EvidenceSource::kImported, "imported"},
       {EvidenceSource::kObserved, "observed"}}};
  round_trip_all<EvidenceSource, 3>(sources, cxf::source_name, cxf::parse_source);
  // Authority rises from a claim to an observation, which the ordering encodes.
  CHECK(cxf::source_authority(EvidenceSource::kDeclared) <
        cxf::source_authority(EvidenceSource::kImported));
  CHECK(cxf::source_authority(EvidenceSource::kImported) <
        cxf::source_authority(EvidenceSource::kObserved));

  static const std::array<std::pair<ActivationResultKind, std::string_view>, 3> results = {
      {{ActivationResultKind::kSucceeded, "succeeded"},
       {ActivationResultKind::kFailed, "failed"},
       {ActivationResultKind::kDeferred, "deferred"}}};
  round_trip_all<ActivationResultKind, 3>(results, cxf::activation_result_name,
                                          cxf::parse_activation_result);
}

CXF_TEST(unit, enum_dependency_and_quarantine_round_trip) {
  static const std::array<std::pair<DependencyKind, std::string_view>,
                          cxf::kDependencyKindCount>
      kinds = {{{DependencyKind::kSite, "site"},
                {DependencyKind::kRack, "rack"},
                {DependencyKind::kPowerFeed, "power_feed"},
                {DependencyKind::kCoolingZone, "cooling_zone"},
                {DependencyKind::kNetworkFabric, "network_fabric"},
                {DependencyKind::kUpstreamAsset, "upstream_asset"},
                {DependencyKind::kServiceClass, "service_class"},
                {DependencyKind::kOwnership, "ownership"}}};
  round_trip_all<DependencyKind, cxf::kDependencyKindCount>(kinds,
                                                            cxf::dependency_kind_name,
                                                            cxf::parse_dependency_kind);
  const std::array<DependencyKind, cxf::kDependencyKindCount> all = cxf::all_dependency_kinds();
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    CHECK_EQ(all[i], kinds[i].first);
    CHECK_EQ(static_cast<std::uint8_t>(kinds[i].first), static_cast<std::uint8_t>(i));
  }

  static const std::array<std::pair<QuarantineReason, std::string_view>, 9> reasons = {
      {{QuarantineReason::kNone, "none"},
       {QuarantineReason::kIdentityConflict, "identity_conflict"},
       {QuarantineReason::kUnsupportedHardware, "unsupported_hardware"},
       {QuarantineReason::kFailedDiagnostics, "failed_diagnostics"},
       {QuarantineReason::kStaleReadiness, "stale_readiness"},
       {QuarantineReason::kDependencyAmbiguity, "dependency_ambiguity"},
       {QuarantineReason::kContradictoryEvidence, "contradictory_evidence"},
       {QuarantineReason::kPolicyDenied, "policy_denied"},
       {QuarantineReason::kOperatorRequest, "operator_request"}}};
  round_trip_all<QuarantineReason, 9>(reasons, cxf::quarantine_reason_name,
                                      cxf::parse_quarantine_reason);
}

CXF_TEST(unit, enum_request_and_event_round_trip) {
  static const std::array<std::pair<RequestKind, std::string_view>, 11> requests = {
      {{RequestKind::kAdmitCandidate, "admit_candidate"},
       {RequestKind::kBindIdentity, "bind_identity"},
       {RequestKind::kBindPlacement, "bind_placement"},
       {RequestKind::kSubmitEvidence, "submit_evidence"},
       {RequestKind::kEvaluateReadiness, "evaluate_readiness"},
       {RequestKind::kAuthorizeActivation, "authorize_activation"},
       {RequestKind::kReportActivation, "report_activation"},
       {RequestKind::kQuarantine, "quarantine"},
       {RequestKind::kReleaseQuarantine, "release_quarantine"},
       {RequestKind::kCancel, "cancel"},
       {RequestKind::kRecordFacilityChange, "record_facility_change"}}};
  round_trip_all<RequestKind, 11>(requests, cxf::request_kind_name, cxf::parse_request_kind);
  for (std::size_t i = 0; i < requests.size(); ++i) {
    CHECK_EQ(static_cast<std::uint8_t>(requests[i].first), static_cast<std::uint8_t>(i));
  }

  static const std::array<std::pair<EventKind, std::string_view>, 18> events = {
      {{EventKind::kFabricCreated, "fabric_created"},
       {EventKind::kFabricOpened, "fabric_opened"},
       {EventKind::kRecovered, "recovered"},
       {EventKind::kCandidateAdmitted, "candidate_admitted"},
       {EventKind::kIdentityBound, "identity_bound"},
       {EventKind::kPlacementBound, "placement_bound"},
       {EventKind::kEvidenceRecorded, "evidence_recorded"},
       {EventKind::kReadinessEvaluated, "readiness_evaluated"},
       {EventKind::kActivationAuthorized, "activation_authorized"},
       {EventKind::kActivationReported, "activation_reported"},
       {EventKind::kCommissioned, "commissioned"},
       {EventKind::kQuarantined, "quarantined"},
       {EventKind::kQuarantineReleased, "quarantine_released"},
       {EventKind::kCancelled, "cancelled"},
       {EventKind::kFailed, "failed"},
       {EventKind::kAttemptOpened, "attempt_opened"},
       {EventKind::kFacilityChanged, "facility_changed"},
       {EventKind::kDependencyUnknown, "dependency_unknown"}}};
  round_trip_all<EventKind, 18>(events, cxf::event_kind_name, cxf::parse_event_kind);
  for (std::size_t i = 0; i < events.size(); ++i) {
    CHECK_EQ(static_cast<std::uint8_t>(events[i].first), static_cast<std::uint8_t>(i));
  }
}

CXF_TEST(unit, enum_record_frame_crash_and_liveness_round_trip) {
  // The durable spellings of these three are not fixed by a table in the
  // header, so the contract asserted is the one the header does pin: name() and
  // parse() are exact inverses over a lowercase, non-empty spelling.
  for (int i = 1; i <= static_cast<int>(cxf::kRecordKindCount); ++i) {
    const auto kind = static_cast<RecordKind>(static_cast<std::uint16_t>(i));
    const std::string_view spelling = cxf::record_kind_name(kind);
    CHECK(!spelling.empty());
    CHECK(std::none_of(spelling.begin(), spelling.end(),
                       [](char c) { return c >= 'A' && c <= 'Z'; }));
    const cxf::Outcome<RecordKind> parsed = cxf::parse_record_kind(spelling);
    REQUIRE(parsed.ok());
    CHECK_EQ(parsed.value(), kind);
    CHECK(!cxf::parse_record_kind("").ok());
    CHECK(!cxf::parse_record_kind("not_a_record_kind").ok());
    CHECK(!cxf::parse_record_kind(cxf::upper_ascii(spelling)).ok());
  }

  static const std::array<FrameKind, 2> frame_kinds = {FrameKind::kCommitGroup,
                                                       FrameKind::kSnapshot};
  for (const FrameKind kind : frame_kinds) {
    const std::string_view spelling = cxf::frame_kind_name(kind);
    CHECK(!spelling.empty());
    const cxf::Outcome<FrameKind> parsed = cxf::parse_frame_kind(spelling);
    REQUIRE(parsed.ok());
    CHECK_EQ(parsed.value(), kind);
    CHECK(!cxf::parse_frame_kind("").ok());
    CHECK(!cxf::parse_frame_kind("frame").ok());
  }

  static const std::array<CrashPoint, 4> crash_points = {
      CrashPoint::kNone, CrashPoint::kBeforeFlush, CrashPoint::kAfterFlush,
      CrashPoint::kAfterPublish};
  for (const CrashPoint point : crash_points) {
    const std::string_view spelling = cxf::crash_point_name(point);
    CHECK(!spelling.empty());
    const cxf::Outcome<CrashPoint> parsed = cxf::parse_crash_point(spelling);
    REQUIRE(parsed.ok());
    CHECK_EQ(parsed.value(), point);
    CHECK(!cxf::parse_crash_point("").ok());
    CHECK(!cxf::parse_crash_point("during_write").ok());
  }
  CHECK_EQ(static_cast<std::uint8_t>(CrashPoint::kNone), std::uint8_t{0});

  static const std::array<Liveness, 3> livenesses = {
      Liveness::kLive, Liveness::kExpired, Liveness::kGenerationMismatch};
  for (const Liveness liveness : livenesses) {
    CHECK(!cxf::liveness_name(liveness).empty());
  }
}

CXF_TEST(unit, enum_reason_code_vocabulary) {
  // Every declared reason code has a stable name and a real explanation, and
  // the durable decoder accepts exactly the declared numeric values.
  struct Entry {
    Code code;
    std::string_view name;
  };
  static const std::array<Entry, 49> codes = {{
      {Code::kOk, "Ok"},
      {Code::kMalformedInput, "MalformedInput"},
      {Code::kFieldMissing, "FieldMissing"},
      {Code::kFieldTooLong, "FieldTooLong"},
      {Code::kFieldInvalidUtf8, "FieldInvalidUtf8"},
      {Code::kFieldEmpty, "FieldEmpty"},
      {Code::kFieldOutOfRange, "FieldOutOfRange"},
      {Code::kFieldConflict, "FieldConflict"},
      {Code::kReservedFieldNonZero, "ReservedFieldNonZero"},
      {Code::kUnsupportedFormatVersion, "UnsupportedFormatVersion"},
      {Code::kTrailingBytes, "TrailingBytes"},
      {Code::kCandidateNotFound, "CandidateNotFound"},
      {Code::kCandidateAlreadyExists, "CandidateAlreadyExists"},
      {Code::kAssetIdentityConflict, "AssetIdentityConflict"},
      {Code::kAssetAlreadyCommissioned, "AssetAlreadyCommissioned"},
      {Code::kStateNotAllowed, "StateNotAllowed"},
      {Code::kTransitionNotAllowed, "TransitionNotAllowed"},
      {Code::kAttemptNotFound, "AttemptNotFound"},
      {Code::kCandidateTerminal, "CandidateTerminal"},
      {Code::kEvidenceUnknownDimension, "EvidenceUnknownDimension"},
      {Code::kEvidenceStale, "EvidenceStale"},
      {Code::kEvidenceContradictory, "EvidenceContradictory"},
      {Code::kEvidenceNegative, "EvidenceNegative"},
      {Code::kEvidenceMissing, "EvidenceMissing"},
      {Code::kEvidenceGenerationMismatch, "EvidenceGenerationMismatch"},
      {Code::kEvidenceDuplicateDigest, "EvidenceDuplicateDigest"},
      {Code::kPlanNotFound, "PlanNotFound"},
      {Code::kPlanStale, "PlanStale"},
      {Code::kAuthorityTokenInvalid, "AuthorityTokenInvalid"},
      {Code::kAuthorityTokenExpired, "AuthorityTokenExpired"},
      {Code::kAuthorityTokenConsumed, "AuthorityTokenConsumed"},
      {Code::kIdempotencyKeyReuse, "IdempotencyKeyReuse"},
      {Code::kGenerationRegression, "GenerationRegression"},
      {Code::kFencingTokenStale, "FencingTokenStale"},
      {Code::kDependencyUnsatisified, "DependencyUnsatisfied"},
      {Code::kDependencyCycle, "DependencyCycle"},
      {Code::kDependencyAmbiguous, "DependencyAmbiguous"},
      {Code::kPlacementUnbound, "PlacementUnbound"},
      {Code::kCompatibilityUnsupported, "CompatibilityUnsupported"},
      {Code::kPolicyNotSatisfied, "PolicyNotSatisfied"},
      {Code::kServiceClassUnsatisfied, "ServiceClassUnsatisfied"},
      {Code::kStorageUnavailable, "StorageUnavailable"},
      {Code::kStorageCorrupt, "StorageCorrupt"},
      {Code::kStorageLocked, "StorageLocked"},
      {Code::kStorageIo, "StorageIo"},
      {Code::kStorageUnsupportedFormat, "StorageUnsupportedFormat"},
      {Code::kInternalError, "InternalError"},
      {Code::kPreconditionViolated, "PreconditionViolated"},
      {Code::kNotImplemented, "NotImplemented"},
  }};
  for (const Entry& entry : codes) {
    CHECK_EQ(cxf::code_name(entry.code), entry.name);
    CHECK(!cxf::code_message(entry.code).empty());
    CHECK(cxf::code_message(entry.code) != std::string_view("unknown code"));
    CHECK(cxf::is_valid_code_value(static_cast<std::uint16_t>(entry.code)));
  }
  CHECK_EQ(cxf::code_name(Code::kOk), std::string_view("Ok"));
  CHECK(cxf::code_name(static_cast<Code>(1234)) == std::string_view("Unknown"));

  // A value that is not a declared code is refused rather than mapped onto a
  // neighbouring code.
  for (const unsigned int value :
       {1u, 9u, 24u, 29u, 39u, 49u, 59u, 69u, 79u, 83u, 100u, 65535u}) {
    CHECK(!cxf::is_valid_code_value(static_cast<std::uint16_t>(value)));
  }
  CHECK(cxf::is_storage_code(Code::kStorageUnavailable));
  CHECK(cxf::is_storage_code(Code::kStorageUnsupportedFormat));
  CHECK(!cxf::is_storage_code(Code::kOk));
  CHECK(!cxf::is_storage_code(Code::kInternalError));
  CHECK(cxf::is_storage_code(Code::kStorageLocked));

  const cxf::Status status = cxf::Status::error(Code::kStorageCorrupt, "detail", "context");
  CHECK(status.failed());
  CHECK(!status.ok());
  CHECK_EQ(status.code(), Code::kStorageCorrupt);
  CHECK_EQ(status.detail(), std::string("detail"));
  CHECK_EQ(status.context(), std::string("context"));
  CHECK(cxf::test::contains_substring(status.message(), "StorageCorrupt"));
  CHECK(cxf::test::contains_substring(status.message(), "detail"));
  CHECK(cxf::Status::success().ok());
  CHECK_EQ(cxf::Status().code(), Code::kOk);
}

CXF_TEST(unit, enum_domain_guards) {
  for (int i = 0; i < static_cast<int>(cxf::kLifecycleStateCount); ++i) {
    CHECK(cxf::enum_value_valid(static_cast<LifecycleState>(static_cast<std::uint8_t>(i))));
  }
  CHECK(!cxf::enum_value_valid(static_cast<LifecycleState>(cxf::kLifecycleStateCount)));
  CHECK(!cxf::enum_value_valid(static_cast<LifecycleState>(255)));

  for (int i = 0; i < static_cast<int>(cxf::kEvidenceDimensionCount); ++i) {
    CHECK(cxf::enum_value_valid(static_cast<EvidenceDimension>(static_cast<std::uint8_t>(i))));
  }
  CHECK(!cxf::enum_value_valid(static_cast<EvidenceDimension>(cxf::kEvidenceDimensionCount)));

  CHECK(cxf::enum_value_valid(EvidenceVerdict::kUnsatisfied));
  CHECK(!cxf::enum_value_valid(static_cast<EvidenceVerdict>(3)));
  CHECK(cxf::enum_value_valid(EvidenceSource::kObserved));
  CHECK(!cxf::enum_value_valid(static_cast<EvidenceSource>(3)));
  CHECK(cxf::enum_value_valid(ActivationResultKind::kDeferred));
  CHECK(!cxf::enum_value_valid(static_cast<ActivationResultKind>(3)));

  for (int i = 0; i < static_cast<int>(cxf::kDependencyKindCount); ++i) {
    CHECK(cxf::enum_value_valid(static_cast<DependencyKind>(static_cast<std::uint8_t>(i))));
  }
  CHECK(!cxf::enum_value_valid(static_cast<DependencyKind>(cxf::kDependencyKindCount)));

  CHECK(cxf::enum_value_valid(QuarantineReason::kOperatorRequest));
  CHECK(!cxf::enum_value_valid(static_cast<QuarantineReason>(9)));
  CHECK(cxf::enum_value_valid(RequestKind::kRecordFacilityChange));
  CHECK(!cxf::enum_value_valid(static_cast<RequestKind>(11)));
  CHECK(cxf::enum_value_valid(EventKind::kDependencyUnknown));
  CHECK(!cxf::enum_value_valid(static_cast<EventKind>(18)));
  CHECK(cxf::enum_value_valid(RecordKind::kIdAllocators));
  CHECK(!cxf::enum_value_valid(static_cast<RecordKind>(0)));
  CHECK(!cxf::enum_value_valid(static_cast<RecordKind>(10)));
  CHECK(cxf::enum_value_valid(FrameKind::kSnapshot));
  CHECK(!cxf::enum_value_valid(static_cast<FrameKind>(0)));
  CHECK(!cxf::enum_value_valid(static_cast<FrameKind>(3)));
}

CXF_TEST(unit, lifecycle_milestone_helpers) {
  const std::array<LifecycleState, cxf::kMilestoneStateCount> milestones =
      cxf::milestone_states();
  for (std::size_t i = 0; i < milestones.size(); ++i) {
    CHECK(cxf::is_milestone_state(milestones[i]));
    CHECK_EQ(cxf::state_rank(milestones[i]), i);
    CHECK_EQ(static_cast<std::uint8_t>(milestones[i]), static_cast<std::uint8_t>(i));
    CHECK(!cxf::is_terminal_state(milestones[i]) || milestones[i] == LifecycleState::kCommissioned);
  }
  // Declared is a chain state; the emergency exits are not.
  CHECK(cxf::is_milestone_state(LifecycleState::kDeclared));
  CHECK(cxf::is_milestone_state(LifecycleState::kCommissioned));
  CHECK(!cxf::is_milestone_state(LifecycleState::kFailed));
  CHECK(!cxf::is_milestone_state(LifecycleState::kQuarantined));
  CHECK(!cxf::is_milestone_state(LifecycleState::kCancelled));
  CHECK_EQ(cxf::state_rank(LifecycleState::kQuarantined), cxf::kMilestoneStateCount);
  CHECK(cxf::is_terminal_state(LifecycleState::kCommissioned));
  CHECK(cxf::is_terminal_state(LifecycleState::kFailed));
  CHECK(cxf::is_terminal_state(LifecycleState::kQuarantined));
  CHECK(cxf::is_terminal_state(LifecycleState::kCancelled));
  CHECK(!cxf::is_terminal_state(LifecycleState::kActivating));
  CHECK(!cxf::is_terminal_state(LifecycleState::kDeclared));
}

// ---------------------------------------------------------------------------
// Generations.
// ---------------------------------------------------------------------------

CXF_TEST(unit, generation_next_saturates) {
  using Gen = cxf::TopologyGeneration;
  CHECK_EQ(Gen::from_value(0).next().value(), 1u);
  CHECK_EQ(Gen::from_value(41).next().value(), 42u);
  CHECK_EQ(Gen::from_value(UINT64_MAX).next().value(), UINT64_MAX);
  // Saturating rather than wrapping: a wrapped generation would make stale
  // authority look current.
  CHECK(Gen::from_value(UINT64_MAX).next() == Gen::from_value(UINT64_MAX));
  CHECK(Gen::from_value(UINT64_MAX) > Gen::from_value(UINT64_MAX - 1));

  const cxf::Outcome<Gen> parsed = Gen::parse("18446744073709551615");
  REQUIRE_OK(parsed);
  CHECK_EQ(parsed.value().value(), UINT64_MAX);
  CHECK_EQ(parsed.value().str(), std::string("18446744073709551615"));
  CHECK_EQ(Gen::from_value(7).str(), std::string("7"));
  CHECK(Gen::from_value(0).is_zero());
  CHECK(!Gen::from_value(1).is_zero());

  CHECK_CODE(Gen::parse(""), Code::kFieldEmpty);
  CHECK_CODE(Gen::parse("007"), Code::kMalformedInput);
  CHECK_CODE(Gen::parse("-1"), Code::kMalformedInput);
  CHECK_CODE(Gen::parse("abc"), Code::kMalformedInput);
  CHECK_CODE(Gen::parse("18446744073709551616"), Code::kFieldOutOfRange);

  // Distinct tagged counters cannot be compared across tags, which is the
  // compile-time half of the guarantee; the runtime half is that the value is
  // carried unchanged through value()/from_value().
  CHECK_EQ(cxf::LifecycleGeneration::from_value(3).value(), 3u);
  CHECK_EQ(cxf::Revision::from_value(9).value(), 9u);
  CHECK_EQ(cxf::CommitSequence::from_value(11).value(), 11u);
}

// ---------------------------------------------------------------------------
// Identities.
// ---------------------------------------------------------------------------

CXF_TEST(unit, id_parse_format_and_zero) {
  using Candidate = cxf::CandidateId;
  const cxf::Outcome<Candidate> zero = Candidate::parse("0");
  REQUIRE_OK(zero);
  CHECK(zero.value().is_zero());
  CHECK_EQ(zero.value().value(), 0u);
  CHECK_EQ(zero.value().str(), std::string("0"));

  const cxf::Outcome<Candidate> twelve = Candidate::parse("12");
  REQUIRE_OK(twelve);
  CHECK(!twelve.value().is_zero());
  CHECK_EQ(twelve.value().str(), std::string("12"));
  CHECK_EQ(Candidate::parse(twelve.value().str()).value(), twelve.value());

  const cxf::Outcome<Candidate> maximum = Candidate::parse("18446744073709551615");
  REQUIRE_OK(maximum);
  CHECK_EQ(maximum.value().value(), UINT64_MAX);
  CHECK(twelve.value() < maximum.value());
  CHECK(maximum.value() != twelve.value());
  CHECK(Candidate::from_value(12) == twelve.value());

  CHECK_CODE(Candidate::parse(""), Code::kFieldEmpty);
  CHECK_CODE(Candidate::parse("007"), Code::kMalformedInput);
  CHECK_CODE(Candidate::parse("+1"), Code::kMalformedInput);
  CHECK_CODE(Candidate::parse("-1"), Code::kMalformedInput);
  CHECK_CODE(Candidate::parse("1a"), Code::kMalformedInput);
  CHECK_CODE(Candidate::parse(" 1"), Code::kMalformedInput);
  CHECK_CODE(Candidate::parse("1 "), Code::kMalformedInput);
  CHECK_CODE(Candidate::parse("18446744073709551616"), Code::kFieldOutOfRange);

  // Each identity type is distinct at compile time; the standard hash is
  // usable, which is what the in-memory indexes rely on.
  CHECK(cxf::EvidenceId::from_value(12) != cxf::EvidenceId::from_value(13));
  CHECK(cxf::AssetId::from_value(12) == cxf::AssetId::from_value(12));
  const std::hash<cxf::CandidateId> hasher;
  CHECK_EQ(hasher(twelve.value()), static_cast<std::size_t>(12));
}

// ---------------------------------------------------------------------------
// Timestamps.
// ---------------------------------------------------------------------------

CXF_TEST(unit, timestamp_round_trips) {
  static const std::array<std::string_view, 11> canonical = {
      "1970-01-01T00:00:00Z",
      "2024-02-29T12:34:56Z",
      "2024-02-29T12:34:56.123456789Z",
      "2024-01-01T00:00:00.5Z",
      "2024-01-01T00:00:00.000000001Z",
      "1969-12-31T23:59:59.999999999Z",
      "1969-12-31T23:59:59Z",
      "1678-01-01T00:00:00Z",
      "2261-01-01T00:00:00Z",
      "2000-02-29T23:59:59.999999999Z",
      "1900-03-01T00:00:00Z"};
  for (const std::string_view text : canonical) {
    const cxf::Outcome<Timestamp> parsed = Timestamp::parse_rfc3339(text);
    REQUIRE(parsed.ok());
    // One spelling per instant: formatting what was parsed gives it back byte
    // for byte, and parsing the formatted text gives the same instant.
    CHECK_EQ(parsed.value().to_rfc3339(), std::string(text));
    const cxf::Outcome<Timestamp> again = Timestamp::parse_rfc3339(parsed.value().to_rfc3339());
    REQUIRE(again.ok());
    CHECK_EQ(again.value(), parsed.value());
  }

  // The leap day exists in 2024 and in 2000 (divisible by 400) and is refused
  // in 1900 (divisible by 100 but not 400).
  CHECK(Timestamp::parse_rfc3339("2024-02-29T00:00:00Z").ok());
  CHECK(Timestamp::parse_rfc3339("2000-02-29T00:00:00Z").ok());
  CHECK(!Timestamp::parse_rfc3339("1900-02-29T00:00:00Z").ok());

  // Nanosecond precision is carried exactly: one nanosecond is one tick.
  const cxf::Outcome<Timestamp> precise =
      Timestamp::parse_rfc3339("2024-01-01T00:00:00.000000001Z");
  REQUIRE_OK(precise);
  CHECK_EQ(precise.value().unix_nanos() % cxf::kNanosPerSecond, 1);
  const cxf::Outcome<Timestamp> precise_end =
      Timestamp::parse_rfc3339("2024-01-01T00:00:00.999999999Z");
  REQUIRE_OK(precise_end);
  CHECK_EQ(precise_end.value() - precise.value(), Duration::from_nanos(999999998));

  const cxf::Outcome<Timestamp> epoch = Timestamp::parse_rfc3339("1970-01-01T00:00:00Z");
  REQUIRE_OK(epoch);
  CHECK_EQ(epoch.value(), Timestamp::epoch());
  CHECK_EQ(epoch.value().unix_nanos(), 0);
  CHECK_EQ(epoch.value().unix_seconds(), 0);
  CHECK_EQ(Timestamp::from_unix_seconds(10).unix_nanos(), 10 * cxf::kNanosPerSecond);
}

CXF_TEST(unit, timestamp_rejects_non_canonical_forms) {
  CHECK_CODE(Timestamp::parse_rfc3339(""), Code::kMalformedInput);
  CHECK_CODE(Timestamp::parse_rfc3339("not-a-time"), Code::kMalformedInput);
  // Month, day, hour, minute and second ranges.
  CHECK_CODE(Timestamp::parse_rfc3339("2024-13-01T00:00:00Z"), Code::kFieldOutOfRange);
  CHECK_CODE(Timestamp::parse_rfc3339("2024-00-01T00:00:00Z"), Code::kFieldOutOfRange);
  CHECK_CODE(Timestamp::parse_rfc3339("2024-01-32T00:00:00Z"), Code::kFieldOutOfRange);
  CHECK_CODE(Timestamp::parse_rfc3339("2024-01-00T00:00:00Z"), Code::kFieldOutOfRange);
  CHECK_CODE(Timestamp::parse_rfc3339("2023-02-29T00:00:00Z"), Code::kFieldOutOfRange);
  CHECK_CODE(Timestamp::parse_rfc3339("2024-04-31T00:00:00Z"), Code::kFieldOutOfRange);
  CHECK_CODE(Timestamp::parse_rfc3339("2024-01-01T24:00:00Z"), Code::kFieldOutOfRange);
  CHECK_CODE(Timestamp::parse_rfc3339("2024-01-01T00:60:00Z"), Code::kFieldOutOfRange);
  CHECK_CODE(Timestamp::parse_rfc3339("2024-01-01T00:00:60Z"), Code::kFieldOutOfRange);
  // Separators are exactly one spelling: 'T' and 'Z' are required.
  CHECK_CODE(Timestamp::parse_rfc3339("2024-01-01 00:00:00Z"), Code::kMalformedInput);
  CHECK_CODE(Timestamp::parse_rfc3339("2024-01-01T00:00:00"), Code::kMalformedInput);
  CHECK_CODE(Timestamp::parse_rfc3339("2024/01/01T00:00:00Z"), Code::kMalformedInput);
  CHECK_CODE(Timestamp::parse_rfc3339("2024-01-01T00-00-00Z"), Code::kMalformedInput);
  CHECK_CODE(Timestamp::parse_rfc3339("2024-01-01t00:00:00Z"), Code::kMalformedInput);
  CHECK_CODE(Timestamp::parse_rfc3339("2024-01-01T00:00:00z"), Code::kMalformedInput);
  // Fractions are 1..9 digits.
  CHECK_CODE(Timestamp::parse_rfc3339("2024-01-01T00:00:00.1234567890Z"),
             Code::kFieldOutOfRange);
  CHECK_CODE(Timestamp::parse_rfc3339("2024-01-01T00:00:00.Z"), Code::kFieldOutOfRange);
  CHECK_CODE(Timestamp::parse_rfc3339("2024-01-01T00:00:00.12aZ"), Code::kMalformedInput);
  // The supported year range is explicit.
  CHECK_CODE(Timestamp::parse_rfc3339("1677-12-31T23:59:59Z"), Code::kFieldOutOfRange);
  CHECK_CODE(Timestamp::parse_rfc3339("2262-01-01T00:00:00Z"), Code::kFieldOutOfRange);
}

CXF_TEST(unit, timestamp_arithmetic_and_order) {
  const Timestamp base = Timestamp::from_unix_seconds(1000);
  CHECK_EQ(base + Duration::from_seconds(5), Timestamp::from_unix_seconds(1005));
  CHECK_EQ(base + Duration::from_nanos(1) - base, Duration::from_nanos(1));
  CHECK(base < base + Duration::from_nanos(1));
  CHECK(base + Duration::from_nanos(1) > base);
  CHECK(base <= base);
  CHECK(base >= base);
  CHECK(base == Timestamp::from_unix_seconds(1000));
  CHECK(base != Timestamp::from_unix_seconds(1001));
  CHECK(Timestamp::from_unix_nanos(-1) < Timestamp::epoch());

  cxf::ManualClock clock(Timestamp::from_unix_seconds(500));
  CHECK_EQ(clock.now(), Timestamp::from_unix_seconds(500));
  clock.advance(Duration::from_seconds(2));
  CHECK_EQ(clock.now(), Timestamp::from_unix_seconds(502));
  clock.set(Timestamp::from_unix_seconds(1));
  CHECK_EQ(clock.now(), Timestamp::from_unix_seconds(1));
  cxf::SystemClock system;
  CHECK(system.now() > Timestamp::from_unix_seconds(1600000000));
}

// ---------------------------------------------------------------------------
// Freshness windows.
// ---------------------------------------------------------------------------

CXF_TEST(unit, freshness_window_parsing) {
  const FreshnessWindow never = FreshnessWindow::never();
  CHECK(never.allows_all_time());
  CHECK_EQ(never.str(), std::string("never"));

  const cxf::Outcome<FreshnessWindow> parsed_never = FreshnessWindow::parse("never");
  REQUIRE_OK(parsed_never);
  CHECK(parsed_never.value().allows_all_time());
  CHECK(parsed_never.value() == never);

  const cxf::Outcome<FreshnessWindow> hour = FreshnessWindow::parse("3600");
  REQUIRE_OK(hour);
  CHECK(!hour.value().allows_all_time());
  CHECK_EQ(hour.value().window(), Duration::from_seconds(3600));
  CHECK_EQ(hour.value().str(), std::string("3600"));
  CHECK(hour.value() == FreshnessWindow::of(Duration::from_seconds(3600)));
  CHECK(hour.value() != never);

  const cxf::Outcome<FreshnessWindow> zero = FreshnessWindow::parse("0");
  REQUIRE_OK(zero);
  CHECK_EQ(zero.value().window(), Duration());
  CHECK(zero.value().window().is_zero());
  CHECK_EQ(zero.value().str(), std::string("0"));

  // Anything that is not "never" or a canonical decimal number of seconds is
  // refused, so a typo can never widen a window silently.
  CHECK_CODE(FreshnessWindow::parse(""), Code::kMalformedInput);
  CHECK_CODE(FreshnessWindow::parse("Never"), Code::kMalformedInput);
  CHECK_CODE(FreshnessWindow::parse("NEVER"), Code::kMalformedInput);
  CHECK_CODE(FreshnessWindow::parse(" never"), Code::kMalformedInput);
  CHECK_CODE(FreshnessWindow::parse("-1"), Code::kMalformedInput);
  CHECK_CODE(FreshnessWindow::parse("1.5"), Code::kMalformedInput);
  CHECK_CODE(FreshnessWindow::parse("0x10"), Code::kMalformedInput);
  CHECK_CODE(FreshnessWindow::parse("01"), Code::kMalformedInput);
}

CXF_TEST(unit, freshness_window_expiry_semantics) {
  const Timestamp observed = Timestamp::from_unix_seconds(1000);
  const FreshnessWindow ten = FreshnessWindow::of(Duration::from_seconds(10));

  // Fresh at the instant of observation and strictly inside the window.
  CHECK(!ten.is_expired(observed, observed));
  CHECK(!ten.is_expired(observed, observed + Duration::from_seconds(9)));
  CHECK(!ten.is_expired(observed, observed + Duration::from_seconds(10)));
  // Expired only once the window has been exceeded; the boundary itself is
  // still live, which is what "not elapsed" means.
  CHECK(ten.is_expired(observed, observed + Duration::from_seconds(10) + Duration::from_nanos(1)));
  CHECK(ten.is_expired(observed, observed + Duration::from_seconds(11)));

  // A clock that moved backwards is never treated as fresh.
  // (Negative durations are how a backwards clock is expressed: the header
  // defines Timestamp + Duration and Timestamp - Timestamp, not Timestamp -
  // Duration.)
  CHECK(ten.is_expired(observed, observed + Duration::from_nanos(-1)));
  CHECK(ten.is_expired(observed, observed + Duration::from_seconds(-1)));

  // "never" is the one window a backwards clock cannot trip, and the zero
  // window is expired for any positive elapsed time.
  CHECK(!FreshnessWindow::never().is_expired(observed, observed + Duration::from_seconds(999999)));
  CHECK(!FreshnessWindow::never().is_expired(observed, observed + Duration::from_seconds(-1)));
  const FreshnessWindow zero = FreshnessWindow::of(Duration());
  CHECK(!zero.is_expired(observed, observed));
  CHECK(zero.is_expired(observed, observed + Duration::from_nanos(1)));
  CHECK(zero.is_expired(observed, observed + Duration::from_nanos(-1)));

  CHECK_EQ(cxf::render_duration(Duration::from_seconds(90)), std::string("90s"));
  CHECK_EQ(cxf::render_duration(Duration::from_millis(1500)), std::string("1500ms"));
  CHECK_EQ(cxf::render_duration(Duration::from_nanos(1234)), std::string("1234ns"));
  CHECK_EQ(cxf::render_duration(Duration::from_nanos(0)), std::string("0s"));
  CHECK_EQ(cxf::render_duration(Duration::from_nanos(-5)), std::string("-5ns"));
}

// ---------------------------------------------------------------------------
// Generation binding description.
// ---------------------------------------------------------------------------

CXF_TEST(unit, facility_generations_differences) {
  const FacilityGenerations base{};
  CHECK(base.differences(base).empty());

  FacilityGenerations one = base;
  one.power = cxf::PowerGeneration::from_value(3);
  const std::vector<std::string> moved_power = base.differences(one);
  CHECK_EQ(moved_power.size(), std::size_t{1});
  CHECK_EQ(moved_power[0], std::string("power"));
  // The relation is symmetric in content and names exactly the same fields.
  const std::vector<std::string> moved_back = one.differences(base);
  CHECK_EQ(moved_back.size(), std::size_t{1});
  CHECK_EQ(moved_back[0], std::string("power"));

  FacilityGenerations some = base;
  some.topology = cxf::TopologyGeneration::from_value(1);
  some.cooling = cxf::CoolingGeneration::from_value(2);
  const std::vector<std::string> moved_some = base.differences(some);
  CHECK_EQ(moved_some.size(), std::size_t{2});
  CHECK_EQ(moved_some[0], std::string("topology"));
  CHECK_EQ(moved_some[1], std::string("cooling"));

  FacilityGenerations all = base;
  all.topology = cxf::TopologyGeneration::from_value(1);
  all.power = cxf::PowerGeneration::from_value(1);
  all.cooling = cxf::CoolingGeneration::from_value(1);
  all.network = cxf::NetworkGeneration::from_value(1);
  all.policy = cxf::PolicyGeneration::from_value(1);
  all.dependency = cxf::DependencyGeneration::from_value(1);
  all.firmware = cxf::FirmwareGeneration::from_value(1);
  all.hardware = cxf::HardwareGeneration::from_value(1);
  all.lifecycle = cxf::LifecycleGeneration::from_value(1);
  const std::vector<std::string> moved_all = base.differences(all);
  const std::vector<std::string> expected = {"topology", "power",       "cooling",
                                             "network",  "policy",      "dependency",
                                             "firmware", "hardware",    "lifecycle"};
  CHECK_EQ(moved_all.size(), expected.size());
  for (std::size_t i = 0; i < expected.size() && i < moved_all.size(); ++i) {
    CHECK_EQ(moved_all[i], expected[i]);
  }
  CHECK(base != all);
  CHECK(all == all);
  CHECK(base == FacilityGenerations{});
}

}  // namespace
