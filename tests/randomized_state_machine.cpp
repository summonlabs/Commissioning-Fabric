// Randomised state machine against a real Fabric on a real store.
//
// A seeded command sequence drives the public API; after every single step the
// invariants that make the runtime trustworthy are re-checked. The sequence is
// deliberately mixed: facilities advance, dependencies drift, evidence expires,
// plans go stale and tokens are consumed underneath each other, so most of the
// interesting states are reached without being scheduled by hand.
//
// A second, smaller test re-derives the lifecycle prefix rule from an
// independent in-memory oracle: the furthest state is the longest satisfied
// prefix of the chain, and the chain never claims a state the dimensions do not
// support.

#include "support/test_harness.hpp"

#include "cxf/model/lifecycle.hpp"
#include "cxf/runtime/fabric.hpp"
#include "cxf/support/clock.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using cxf::ActivationGrant;
using cxf::ActivationResultKind;
using cxf::CandidateSummary;
using cxf::Code;
using cxf::CommitSequence;
using cxf::CoolingGeneration;
using cxf::DependencyGeneration;
using cxf::DependencyKind;
using cxf::Digest;
using cxf::Duration;
using cxf::EvidenceDimension;
using cxf::EvidenceId;
using cxf::EvidenceSource;
using cxf::EvidenceVerdict;
using cxf::Fabric;
using cxf::FabricOptions;
using cxf::FreshnessWindow;
using cxf::HardwareGeneration;
using cxf::LifecycleState;
using cxf::PlanId;
using cxf::RequestKind;
using cxf::RequestOutcome;
using cxf::TokenId;
using cxf::TopologyGeneration;

constexpr int kSteps = 140;

using Summaries = std::map<std::string, CandidateSummary>;

/// One recorded, accepted request. Exactly one payload member matches the
/// recorded kind; the same value is re-issued later to prove that a replay
/// returns the recorded effect rather than re-applying the mutation.
struct Recorded {
  RequestKind kind{RequestKind::kAdmitCandidate};
  std::uint64_t request{0};
  Digest effect{};
  std::uint64_t commit{0};
  cxf::AdmitCandidateRequest admit{};
  cxf::BindIdentityRequest bind_identity{};
  cxf::BindPlacementRequest bind_placement{};
  cxf::SubmitEvidenceRequest submit_evidence{};
  cxf::EvaluateReadinessRequest evaluate{};
  cxf::AuthorizeActivationRequest authorize{};
  cxf::ReportActivationRequest report{};
  cxf::QuarantineRequest quarantine{};
  cxf::ReleaseQuarantineRequest release{};
  cxf::CancelRequest cancel{};
  cxf::FacilityChangeRequest facility{};
};

[[nodiscard]] Summaries summarize(const Fabric& fabric) {
  Summaries summaries;
  const cxf::Outcome<std::vector<CandidateSummary>> list = fabric.list_candidates();
  if (!list.ok()) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK_OK", "list_candidates",
                            list.status().message());
    return summaries;
  }
  for (const CandidateSummary& summary : *list) {
    summaries[summary.name] = summary;
  }
  return summaries;
}

/// The invariants that must hold after every step, whatever the step did.
void check_monotonicity(const Summaries& before, const Summaries& after) {
  for (const std::pair<const std::string, CandidateSummary>& entry : before) {
    const auto found = after.find(entry.first);
    CHECK(found != after.end());
    if (found == after.end()) {
      continue;
    }
    const CandidateSummary& now = found->second;
    const CandidateSummary& then = entry.second;
    // A revision is a fence: it never goes backwards, so an older plan can
    // always be recognised as older.
    CHECK(now.revision >= then.revision);
    // The lifecycle generation is a fencing counter and is monotone by
    // construction.
    CHECK(now.lifecycle >= then.lifecycle);
    // The chain is never re-entered backwards. (Quarantine release leaves the
    // non-chain Quarantined state, so it is not a backwards chain move.)
    if (cxf::is_milestone_state(then.state) && cxf::is_milestone_state(now.state)) {
      CHECK(cxf::state_rank(now.state) >= cxf::state_rank(then.state));
    }
    // A terminal state is final, with one documented exception: a quarantine is
    // left either by an explicit release (back to Identified, where the bound
    // identity is still asserted) or by another exit (cancelled). It is never
    // left by quietly advancing along the validation chain.
    if (then.state == LifecycleState::kQuarantined) {
      if (now.state != then.state && now.state != LifecycleState::kIdentified &&
          !cxf::is_terminal_state(now.state)) {
        ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", "quarantine exit",
                                std::string(cxf::state_name(then.state)) + " -> " +
                                    std::string(cxf::state_name(now.state)));
      }
    } else if (cxf::is_terminal_state(then.state) && now.state != then.state) {
      ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", "terminal state changed",
                              std::string(cxf::state_name(then.state)) + " -> " +
                                  std::string(cxf::state_name(now.state)));
    }
    CHECK(now.id == then.id);
    CHECK_EQ(now.name, then.name);
  }
}

/// Stamp the runtime-reported idempotency key onto the payload member that
/// matches the recorded kind, so a replay carries the same key the runtime
/// stored rather than a fresh one.
void set_request_id(Recorded& recorded, cxf::RequestId id) {
  recorded.request = id.value();
  switch (recorded.kind) {
    case RequestKind::kAdmitCandidate:
      recorded.admit.request = id;
      break;
    case RequestKind::kBindIdentity:
      recorded.bind_identity.request = id;
      break;
    case RequestKind::kBindPlacement:
      recorded.bind_placement.request = id;
      break;
    case RequestKind::kSubmitEvidence:
      recorded.submit_evidence.request = id;
      break;
    case RequestKind::kEvaluateReadiness:
      recorded.evaluate.request = id;
      break;
    case RequestKind::kAuthorizeActivation:
      recorded.authorize.request = id;
      break;
    case RequestKind::kReportActivation:
      recorded.report.request = id;
      break;
    case RequestKind::kQuarantine:
      recorded.quarantine.request = id;
      break;
    case RequestKind::kReleaseQuarantine:
      recorded.release.request = id;
      break;
    case RequestKind::kCancel:
      recorded.cancel.request = id;
      break;
    case RequestKind::kRecordFacilityChange:
      recorded.facility.request = id;
      break;
  }
}

/// Re-issue a recorded request. Used both to prove replay identity and to prove
/// that a rejected request is not stored.
[[nodiscard]] cxf::Outcome<RequestOutcome> reissue(Fabric& fabric, const Recorded& request) {
  switch (request.kind) {
    case RequestKind::kAdmitCandidate:
      return fabric.admit_candidate(request.admit);
    case RequestKind::kBindIdentity:
      return fabric.bind_identity(request.bind_identity);
    case RequestKind::kBindPlacement:
      return fabric.bind_placement(request.bind_placement);
    case RequestKind::kSubmitEvidence:
      return fabric.submit_evidence(request.submit_evidence);
    case RequestKind::kEvaluateReadiness: {
      const cxf::Outcome<cxf::ReadinessResult> result = fabric.evaluate_readiness(request.evaluate);
      if (!result.ok()) {
        return result.status();
      }
      return result->outcome;
    }
    case RequestKind::kAuthorizeActivation: {
      const cxf::Outcome<ActivationGrant> grant = fabric.authorize_activation(request.authorize);
      if (!grant.ok()) {
        return grant.status();
      }
      return grant->outcome;
    }
    case RequestKind::kReportActivation:
      return fabric.report_activation(request.report);
    case RequestKind::kQuarantine:
      return fabric.quarantine(request.quarantine);
    case RequestKind::kReleaseQuarantine:
      return fabric.release_quarantine(request.release);
    case RequestKind::kCancel:
      return fabric.cancel(request.cancel);
    case RequestKind::kRecordFacilityChange:
      return fabric.record_facility_change(request.facility);
  }
  return cxf::make_error(Code::kInternalError, "recorded request kind is not handled");
}

}  // namespace

CXF_TEST(randomized, fabric_command_sequence) {
  cxf::test::Rng& rng = cxf::test::rng();
  cxf::test::TempDirectory directory("cxf-randomized");
  REQUIRE(directory.ok());

  FabricOptions options;
  options.directory = directory.child("store");
  options.holder = "randomized-state-machine";
  cxf::ManualClock clock(cxf::Timestamp::from_unix_seconds(1700000000));

  Summaries final_summaries;
  std::uint64_t final_commit = 0;
  std::uint64_t final_candidates = 0;
  std::uint64_t final_evidence = 0;
  int accepted_steps = 0;
  int rejected_steps = 0;
  int idle_steps = 0;
  std::vector<Recorded> accepted;

  {
    cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
    REQUIRE_OK(fabric);

    std::vector<std::string> names;
    std::map<std::string, cxf::AdmitCandidateRequest> declarations;
    std::map<std::string, std::pair<EvidenceId, std::string>> identity_evidence;
    std::map<std::string, std::pair<EvidenceId, std::string>> placement_evidence;
    std::map<std::string, PlanId> last_plan;
    std::map<std::string, Digest> last_plan_digest;
    std::map<std::string, TokenId> last_token;
    std::map<std::string, Digest> last_binding;

    const std::string rack_name = "rack-a";
    std::uint64_t facility_generation = 0;
    std::uint64_t admitted = 0;

    for (int step = 0; step < kSteps; ++step) {
      const CommitSequence commit_before = fabric->meta().commit;
      const Summaries before = summarize(*fabric);
      const std::string target = names.empty() ? std::string() : names[rng.below(names.size())];

      // Common tail: check the commit invariant, record the accepted request,
      // re-issue a rejected one and re-check monotonicity.
      const auto finish = [&](const Recorded& request,
                              const cxf::Outcome<RequestOutcome>& outcome, bool issued) {
        const CommitSequence commit_after = fabric->meta().commit;
        if (!issued) {
          ++idle_steps;
          CHECK_EQ(commit_after, commit_before);
        } else if (outcome.ok()) {
          ++accepted_steps;
          // An accepted mutation publishes exactly one new commit, and the
          // commit counter strictly increases.
          CHECK(commit_after > commit_before);
          CHECK_EQ(outcome->commit, commit_after);
          Recorded recorded = request;
          set_request_id(recorded, outcome->request);
          recorded.effect = outcome->effect_digest;
          recorded.commit = outcome->commit.value();
          accepted.push_back(recorded);
        } else {
          ++rejected_steps;
          // A rejected mutation is not durable: the commit does not move, and
          // the same request from the same state answers the same way.
          CHECK_EQ(commit_after, commit_before);
          const cxf::Outcome<RequestOutcome> again = reissue(*fabric, request);
          CHECK(!again.ok());
          CHECK_EQ(again.code(), outcome.code());
        }
        check_monotonicity(before, summarize(*fabric));
        if (step % 16 == 0) {
          CHECK(fabric->verify_store().ok());
        }
      };

      const int roll = static_cast<int>(rng.below(100));
      if (names.empty() || roll < 14) {
        // --- admit ---------------------------------------------------------
        const std::string name = "node-" + std::to_string(admitted);
        ++admitted;
        Recorded request;
        request.kind = RequestKind::kAdmitCandidate;
        request.admit.name = name;
        request.admit.declaration.registry_name = "registry-" + name;
        request.admit.declaration.model = "model-" + std::to_string(rng.below(4));
        request.admit.declaration.serial = "SN-" + name;
        request.admit.declaration.hardware = HardwareGeneration::from_value(rng.inclusive(1, 3));
        request.admit.declaration.firmware =
            cxf::FirmwareGeneration::from_value(rng.inclusive(1, 9));
        if (rng.chance(1, 2)) {
          request.admit.dependencies.push_back({DependencyKind::kRack, rack_name,
                                                DependencyGeneration::from_value(facility_generation),
                                                true});
        }
        const cxf::Outcome<RequestOutcome> outcome = fabric->admit_candidate(request.admit);
        if (outcome.ok()) {
          names.push_back(name);
          declarations[name] = request.admit;
        }
        finish(request, outcome, true);
      } else if (roll < 26) {
        // --- facility change: moves generations and re-publishes the fact ---
        ++facility_generation;
        Recorded request;
        request.kind = RequestKind::kRecordFacilityChange;
        request.facility.topology = TopologyGeneration::from_value(facility_generation);
        request.facility.cooling = CoolingGeneration::from_value(facility_generation);
        request.facility.dependency = DependencyGeneration::from_value(facility_generation);
        request.facility.dependency_facts.push_back(
            {DependencyKind::kRack, rack_name, DependencyGeneration::from_value(facility_generation)});
        request.facility.detail = "facility generation " + std::to_string(facility_generation);
        const cxf::Outcome<RequestOutcome> outcome =
            fabric->record_facility_change(request.facility);
        finish(request, outcome, true);
      } else if (roll < 50) {
        // --- evidence ------------------------------------------------------
        Recorded request;
        request.kind = RequestKind::kSubmitEvidence;
        request.submit_evidence.candidate = target;
        request.submit_evidence.dimension =
            static_cast<EvidenceDimension>(static_cast<std::uint8_t>(rng.below(10)));
        static const char* const kSubjects[] = {"rack-a", "pdu-a", "zone-3", "fabric-east",
                                                "baseline-1"};
        request.submit_evidence.subject = kSubjects[rng.below(5)];
        request.submit_evidence.verdict =
            rng.chance(3, 4) ? EvidenceVerdict::kSatisfied : EvidenceVerdict::kUnsatisfied;
        request.submit_evidence.source =
            rng.chance(1, 8) ? EvidenceSource::kDeclared : EvidenceSource::kObserved;
        request.submit_evidence.source_name = "source-" + rng.token(4);
        // Distinct content per submission: a duplicate payload is refused as a
        // duplicate rather than recorded as a new observation.
        request.submit_evidence.detail =
            "observation " + std::to_string(step) + "-" + rng.token(8);
        request.submit_evidence.observed_at =
            clock.now() + Duration::from_seconds(-static_cast<std::int64_t>(rng.below(30)));
        request.submit_evidence.freshness = FreshnessWindow::of(
            Duration::from_seconds(static_cast<std::int64_t>(rng.inclusive(0, 3600))));
        const cxf::Outcome<RequestOutcome> outcome =
            fabric->submit_evidence(request.submit_evidence);
        if (outcome.ok()) {
          if (request.submit_evidence.dimension == EvidenceDimension::kIdentity) {
            identity_evidence[target] = {outcome->evidence, request.submit_evidence.subject};
          }
          if (request.submit_evidence.dimension == EvidenceDimension::kPlacement) {
            placement_evidence[target] = {outcome->evidence, request.submit_evidence.subject};
          }
        }
        finish(request, outcome, true);
      } else if (roll < 58) {
        // --- bind identity from a live observation -------------------------
        const auto evidence = identity_evidence.find(target);
        const auto declaration = declarations.find(target);
        if (evidence == identity_evidence.end() || declaration == declarations.end()) {
          finish(Recorded{}, cxf::Outcome<RequestOutcome>(RequestOutcome{}), false);
        } else {
          Recorded request;
          request.kind = RequestKind::kBindIdentity;
          request.bind_identity.candidate = target;
          request.bind_identity.evidence = evidence->second.first;
          request.bind_identity.identity.asset = cxf::AssetId::from_value(rng.inclusive(1, 9999));
          request.bind_identity.identity.registry_name = evidence->second.second;
          request.bind_identity.identity.model = declaration->second.declaration.model;
          request.bind_identity.identity.serial = declaration->second.declaration.serial;
          request.bind_identity.identity.hardware = declaration->second.declaration.hardware;
          request.bind_identity.identity.firmware = declaration->second.declaration.firmware;
          const cxf::Outcome<RequestOutcome> outcome =
              fabric->bind_identity(request.bind_identity);
          finish(request, outcome, true);
        }
      } else if (roll < 66) {
        // --- bind placement ------------------------------------------------
        const auto evidence = placement_evidence.find(target);
        if (evidence == placement_evidence.end()) {
          finish(Recorded{}, cxf::Outcome<RequestOutcome>(RequestOutcome{}), false);
        } else {
          Recorded request;
          request.kind = RequestKind::kBindPlacement;
          request.bind_placement.candidate = target;
          request.bind_placement.evidence = evidence->second.first;
          request.bind_placement.placement.site = cxf::SiteId::from_value(1);
          request.bind_placement.placement.rack = cxf::RackId::from_value(1);
          request.bind_placement.placement.position = "U" + std::to_string(rng.inclusive(1, 48));
          request.bind_placement.placement.topology = TopologyGeneration::from_value(1);
          const cxf::Outcome<RequestOutcome> outcome =
              fabric->bind_placement(request.bind_placement);
          finish(request, outcome, true);
        }
      } else if (roll < 78) {
        // --- evaluate readiness: freezes a plan ----------------------------
        Recorded request;
        request.kind = RequestKind::kEvaluateReadiness;
        request.evaluate.candidate = target;
        const cxf::Outcome<cxf::ReadinessResult> outcome =
            fabric->evaluate_readiness(request.evaluate);
        if (outcome.ok()) {
          last_plan[target] = outcome->plan;
          last_plan_digest[target] = outcome->report.digest;
        }
        finish(request,
               outcome.ok() ? cxf::Outcome<RequestOutcome>(outcome->outcome)
                            : cxf::Outcome<RequestOutcome>(outcome.status()),
               true);
      } else if (roll < 86) {
        // --- authorize activation against the last plan --------------------
        const auto plan = last_plan.find(target);
        const auto digest = last_plan_digest.find(target);
        if (plan == last_plan.end() || digest == last_plan_digest.end()) {
          finish(Recorded{}, cxf::Outcome<RequestOutcome>(RequestOutcome{}), false);
        } else {
          Recorded request;
          request.kind = RequestKind::kAuthorizeActivation;
          request.authorize.candidate = target;
          request.authorize.plan = plan->second;
          request.authorize.plan_digest = digest->second;
          const cxf::Outcome<ActivationGrant> outcome =
              fabric->authorize_activation(request.authorize);
          if (outcome.ok()) {
            last_token[target] = outcome->token;
            last_binding[target] = outcome->binding;
          }
          finish(request,
                 outcome.ok() ? cxf::Outcome<RequestOutcome>(outcome->outcome)
                              : cxf::Outcome<RequestOutcome>(outcome.status()),
                 true);
        }
      } else if (roll < 92) {
        // --- report an activation attempt ----------------------------------
        const auto token = last_token.find(target);
        const auto binding = last_binding.find(target);
        if (token == last_token.end() || binding == last_binding.end()) {
          finish(Recorded{}, cxf::Outcome<RequestOutcome>(RequestOutcome{}), false);
        } else {
          Recorded request;
          request.kind = RequestKind::kReportActivation;
          request.report.candidate = target;
          request.report.token = token->second;
          request.report.binding = binding->second;
          const int result = static_cast<int>(rng.below(3));
          request.report.result = result == 0   ? ActivationResultKind::kSucceeded
                                  : result == 1 ? ActivationResultKind::kFailed
                                                : ActivationResultKind::kDeferred;
          request.report.detail = "activation report " + rng.token(6);
          request.report.observed_at = clock.now();
          const cxf::Outcome<RequestOutcome> outcome =
              fabric->report_activation(request.report);
          finish(request, outcome, true);
        }
      } else if (roll < 96) {
        // --- quarantine -----------------------------------------------------
        Recorded request;
        request.kind = RequestKind::kQuarantine;
        request.quarantine.candidate = target;
        request.quarantine.reason =
            static_cast<cxf::QuarantineReason>(static_cast<std::uint8_t>(rng.below(9)));
        request.quarantine.detail = "quarantine " + rng.token(6);
        const cxf::Outcome<RequestOutcome> outcome = fabric->quarantine(request.quarantine);
        finish(request, outcome, true);
      } else if (roll < 98) {
        // --- release quarantine ---------------------------------------------
        Recorded request;
        request.kind = RequestKind::kReleaseQuarantine;
        request.release.candidate = target;
        request.release.detail = "release " + rng.token(6);
        const cxf::Outcome<RequestOutcome> outcome =
            fabric->release_quarantine(request.release);
        finish(request, outcome, true);
      } else if (roll < 99) {
        // --- cancel ----------------------------------------------------------
        Recorded request;
        request.kind = RequestKind::kCancel;
        request.cancel.candidate = target;
        request.cancel.detail = "cancel " + rng.token(6);
        const cxf::Outcome<RequestOutcome> outcome = fabric->cancel(request.cancel);
        finish(request, outcome, true);
      } else {
        // --- clock advance: not a mutation, and must not move the commit ----
        clock.advance(Duration::from_seconds(static_cast<std::int64_t>(rng.inclusive(1, 600))));
        finish(Recorded{}, cxf::Outcome<RequestOutcome>(RequestOutcome{}), false);
      }
    }

    // Replaying accepted requests must return the identical ids, digests and
    // commit, and must not apply anything again.
    CHECK(!accepted.empty());
    for (int i = 0; i < 16 && !accepted.empty(); ++i) {
      const Recorded& request = accepted[rng.below(accepted.size())];
      const CommitSequence commit_before = fabric->meta().commit;
      const cxf::Outcome<RequestOutcome> replay = reissue(*fabric, request);
      REQUIRE_OK(replay);
      CHECK_EQ(replay->request, cxf::RequestId::from_value(request.request));
      CHECK_EQ(replay->effect_digest, request.effect);
      CHECK_EQ(replay->commit, CommitSequence::from_value(request.commit));
      CHECK(replay->replayed);
      CHECK_EQ(fabric->meta().commit, commit_before);
    }

    CHECK(fabric->verify_store().ok());
    final_summaries = summarize(*fabric);
    final_commit = fabric->meta().commit.value();
    final_candidates = fabric->stats().candidates;
    final_evidence = fabric->stats().evidence;
  }

  // The sequence must actually have exercised both outcomes: a test that
  // silently accepted or rejected everything would prove nothing.
  CHECK(accepted_steps > 20);
  CHECK(rejected_steps > 0);
  CHECK(idle_steps > 0);

  // Reopening the store reproduces one authoritative generation: the same
  // commit, the same record counts and the same candidate states.
  {
    cxf::ManualClock reopened_clock(cxf::Timestamp::from_unix_seconds(1700000000));
    const cxf::Outcome<Fabric> reopened = Fabric::open(options, reopened_clock);
    REQUIRE_OK(reopened);
    CHECK(reopened->stats().recovered);
    CHECK_EQ(reopened->meta().commit, CommitSequence::from_value(final_commit));
    CHECK_EQ(reopened->stats().candidates, final_candidates);
    CHECK_EQ(reopened->stats().evidence, final_evidence);
    const Summaries summaries = summarize(*reopened);
    CHECK_EQ(summaries.size(), final_summaries.size());
    for (const std::pair<const std::string, CandidateSummary>& entry : final_summaries) {
      const auto found = summaries.find(entry.first);
      REQUIRE(found != summaries.end());
      CHECK_EQ(found->second.id, entry.second.id);
      CHECK_EQ(found->second.state, entry.second.state);
      CHECK_EQ(found->second.lifecycle, entry.second.lifecycle);
      CHECK_EQ(found->second.incarnation, entry.second.incarnation);
      CHECK_EQ(found->second.revision, entry.second.revision);
      CHECK_EQ(found->second.attempt, entry.second.attempt);
      CHECK_EQ(found->second.quarantine_reason, entry.second.quarantine_reason);
    }
    CHECK(reopened->verify_store().ok());
  }
}

CXF_TEST(randomized, lifecycle_prefix_oracle) {
  cxf::test::Rng& rng = cxf::test::rng();
  using Mask = std::uint16_t;
  const auto bit = [](EvidenceDimension dimension) {
    return static_cast<Mask>(1u << cxf::dimension_rank(dimension));
  };

  // The requirement set of every chain state, derived independently of the
  // implementation: a state is reachable exactly when every dimension it
  // asserts has been satisfied.
  std::array<Mask, cxf::kMilestoneStateCount> needs{};
  needs[0] = 0;                                 // declared
  needs[1] = bit(EvidenceDimension::kIdentity);  // identified
  needs[2] = needs[1] | bit(EvidenceDimension::kPlacement);
  needs[3] = needs[2] | bit(EvidenceDimension::kDependency);
  needs[4] = needs[3] | bit(EvidenceDimension::kCompatibility);
  needs[5] = needs[4] | bit(EvidenceDimension::kElectrical) | bit(EvidenceDimension::kCooling);
  needs[6] = needs[5] | bit(EvidenceDimension::kNetwork);
  needs[7] = needs[6] | bit(EvidenceDimension::kHealth);
  needs[8] = needs[7] | bit(EvidenceDimension::kPolicy) | bit(EvidenceDimension::kServiceClass);
  // Activating and Commissioned are not reachable from readiness alone: they
  // need an explicit authorization and a reported activation.
  needs[9] = 0xFFFF;
  needs[10] = 0xFFFF;

  const std::array<LifecycleState, cxf::kMilestoneStateCount> chain = cxf::milestone_states();
  for (int iteration = 0; iteration < 400; ++iteration) {
    std::array<bool, cxf::kEvidenceDimensionCount> satisfied{};
    Mask mask = 0;
    for (std::size_t i = 0; i < satisfied.size(); ++i) {
      if (rng.chance(1, 2)) {
        satisfied[i] = true;
        mask = static_cast<Mask>(mask | (1u << i));
      }
    }
    std::size_t expected = 0;
    for (std::size_t i = 0; i <= 8; ++i) {
      if ((mask & needs[i]) == needs[i]) {
        expected = i;
      }
    }
    const LifecycleState actual = cxf::furthest_reachable_state(satisfied);
    CHECK_EQ(actual, chain[expected]);
    // Whatever the input, the chain never claims a state that readiness alone
    // cannot justify.
    CHECK(cxf::state_rank(actual) <= cxf::state_rank(LifecycleState::kActivationAuthorized));
  }
}
