// Adversarial evidence and input handling.
//
// Everything here is an input the runtime must survive rather than trust:
// absurd lengths, hostile bytes, absent identities, unknown enumerations,
// observations dated in the future, duplicated payloads, contradictions,
// evidence aimed at candidates that are already quarantined or cancelled, and
// facility generations that move underneath a recorded observation.

#include "support/test_harness.hpp"

#include "cxf/model/lifecycle.hpp"
#include "cxf/model/readiness.hpp"
#include "cxf/persist/record_io.hpp"
#include "cxf/runtime/fabric.hpp"
#include "cxf/support/clock.hpp"
#include "cxf/types/enums.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using cxf::CandidateDeclaration;
using cxf::Code;
using cxf::DependencyGeneration;
using cxf::DependencyKind;
using cxf::DependencyRef;
using cxf::Duration;
using cxf::EvidenceDimension;
using cxf::EvidenceSource;
using cxf::EvidenceVerdict;
using cxf::Fabric;
using cxf::FabricOptions;
using cxf::FreshnessWindow;
using cxf::LifecycleState;
using cxf::RequestOutcome;
using cxf::Timestamp;

constexpr std::int64_t kBase = 1700000000;

[[nodiscard]] CandidateDeclaration declaration_of(const std::string& name) {
  CandidateDeclaration declaration;
  declaration.registry_name = "registry-" + name;
  declaration.model = "model-x";
  declaration.serial = "SN-" + name;
  declaration.hardware = cxf::HardwareGeneration::from_value(1);
  declaration.firmware = cxf::FirmwareGeneration::from_value(1);
  return declaration;
}

void admit_one(Fabric& fabric, const std::string& name,
               const std::vector<DependencyRef>& dependencies = {}) {
  cxf::AdmitCandidateRequest request;
  request.name = name;
  request.declaration = declaration_of(name);
  request.dependencies = dependencies;
  const cxf::Outcome<RequestOutcome> outcome = fabric.admit_candidate(request);
  REQUIRE_OK(outcome);
}

[[nodiscard]] cxf::Outcome<RequestOutcome> submit(Fabric& fabric, const std::string& candidate,
                                                  EvidenceDimension dimension,
                                                  const std::string& subject,
                                                  EvidenceVerdict verdict,
                                                  EvidenceSource source, Timestamp observed_at,
                                                  FreshnessWindow freshness,
                                                  const std::string& source_name = "source",
                                                  const std::string& detail = "observation") {
  cxf::SubmitEvidenceRequest request;
  request.candidate = candidate;
  request.dimension = dimension;
  request.subject = subject;
  request.verdict = verdict;
  request.source = source;
  request.source_name = source_name;
  request.detail = detail;
  request.observed_at = observed_at;
  request.freshness = freshness;
  return fabric.submit_evidence(request);
}

/// A store with one admitted candidate called "node-1".
struct Fixture {
  cxf::test::TempDirectory directory{"adversarial"};
  cxf::ManualClock clock{Timestamp::from_unix_seconds(kBase)};
  FabricOptions options{};
  cxf::Outcome<Fabric> fabric{cxf::Status::error(Code::kInternalError, "not opened")};
};

void open_fixture(Fixture& fixture) {
  REQUIRE(fixture.directory.ok());
  fixture.options.directory = fixture.directory.child("store");
  fixture.options.holder = "adversarial-evidence";
  fixture.fabric = Fabric::open(fixture.options, fixture.clock);
  REQUIRE_OK(fixture.fabric);
}

}  // namespace

CXF_TEST(adversarial, hostile_candidate_names_are_refused) {
  Fixture fixture;
  open_fixture(fixture);

  struct Case {
    const char* label;
    std::string name;
  };
  const std::vector<Case> cases = {
      {"empty", std::string()},
      {"four_kib", std::string(4097, 'x')},
      {"spaces", std::string("node 1")},
      {"slash", std::string("node/1")},
      {"invalid_utf8", std::string("\xC0\x80", 2)},
      {"surrogate", std::string("\xED\xA0\x80", 3)},
      {"embedded_nul", std::string("node\0one", 8)},
      {"control", std::string("\x01", 1)},
      {"newline", std::string("node\n1")}};

  const cxf::CommitSequence baseline = fixture.fabric->meta().commit;
  for (const Case& item : cases) {
    cxf::AdmitCandidateRequest request;
    request.name = item.name;
    request.declaration = declaration_of("hostile");
    const cxf::Outcome<RequestOutcome> outcome = fixture.fabric->admit_candidate(request);
    // A hostile name is refused with a clear input status and nothing is
    // written; the process stays alive and the store stays usable.
    if (outcome.ok()) {
      ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", item.label,
                              "hostile candidate name was accepted");
    } else {
      CHECK(cxf::test::is_input_rejection(outcome.code()));
    }
    CHECK_EQ(fixture.fabric->meta().commit, baseline);
    CHECK_EQ(fixture.fabric->stats().candidates, 0u);
  }

  // The runtime is still fully usable afterwards.
  admit_one(*fixture.fabric, "node-1");
  CHECK_EQ(fixture.fabric->stats().candidates, 1u);
}

CXF_TEST(adversarial, unknown_enum_spellings_never_resolve) {
  // Spellings that no enumeration accepts.
  const std::vector<std::string> spellings = {"",      " ",        "bogus",   "IDENTITY",
                                              "Identity", "identity ", "identity\n",
                                              "declared2", "0",      "-none"};
  for (const std::string& spelling : spellings) {
    CHECK(!cxf::parse_state(spelling).ok());
    CHECK(!cxf::parse_dimension(spelling).ok());
    CHECK(!cxf::parse_verdict(spelling).ok());
    CHECK(!cxf::parse_source(spelling).ok());
    CHECK(!cxf::parse_activation_result(spelling).ok());
    CHECK(!cxf::parse_dependency_kind(spelling).ok());
    CHECK(!cxf::parse_quarantine_reason(spelling).ok());
    CHECK(!cxf::parse_request_kind(spelling).ok());
    CHECK(!cxf::parse_event_kind(spelling).ok());
    CHECK(!cxf::parse_record_kind(spelling).ok());
    CHECK(!cxf::parse_frame_kind(spelling).ok());
    CHECK(!cxf::parse_crash_point(spelling).ok());
  }
  // A NUL byte inside a spelling is not a spelling.
  CHECK(!cxf::parse_state(std::string("declared\0", 9)).ok());
  // "none" is a real spelling where a reason or a crash point is named, and it
  // must not leak into the vocabularies that have no such value.
  CHECK(cxf::parse_quarantine_reason("none").ok());
  CHECK(cxf::parse_crash_point("none").ok());
  CHECK(!cxf::parse_state("none").ok());
  CHECK(!cxf::parse_dimension("none").ok());
  CHECK(!cxf::parse_frame_kind("none").ok());
  CHECK(!cxf::parse_record_kind("none").ok());
}

CXF_TEST(adversarial, hostile_evidence_fields_are_refused) {
  Fixture fixture;
  open_fixture(fixture);
  admit_one(*fixture.fabric, "node-1");

  struct Case {
    const char* label;
    EvidenceDimension dimension;
    std::string subject;
    std::string detail;
  };
  const std::vector<Case> cases = {
      {"empty_subject", EvidenceDimension::kIdentity, std::string(), "detail"},
      {"four_kib_subject", EvidenceDimension::kIdentity, std::string(4097, 's'), "detail"},
      {"invalid_utf8_subject", EvidenceDimension::kIdentity, std::string("\xC0\x80", 2), "detail"},
      {"nul_subject", EvidenceDimension::kIdentity, std::string("a\0b", 3), "detail"},
      {"invalid_utf8_detail", EvidenceDimension::kIdentity, "subject", std::string("\xED\xBF\xBF", 3)},
      {"four_kib_detail", EvidenceDimension::kIdentity, "subject", std::string(513, 'd')}};

  for (const Case& item : cases) {
    const cxf::Outcome<RequestOutcome> outcome =
        submit(*fixture.fabric, "node-1", item.dimension, item.subject,
               EvidenceVerdict::kSatisfied, EvidenceSource::kObserved, fixture.clock.now(),
               FreshnessWindow::of(Duration::from_seconds(3600)), "source", item.detail);
    if (outcome.ok()) {
      ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", item.label,
                              "hostile evidence field was accepted");
    } else {
      CHECK(cxf::test::is_input_rejection(outcome.code()));
    }
  }

  // An out-of-domain dimension value is refused by name, not coerced.
  cxf::SubmitEvidenceRequest unknown;
  unknown.candidate = "node-1";
  unknown.dimension = static_cast<EvidenceDimension>(200);
  unknown.subject = "subject";
  unknown.verdict = EvidenceVerdict::kSatisfied;
  unknown.source = EvidenceSource::kObserved;
  unknown.observed_at = fixture.clock.now();
  unknown.freshness = FreshnessWindow::of(Duration::from_seconds(3600));
  CHECK_CODE(fixture.fabric->submit_evidence(unknown), Code::kEvidenceUnknownDimension);

  // And a well-formed submission still works.
  const cxf::Outcome<RequestOutcome> good =
      submit(*fixture.fabric, "node-1", EvidenceDimension::kIdentity, "registry-node-1",
             EvidenceVerdict::kSatisfied, EvidenceSource::kObserved, fixture.clock.now(),
             FreshnessWindow::of(Duration::from_seconds(3600)), "asset-registry", "observed");
  REQUIRE_OK(good);
  CHECK(!good->evidence.is_zero());
}

CXF_TEST(adversarial, evidence_for_an_absent_candidate_is_refused) {
  Fixture fixture;
  open_fixture(fixture);
  const cxf::Outcome<RequestOutcome> outcome =
      submit(*fixture.fabric, "no-such-node", EvidenceDimension::kIdentity, "subject",
             EvidenceVerdict::kSatisfied, EvidenceSource::kObserved, fixture.clock.now(),
             FreshnessWindow::of(Duration::from_seconds(3600)));
  CHECK_CODE(outcome, Code::kCandidateNotFound);
  CHECK_EQ(fixture.fabric->stats().evidence, 0u);
}

CXF_TEST(adversarial, future_observations_are_never_fresh) {
  Fixture fixture;
  open_fixture(fixture);
  admit_one(*fixture.fabric, "node-1");

  // An observation stamped a year into the future is refused outright: a clock
  // that disagrees with the durable timeline fails closed rather than being
  // recorded as an observation that can never be trusted.
  const Timestamp future = fixture.clock.now() + Duration::from_seconds(31536000);
  const cxf::Outcome<RequestOutcome> outcome =
      submit(*fixture.fabric, "node-1", EvidenceDimension::kIdentity, "registry-node-1",
             EvidenceVerdict::kSatisfied, EvidenceSource::kObserved, future,
             FreshnessWindow::of(Duration::from_seconds(3600)));
  CHECK_CODE(outcome, Code::kFieldOutOfRange);

  // Nothing was recorded and nothing became ready.
  CHECK_EQ(fixture.fabric->stats().evidence, 0u);
  const cxf::Outcome<cxf::ReadinessReport> report = fixture.fabric->preview_readiness("node-1");
  REQUIRE_OK(report);
  const cxf::DimensionEvaluation& identity =
      report->dimensions[cxf::dimension_rank(EvidenceDimension::kIdentity)];
  CHECK(identity.verdict != EvidenceVerdict::kSatisfied);
  CHECK_EQ(identity.live_records, 0u);
  CHECK_EQ(identity.stale_records, 0u);
  CHECK(!report->ready);

  // An observation two hours old under a one-hour window is long expired. (The
  // value is deliberately not the epoch: the model treats epoch as "absent".)
  const cxf::Outcome<RequestOutcome> ancient =
      submit(*fixture.fabric, "node-1", EvidenceDimension::kPlacement, "rack-1",
             EvidenceVerdict::kSatisfied, EvidenceSource::kObserved,
             fixture.clock.now() + Duration::from_seconds(-7200),
             FreshnessWindow::of(Duration::from_seconds(3600)));
  REQUIRE_OK(ancient);
  const cxf::Outcome<cxf::ReadinessReport> second = fixture.fabric->preview_readiness("node-1");
  REQUIRE_OK(second);
  const cxf::DimensionEvaluation& placement =
      second->dimensions[cxf::dimension_rank(EvidenceDimension::kPlacement)];
  CHECK(placement.verdict != EvidenceVerdict::kSatisfied);
  CHECK_EQ(placement.live_records, 0u);
  CHECK(placement.stale_records >= 1u);
}

CXF_TEST(adversarial, duplicate_payload_digests_are_refused) {
  Fixture fixture;
  open_fixture(fixture);
  admit_one(*fixture.fabric, "node-1");

  const cxf::Outcome<RequestOutcome> first =
      submit(*fixture.fabric, "node-1", EvidenceDimension::kIdentity, "registry-node-1",
             EvidenceVerdict::kSatisfied, EvidenceSource::kObserved, fixture.clock.now(),
             FreshnessWindow::of(Duration::from_seconds(3600)), "asset-registry", "observed");
  REQUIRE_OK(first);

  // Byte-identical content is the same observation: recording it twice would
  // inflate the evidence count without adding information.
  const cxf::Outcome<RequestOutcome> duplicate =
      submit(*fixture.fabric, "node-1", EvidenceDimension::kIdentity, "registry-node-1",
             EvidenceVerdict::kSatisfied, EvidenceSource::kObserved, fixture.clock.now(),
             FreshnessWindow::of(Duration::from_seconds(3600)), "asset-registry", "observed");
  CHECK_CODE(duplicate, Code::kEvidenceDuplicateDigest);

  // A different detail is a different observation and is accepted.
  const cxf::Outcome<RequestOutcome> different =
      submit(*fixture.fabric, "node-1", EvidenceDimension::kIdentity, "registry-node-1",
             EvidenceVerdict::kSatisfied, EvidenceSource::kObserved, fixture.clock.now(),
             FreshnessWindow::of(Duration::from_seconds(3600)), "asset-registry",
             "observed again");
  REQUIRE_OK(different);
  CHECK(different->evidence != first->evidence);
}

CXF_TEST(adversarial, contradictions_never_satisfy_and_quarantine) {
  Fixture fixture;
  open_fixture(fixture);
  admit_one(*fixture.fabric, "node-1");

  const cxf::Outcome<RequestOutcome> yes =
      submit(*fixture.fabric, "node-1", EvidenceDimension::kElectrical, "pdu-a",
             EvidenceVerdict::kSatisfied, EvidenceSource::kObserved, fixture.clock.now(),
             FreshnessWindow::of(Duration::from_seconds(3600)), "power-control", "feed is up");
  REQUIRE_OK(yes);

  // The same subject observed as unsatisfied by another live observation. The
  // content differs, so this is not a duplicate digest.
  const cxf::Outcome<RequestOutcome> no =
      submit(*fixture.fabric, "node-1", EvidenceDimension::kElectrical, "pdu-a",
             EvidenceVerdict::kUnsatisfied, EvidenceSource::kObserved, fixture.clock.now(),
             FreshnessWindow::of(Duration::from_seconds(3600)), "power-control", "feed is down");

  const cxf::Outcome<cxf::StatusView> status = fixture.fabric->status("node-1");
  REQUIRE_OK(status);
  const bool quarantined = status->summary.state == LifecycleState::kQuarantined &&
                           status->summary.quarantine_reason == cxf::QuarantineReason::kContradictoryEvidence;
  const bool refused = !no.ok() && no.code() == Code::kEvidenceContradictory;
  if (!quarantined && !refused) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", "contradiction handling",
                            "live satisfied and unsatisfied evidence for one key neither "
                            "quarantined the candidate nor was refused as contradictory");
  }
  // Whatever the runtime did, the contradiction is never reported as readiness.
  if (!quarantined) {
    const cxf::Outcome<cxf::ReadinessReport> report =
        fixture.fabric->preview_readiness("node-1");
    REQUIRE_OK(report);
    const cxf::DimensionEvaluation& electrical =
        report->dimensions[cxf::dimension_rank(EvidenceDimension::kElectrical)];
    CHECK(electrical.verdict != EvidenceVerdict::kSatisfied);
    CHECK(!report->ready);
    if (no.ok()) {
      CHECK_EQ(electrical.code, Code::kEvidenceContradictory);
      CHECK_EQ(electrical.verdict, EvidenceVerdict::kUnknown);
    }
  }
}

CXF_TEST(adversarial, evidence_for_quarantined_and_terminal_candidates) {
  Fixture fixture;
  open_fixture(fixture);
  admit_one(*fixture.fabric, "quarantined-node");
  admit_one(*fixture.fabric, "cancelled-node");

  cxf::QuarantineRequest quarantine;
  quarantine.candidate = "quarantined-node";
  quarantine.reason = cxf::QuarantineReason::kFailedDiagnostics;
  quarantine.detail = "diagnostics failed";
  REQUIRE_OK(fixture.fabric->quarantine(quarantine));

  cxf::CancelRequest cancel;
  cancel.candidate = "cancelled-node";
  cancel.detail = "operator cancelled";
  REQUIRE_OK(fixture.fabric->cancel(cancel));

  // A cancelled candidate is terminal: no observation can be recorded against
  // it, because no observation could ever be used.
  const cxf::Outcome<RequestOutcome> for_cancelled =
      submit(*fixture.fabric, "cancelled-node", EvidenceDimension::kHealth, "diagnostics",
             EvidenceVerdict::kSatisfied, EvidenceSource::kObserved, fixture.clock.now(),
             FreshnessWindow::of(Duration::from_seconds(3600)));
  if (for_cancelled.ok()) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", "cancelled-node",
                            "evidence was recorded for a terminal candidate");
  } else {
    const Code code = for_cancelled.code();
    CHECK(code == Code::kStateNotAllowed || code == Code::kCandidateTerminal ||
          code == Code::kTransitionNotAllowed);
  }

  // A quarantined candidate may be either refused or recorded for the audit
  // trail, but it may not become ready while it is quarantined.
  const cxf::Outcome<RequestOutcome> for_quarantined =
      submit(*fixture.fabric, "quarantined-node", EvidenceDimension::kHealth, "diagnostics",
             EvidenceVerdict::kSatisfied, EvidenceSource::kObserved, fixture.clock.now(),
             FreshnessWindow::of(Duration::from_seconds(3600)));
  if (!for_quarantined.ok()) {
    const Code code = for_quarantined.code();
    CHECK(code == Code::kStateNotAllowed || code == Code::kCandidateTerminal ||
          code == Code::kTransitionNotAllowed || code == Code::kEvidenceStale ||
          code == Code::kEvidenceGenerationMismatch);
  } else {
    const cxf::Outcome<cxf::StatusView> view = fixture.fabric->status("quarantined-node");
    REQUIRE_OK(view);
    CHECK_EQ(view->summary.state, LifecycleState::kQuarantined);
    CHECK(!view->ready);
  }

  // The states themselves are unchanged by the refused submissions.
  const cxf::Outcome<cxf::StatusView> quarantined = fixture.fabric->status("quarantined-node");
  REQUIRE_OK(quarantined);
  CHECK_EQ(quarantined->summary.state, LifecycleState::kQuarantined);
  const cxf::Outcome<cxf::StatusView> cancelled = fixture.fabric->status("cancelled-node");
  REQUIRE_OK(cancelled);
  CHECK_EQ(cancelled->summary.state, LifecycleState::kCancelled);
}

CXF_TEST(adversarial, reordered_evidence_evaluates_identically) {
  const cxf::FacilityGenerations current;
  cxf::CommissioningCandidate candidate;
  candidate.id = cxf::CandidateId::from_value(1);
  candidate.name = "node-1";
  candidate.revision = cxf::Revision::from_value(1);
  candidate.lifecycle = cxf::LifecycleGeneration::from_value(1);
  candidate.incarnation = cxf::IncarnationId::from_value(1);
  candidate.attempt.id = cxf::AttemptId::from_value(1);
  candidate.generations = current;

  // Observations whose accepted sequence, provenance and instant deliberately
  // disagree, so only the documented precedence can produce a decision.
  std::vector<cxf::EvidenceRecord> records;
  const auto make = [&current](EvidenceDimension dimension, const char* subject,
                               EvidenceVerdict verdict, EvidenceSource source,
                               std::uint64_t id, std::uint64_t sequence,
                               std::int64_t seconds) {
    cxf::EvidenceRecord record;
    record.id = cxf::EvidenceId::from_value(id);
    record.candidate = cxf::CandidateId::from_value(1);
    record.attempt = cxf::AttemptId::from_value(1);
    record.dimension = dimension;
    record.subject = subject;
    record.verdict = verdict;
    record.source = source;
    record.source_name = "source";
    record.observed_at = Timestamp::from_unix_seconds(kBase + seconds);
    record.freshness = FreshnessWindow::never();
    record.observed_sequence = cxf::ObservationSequence::from_value(sequence);
    record.generations = current;
    record.payload = cxf::DigestBuilder::of(std::to_string(id) + subject);
    return record;
  };
  records.push_back(make(EvidenceDimension::kHealth, "diagnostics", EvidenceVerdict::kSatisfied,
                         EvidenceSource::kObserved, 4, 1, 100));
  records.push_back(make(EvidenceDimension::kHealth, "diagnostics", EvidenceVerdict::kSatisfied,
                         EvidenceSource::kImported, 2, 9, 900));
  records.push_back(make(EvidenceDimension::kHealth, "diagnostics", EvidenceVerdict::kSatisfied,
                         EvidenceSource::kObserved, 1, 5, 500));
  records.push_back(make(EvidenceDimension::kPolicy, "policy-gold", EvidenceVerdict::kUnsatisfied,
                         EvidenceSource::kObserved, 3, 3, 300));

  const cxf::ReadinessPolicy policy = cxf::ReadinessPolicy::all_required();
  const cxf::DependencyResolution dependencies;
  const cxf::ReadinessReport forward = cxf::evaluate_readiness(
      candidate, records, policy, current, dependencies, Timestamp::from_unix_seconds(kBase),
      cxf::ObservationSequence::from_value(11));
  std::vector<cxf::EvidenceRecord> reversed(records.rbegin(), records.rend());
  const cxf::ReadinessReport backward = cxf::evaluate_readiness(
      candidate, reversed, policy, current, dependencies, Timestamp::from_unix_seconds(kBase),
      cxf::ObservationSequence::from_value(11));
  // Iteration order is not part of the answer: the report is the same value.
  CHECK_EQ(forward.digest, backward.digest);
  CHECK_EQ(cxf::compute_report_digest(forward), cxf::compute_report_digest(backward));
  for (std::size_t i = 0; i < forward.dimensions.size(); ++i) {
    CHECK_EQ(forward.dimensions[i].verdict, backward.dimensions[i].verdict);
    CHECK_EQ(forward.dimensions[i].code, backward.dimensions[i].code);
    CHECK_EQ(forward.dimensions[i].witness, backward.dimensions[i].witness);
  }
}

CXF_TEST(adversarial, generation_drift_between_submission_and_evaluation) {
  Fixture fixture;
  open_fixture(fixture);
  admit_one(*fixture.fabric, "node-1");

  const cxf::Outcome<RequestOutcome> recorded =
      submit(*fixture.fabric, "node-1", EvidenceDimension::kIdentity, "registry-node-1",
             EvidenceVerdict::kSatisfied, EvidenceSource::kObserved, fixture.clock.now(),
             FreshnessWindow::never(), "asset-registry", "observed");
  REQUIRE_OK(recorded);

  // The identity dimension asserts both a live observation and a recorded
  // binding, so the binding is established before the drift.
  cxf::BindIdentityRequest bind;
  bind.candidate = "node-1";
  bind.evidence = recorded->evidence;
  bind.identity.asset = cxf::AssetId::from_value(1);
  bind.identity.registry_name = "registry-node-1";
  bind.identity.model = "model-x";
  bind.identity.serial = "SN-node-1";
  REQUIRE_OK(fixture.fabric->bind_identity(bind));

  const cxf::Outcome<cxf::ReadinessReport> before = fixture.fabric->preview_readiness("node-1");
  REQUIRE_OK(before);
  CHECK_EQ(before->dimensions[cxf::dimension_rank(EvidenceDimension::kIdentity)].verdict,
           EvidenceVerdict::kSatisfied);

  // A facility fact moves: every observation taken under the previous
  // generations stops being live, even though it has not expired.
  cxf::FacilityChangeRequest drift;
  drift.power = cxf::PowerGeneration::from_value(9);
  drift.detail = "power feed maintenance";
  REQUIRE_OK(fixture.fabric->record_facility_change(drift));

  const cxf::Outcome<cxf::ReadinessReport> after = fixture.fabric->preview_readiness("node-1");
  REQUIRE_OK(after);
  const cxf::DimensionEvaluation& identity =
      after->dimensions[cxf::dimension_rank(EvidenceDimension::kIdentity)];
  CHECK(identity.verdict != EvidenceVerdict::kSatisfied);
  CHECK_EQ(identity.live_records, 0u);
  CHECK(identity.stale_records >= 1u);
  CHECK(!after->ready);
  CHECK(after->generations != before->generations);
}

CXF_TEST(adversarial, declared_dependency_generation_drift) {
  Fixture fixture;
  open_fixture(fixture);

  cxf::FacilityChangeRequest facility;
  facility.dependency = DependencyGeneration::from_value(1);
  facility.dependency_facts = {{DependencyKind::kRack, "rack-a",
                                DependencyGeneration::from_value(1)}};
  REQUIRE_OK(fixture.fabric->record_facility_change(facility));

  admit_one(*fixture.fabric, "node-1",
            {DependencyRef{DependencyKind::kRack, "rack-a", DependencyGeneration::from_value(1),
                           true}});
  const cxf::Outcome<cxf::ReadinessReport> before = fixture.fabric->preview_readiness("node-1");
  REQUIRE_OK(before);
  CHECK_EQ(before->dimensions[cxf::dimension_rank(EvidenceDimension::kDependency)].verdict,
           EvidenceVerdict::kSatisfied);

  // The rack is re-published at a new generation. The candidate expected
  // generation 1 and is not silently re-bound to the new fact.
  cxf::FacilityChangeRequest moved;
  moved.dependency = DependencyGeneration::from_value(2);
  moved.dependency_facts = {{DependencyKind::kRack, "rack-a",
                             DependencyGeneration::from_value(2)}};
  REQUIRE_OK(fixture.fabric->record_facility_change(moved));

  const cxf::Outcome<cxf::ReadinessReport> after = fixture.fabric->preview_readiness("node-1");
  REQUIRE_OK(after);
  const cxf::DimensionEvaluation& dependency =
      after->dimensions[cxf::dimension_rank(EvidenceDimension::kDependency)];
  CHECK(dependency.verdict != EvidenceVerdict::kSatisfied);
  CHECK(!after->ready);

  // Duplicate dependency targets are refused at admission, because two
  // expectations for one entity cannot both hold.
  cxf::AdmitCandidateRequest duplicate;
  duplicate.name = "node-2";
  duplicate.declaration = declaration_of("node-2");
  duplicate.dependencies = {
      DependencyRef{DependencyKind::kRack, "rack-a", DependencyGeneration::from_value(2), true},
      DependencyRef{DependencyKind::kRack, "rack-a", DependencyGeneration::from_value(2), false}};
  CHECK(!fixture.fabric->admit_candidate(duplicate).ok());
}

CXF_TEST(adversarial, zero_identities_and_extreme_generations) {
  Fixture fixture;
  open_fixture(fixture);
  admit_one(*fixture.fabric, "node-1");

  // Zero is never a valid identity: it is how "not yet assigned" is spelled.
  cxf::BindIdentityRequest bind;
  bind.candidate = "node-1";
  bind.evidence = cxf::EvidenceId::from_value(0);
  bind.identity.asset = cxf::AssetId::from_value(1);
  bind.identity.registry_name = "registry-node-1";
  bind.identity.serial = "SN-node-1";
  CHECK(!fixture.fabric->bind_identity(bind).ok());

  cxf::BindPlacementRequest place;
  place.candidate = "node-1";
  place.evidence = cxf::EvidenceId::from_value(0);
  place.placement.site = cxf::SiteId::from_value(1);
  place.placement.rack = cxf::RackId::from_value(1);
  place.placement.position = "U1";
  CHECK(!fixture.fabric->bind_placement(place).ok());

  cxf::AuthorizeActivationRequest authorize;
  authorize.candidate = "node-1";
  authorize.plan = cxf::PlanId::from_value(0);
  authorize.plan_digest = cxf::DigestBuilder::of(std::string_view("nothing"));
  CHECK(!fixture.fabric->authorize_activation(authorize).ok());

  cxf::ReportActivationRequest report;
  report.candidate = "node-1";
  report.token = cxf::TokenId::from_value(0);
  report.binding = cxf::DigestBuilder::of(std::string_view("nothing"));
  report.result = cxf::ActivationResultKind::kSucceeded;
  report.observed_at = fixture.clock.now();
  CHECK(!fixture.fabric->report_activation(report).ok());

  cxf::EvaluateReadinessRequest evaluate;
  evaluate.candidate = "node-1";
  REQUIRE_OK(fixture.fabric->evaluate_readiness(evaluate));

  // A generation at the top of the range is accepted, is not wrapped by the
  // next value, and a lower value is refused as a regression rather than
  // absorbed.
  cxf::FacilityChangeRequest maximum;
  maximum.topology = cxf::TopologyGeneration::from_value(UINT64_MAX);
  maximum.cooling = cxf::CoolingGeneration::from_value(UINT64_MAX);
  REQUIRE_OK(fixture.fabric->record_facility_change(maximum));
  CHECK_EQ(fixture.fabric->facility().generations.topology.value(), UINT64_MAX);

  cxf::FacilityChangeRequest regression;
  regression.topology = cxf::TopologyGeneration::from_value(UINT64_MAX - 1);
  CHECK_CODE(fixture.fabric->record_facility_change(regression), Code::kGenerationRegression);
  // The held generation did not move: the refusal is complete.
  CHECK_EQ(fixture.fabric->facility().generations.topology.value(), UINT64_MAX);

  cxf::FacilityChangeRequest equal;
  equal.topology = cxf::TopologyGeneration::from_value(UINT64_MAX);
  const cxf::Outcome<RequestOutcome> repeated = fixture.fabric->record_facility_change(equal);
  CHECK(repeated.ok() || repeated.code() == Code::kGenerationRegression);
  CHECK_EQ(fixture.fabric->facility().generations.topology.value(), UINT64_MAX);
}
