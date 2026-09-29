// Commissioning Fabric benchmark.
//
// Methodology and labelling:
//   * Every measurement wraps a COMPLETED operation. Nothing here times
//     submission or enqueue cost, because that is not what the runtime spends.
//   * Durable operations are measured against a real store on this host's
//     filesystem, including the flush and read-back the commit path performs.
//     Those numbers are REAL storage costs and are labelled as such.
//   * The workload itself (generated candidate and subject names) is SYNTHETIC.
//   * The in-memory readiness evaluation measurement calls the pure model
//     function on synthetic but well-formed inputs: synthetic inputs, real
//     computation.
//   * Percentiles are nearest-rank over the recorded per-operation samples, so
//     they can be reproduced from the printed sample count.

#include <cxf/model/readiness.hpp>
#include <cxf/runtime/fabric.hpp>
#include <cxf/support/clock.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Samples {
  std::vector<double> micros{};

  void add(double value) { micros.push_back(value); }

  [[nodiscard]] double percentile(double fraction) const {
    if (micros.empty()) {
      return 0.0;
    }
    std::vector<double> sorted = micros;
    std::sort(sorted.begin(), sorted.end());
    const auto rank = static_cast<std::size_t>(
        fraction * static_cast<double>(sorted.size() - 1) + 0.5);
    return sorted[std::min(rank, sorted.size() - 1)];
  }

  [[nodiscard]] double mean() const {
    if (micros.empty()) {
      return 0.0;
    }
    double total = 0.0;
    for (double value : micros) {
      total += value;
    }
    return total / static_cast<double>(micros.size());
  }

  [[nodiscard]] double throughput() const {
    const double seconds = mean() * static_cast<double>(micros.size()) / 1e6;
    return seconds > 0.0 ? static_cast<double>(micros.size()) / seconds : 0.0;
  }
};

void report(const char* label, const char* labelling, const Samples& samples) {
  std::cout << "benchmark=" << label << " labelling=" << labelling
            << " ops=" << samples.micros.size() << " mean_us=" << samples.mean()
            << " p50_us=" << samples.percentile(0.50)
            << " p95_us=" << samples.percentile(0.95)
            << " p99_us=" << samples.percentile(0.99)
            << " ops_per_second=" << samples.throughput() << "\n";
}

[[nodiscard]] std::string number(std::uint64_t value) { return std::to_string(value); }

}  // namespace

int main(int argc, char** argv) {
  std::size_t candidates = 16;
  std::size_t evaluations = 256;
  std::string directory = "cxf-benchmark-store";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--candidates" && i + 1 < argc) {
      candidates = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (arg == "--evaluations" && i + 1 < argc) {
      evaluations = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (arg == "--store" && i + 1 < argc) {
      directory = argv[++i];
    } else {
      std::cerr << "usage: cxf_benchmark [--store DIR] [--candidates N] [--evaluations N]\n";
      return 2;
    }
  }

  cxf::SystemClock clock;
  cxf::FabricOptions options;
  options.directory = directory;
  options.holder = "benchmark";

  // The store holds an OS-level single-writer lock, so the recovery measurement
  // below can only run after this instance has been released. The mutation phase
  // therefore lives in its own scope.
  std::uint64_t commit_before_reopen = 0;
  cxf::StoreStats stats_before_reopen;
  Samples admit_samples;
  Samples evidence_samples;
  Samples evaluate_samples;
  Samples checkpoint_samples;
  {
  cxf::Outcome<cxf::Fabric> opened = cxf::Fabric::open(options, clock);
  if (!opened.ok()) {
    std::cerr << "open failed: " << opened.status().message() << "\n";
    return 1;
  }
  cxf::Fabric& fabric = opened.value();

  cxf::FacilityChangeRequest facility;
  facility.topology = cxf::TopologyGeneration::from_value(1);
  facility.power = cxf::PowerGeneration::from_value(1);
  facility.cooling = cxf::CoolingGeneration::from_value(1);
  facility.network = cxf::NetworkGeneration::from_value(1);
  facility.policy = cxf::PolicyGeneration::from_value(1);
  facility.dependency = cxf::DependencyGeneration::from_value(1);
  facility.dependency_facts = {
      {cxf::DependencyKind::kRack, "rack-bench", cxf::DependencyGeneration::from_value(1)}};
  if (const cxf::Outcome<cxf::RequestOutcome> outcome =
          fabric.record_facility_change(facility);
      !outcome.ok()) {
    std::cerr << "facility failed: " << outcome.status().message() << "\n";
    return 1;
  }

  std::vector<std::string> names;
  names.reserve(candidates);
  for (std::size_t i = 0; i < candidates; ++i) {
    names.push_back("bench-node-" + number(i));
  }

  for (const std::string& name : names) {
    cxf::AdmitCandidateRequest admit;
    admit.name = name;
    admit.declaration.registry_name = "registry-" + name;
    admit.declaration.serial = "SN-" + name;
    admit.dependencies = {{cxf::DependencyKind::kRack, "rack-bench",
                           cxf::DependencyGeneration::from_value(1), true}};
    const auto start = Clock::now();
    const cxf::Outcome<cxf::RequestOutcome> outcome = fabric.admit_candidate(admit);
    const auto elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    if (!outcome.ok()) {
      std::cerr << "admit failed: " << outcome.status().message() << "\n";
      return 1;
    }
    admit_samples.add(elapsed);
  }

  for (const std::string& name : names) {
    cxf::SubmitEvidenceRequest evidence;
    evidence.candidate = name;
    evidence.dimension = cxf::EvidenceDimension::kIdentity;
    evidence.subject = "registry-" + name;
    evidence.verdict = cxf::EvidenceVerdict::kSatisfied;
    evidence.source = cxf::EvidenceSource::kObserved;
    evidence.source_name = "benchmark-source";
    evidence.observed_at = clock.now();
    evidence.freshness = cxf::FreshnessWindow::of(cxf::Duration::from_seconds(3600));
    const auto start = Clock::now();
    const cxf::Outcome<cxf::RequestOutcome> outcome = fabric.submit_evidence(evidence);
    const auto elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    if (!outcome.ok()) {
      std::cerr << "evidence failed: " << outcome.status().message() << "\n";
      return 1;
    }
    evidence_samples.add(elapsed);
  }

  for (std::size_t i = 0; i < evaluations; ++i) {
    cxf::EvaluateReadinessRequest request;
    request.candidate = names[i % names.size()];
    const auto start = Clock::now();
    const cxf::Outcome<cxf::ReadinessResult> outcome = fabric.evaluate_readiness(request);
    const auto elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    if (!outcome.ok()) {
      std::cerr << "evaluate failed: " << outcome.status().message() << "\n";
      return 1;
    }
    evaluate_samples.add(elapsed);
  }

  {
    const auto start = Clock::now();
    const cxf::Status status = fabric.checkpoint();
    const auto elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    if (status.failed()) {
      std::cerr << "checkpoint failed: " << status.message() << "\n";
      return 1;
    }
    checkpoint_samples.add(elapsed);
  }

  commit_before_reopen = fabric.meta().commit.value();
  stats_before_reopen = fabric.stats();
  }  // the store lock is released here

  // Recovery cost: open the same store again, which replays the snapshot and the
  // journal, and time the completed open.
  {
    const auto start = Clock::now();
    cxf::Outcome<cxf::Fabric> reopened = cxf::Fabric::open(options, clock);
    const auto elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    if (!reopened.ok()) {
      std::cerr << "reopen failed: " << reopened.status().message() << "\n";
      return 1;
    }
    Samples open_samples;
    open_samples.add(elapsed);
    report("open_recover", "real", open_samples);
  }

  report("admit_candidate", "synthetic_workload_real_durability", admit_samples);
  report("submit_evidence", "synthetic_workload_real_durability", evidence_samples);
  report("evaluate_readiness", "synthetic_workload_real_durability", evaluate_samples);
  report("checkpoint", "real", checkpoint_samples);

  // Pure evaluation cost with synthetic in-memory inputs: no storage involved.
  {
    cxf::CommissioningCandidate candidate;
    candidate.id = cxf::CandidateId::from_value(1);
    candidate.name = "bench-pure";
    candidate.revision = cxf::Revision::from_value(1);
    candidate.lifecycle = cxf::LifecycleGeneration::from_value(1);
    candidate.placement = cxf::PlacementBinding{};
    candidate.placement->site = cxf::SiteId::from_value(1);
    candidate.placement->rack = cxf::RackId::from_value(1);
    candidate.placement->position = "U1";
    candidate.identity = cxf::AssetIdentity{};
    candidate.identity->asset = cxf::AssetId::from_value(1);
    candidate.identity->registry_name = "registry-bench-pure";
    candidate.identity->serial = "SN-PURE";
    std::vector<cxf::EvidenceRecord> evidence;
    for (cxf::EvidenceDimension dimension : cxf::all_dimensions()) {
      cxf::EvidenceRecord record;
      record.id = cxf::EvidenceId::from_value(evidence.size() + 1);
      record.candidate = candidate.id;
      record.dimension = dimension;
      record.subject = "bench-subject";
      record.verdict = cxf::EvidenceVerdict::kSatisfied;
      record.source = cxf::EvidenceSource::kObserved;
      record.source_name = "benchmark-source";
      record.observed_at = cxf::Timestamp::from_unix_seconds(1000);
      record.freshness = cxf::FreshnessWindow::never();
      record.observed_sequence = cxf::ObservationSequence::from_value(evidence.size() + 1);
      evidence.push_back(record);
    }
    cxf::DependencyResolution dependencies;
    cxf::ReadinessPolicy policy = cxf::ReadinessPolicy::all_required();
    Samples pure_samples;
    for (std::size_t i = 0; i < evaluations; ++i) {
      const auto start = Clock::now();
      const cxf::ReadinessReport result =
          cxf::evaluate_readiness(candidate, evidence, policy, candidate.generations,
                                  dependencies, cxf::Timestamp::from_unix_seconds(1000),
                                  cxf::ObservationSequence::from_value(i + 1));
      const auto elapsed =
          std::chrono::duration<double, std::micro>(Clock::now() - start).count();
      if (result.dimensions.size() != cxf::kEvidenceDimensionCount) {
        std::cerr << "pure evaluation produced an incomplete report\n";
        return 1;
      }
      pure_samples.add(elapsed);
    }
    report("evaluate_readiness_pure", "synthetic_inputs_real_computation", pure_samples);
  }

  std::cout << "store_commit=" << commit_before_reopen
            << " candidates=" << stats_before_reopen.candidates
            << " evidence=" << stats_before_reopen.evidence
            << " plans=" << stats_before_reopen.plans
            << " journal_bytes=" << stats_before_reopen.journal_bytes
            << " snapshot_bytes=" << stats_before_reopen.snapshot_bytes << "\n";
  return 0;
}
