// Commissioning Fabric example: one complete governed commissioning journey.
//
// The example drives the public API against a real store in a temporary
// directory and prints what it observes at every step. It is deliberately
// explicit about the separation the runtime enforces: admission is a claim,
// evidence is an observation, a binding is a record of that observation, a plan
// is a frozen evaluation, and activation authority is a bounded permission that
// ages out and is fenced by any relevant generation change.

#include <cxf/runtime/explain.hpp>
#include <cxf/runtime/fabric.hpp>
#include <cxf/support/clock.hpp>
#include <cxf/support/fs.hpp>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr const char* kCandidate = "dc1-rack7-node03";

int fail(const std::string& step, const cxf::Status& status) {
  std::cerr << "example: " << step << " failed: " << status.message() << "\n";
  return 1;
}

/// Submit one observed readiness observation.
cxf::Status observe(cxf::Fabric& fabric, cxf::EvidenceDimension dimension,
                    const std::string& subject, const std::string& source_name,
                    cxf::SystemClock& clock, cxf::EvidenceId& out_id) {
  cxf::SubmitEvidenceRequest request;
  request.candidate = kCandidate;
  request.dimension = dimension;
  request.subject = subject;
  request.verdict = cxf::EvidenceVerdict::kSatisfied;
  request.source = cxf::EvidenceSource::kObserved;
  request.source_name = source_name;
  request.detail = std::string("observed by ") + source_name;
  request.observed_at = clock.now();
  request.freshness = cxf::FreshnessWindow::of(cxf::Duration::from_seconds(3600));
  const cxf::Outcome<cxf::RequestOutcome> outcome = fabric.submit_evidence(request);
  if (!outcome.ok()) {
    return outcome.status();
  }
  out_id = outcome->evidence;
  return cxf::Status::success();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string directory = argc > 1 ? argv[1] : std::string("cxf-example-store");
  cxf::SystemClock clock;

  cxf::FabricOptions options;
  options.directory = directory;
  options.holder = "commissioning-example";

  cxf::Outcome<cxf::Fabric> fabric = cxf::Fabric::open(options, clock);
  if (!fabric.ok()) {
    return fail("open", fabric.status());
  }
  std::cout << "opened store directory=" << directory
            << " incarnation=" << fabric->meta().incarnation.value()
            << " epoch=" << fabric->meta().epoch.value() << "\n";

  // A second run against the same store would replay an admission that already
  // happened and a facility change that would move a generation backwards. Both
  // are refused by the runtime, correctly, so the example reports the state it
  // found instead of pretending to run the journey again.
  {
    const cxf::Outcome<std::vector<cxf::CandidateSummary>> existing =
        fabric->list_candidates();
    if (!existing.ok()) {
      return fail("list_candidates", existing.status());
    }
    for (const cxf::CandidateSummary& summary : existing.value()) {
      if (summary.name == kCandidate) {
        std::cout << "example: store already holds " << kCandidate
                  << " (state=" << cxf::state_name(summary.state)
                  << "); run the example against a fresh directory to see the whole "
                     "journey\n";
        return 0;
      }
    }
  }

  // 1. Publish the facility facts the candidate will depend on. These are the
  //    generations of entities owned by adjacent registries; the fabric stores
  //    the generation it was told about, not the entity. Generations only move
  //    forward, so a re-run over an existing store keeps the newer value.
  const cxf::FacilityGenerations current = fabric->facility().generations;
  const auto at_least = [](auto held, auto desired) { return held < desired ? desired : held; };
  cxf::FacilityChangeRequest facility;
  facility.topology =
      at_least(current.topology, cxf::TopologyGeneration::from_value(4));
  facility.power = at_least(current.power, cxf::PowerGeneration::from_value(9));
  facility.cooling = at_least(current.cooling, cxf::CoolingGeneration::from_value(7));
  facility.network = at_least(current.network, cxf::NetworkGeneration::from_value(11));
  facility.policy = at_least(current.policy, cxf::PolicyGeneration::from_value(2));
  facility.dependency =
      at_least(current.dependency, cxf::DependencyGeneration::from_value(5));
  facility.dependency_facts = {
      {cxf::DependencyKind::kSite, "dc1", cxf::DependencyGeneration::from_value(5)},
      {cxf::DependencyKind::kRack, "rack7", cxf::DependencyGeneration::from_value(5)},
      {cxf::DependencyKind::kPowerFeed, "pdu-a", cxf::DependencyGeneration::from_value(9)},
      {cxf::DependencyKind::kCoolingZone, "zone-3", cxf::DependencyGeneration::from_value(7)},
      {cxf::DependencyKind::kNetworkFabric, "fabric-east",
       cxf::DependencyGeneration::from_value(11)},
      {cxf::DependencyKind::kServiceClass, "gold", cxf::DependencyGeneration::from_value(5)},
      {cxf::DependencyKind::kOwnership, "team-platform",
       cxf::DependencyGeneration::from_value(5)}};
  facility.detail = "initial facility generations";
  const cxf::Outcome<cxf::RequestOutcome> changed = fabric->record_facility_change(facility);
  if (!changed.ok()) {
    return fail("record_facility_change", changed.status());
  }
  std::cout << "facility published commit=" << changed->commit.value() << "\n";

  // 2. Admit the candidate: a declaration plus the generations it expects.
  cxf::AdmitCandidateRequest admit;
  admit.name = kCandidate;
  admit.declaration.registry_name = "registry-asset-7781";
  admit.declaration.model = "trainium-x8";
  admit.declaration.serial = "SN-7781";
  admit.declaration.hardware = cxf::HardwareGeneration::from_value(3);
  admit.declaration.firmware = cxf::FirmwareGeneration::from_value(12);
  admit.dependencies = {
      {cxf::DependencyKind::kSite, "dc1", cxf::DependencyGeneration::from_value(5), true},
      {cxf::DependencyKind::kRack, "rack7", cxf::DependencyGeneration::from_value(5), true},
      {cxf::DependencyKind::kPowerFeed, "pdu-a", cxf::DependencyGeneration::from_value(9),
       true},
      {cxf::DependencyKind::kCoolingZone, "zone-3",
       cxf::DependencyGeneration::from_value(7), true},
      {cxf::DependencyKind::kNetworkFabric, "fabric-east",
       cxf::DependencyGeneration::from_value(11), true},
      {cxf::DependencyKind::kServiceClass, "gold", cxf::DependencyGeneration::from_value(5),
       true},
      {cxf::DependencyKind::kOwnership, "team-platform",
       cxf::DependencyGeneration::from_value(5), true}};
  const cxf::Outcome<cxf::RequestOutcome> admitted = fabric->admit_candidate(admit);
  if (!admitted.ok()) {
    return fail("admit_candidate", admitted.status());
  }
  std::cout << "admitted state=" << cxf::state_name(admitted->state)
            << " commit=" << admitted->commit.value() << "\n";

  // 3. Identity: observation first, binding second. A declared claim can never
  //    stand in for an observation.
  cxf::EvidenceId identity_evidence{};
  if (cxf::Status s = observe(*fabric, cxf::EvidenceDimension::kIdentity,
                              "registry-asset-7781", "asset-registry", clock,
                              identity_evidence);
      s.failed()) {
    return fail("identity evidence", s);
  }
  cxf::BindIdentityRequest bind;
  bind.candidate = kCandidate;
  bind.evidence = identity_evidence;
  bind.identity.asset = cxf::AssetId::from_value(7781);
  bind.identity.registry_name = "registry-asset-7781";
  bind.identity.model = "trainium-x8";
  bind.identity.serial = "SN-7781";
  bind.identity.hardware = cxf::HardwareGeneration::from_value(3);
  bind.identity.firmware = cxf::FirmwareGeneration::from_value(12);
  const cxf::Outcome<cxf::RequestOutcome> bound = fabric->bind_identity(bind);
  if (!bound.ok()) {
    return fail("bind_identity", bound.status());
  }
  std::cout << "identity bound state=" << cxf::state_name(bound->state) << "\n";

  // 4. Placement.
  cxf::EvidenceId placement_evidence{};
  if (cxf::Status s = observe(*fabric, cxf::EvidenceDimension::kPlacement, "rack7",
                              "facility-topology", clock, placement_evidence);
      s.failed()) {
    return fail("placement evidence", s);
  }
  cxf::BindPlacementRequest place;
  place.candidate = kCandidate;
  place.evidence = placement_evidence;
  place.placement.site = cxf::SiteId::from_value(1);
  place.placement.rack = cxf::RackId::from_value(7);
  place.placement.position = "U12";
  place.placement.topology = cxf::TopologyGeneration::from_value(4);
  const cxf::Outcome<cxf::RequestOutcome> placed = fabric->bind_placement(place);
  if (!placed.ok()) {
    return fail("bind_placement", placed.status());
  }
  std::cout << "placement bound state=" << cxf::state_name(placed->state) << "\n";

  // 5. The remaining readiness dimensions, each from its owning system.
  struct Observation {
    cxf::EvidenceDimension dimension;
    const char* subject;
    const char* source;
  };
  const Observation observations[] = {
      {cxf::EvidenceDimension::kCompatibility, "baseline-2026.1", "compatibility-service"},
      {cxf::EvidenceDimension::kElectrical, "pdu-a", "power-control"},
      {cxf::EvidenceDimension::kCooling, "zone-3", "cooling-control"},
      {cxf::EvidenceDimension::kNetwork, "fabric-east", "network-attachment"},
      {cxf::EvidenceDimension::kHealth, "trainium-x8", "health-diagnostics"},
      {cxf::EvidenceDimension::kPolicy, "policy-gold", "policy-engine"},
      {cxf::EvidenceDimension::kServiceClass, "gold", "service-catalog"},
  };
  for (const Observation& observation : observations) {
    cxf::EvidenceId id{};
    if (cxf::Status s = observe(*fabric, observation.dimension, observation.subject,
                                observation.source, clock, id);
        s.failed()) {
      return fail(std::string("evidence ") +
                      std::string(cxf::dimension_name(observation.dimension)),
                  s);
    }
  }

  // 6. Evaluate: this freezes a plan and advances the lifecycle to the furthest
  //    state the evidence supports.
  cxf::EvaluateReadinessRequest evaluate;
  evaluate.candidate = kCandidate;
  const cxf::Outcome<cxf::ReadinessResult> evaluated = fabric->evaluate_readiness(evaluate);
  if (!evaluated.ok()) {
    return fail("evaluate_readiness", evaluated.status());
  }
  std::cout << cxf::render_report(evaluated->report);
  std::cout << "plan=" << evaluated->plan.value() << " ready="
            << (evaluated->report.ready ? "true" : "false") << "\n";

  // 7. Activation authority: bound to the exact plan and to every prerequisite
  //    generation. The binding is the value the caller must return with the
  //    activation result.
  cxf::AuthorizeActivationRequest authorize;
  authorize.candidate = kCandidate;
  authorize.plan = evaluated->plan;
  authorize.plan_digest = evaluated->plan_digest;
  const cxf::Outcome<cxf::ActivationGrant> grant = fabric->authorize_activation(authorize);
  if (!grant.ok()) {
    return fail("authorize_activation", grant.status());
  }
  std::cout << "authorized token=" << grant->token.value()
            << " binding=" << grant->binding.hex() << "\n";

  // 8. Report the activation attempt. A successful report moves the candidate to
  //    Activating - it is not yet proof of active service.
  cxf::ReportActivationRequest report;
  report.candidate = kCandidate;
  report.token = grant->token;
  report.binding = grant->binding;
  report.result = cxf::ActivationResultKind::kSucceeded;
  report.detail = "power control confirmed the asset entered service";
  report.observed_at = clock.now();
  const cxf::Outcome<cxf::RequestOutcome> reported = fabric->report_activation(report);
  if (!reported.ok()) {
    return fail("report_activation", reported.status());
  }
  std::cout << "activation reported state=" << cxf::state_name(reported->state) << "\n";

  // 9. Post-activation observation. Only fresh observed health evidence taken
  //    after the activation report can commission the asset.
  cxf::EvidenceId post_activation{};
  if (cxf::Status s = observe(*fabric, cxf::EvidenceDimension::kHealth,
                              "trainium-x8", "health-diagnostics", clock, post_activation);
      s.failed()) {
    return fail("post-activation health evidence", s);
  }
  cxf::EvaluateReadinessRequest final_evaluation;
  final_evaluation.candidate = kCandidate;
  const cxf::Outcome<cxf::ReadinessResult> commissioned =
      fabric->evaluate_readiness(final_evaluation);
  if (!commissioned.ok()) {
    return fail("final evaluate_readiness", commissioned.status());
  }
  std::cout << "final state=" << cxf::state_name(commissioned->outcome.state) << "\n";

  // 10. Any relevant generation change fences authority. The already commissioned
  //     asset is unaffected, but a new activation would need a new plan.
  cxf::FacilityChangeRequest drift;
  drift.power = cxf::PowerGeneration::from_value(10);
  drift.detail = "power feed pdu-a maintenance";
  const cxf::Outcome<cxf::RequestOutcome> drifted = fabric->record_facility_change(drift);
  if (!drifted.ok()) {
    return fail("record_facility_change (drift)", drifted.status());
  }
  const cxf::Outcome<cxf::StatusView> status = fabric->status(kCandidate);
  if (!status.ok()) {
    return fail("status", status.status());
  }
  std::cout << cxf::render_status(*status);
  std::cout << "example: complete\n";
  return 0;
}
