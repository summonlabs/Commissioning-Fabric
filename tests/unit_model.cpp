// Proof obligations for the model layer: the lifecycle state machine, the
// candidate and dependency validators, evidence liveness and precedence, the
// readiness evaluator, the report digest and the dependency graph.
//
// Readiness is the heart of the runtime, so most checks here are about what a
// report is NOT allowed to claim: a declared claim never satisfies a dimension,
// an expired or generation-mismatched record is never live, a contradiction is
// never resolved by picking a winner, and a gap in the chain is never skipped.

#include "support/test_harness.hpp"

#include "cxf/model/candidate.hpp"
#include "cxf/model/dependency.hpp"
#include "cxf/model/evidence.hpp"
#include "cxf/model/lifecycle.hpp"
#include "cxf/model/readiness.hpp"
#include "cxf/model/record.hpp"
#include "cxf/runtime/event.hpp"
#include "cxf/runtime/fabric.hpp"
#include "cxf/support/clock.hpp"
#include "cxf/support/digest.hpp"
#include "cxf/support/serial.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using cxf::AttemptId;
using cxf::CandidateId;
using cxf::Code;
using cxf::CommissioningCandidate;
using cxf::DependencyFact;
using cxf::DependencyGeneration;
using cxf::DependencyKind;
using cxf::DependencyNode;
using cxf::DependencyOutcome;
using cxf::DependencyRef;
using cxf::DependencyResolution;
using cxf::DimensionEvaluation;
using cxf::Duration;
using cxf::EvidenceDimension;
using cxf::EvidenceId;
using cxf::EvidenceRecord;
using cxf::EvidenceSource;
using cxf::EvidenceVerdict;
using cxf::FacilityGenerations;
using cxf::FacilityRecord;
using cxf::FreshnessWindow;
using cxf::LifecycleGeneration;
using cxf::LifecycleState;
using cxf::Liveness;
using cxf::ObservationSequence;
using cxf::ReadinessPolicy;
using cxf::ReadinessReport;
using cxf::Revision;
using cxf::Timestamp;

constexpr std::int64_t kNow = 1000000;  // seconds since the epoch, fixed

[[nodiscard]] Timestamp now() { return Timestamp::from_unix_seconds(kNow); }

[[nodiscard]] FacilityGenerations generations_v1() {
  FacilityGenerations g;
  g.topology = cxf::TopologyGeneration::from_value(4);
  g.power = cxf::PowerGeneration::from_value(9);
  g.cooling = cxf::CoolingGeneration::from_value(7);
  g.network = cxf::NetworkGeneration::from_value(11);
  g.policy = cxf::PolicyGeneration::from_value(2);
  g.dependency = DependencyGeneration::from_value(5);
  g.firmware = cxf::FirmwareGeneration::from_value(12);
  g.hardware = cxf::HardwareGeneration::from_value(3);
  g.lifecycle = LifecycleGeneration::from_value(1);
  return g;
}

/// A candidate with a bound identity, a bound placement and one live attempt.
[[nodiscard]] CommissioningCandidate bound_candidate() {
  CommissioningCandidate candidate;
  candidate.id = CandidateId::from_value(1);
  candidate.name = "node-1";
  candidate.state = LifecycleState::kDeclared;
  candidate.lifecycle = LifecycleGeneration::from_value(1);
  candidate.incarnation = cxf::IncarnationId::from_value(1);
  candidate.revision = Revision::from_value(1);
  candidate.generations = generations_v1();
  candidate.declaration.registry_name = "registry-1";
  candidate.declaration.model = "model-x";
  candidate.declaration.serial = "SN-1";
  candidate.declaration.hardware = cxf::HardwareGeneration::from_value(3);
  candidate.declaration.firmware = cxf::FirmwareGeneration::from_value(12);
  cxf::AssetIdentity identity;
  identity.asset = cxf::AssetId::from_value(42);
  identity.registry_name = "registry-1";
  identity.model = "model-x";
  identity.serial = "SN-1";
  identity.hardware = cxf::HardwareGeneration::from_value(3);
  identity.firmware = cxf::FirmwareGeneration::from_value(12);
  candidate.identity = identity;
  cxf::PlacementBinding placement;
  placement.site = cxf::SiteId::from_value(1);
  placement.rack = cxf::RackId::from_value(7);
  placement.position = "U12";
  placement.topology = cxf::TopologyGeneration::from_value(4);
  candidate.placement = placement;
  candidate.attempt.id = AttemptId::from_value(1);
  candidate.attempt.generation = LifecycleGeneration::from_value(1);
  candidate.attempt.state = LifecycleState::kDeclared;
  candidate.attempt.opened_at = now();
  return candidate;
}

[[nodiscard]] EvidenceRecord make_evidence(EvidenceDimension dimension, std::string subject,
                                           EvidenceVerdict verdict, EvidenceSource source,
                                           Timestamp observed_at, FreshnessWindow window,
                                           FacilityGenerations generations, std::uint64_t id,
                                           std::uint64_t sequence) {
  EvidenceRecord record;
  record.id = EvidenceId::from_value(id);
  record.candidate = CandidateId::from_value(1);
  record.attempt = AttemptId::from_value(1);
  record.dimension = dimension;
  record.subject = std::move(subject);
  record.verdict = verdict;
  record.source = source;
  record.source_name = "test-source";
  record.detail = "recorded by the proof obligation";
  record.observed_at = observed_at;
  record.freshness = window;
  record.observed_sequence = ObservationSequence::from_value(sequence);
  record.generations = generations;
  record.payload = cxf::DigestBuilder::of(std::to_string(id) + ":" + record.subject);
  return record;
}

/// A live satisfied observation with the default one hour freshness window.
[[nodiscard]] EvidenceRecord live_satisfied(EvidenceDimension dimension, std::string subject,
                                            std::uint64_t id, EvidenceSource source,
                                            FacilityGenerations generations) {
  return make_evidence(dimension, std::move(subject), EvidenceVerdict::kSatisfied, source,
                       now(), FreshnessWindow::of(Duration::from_seconds(3600)), generations, id,
                       id);
}

/// One resolved dependency. An entry satisfies its reference exactly when it is
/// known, not ambiguous, and the facility generation equals the one the
/// candidate expected - so expected and actual are both carried.
[[nodiscard]] DependencyOutcome outcome_of(DependencyKind kind, std::string name, bool known,
                                           bool ambiguous, DependencyGeneration expected,
                                           DependencyGeneration actual) {
  DependencyOutcome outcome;
  outcome.ref.kind = kind;
  outcome.ref.name = std::move(name);
  outcome.ref.required = true;
  outcome.ref.expected = expected;
  outcome.known = known;
  outcome.ambiguous = ambiguous;
  outcome.actual = actual;
  return outcome;
}

[[nodiscard]] DependencyResolution satisfied_resolution() {
  DependencyResolution resolution;
  resolution.entries.push_back(
      outcome_of(DependencyKind::kRack, "rack7", true, false,
                 DependencyGeneration::from_value(5), DependencyGeneration::from_value(5)));
  resolution.entries.push_back(
      outcome_of(DependencyKind::kPowerFeed, "pdu-a", true, false,
                 DependencyGeneration::from_value(9), DependencyGeneration::from_value(9)));
  return resolution;
}

[[nodiscard]] DependencyResolution unsatisfied_resolution() {
  DependencyResolution resolution;
  resolution.entries.push_back(
      outcome_of(DependencyKind::kRack, "rack7", true, false,
                 DependencyGeneration::from_value(5), DependencyGeneration::from_value(5)));
  resolution.entries.push_back(
      outcome_of(DependencyKind::kPowerFeed, "pdu-a", false, false,
                 DependencyGeneration::from_value(9), DependencyGeneration()));
  return resolution;
}

[[nodiscard]] DependencyResolution ambiguous_resolution() {
  DependencyResolution resolution;
  resolution.entries.push_back(
      outcome_of(DependencyKind::kSite, "shared", true, true,
                 DependencyGeneration::from_value(5), DependencyGeneration::from_value(5)));
  return resolution;
}

/// Live satisfied evidence for every dimension, so a report can be made ready.
[[nodiscard]] std::vector<EvidenceRecord> complete_evidence(const FacilityGenerations& current) {
  std::vector<EvidenceRecord> records;
  std::uint64_t id = 1;
  for (const EvidenceDimension dimension : cxf::all_dimensions()) {
    records.push_back(live_satisfied(dimension, std::string("subject-") +
                                                     std::string(cxf::dimension_name(dimension)),
                                     id, EvidenceSource::kObserved, current));
    ++id;
  }
  return records;
}

[[nodiscard]] ReadinessReport evaluate(const CommissioningCandidate& candidate,
                                       const std::vector<EvidenceRecord>& evidence,
                                       const ReadinessPolicy& policy,
                                       const FacilityGenerations& current,
                                       const DependencyResolution& dependencies,
                                       std::uint64_t observation = 7) {
  return cxf::evaluate_readiness(candidate, evidence, policy, current, dependencies, now(),
                                 ObservationSequence::from_value(observation));
}

[[nodiscard]] const DimensionEvaluation& dimension_of(const ReadinessReport& report,
                                                      EvidenceDimension dimension) {
  return report.dimensions[cxf::dimension_rank(dimension)];
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

CXF_TEST(unit, lifecycle_chain_transitions) {
  // The chain is entered in order and never backwards: any strictly later chain
  // state is allowed (the evaluator computes a prefix, and a caller that
  // recorded the evidence for a later milestone may name it directly), while a
  // move to the same, to an earlier, or to the admission state is refused.
  const std::array<LifecycleState, cxf::kMilestoneStateCount> chain = cxf::milestone_states();
  for (std::size_t from = 0; from < chain.size(); ++from) {
    for (std::size_t to = 0; to < chain.size(); ++to) {
      const bool forward = cxf::state_rank(chain[to]) > cxf::state_rank(chain[from]);
      const bool allowed = cxf::transition_allowed(chain[from], chain[to]);
      if (forward) {
        CHECK(allowed);
        CHECK_CODE(cxf::require_transition(chain[from], chain[to]), Code::kOk);
      } else {
        CHECK(!allowed);
        CHECK_CODE(cxf::require_transition(chain[from], chain[to]), Code::kTransitionNotAllowed);
      }
    }
  }
  // A refused move is refused with the canonical code, never with success.
  CHECK(!cxf::transition_allowed(LifecycleState::kActivating, LifecycleState::kDeclared));
  CHECK(!cxf::transition_allowed(LifecycleState::kCommissioned, LifecycleState::kActivating));
  CHECK(!cxf::transition_allowed(LifecycleState::kIdentified, LifecycleState::kDeclared));
  CHECK(!cxf::transition_allowed(LifecycleState::kDeclared, LifecycleState::kDeclared));
  CHECK(cxf::transition_allowed(LifecycleState::kDeclared, LifecycleState::kCommissioned));
  CHECK(cxf::transition_allowed(LifecycleState::kLocated, LifecycleState::kHealthValidated));
}

CXF_TEST(unit, lifecycle_exit_states) {
  const std::array<LifecycleState, 3> exits = {LifecycleState::kFailed,
                                               LifecycleState::kQuarantined,
                                               LifecycleState::kCancelled};
  for (std::uint8_t i = 0; i < cxf::kLifecycleStateCount; ++i) {
    const auto state = static_cast<LifecycleState>(i);
    for (const LifecycleState exit : exits) {
      const bool allowed = cxf::transition_allowed(state, exit);
      if (state == LifecycleState::kQuarantined) {
        // Quarantine is the one terminal state with an exit: an explicit
        // release to Identified, or an operator cancellation. It is not a
        // failure and not a re-quarantine.
        CHECK_EQ(allowed, exit == LifecycleState::kCancelled);
      } else if (cxf::is_terminal_state(state)) {
        // Every other terminal state is closed: a commissioned asset is not
        // failed by a later request, and a cancelled one is not quarantined.
        CHECK(!allowed);
      } else {
        CHECK(allowed);
      }
    }
  }
  // Quarantine release restarts at Identified, the state whose assertion (the
  // identity is bound) still holds after a quarantine.
  CHECK_EQ(cxf::kQuarantineReleaseState, LifecycleState::kIdentified);
  CHECK(cxf::transition_allowed(LifecycleState::kQuarantined, cxf::kQuarantineReleaseState));
  CHECK(!cxf::transition_allowed(LifecycleState::kQuarantined, LifecycleState::kLocated));
  CHECK(!cxf::transition_allowed(LifecycleState::kQuarantined, LifecycleState::kDeclared));
  CHECK(!cxf::transition_allowed(LifecycleState::kCancelled, LifecycleState::kIdentified));
  CHECK(!cxf::transition_allowed(LifecycleState::kFailed, LifecycleState::kIdentified));
}

CXF_TEST(unit, lifecycle_next_milestone) {
  const std::array<LifecycleState, cxf::kMilestoneStateCount> chain = cxf::milestone_states();
  for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
    const cxf::Outcome<LifecycleState> next = cxf::next_milestone(chain[i]);
    REQUIRE_OK(next);
    CHECK_EQ(next.value(), chain[i + 1]);
  }
  // The end of the chain and the exits both refuse, and both name the state as
  // outside the stepping chain rather than pretending it has a successor.
  CHECK_CODE(cxf::next_milestone(LifecycleState::kCommissioned), Code::kFieldOutOfRange);
  CHECK_CODE(cxf::next_milestone(LifecycleState::kQuarantined), Code::kFieldOutOfRange);
  CHECK_CODE(cxf::next_milestone(LifecycleState::kFailed), Code::kFieldOutOfRange);
  CHECK_CODE(cxf::next_milestone(LifecycleState::kCancelled), Code::kFieldOutOfRange);
}

CXF_TEST(unit, lifecycle_milestone_for_dimension) {
  // The mapping is what turns satisfied dimensions into chain progress.
  CHECK_EQ(cxf::milestone_for_dimension(EvidenceDimension::kIdentity),
           LifecycleState::kIdentified);
  CHECK_EQ(cxf::milestone_for_dimension(EvidenceDimension::kPlacement),
           LifecycleState::kLocated);
  CHECK_EQ(cxf::milestone_for_dimension(EvidenceDimension::kDependency),
           LifecycleState::kDependencyValidated);
  CHECK_EQ(cxf::milestone_for_dimension(EvidenceDimension::kCompatibility),
           LifecycleState::kCompatibilityValidated);
  // Electrical and cooling are jointly required: neither alone unlocks it.
  CHECK_EQ(cxf::milestone_for_dimension(EvidenceDimension::kElectrical),
           LifecycleState::kUtilitiesReady);
  CHECK_EQ(cxf::milestone_for_dimension(EvidenceDimension::kCooling),
           LifecycleState::kUtilitiesReady);
  CHECK_EQ(cxf::milestone_for_dimension(EvidenceDimension::kNetwork),
           LifecycleState::kNetworkReady);
  CHECK_EQ(cxf::milestone_for_dimension(EvidenceDimension::kHealth),
           LifecycleState::kHealthValidated);
  CHECK_EQ(cxf::milestone_for_dimension(EvidenceDimension::kPolicy),
           LifecycleState::kActivationAuthorized);
  CHECK_EQ(cxf::milestone_for_dimension(EvidenceDimension::kServiceClass),
           LifecycleState::kActivationAuthorized);
  // Progress is monotone in the fixed dimension order.
  for (std::size_t i = 0; i + 1 < cxf::kEvidenceDimensionCount; ++i) {
    const LifecycleState earlier = cxf::milestone_for_dimension(
        static_cast<EvidenceDimension>(static_cast<std::uint8_t>(i)));
    const LifecycleState later = cxf::milestone_for_dimension(
        static_cast<EvidenceDimension>(static_cast<std::uint8_t>(i + 1)));
    CHECK(cxf::state_rank(earlier) <= cxf::state_rank(later));
  }
}

CXF_TEST(unit, lifecycle_furthest_reachable_state_is_a_prefix) {
  std::array<bool, cxf::kEvidenceDimensionCount> satisfied{};
  satisfied.fill(false);
  CHECK_EQ(cxf::furthest_reachable_state(satisfied), LifecycleState::kDeclared);

  const auto set = [&satisfied](EvidenceDimension dimension) {
    satisfied[cxf::dimension_rank(dimension)] = true;
  };

  set(EvidenceDimension::kIdentity);
  CHECK_EQ(cxf::furthest_reachable_state(satisfied), LifecycleState::kIdentified);
  set(EvidenceDimension::kPlacement);
  CHECK_EQ(cxf::furthest_reachable_state(satisfied), LifecycleState::kLocated);
  set(EvidenceDimension::kDependency);
  CHECK_EQ(cxf::furthest_reachable_state(satisfied), LifecycleState::kDependencyValidated);
  set(EvidenceDimension::kCompatibility);
  CHECK_EQ(cxf::furthest_reachable_state(satisfied), LifecycleState::kCompatibilityValidated);
  // Electrical alone is not enough: cooling is part of the same milestone.
  set(EvidenceDimension::kElectrical);
  CHECK_EQ(cxf::furthest_reachable_state(satisfied), LifecycleState::kCompatibilityValidated);
  set(EvidenceDimension::kCooling);
  CHECK_EQ(cxf::furthest_reachable_state(satisfied), LifecycleState::kUtilitiesReady);
  set(EvidenceDimension::kNetwork);
  CHECK_EQ(cxf::furthest_reachable_state(satisfied), LifecycleState::kNetworkReady);
  set(EvidenceDimension::kHealth);
  CHECK_EQ(cxf::furthest_reachable_state(satisfied), LifecycleState::kHealthValidated);
  // Policy alone is not enough either.
  set(EvidenceDimension::kPolicy);
  CHECK_EQ(cxf::furthest_reachable_state(satisfied), LifecycleState::kHealthValidated);
  // Service class completes the prerequisites for authorization, but readiness
  // never reaches ActivationAuthorized: that state asserts a token was issued,
  // which no satisfied dimension can stand in for.
  set(EvidenceDimension::kServiceClass);
  CHECK_EQ(cxf::furthest_reachable_state(satisfied), LifecycleState::kHealthValidated);
  CHECK_EQ(cxf::milestone_for_dimension(EvidenceDimension::kServiceClass),
           LifecycleState::kActivationAuthorized);

  // A gap in the middle stops the chain even when everything later is satisfied.
  std::array<bool, cxf::kEvidenceDimensionCount> gap{};
  gap.fill(true);
  gap[cxf::dimension_rank(EvidenceDimension::kElectrical)] = false;
  CHECK_EQ(cxf::furthest_reachable_state(gap), LifecycleState::kCompatibilityValidated);
  std::array<bool, cxf::kEvidenceDimensionCount> early_gap{};
  early_gap.fill(true);
  early_gap[cxf::dimension_rank(EvidenceDimension::kPlacement)] = false;
  CHECK_EQ(cxf::furthest_reachable_state(early_gap), LifecycleState::kIdentified);
  // The chain never reports a state it cannot justify.
  CHECK(cxf::state_rank(cxf::furthest_reachable_state(satisfied)) <=
        cxf::state_rank(LifecycleState::kActivationAuthorized));
}

CXF_TEST(unit, lifecycle_state_assertions_are_present) {
  // Every state documents what it asserts and what it does not; an empty string
  // would mean a state with no stated meaning.
  for (std::uint8_t i = 0; i < cxf::kLifecycleStateCount; ++i) {
    const auto state = static_cast<LifecycleState>(i);
    CHECK(!cxf::state_assertion(state).empty());
    CHECK(!cxf::state_non_assertion(state).empty());
  }
}

// ---------------------------------------------------------------------------
// Candidate and dependency validation.
// ---------------------------------------------------------------------------

CXF_TEST(unit, candidate_name_validation) {
  CHECK_CODE(cxf::validate_candidate_name("node-1"), Code::kOk);
  CHECK_CODE(cxf::validate_candidate_name(std::string(cxf::kMaxNameBytes, 'n')), Code::kOk);
  const cxf::Status empty = cxf::validate_candidate_name("");
  CHECK(empty.failed());
  CHECK(cxf::test::is_input_rejection(empty.code()));
  const cxf::Status too_long =
      cxf::validate_candidate_name(std::string(cxf::kMaxNameBytes + 1, 'n'));
  CHECK(too_long.failed());
  CHECK(cxf::test::is_input_rejection(too_long.code()));
  CHECK(cxf::validate_candidate_name("has space").failed());
  CHECK(cxf::validate_candidate_name("slash/inside").failed());
  CHECK(cxf::validate_candidate_name("caf\xC3\xA9").failed());
  CHECK(cxf::validate_candidate_name(std::string("a\0b", 3)).failed());
  CHECK(cxf::validate_candidate_name(std::string("\xC0\x80", 2)).failed());
}

CXF_TEST(unit, candidate_declaration_validation) {
  cxf::CandidateDeclaration declaration;
  declaration.registry_name = "registry-1";
  declaration.model = "model-x";
  declaration.serial = "SN-1";
  declaration.hardware = cxf::HardwareGeneration::from_value(3);
  declaration.firmware = cxf::FirmwareGeneration::from_value(12);
  CHECK_CODE(cxf::validate_declaration(declaration), Code::kOk);

  cxf::CandidateDeclaration invalid_utf8 = declaration;
  invalid_utf8.model = std::string("\xC0\x80", 2);
  CHECK(cxf::validate_declaration(invalid_utf8).failed());
  cxf::CandidateDeclaration too_long = declaration;
  too_long.serial = std::string(cxf::kMaxTextFieldBytes + 1, 's');
  CHECK(cxf::validate_declaration(too_long).failed());
  cxf::CandidateDeclaration spaced = declaration;
  spaced.registry_name = "registry one";
  CHECK(cxf::validate_declaration(spaced).failed());

  // Detail text is a display string: bounded and well formed, controls refused.
  // An empty note is legitimate: it is absent text, not malformed text.
  CHECK_CODE(cxf::validate_detail_text("a normal note"), Code::kOk);
  CHECK_CODE(cxf::validate_detail_text(""), Code::kOk);
  CHECK(cxf::validate_detail_text(std::string(cxf::kMaxDetailBytes + 1, 'd')).failed());
  CHECK(cxf::validate_detail_text(std::string("\x01", 1)).failed());
}

CXF_TEST(unit, candidate_identity_validation) {
  cxf::AssetIdentity identity;
  identity.asset = cxf::AssetId::from_value(42);
  identity.registry_name = "registry-1";
  identity.model = "model-x";
  identity.serial = "SN-1";
  identity.hardware = cxf::HardwareGeneration::from_value(3);
  identity.firmware = cxf::FirmwareGeneration::from_value(12);
  CHECK_CODE(cxf::validate_asset_identity(identity), Code::kOk);

  // Zero is how "not yet assigned" is spelled, so it is never a valid identity.
  cxf::AssetIdentity zero = identity;
  zero.asset = cxf::AssetId::from_value(0);
  const cxf::Status zero_status = cxf::validate_asset_identity(zero);
  CHECK(zero_status.failed());
  CHECK(cxf::test::is_input_rejection(zero_status.code()));

  cxf::AssetIdentity no_registry = identity;
  no_registry.registry_name.clear();
  CHECK(cxf::validate_asset_identity(no_registry).failed());
  cxf::AssetIdentity bad_text = identity;
  bad_text.serial = std::string(cxf::kMaxTextFieldBytes + 1, 's');
  CHECK(cxf::validate_asset_identity(bad_text).failed());
  cxf::AssetIdentity invalid_utf8 = identity;
  invalid_utf8.model = std::string("\xED\xA0\x80", 3);
  CHECK(cxf::validate_asset_identity(invalid_utf8).failed());
}

CXF_TEST(unit, candidate_placement_validation) {
  cxf::PlacementBinding placement;
  placement.site = cxf::SiteId::from_value(1);
  placement.rack = cxf::RackId::from_value(7);
  placement.position = "U12";
  placement.topology = cxf::TopologyGeneration::from_value(4);
  CHECK_CODE(cxf::validate_placement(placement), Code::kOk);

  cxf::PlacementBinding no_site = placement;
  no_site.site = cxf::SiteId::from_value(0);
  CHECK(cxf::validate_placement(no_site).failed());
  cxf::PlacementBinding no_rack = placement;
  no_rack.rack = cxf::RackId::from_value(0);
  CHECK(cxf::validate_placement(no_rack).failed());
  // A placement binding that names no position does not say where the asset is.
  cxf::PlacementBinding no_position = placement;
  no_position.position.clear();
  CHECK(cxf::validate_placement(no_position).failed());
  cxf::PlacementBinding long_position = placement;
  long_position.position = std::string(cxf::kMaxTextFieldBytes + 1, 'p');
  CHECK(cxf::validate_placement(long_position).failed());
  cxf::PlacementBinding invalid_utf8 = placement;
  invalid_utf8.position = std::string("\x80", 1);
  CHECK(cxf::validate_placement(invalid_utf8).failed());
}

CXF_TEST(unit, candidate_dependency_validation_and_duplicates) {
  DependencyRef rack;
  rack.kind = DependencyKind::kRack;
  rack.name = "rack7";
  rack.expected = DependencyGeneration::from_value(5);
  rack.required = true;
  CHECK_CODE(cxf::validate_dependency(rack), Code::kOk);

  DependencyRef no_name = rack;
  no_name.name.clear();
  CHECK(cxf::validate_dependency(no_name).failed());
  DependencyRef spaced = rack;
  spaced.name = "rack 7";
  CHECK(cxf::validate_dependency(spaced).failed());
  DependencyRef too_long = rack;
  too_long.name = std::string(cxf::kMaxNameBytes + 1, 'r');
  CHECK(cxf::validate_dependency(too_long).failed());
  DependencyRef invalid_utf8 = rack;
  invalid_utf8.name = std::string("\xC3", 1);
  CHECK(cxf::validate_dependency(invalid_utf8).failed());

  // An optional dependency is still a dependency with a name.
  DependencyRef optional = rack;
  optional.required = false;
  CHECK_CODE(cxf::validate_dependency(optional), Code::kOk);

  CHECK(cxf::same_dependency_target(rack, optional));
  DependencyRef other_kind = rack;
  other_kind.kind = DependencyKind::kSite;
  CHECK(!cxf::same_dependency_target(rack, other_kind));
  DependencyRef other_name = rack;
  other_name.name = "rack8";
  CHECK(!cxf::same_dependency_target(rack, other_name));

  // The same (kind, name) declared twice is a contradiction about the expected
  // generation, so it is refused rather than silently de-duplicated.
  std::vector<DependencyRef> duplicates = {rack, rack};
  CHECK(cxf::validate_dependencies(duplicates).failed());
  std::vector<DependencyRef> same_name_other_kind = {rack, other_kind};
  CHECK_CODE(cxf::validate_dependencies(same_name_other_kind), Code::kOk);
  std::vector<DependencyRef> distinct = {rack, other_name};
  CHECK_CODE(cxf::validate_dependencies(distinct), Code::kOk);
  std::vector<DependencyRef> empty;
  CHECK_CODE(cxf::validate_dependencies(empty), Code::kOk);
  std::vector<DependencyRef> one_invalid = {rack, no_name};
  CHECK(cxf::validate_dependencies(one_invalid).failed());
}

CXF_TEST(unit, dependency_resolution_against_facility_facts) {
  FacilityRecord facility;
  facility.generations = generations_v1();
  facility.dependencies = {
      DependencyFact{DependencyKind::kRack, "rack7", DependencyGeneration::from_value(5)},
      DependencyFact{DependencyKind::kSite, "shared", DependencyGeneration::from_value(5)},
      DependencyFact{DependencyKind::kRack, "shared", DependencyGeneration::from_value(5)},
      DependencyFact{DependencyKind::kPowerFeed, "pdu-a", DependencyGeneration::from_value(9)}};

  CommissioningCandidate candidate = bound_candidate();
  DependencyRef exact{DependencyKind::kRack, "rack7", DependencyGeneration::from_value(5), true};
  candidate.dependencies = {exact};
  const DependencyResolution matched = cxf::resolve_dependencies(candidate, facility);
  CHECK_EQ(matched.entries.size(), std::size_t{1});
  CHECK(matched.all_required_satisfied());
  CHECK_EQ(matched.unsatisfied_required(), std::size_t{0});
  CHECK(!matched.any_ambiguous());
  CHECK(matched.entries[0].known);
  CHECK(!matched.entries[0].ambiguous);
  CHECK_EQ(matched.entries[0].actual, DependencyGeneration::from_value(5));

  // A moved generation does not silently re-bind: the expectation is unmet.
  candidate.dependencies = {
      DependencyRef{DependencyKind::kRack, "rack7", DependencyGeneration::from_value(4), true}};
  const DependencyResolution drifted = cxf::resolve_dependencies(candidate, facility);
  CHECK(!drifted.all_required_satisfied());
  CHECK_EQ(drifted.unsatisfied_required(), std::size_t{1});

  // A name nothing declares is unknown.
  candidate.dependencies = {DependencyRef{DependencyKind::kRack, "rack-absent",
                                          DependencyGeneration::from_value(5), true}};
  const DependencyResolution unknown = cxf::resolve_dependencies(candidate, facility);
  CHECK(!unknown.all_required_satisfied());
  CHECK(!unknown.entries[0].known);
  CHECK(!unknown.entries[0].ambiguous);

  // One name declared as two kinds is ambiguous, never resolved by preference.
  candidate.dependencies = {
      DependencyRef{DependencyKind::kSite, "shared", DependencyGeneration::from_value(5), true}};
  const DependencyResolution ambiguous = cxf::resolve_dependencies(candidate, facility);
  CHECK(ambiguous.any_ambiguous());
  CHECK(!ambiguous.all_required_satisfied());
  CHECK(ambiguous.entries[0].ambiguous);

  // Two facts with the same (kind, name) at different generations are a
  // contradiction about the entity, not a "take the first" choice.
  FacilityRecord conflicting;
  conflicting.generations = generations_v1();
  conflicting.dependencies = {
      DependencyFact{DependencyKind::kRack, "rack7", DependencyGeneration::from_value(5)},
      DependencyFact{DependencyKind::kRack, "rack7", DependencyGeneration::from_value(6)}};
  candidate.dependencies = {
      DependencyRef{DependencyKind::kRack, "rack7", DependencyGeneration::from_value(5), true}};
  const DependencyResolution contradiction = cxf::resolve_dependencies(candidate, conflicting);
  CHECK(contradiction.any_ambiguous());
  CHECK(!contradiction.all_required_satisfied());
  CHECK_EQ(contradiction.unsatisfied_required(), std::size_t{1});
  CHECK(contradiction.entries[0].ambiguous);

  // An optional dependency that is unknown does not block the required set.
  candidate.dependencies = {
      DependencyRef{DependencyKind::kRack, "rack7", DependencyGeneration::from_value(5), true},
      DependencyRef{DependencyKind::kRack, "rack-absent", DependencyGeneration::from_value(5),
                    false}};
  const DependencyResolution optional = cxf::resolve_dependencies(candidate, facility);
  CHECK_EQ(optional.entries.size(), std::size_t{2});
  CHECK(optional.all_required_satisfied());
  CHECK_EQ(optional.unsatisfied_required(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// Evidence.
// ---------------------------------------------------------------------------

CXF_TEST(unit, evidence_key_and_validation) {
  EvidenceRecord record = live_satisfied(EvidenceDimension::kElectrical, "pdu-a", 3,
                                         EvidenceSource::kObserved, generations_v1());
  const cxf::EvidenceKey key = cxf::key_of(record);
  CHECK_EQ(key.dimension, EvidenceDimension::kElectrical);
  CHECK_EQ(key.subject, std::string("pdu-a"));
  const cxf::EvidenceKey same_key{EvidenceDimension::kElectrical, "pdu-a"};
  const cxf::EvidenceKey other_subject{EvidenceDimension::kElectrical, "pdu-b"};
  const cxf::EvidenceKey other_dimension{EvidenceDimension::kCooling, "pdu-a"};
  CHECK(key == same_key);
  CHECK(key != other_subject);
  CHECK(key < other_dimension);
  CHECK(other_dimension != same_key);
  CHECK_CODE(cxf::validate_evidence(record), Code::kOk);

  // A verdict of unknown is a legitimate recorded observation: it just never
  // satisfies anything.
  EvidenceRecord unknown = record;
  unknown.verdict = EvidenceVerdict::kUnknown;
  CHECK_CODE(cxf::validate_evidence(unknown), Code::kOk);

  // A claimed verdict must carry the content address of the observation body it
  // claims: a claim with no payload is a missing field, not a satisfied one.
  EvidenceRecord claimed_without_payload = record;
  claimed_without_payload.payload = cxf::Digest();
  CHECK_CODE(cxf::validate_evidence(claimed_without_payload), Code::kFieldMissing);
  EvidenceRecord negative_without_payload = claimed_without_payload;
  negative_without_payload.verdict = EvidenceVerdict::kUnsatisfied;
  CHECK_CODE(cxf::validate_evidence(negative_without_payload), Code::kFieldMissing);
  // An unknown verdict claims nothing, so an absent payload is not a claim.
  EvidenceRecord unknown_without_payload = claimed_without_payload;
  unknown_without_payload.verdict = EvidenceVerdict::kUnknown;
  CHECK_CODE(cxf::validate_evidence(unknown_without_payload), Code::kOk);

  EvidenceRecord no_subject = record;
  no_subject.subject.clear();
  CHECK(cxf::validate_evidence(no_subject).failed());
  EvidenceRecord long_subject = record;
  long_subject.subject = std::string(cxf::kMaxNameBytes + 1, 's');
  CHECK(cxf::validate_evidence(long_subject).failed());
  EvidenceRecord long_detail = record;
  long_detail.detail = std::string(cxf::kMaxDetailBytes + 1, 'd');
  CHECK(cxf::validate_evidence(long_detail).failed());
  EvidenceRecord bad_source_name = record;
  bad_source_name.source_name = std::string("\x01", 1);
  CHECK(cxf::validate_evidence(bad_source_name).failed());
  EvidenceRecord invalid_utf8 = record;
  invalid_utf8.subject = std::string("\xE0\x80\xAF", 3);
  CHECK(cxf::validate_evidence(invalid_utf8).failed());
  EvidenceRecord no_candidate = record;
  no_candidate.candidate = CandidateId::from_value(0);
  CHECK(cxf::validate_evidence(no_candidate).failed());
}

CXF_TEST(unit, evidence_liveness_rules) {
  const FacilityGenerations current = generations_v1();
  EvidenceRecord record = live_satisfied(EvidenceDimension::kHealth, "diagnostics", 1,
                                         EvidenceSource::kObserved, current);

  CHECK_EQ(cxf::liveness_of(record, current, now()), Liveness::kLive);
  CHECK_EQ(cxf::liveness_of(record, current, now() + Duration::from_seconds(3599)),
           Liveness::kLive);
  CHECK_EQ(cxf::liveness_of(record, current, now() + Duration::from_seconds(3601)),
           Liveness::kExpired);
  // The record is still retained and counted; it is simply not live.
  CHECK_EQ(record.verdict, EvidenceVerdict::kSatisfied);

  // A backwards clock is never fresh.
  CHECK_EQ(cxf::liveness_of(record, current, now() + Duration::from_seconds(-1)),
           Liveness::kExpired);

  // Any moved generation fences the observation.
  FacilityGenerations moved = current;
  moved.power = cxf::PowerGeneration::from_value(current.power.value() + 1);
  CHECK_EQ(cxf::liveness_of(record, moved, now()), Liveness::kGenerationMismatch);
  moved = current;
  moved.lifecycle = LifecycleGeneration::from_value(current.lifecycle.value() + 1);
  CHECK_EQ(cxf::liveness_of(record, moved, now()), Liveness::kGenerationMismatch);

  // "never" removes only the expiry rule, not the generation binding.
  EvidenceRecord forever = live_satisfied(EvidenceDimension::kHealth, "diagnostics", 2,
                                          EvidenceSource::kObserved, current);
  forever.freshness = FreshnessWindow::never();
  CHECK_EQ(cxf::liveness_of(forever, current, now() + Duration::from_seconds(1000000)),
           Liveness::kLive);
  CHECK_EQ(cxf::liveness_of(forever, moved, now()), Liveness::kGenerationMismatch);
}

CXF_TEST(unit, evidence_outranks_is_a_total_order) {
  const FacilityGenerations current = generations_v1();
  EvidenceRecord base = live_satisfied(EvidenceDimension::kHealth, "diagnostics", 10,
                                       EvidenceSource::kObserved, current);

  // Stronger provenance wins even against a later observation instant.
  EvidenceRecord observed = base;
  observed.source = EvidenceSource::kObserved;
  EvidenceRecord imported_later = base;
  imported_later.source = EvidenceSource::kImported;
  imported_later.observed_at = now() + Duration::from_seconds(10);
  CHECK(cxf::outranks(observed, imported_later));
  CHECK(!cxf::outranks(imported_later, observed));

  EvidenceRecord declared = base;
  declared.source = EvidenceSource::kDeclared;
  declared.observed_at = now() + Duration::from_seconds(100);
  CHECK(cxf::outranks(imported_later, declared));
  CHECK(cxf::outranks(observed, declared));

  // Same provenance: the later observation instant wins.
  EvidenceRecord earlier = base;
  earlier.source = EvidenceSource::kImported;
  earlier.observed_at = now();
  EvidenceRecord later = earlier;
  later.observed_at = now() + Duration::from_seconds(1);
  CHECK(cxf::outranks(later, earlier));
  CHECK(!cxf::outranks(earlier, later));

  // Same provenance and instant: the higher accepted sequence wins.
  EvidenceRecord low_sequence = earlier;
  low_sequence.observed_sequence = ObservationSequence::from_value(3);
  EvidenceRecord high_sequence = earlier;
  high_sequence.observed_sequence = ObservationSequence::from_value(4);
  CHECK(cxf::outranks(high_sequence, low_sequence));
  CHECK(!cxf::outranks(low_sequence, high_sequence));

  // Same everything but the id: the lower id wins, so the order is total and
  // evaluation never depends on iteration order.
  EvidenceRecord low_id = low_sequence;
  low_id.id = EvidenceId::from_value(2);
  EvidenceRecord high_id = low_sequence;
  high_id.id = EvidenceId::from_value(3);
  CHECK(cxf::outranks(low_id, high_id));
  CHECK(!cxf::outranks(high_id, low_id));

  // Identical records do not outrank each other.
  EvidenceRecord twin = low_id;
  CHECK(!cxf::outranks(low_id, twin));
  CHECK(!cxf::outranks(twin, low_id));

  // Exactly one direction holds for any pair that differs.
  const std::array<EvidenceRecord, 5> records = {observed, imported_later, declared, low_id,
                                                 high_id};
  for (const EvidenceRecord& a : records) {
    for (const EvidenceRecord& b : records) {
      if (a.id == b.id && a.source == b.source && a.observed_at == b.observed_at &&
          a.observed_sequence == b.observed_sequence) {
        continue;
      }
      CHECK(cxf::outranks(a, b) != cxf::outranks(b, a));
    }
  }
}

// ---------------------------------------------------------------------------
// Readiness evaluation.
// ---------------------------------------------------------------------------

CXF_TEST(unit, readiness_policy_helpers) {
  const ReadinessPolicy all = ReadinessPolicy::all_required();
  CHECK_EQ(all.required_count(), cxf::kEvidenceDimensionCount);
  for (const EvidenceDimension dimension : cxf::all_dimensions()) {
    CHECK(all.is_required(dimension));
  }
  ReadinessPolicy relaxed = all;
  relaxed.set_required(EvidenceDimension::kHealth, false);
  CHECK(!relaxed.is_required(EvidenceDimension::kHealth));
  CHECK(relaxed.is_required(EvidenceDimension::kIdentity));
  CHECK_EQ(relaxed.required_count(), cxf::kEvidenceDimensionCount - 1);
}

CXF_TEST(unit, readiness_without_evidence_is_unknown) {
  const FacilityGenerations current = generations_v1();
  const CommissioningCandidate candidate = bound_candidate();
  // One required dependency is unknown, so the dependency dimension cannot be
  // satisfied under any reading of the resolution contract.
  const DependencyResolution dependencies = unsatisfied_resolution();
  const ReadinessReport report =
      evaluate(candidate, {}, ReadinessPolicy::all_required(), current, dependencies);

  CHECK_EQ(report.dimensions.size(), cxf::kEvidenceDimensionCount);
  CHECK(!report.ready);
  CHECK_EQ(report.furthest_state, LifecycleState::kDeclared);
  CHECK_EQ(report.candidate, candidate.id);
  CHECK_EQ(report.revision, candidate.revision);
  CHECK_EQ(report.lifecycle, candidate.lifecycle);
  CHECK_EQ(report.incarnation, candidate.incarnation);
  CHECK_EQ(report.state, candidate.state);
  CHECK_EQ(report.generations, current);
  CHECK_EQ(report.evaluated_at, now());
  CHECK_EQ(report.observation, ObservationSequence::from_value(7));

  for (std::size_t i = 0; i < report.dimensions.size(); ++i) {
    const DimensionEvaluation& dimension = report.dimensions[i];
    // Fixed order, all ten, all required by the default policy.
    CHECK_EQ(dimension.dimension, static_cast<EvidenceDimension>(static_cast<std::uint8_t>(i)));
    CHECK(dimension.required);
    CHECK(dimension.verdict != EvidenceVerdict::kSatisfied);
    CHECK(dimension.code != Code::kOk);
    CHECK(dimension.witness.is_zero());
    CHECK_EQ(dimension.live_records, 0u);
    if (dimension.dimension != EvidenceDimension::kDependency) {
      CHECK_EQ(dimension.code, Code::kEvidenceMissing);
      CHECK_EQ(dimension.verdict, EvidenceVerdict::kUnknown);
    }
  }
  // The dependency dimension is decided by the resolution; both a missing
  // observation and an unsatisfied requirement are honest reports of it.
  const Code dependency_code = dimension_of(report, EvidenceDimension::kDependency).code;
  CHECK(dependency_code == Code::kEvidenceMissing ||
        dependency_code == Code::kDependencyUnsatisified);

  const std::vector<const DimensionEvaluation*> blocking = cxf::blocking_dimensions(report);
  CHECK_EQ(blocking.size(), cxf::kEvidenceDimensionCount);
  for (std::size_t i = 0; i < blocking.size(); ++i) {
    CHECK_EQ(blocking[i]->dimension, static_cast<EvidenceDimension>(static_cast<std::uint8_t>(i)));
  }
  CHECK(!cxf::first_blocker_summary(report).empty());
  CHECK(cxf::first_blocker_summary(report).find('\n') == std::string::npos);
  CHECK_EQ(cxf::first_blocker_summary(report), cxf::first_blocker_summary(report));
}

CXF_TEST(unit, readiness_declared_claims_never_satisfy) {
  const FacilityGenerations current = generations_v1();
  const CommissioningCandidate candidate = bound_candidate();
  std::vector<EvidenceRecord> declared;
  std::uint64_t id = 1;
  for (const EvidenceDimension dimension : cxf::all_dimensions()) {
    declared.push_back(live_satisfied(dimension, "claimed", id, EvidenceSource::kDeclared,
                                      current));
    ++id;
  }
  const ReadinessReport report =
      evaluate(candidate, declared, ReadinessPolicy::all_required(), current,
               satisfied_resolution());
  CHECK(!report.ready);
  for (const DimensionEvaluation& dimension : report.dimensions) {
    if (dimension.dimension == EvidenceDimension::kDependency) {
      // The dependency dimension is decided by the typed resolution, not by
      // any observation, so a declared claim neither helps nor harms it.
      CHECK_EQ(dimension.verdict, EvidenceVerdict::kSatisfied);
      CHECK_EQ(dimension.live_records, 0u);
      continue;
    }
    // The claim is recorded and live, but it is not evidence of anything.
    CHECK(dimension.verdict != EvidenceVerdict::kSatisfied);
    CHECK(dimension.code != Code::kOk);
  }
  // Readiness still stops at the first unsatisfied gate: identity.
  CHECK_EQ(report.furthest_state, LifecycleState::kDeclared);
}

CXF_TEST(unit, readiness_single_observed_dimension_satisfies_exactly_it) {
  const FacilityGenerations current = generations_v1();
  const CommissioningCandidate candidate = bound_candidate();
  const EvidenceRecord health = live_satisfied(EvidenceDimension::kHealth, "diagnostics", 5,
                                               EvidenceSource::kObserved, current);
  const ReadinessReport report =
      evaluate(candidate, {health}, ReadinessPolicy::all_required(), current,
               satisfied_resolution());

  const DimensionEvaluation& satisfied = dimension_of(report, EvidenceDimension::kHealth);
  CHECK_EQ(satisfied.verdict, EvidenceVerdict::kSatisfied);
  CHECK_EQ(satisfied.code, Code::kOk);
  CHECK_EQ(satisfied.witness, health.id);
  CHECK_EQ(satisfied.witness_source, EvidenceSource::kObserved);
  CHECK_EQ(satisfied.witness_sequence, health.observed_sequence);
  CHECK_EQ(satisfied.live_records, 1u);
  CHECK_EQ(satisfied.stale_records, 0u);
  CHECK(!satisfied.explanation.empty());

  // Nothing else is satisfied: one dimension is not a lifecycle.
  for (const DimensionEvaluation& dimension : report.dimensions) {
    if (dimension.dimension == EvidenceDimension::kHealth ||
        dimension.dimension == EvidenceDimension::kDependency) {
      continue;
    }
    CHECK(dimension.verdict != EvidenceVerdict::kSatisfied);
  }
  CHECK(!report.ready);
  // The furthest state still cannot pass the earlier gap (nothing before health
  // is satisfied except the dependency dimension).
  CHECK(cxf::state_rank(report.furthest_state) <=
        cxf::state_rank(LifecycleState::kDependencyValidated));
  const std::vector<const DimensionEvaluation*> blocking = cxf::blocking_dimensions(report);
  CHECK(!blocking.empty());
  CHECK_EQ(blocking.front()->dimension, EvidenceDimension::kIdentity);
}

CXF_TEST(unit, readiness_unsatisfied_observation_blocks) {
  const FacilityGenerations current = generations_v1();
  const CommissioningCandidate candidate = bound_candidate();
  const EvidenceRecord negative = make_evidence(
      EvidenceDimension::kElectrical, "pdu-a", EvidenceVerdict::kUnsatisfied,
      EvidenceSource::kObserved, now(), FreshnessWindow::of(Duration::from_seconds(3600)),
      current, 9, 9);
  const ReadinessReport report =
      evaluate(candidate, {negative}, ReadinessPolicy::all_required(), current,
               satisfied_resolution());

  const DimensionEvaluation& electrical = dimension_of(report, EvidenceDimension::kElectrical);
  CHECK_EQ(electrical.verdict, EvidenceVerdict::kUnsatisfied);
  CHECK_EQ(electrical.code, Code::kEvidenceNegative);
  CHECK_EQ(electrical.witness, negative.id);
  CHECK_EQ(electrical.live_records, 1u);
  CHECK(!report.ready);
}

CXF_TEST(unit, readiness_stale_records_are_counted_not_used) {
  const FacilityGenerations current = generations_v1();
  const CommissioningCandidate candidate = bound_candidate();

  // Expired: same generations, window elapsed.
  EvidenceRecord expired = make_evidence(
      EvidenceDimension::kHealth, "diagnostics", EvidenceVerdict::kSatisfied,
      EvidenceSource::kObserved, now() + Duration::from_seconds(-7200),
      FreshnessWindow::of(Duration::from_seconds(3600)), current, 4, 4);
  {
    const ReadinessReport report = evaluate(candidate, {expired}, ReadinessPolicy::all_required(),
                                            current, satisfied_resolution());
    const DimensionEvaluation& health = dimension_of(report, EvidenceDimension::kHealth);
    CHECK(health.verdict != EvidenceVerdict::kSatisfied);
    CHECK(health.code != Code::kOk);
    CHECK_EQ(health.live_records, 0u);
    CHECK(health.stale_records >= 1u);
    CHECK(!report.ready);
  }

  // Generation mismatch: fresh but observed under a different facility.
  FacilityGenerations moved = current;
  moved.cooling = cxf::CoolingGeneration::from_value(current.cooling.value() + 1);
  EvidenceRecord mismatched = live_satisfied(EvidenceDimension::kCooling, "zone-3", 5,
                                             EvidenceSource::kObserved, moved);
  {
    const ReadinessReport report = evaluate(candidate, {mismatched},
                                            ReadinessPolicy::all_required(), current,
                                            satisfied_resolution());
    const DimensionEvaluation& cooling = dimension_of(report, EvidenceDimension::kCooling);
    CHECK(cooling.verdict != EvidenceVerdict::kSatisfied);
    CHECK(cooling.code != Code::kOk);
    CHECK_EQ(cooling.live_records, 0u);
    CHECK(cooling.stale_records >= 1u);
  }

  // A stale record does not shadow a live one; the live record is the witness.
  EvidenceRecord fresh = live_satisfied(EvidenceDimension::kCooling, "zone-3", 6,
                                        EvidenceSource::kObserved, current);
  {
    const ReadinessReport report = evaluate(candidate, {mismatched, fresh},
                                            ReadinessPolicy::all_required(), current,
                                            satisfied_resolution());
    const DimensionEvaluation& cooling = dimension_of(report, EvidenceDimension::kCooling);
    CHECK_EQ(cooling.verdict, EvidenceVerdict::kSatisfied);
    CHECK_EQ(cooling.witness, fresh.id);
    CHECK_EQ(cooling.live_records, 1u);
    CHECK_EQ(cooling.stale_records, 1u);
  }
}

CXF_TEST(unit, readiness_contradiction_is_unknown) {
  const FacilityGenerations current = generations_v1();
  const CommissioningCandidate candidate = bound_candidate();
  // Two live observations of the same key that disagree: neither may win.
  const EvidenceRecord satisfied = live_satisfied(EvidenceDimension::kNetwork, "fabric-east", 11,
                                                  EvidenceSource::kObserved, current);
  const EvidenceRecord unsatisfied = make_evidence(
      EvidenceDimension::kNetwork, "fabric-east", EvidenceVerdict::kUnsatisfied,
      EvidenceSource::kObserved, now(), FreshnessWindow::of(Duration::from_seconds(3600)),
      current, 12, 12);
  const ReadinessReport report = evaluate(candidate, {satisfied, unsatisfied},
                                          ReadinessPolicy::all_required(), current,
                                          satisfied_resolution());

  const DimensionEvaluation& network = dimension_of(report, EvidenceDimension::kNetwork);
  CHECK_EQ(network.verdict, EvidenceVerdict::kUnknown);
  CHECK_EQ(network.code, Code::kEvidenceContradictory);
  CHECK(!report.ready);
  // The contradiction is reported as such, not as a missing observation.
  CHECK(network.code != Code::kEvidenceMissing);

  // The same contradiction stated the other way round gives the same verdict:
  // the evaluator has no iteration-order dependence.
  const ReadinessReport reversed = evaluate(candidate, {unsatisfied, satisfied},
                                            ReadinessPolicy::all_required(), current,
                                            satisfied_resolution());
  CHECK_EQ(dimension_of(reversed, EvidenceDimension::kNetwork).verdict,
           EvidenceVerdict::kUnknown);
  CHECK_EQ(dimension_of(reversed, EvidenceDimension::kNetwork).code,
           Code::kEvidenceContradictory);
}

CXF_TEST(unit, readiness_dependency_dimension_follows_the_resolution) {
  const FacilityGenerations current = generations_v1();
  const CommissioningCandidate candidate = bound_candidate();
  const EvidenceRecord dependency_evidence = live_satisfied(
      EvidenceDimension::kDependency, "rack7", 21, EvidenceSource::kObserved, current);

  {
    const ReadinessReport report = evaluate(candidate, {dependency_evidence},
                                            ReadinessPolicy::all_required(), current,
                                            satisfied_resolution());
    const DimensionEvaluation& dependency = dimension_of(report, EvidenceDimension::kDependency);
    CHECK_EQ(dependency.verdict, EvidenceVerdict::kSatisfied);
    CHECK_EQ(dependency.code, Code::kOk);
    // The dimension is decided by the typed resolution, so evidence recorded
    // under it is never consulted and never counted as a live record.
    CHECK_EQ(dependency.live_records, 0u);
    CHECK(dependency.witness.is_zero());
  }
  {
    // Live evidence cannot make up for a required dependency that is unknown.
    const ReadinessReport report = evaluate(candidate, {dependency_evidence},
                                            ReadinessPolicy::all_required(), current,
                                            unsatisfied_resolution());
    const DimensionEvaluation& dependency = dimension_of(report, EvidenceDimension::kDependency);
    CHECK(dependency.verdict != EvidenceVerdict::kSatisfied);
    CHECK(dependency.code != Code::kOk);
    CHECK(!report.ready);
  }
  {
    // An ambiguous reference is reported as ambiguous, never resolved silently.
    const ReadinessReport report = evaluate(candidate, {dependency_evidence},
                                            ReadinessPolicy::all_required(), current,
                                            ambiguous_resolution());
    const DimensionEvaluation& dependency = dimension_of(report, EvidenceDimension::kDependency);
    CHECK(dependency.verdict != EvidenceVerdict::kSatisfied);
    CHECK(dependency.code != Code::kOk);
    CHECK(!report.ready);
  }
}

CXF_TEST(unit, readiness_identity_and_placement_need_a_binding) {
  const FacilityGenerations current = generations_v1();
  const EvidenceRecord identity = live_satisfied(EvidenceDimension::kIdentity, "registry-1", 31,
                                                 EvidenceSource::kObserved, current);
  const EvidenceRecord placement = live_satisfied(EvidenceDimension::kPlacement, "rack7", 32,
                                                  EvidenceSource::kObserved, current);
  const std::vector<EvidenceRecord> evidence = {identity, placement};

  CommissioningCandidate unbound = bound_candidate();
  unbound.identity.reset();
  unbound.placement.reset();
  const ReadinessReport report = evaluate(unbound, evidence, ReadinessPolicy::all_required(),
                                          current, satisfied_resolution());
  // Observed identity evidence without a recorded binding is not an identity.
  CHECK(dimension_of(report, EvidenceDimension::kIdentity).verdict !=
        EvidenceVerdict::kSatisfied);
  CHECK(dimension_of(report, EvidenceDimension::kIdentity).code != Code::kOk);
  CHECK(dimension_of(report, EvidenceDimension::kPlacement).verdict !=
        EvidenceVerdict::kSatisfied);
  CHECK(dimension_of(report, EvidenceDimension::kPlacement).code != Code::kOk);
  CHECK(!report.ready);

  // With both bindings recorded the same evidence does satisfy both.
  const CommissioningCandidate bound = bound_candidate();
  const ReadinessReport bound_report = evaluate(bound, evidence,
                                                ReadinessPolicy::all_required(), current,
                                                satisfied_resolution());
  CHECK_EQ(dimension_of(bound_report, EvidenceDimension::kIdentity).verdict,
           EvidenceVerdict::kSatisfied);
  CHECK_EQ(dimension_of(bound_report, EvidenceDimension::kPlacement).verdict,
           EvidenceVerdict::kSatisfied);
}

CXF_TEST(unit, readiness_ready_requires_every_required_dimension) {
  const FacilityGenerations current = generations_v1();
  const CommissioningCandidate candidate = bound_candidate();
  const std::vector<EvidenceRecord> evidence = complete_evidence(current);
  const ReadinessReport report = evaluate(candidate, evidence, ReadinessPolicy::all_required(),
                                          current, satisfied_resolution());
  CHECK(report.ready);
  for (const DimensionEvaluation& dimension : report.dimensions) {
    CHECK_EQ(dimension.verdict, EvidenceVerdict::kSatisfied);
    CHECK_EQ(dimension.code, Code::kOk);
    // Every dimension except the dependency one is decided by live evidence.
    if (dimension.dimension != EvidenceDimension::kDependency) {
      CHECK(dimension.live_records >= 1u);
    }
  }
  // Readiness supports the whole chain up to HealthValidated; the states that
  // assert an issued token or a reported activation are never claimed here.
  CHECK_EQ(report.furthest_state, LifecycleState::kHealthValidated);
  CHECK(cxf::blocking_dimensions(report).empty());
  CHECK(cxf::first_blocker_summary(report).empty());

  // A dimension that is not required cannot block readiness, but it is still
  // reported in the fixed order.
  ReadinessPolicy relaxed = ReadinessPolicy::all_required();
  relaxed.set_required(EvidenceDimension::kHealth, false);
  std::vector<EvidenceRecord> without_health;
  for (const EvidenceRecord& record : evidence) {
    if (record.dimension != EvidenceDimension::kHealth) {
      without_health.push_back(record);
    }
  }
  const ReadinessReport relaxed_report =
      evaluate(candidate, without_health, relaxed, current, satisfied_resolution());
  CHECK(relaxed_report.ready);
  CHECK_EQ(relaxed_report.dimensions.size(), cxf::kEvidenceDimensionCount);
  CHECK(!dimension_of(relaxed_report, EvidenceDimension::kHealth).required);
  CHECK(cxf::blocking_dimensions(relaxed_report).empty());

  // The required set still blocks when one required dimension is missing.
  ReadinessPolicy strict = ReadinessPolicy::all_required();
  strict.set_required(EvidenceDimension::kPolicy, true);
  std::vector<EvidenceRecord> without_policy;
  for (const EvidenceRecord& record : evidence) {
    if (record.dimension != EvidenceDimension::kPolicy) {
      without_policy.push_back(record);
    }
  }
  const ReadinessReport blocked =
      evaluate(candidate, without_policy, strict, current, satisfied_resolution());
  CHECK(!blocked.ready);
  const std::vector<const DimensionEvaluation*> blocking = cxf::blocking_dimensions(blocked);
  REQUIRE(blocking.size() == std::size_t{1});
  CHECK_EQ(blocking[0]->dimension, EvidenceDimension::kPolicy);
  CHECK(!cxf::first_blocker_summary(blocked).empty());
  // The furthest state stops before the gap but is still a prefix of the chain.
  CHECK(cxf::state_rank(blocked.furthest_state) <
        cxf::state_rank(LifecycleState::kActivationAuthorized));
}

CXF_TEST(unit, readiness_report_digest_is_stable_and_sensitive) {
  const FacilityGenerations current = generations_v1();
  const CommissioningCandidate candidate = bound_candidate();
  const std::vector<EvidenceRecord> evidence = complete_evidence(current);
  const ReadinessReport first = evaluate(candidate, evidence, ReadinessPolicy::all_required(),
                                         current, satisfied_resolution());
  const ReadinessReport second = evaluate(candidate, evidence, ReadinessPolicy::all_required(),
                                          current, satisfied_resolution());
  CHECK(!first.digest.is_zero());
  CHECK_EQ(first.digest, second.digest);
  // The published digest is exactly the digest over the report with the digest
  // field zeroed, so a reader can recompute it.
  CHECK_EQ(first.digest, cxf::compute_report_digest(first));
  ReadinessReport recomputed = first;
  recomputed.digest = cxf::Digest();
  CHECK_EQ(cxf::compute_report_digest(recomputed), first.digest);

  // Any change to the report changes the address: a plan bound to it must not
  // survive a changed evaluation.
  const ReadinessReport other_observation =
      evaluate(candidate, evidence, ReadinessPolicy::all_required(), current,
               satisfied_resolution(), 8);
  CHECK(other_observation.digest != first.digest);

  EvidenceRecord extra = live_satisfied(EvidenceDimension::kHealth, "diagnostics-2", 99,
                                        EvidenceSource::kObserved, current);
  std::vector<EvidenceRecord> more = evidence;
  more.push_back(extra);
  const ReadinessReport changed_evidence =
      evaluate(candidate, more, ReadinessPolicy::all_required(), current,
               satisfied_resolution());
  CHECK(changed_evidence.digest != first.digest);

  ReadinessPolicy relaxed = ReadinessPolicy::all_required();
  relaxed.set_required(EvidenceDimension::kHealth, false);
  const ReadinessReport changed_policy =
      evaluate(candidate, evidence, relaxed, current, satisfied_resolution());
  CHECK(changed_policy.digest != first.digest);

  FacilityGenerations moved = current;
  moved.policy = cxf::PolicyGeneration::from_value(current.policy.value() + 1);
  const ReadinessReport changed_generations =
      evaluate(candidate, evidence, ReadinessPolicy::all_required(), moved,
               satisfied_resolution());
  CHECK(changed_generations.digest != first.digest);

  // Touching only the evaluated instant changes the address too.
  ReadinessReport touched = first;
  touched.evaluated_at = first.evaluated_at + Duration::from_nanos(1);
  CHECK(cxf::compute_report_digest(touched) != first.digest);
}

// ---------------------------------------------------------------------------
// Dependency graph.
// ---------------------------------------------------------------------------

CXF_TEST(unit, dependency_cycle_detection_is_deterministic) {
  const std::vector<DependencyNode> acyclic = {
      DependencyNode{"a", {"b"}}, DependencyNode{"b", {"c"}}, DependencyNode{"c", {}}};
  std::vector<std::string> path;
  CHECK_CODE(cxf::find_dependency_cycle(acyclic, path), Code::kOk);
  CHECK(path.empty());

  const std::vector<DependencyNode> cyclic = {
      DependencyNode{"d", {"b"}}, DependencyNode{"b", {"c"}}, DependencyNode{"c", {"b"}}};
  std::vector<std::string> first_path;
  CHECK_CODE(cxf::find_dependency_cycle(cyclic, first_path), Code::kDependencyCycle);
  CHECK(!first_path.empty());
  std::vector<std::string> second_path;
  CHECK_CODE(cxf::find_dependency_cycle(cyclic, second_path), Code::kDependencyCycle);
  // Same graph, same cycle, same starting node.
  CHECK_EQ(first_path, second_path);
  // The reported cycle starts at its lexicographically smallest node.
  CHECK_EQ(first_path.front(), std::string("b"));

  // Reversing the declaration order of an unrelated graph must not change the
  // answer either.
  const std::vector<DependencyNode> reversed = {cyclic[2], cyclic[1], cyclic[0]};
  std::vector<std::string> reversed_path;
  CHECK_CODE(cxf::find_dependency_cycle(reversed, reversed_path), Code::kDependencyCycle);
  CHECK_EQ(reversed_path, first_path);

  // A self-reference is a cycle.
  const std::vector<DependencyNode> self = {DependencyNode{"loop", {"loop"}}};
  std::vector<std::string> self_path;
  CHECK_CODE(cxf::find_dependency_cycle(self, self_path), Code::kDependencyCycle);
  CHECK(!self_path.empty());

  const std::vector<DependencyNode> empty;
  std::vector<std::string> empty_path;
  CHECK_CODE(cxf::find_dependency_cycle(empty, empty_path), Code::kOk);
}

CXF_TEST(unit, dependency_order_is_topological_and_deterministic) {
  // Orientation: "u depends_on v" is the edge u -> v, and a node appears BEFORE
  // the nodes it depends on. An asset therefore sorts after the rack it sits in.
  const std::vector<DependencyNode> graph = {DependencyNode{"site", {}},
                                             DependencyNode{"rack", {"site"}},
                                             DependencyNode{"asset", {"rack", "site"}}};
  const cxf::Outcome<std::vector<std::string>> order = cxf::dependency_order(graph);
  REQUIRE_OK(order);
  CHECK_EQ(order->size(), std::size_t{3});
  const auto position = [&order](std::string_view name) {
    const auto found = std::find(order->begin(), order->end(), name);
    return found == order->end() ? std::size_t{order->size()}
                                 : static_cast<std::size_t>(found - order->begin());
  };
  CHECK(position("asset") < position("rack"));
  CHECK(position("rack") < position("site"));
  CHECK(position("asset") < position("site"));
  CHECK_EQ((*order)[0], std::string("asset"));
  CHECK_EQ((*order)[2], std::string("site"));

  // Declaring the same graph in another order gives the same answer.
  const std::vector<DependencyNode> reordered = {graph[2], graph[0], graph[1]};
  const cxf::Outcome<std::vector<std::string>> again = cxf::dependency_order(reordered);
  REQUIRE_OK(again);
  CHECK_EQ(*again, *order);

  // A name is never a substitute for the orientation: a short name that depends
  // on a long one still comes first.
  const std::vector<DependencyNode> named = {DependencyNode{"a", {"z"}},
                                             DependencyNode{"z", {}}};
  const cxf::Outcome<std::vector<std::string>> named_order = cxf::dependency_order(named);
  REQUIRE_OK(named_order);
  CHECK_EQ((*named_order)[0], std::string("a"));
  CHECK_EQ((*named_order)[1], std::string("z"));

  // Independent nodes are ordered by name so the answer is reproducible.
  const std::vector<DependencyNode> independent = {DependencyNode{"z", {}},
                                                   DependencyNode{"a", {}},
                                                   DependencyNode{"m", {}}};
  const cxf::Outcome<std::vector<std::string>> sorted = cxf::dependency_order(independent);
  REQUIRE_OK(sorted);
  CHECK_EQ((*sorted)[0], std::string("a"));
  CHECK_EQ((*sorted)[1], std::string("m"));
  CHECK_EQ((*sorted)[2], std::string("z"));

  const std::vector<DependencyNode> cyclic = {DependencyNode{"x", {"y"}},
                                             DependencyNode{"y", {"x"}}};
  const cxf::Outcome<std::vector<std::string>> refused = cxf::dependency_order(cyclic);
  CHECK_CODE(refused, Code::kDependencyCycle);
}

// ---------------------------------------------------------------------------
// Durable record vocabulary.
// ---------------------------------------------------------------------------

CXF_TEST(unit, record_validation_helpers) {
  cxf::FabricMeta meta;
  meta.format = cxf::FormatVersion::from_value(cxf::kStoreFormatVersion);
  meta.epoch = cxf::ControlEpoch::from_value(1);
  meta.incarnation = cxf::IncarnationId::from_value(1);
  meta.commit = cxf::CommitSequence::from_value(1);
  meta.created_at = now();
  meta.updated_at = now();
  CHECK_CODE(cxf::validate_fabric_meta(meta), Code::kOk);
  cxf::FabricMeta zero_format = meta;
  zero_format.format = cxf::FormatVersion::from_value(0);
  CHECK(cxf::validate_fabric_meta(zero_format).failed());
  cxf::FabricMeta zero_incarnation = meta;
  zero_incarnation.incarnation = cxf::IncarnationId::from_value(0);
  CHECK(cxf::validate_fabric_meta(zero_incarnation).failed());

  cxf::RequestResult result;
  result.request = cxf::RequestId::from_value(1);
  result.kind = cxf::RequestKind::kAdmitCandidate;
  result.request_digest = cxf::DigestBuilder::of(std::string_view("request"));
  result.accepted = true;
  result.code = Code::kOk;
  result.effect_digest = cxf::DigestBuilder::of(std::string_view("effect"));
  result.commit = cxf::CommitSequence::from_value(1);
  result.recorded_at = now();
  CHECK_CODE(cxf::validate_request_result(result), Code::kOk);
  cxf::RequestResult no_request = result;
  no_request.request = cxf::RequestId::from_value(0);
  CHECK(cxf::validate_request_result(no_request).failed());

  cxf::EventRecord event;
  event.sequence = cxf::CommitSequence::from_value(1);
  event.kind = cxf::EventKind::kCandidateAdmitted;
  event.at = now();
  event.candidate = CandidateId::from_value(1);
  event.lifecycle = LifecycleGeneration::from_value(1);
  event.generations = generations_v1();
  event.detail = "admitted";
  CHECK_CODE(cxf::validate_event(event), Code::kOk);
  cxf::EventRecord bad_detail = event;
  bad_detail.detail = std::string("\x02", 1);
  CHECK(cxf::validate_event(bad_detail).failed());
}

CXF_TEST(unit, event_log_window_and_queries) {
  cxf::EventLog log;
  CHECK(log.empty());
  CHECK_EQ(log.size(), std::size_t{0});
  for (std::uint64_t i = 1; i <= 5; ++i) {
    cxf::EventRecord event;
    event.sequence = cxf::CommitSequence::from_value(i);
    event.kind = cxf::EventKind::kEvidenceRecorded;
    event.at = now() + Duration::from_seconds(static_cast<std::int64_t>(i));
    event.candidate = CandidateId::from_value(1);
    event.detail = "event " + std::to_string(i);
    log.append(event);
  }
  CHECK_EQ(log.size(), std::size_t{5});
  CHECK_EQ(log.highest_sequence(), cxf::CommitSequence::from_value(5));
  CHECK_EQ(log.records().front().sequence, cxf::CommitSequence::from_value(1));
  CHECK_EQ(log.records().back().sequence, cxf::CommitSequence::from_value(5));

  // A query returns at most the requested number of entries, oldest first, and
  // only ever entries that belong to the candidate asked about.
  const std::vector<cxf::EventRecord> limited = log.for_candidate(CandidateId::from_value(1), 3);
  CHECK_EQ(limited.size(), std::size_t{3});
  CHECK_EQ(limited.front().sequence, cxf::CommitSequence::from_value(1));
  CHECK_EQ(limited.back().sequence, cxf::CommitSequence::from_value(3));
  for (const cxf::EventRecord& record : limited) {
    CHECK(record.candidate == CandidateId::from_value(1));
  }
  CHECK_EQ(log.for_candidate(CandidateId::from_value(1), 0).size(), std::size_t{0});
  CHECK_EQ(log.for_candidate(CandidateId::from_value(1), 999).size(), std::size_t{5});
  CHECK(log.for_candidate(CandidateId::from_value(99), 3).empty());

  // Replacing the window replaces its fence too: a recovered window states the
  // highest sequence it actually holds.
  log.reset({});
  CHECK(log.empty());
  CHECK_EQ(log.highest_sequence(), cxf::CommitSequence::from_value(0));

  std::vector<cxf::EventRecord> replacement;
  cxf::EventRecord restored;
  restored.sequence = cxf::CommitSequence::from_value(42);
  restored.kind = cxf::EventKind::kRecovered;
  restored.at = now();
  replacement.push_back(restored);
  log.reset(replacement);
  CHECK_EQ(log.size(), std::size_t{1});
  CHECK_EQ(log.highest_sequence(), cxf::CommitSequence::from_value(42));
  CHECK_EQ(log.for_candidate(CandidateId::from_value(0), 1).size(), std::size_t{1});
}

}  // namespace
