// Downstream consumer: exercises the installed library through its public
// headers and the imported target only. It performs a real commissioning
// sequence against a store it creates, and fails loudly when the runtime does
// not behave as the documentation says it does.

#include <cxf/runtime/fabric.hpp>
#include <cxf/support/clock.hpp>
#include <cxf/support/status.hpp>

#include <iostream>
#include <string>

namespace {

int fail(const std::string& what, const cxf::Status& status) {
  std::cerr << "downstream-consumer: " << what << ": " << status.message() << "\n";
  return 3;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string directory = argc > 1 ? argv[1] : std::string("cxf-downstream-store");

  cxf::SystemClock clock;
  cxf::FabricOptions options;
  options.directory = directory;
  options.holder = "downstream-consumer";

  cxf::Outcome<cxf::Fabric> fabric = cxf::Fabric::open(options, clock);
  if (!fabric.ok()) {
    return fail("open", fabric.status());
  }

  cxf::AdmitCandidateRequest admit;
  admit.name = "downstream-node-1";
  admit.declaration.registry_name = "registry-asset-1";
  admit.declaration.model = "example-model";
  admit.declaration.serial = "SN-DOWNSTREAM-1";

  cxf::Outcome<cxf::RequestOutcome> admitted = fabric->admit_candidate(admit);
  if (!admitted.ok()) {
    return fail("admit", admitted.status());
  }
  if (admitted->state != cxf::LifecycleState::kDeclared) {
    std::cerr << "downstream-consumer: admit did not report the Declared state\n";
    return 4;
  }

  cxf::SubmitEvidenceRequest evidence;
  evidence.candidate = "downstream-node-1";
  evidence.dimension = cxf::EvidenceDimension::kIdentity;
  evidence.subject = "registry-asset-1";
  evidence.verdict = cxf::EvidenceVerdict::kSatisfied;
  evidence.source = cxf::EvidenceSource::kObserved;
  evidence.source_name = "asset-registry";
  evidence.detail = "registry identity observed";
  evidence.observed_at = clock.now();
  evidence.freshness = cxf::FreshnessWindow::of(cxf::Duration::from_seconds(3600));

  cxf::Outcome<cxf::RequestOutcome> recorded = fabric->submit_evidence(evidence);
  if (!recorded.ok()) {
    return fail("submit_evidence", recorded.status());
  }

  cxf::EvaluateReadinessRequest evaluate;
  evaluate.candidate = "downstream-node-1";
  cxf::Outcome<cxf::ReadinessResult> evaluated = fabric->evaluate_readiness(evaluate);
  if (!evaluated.ok()) {
    return fail("evaluate_readiness", evaluated.status());
  }
  if (evaluated->report.dimensions.size() != cxf::kEvidenceDimensionCount) {
    std::cerr << "downstream-consumer: readiness report is not complete\n";
    return 5;
  }
  if (evaluated->report.ready) {
    std::cerr << "downstream-consumer: a candidate with one dimension observed "
                 "must not be ready\n";
    return 6;
  }
  if (evaluated->report.state != cxf::LifecycleState::kDeclared) {
    std::cerr << "downstream-consumer: identity evidence alone must not advance "
                 "the lifecycle\n";
    return 7;
  }

  std::cout << "downstream-consumer: ok dims=" << evaluated->report.dimensions.size()
            << " ready=false commit=" << fabric->meta().commit.value() << "\n";
  return 0;
}
