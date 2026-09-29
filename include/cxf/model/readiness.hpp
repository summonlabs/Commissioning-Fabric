// Commissioning Fabric - readiness evaluation.
//
// Readiness is a report, never a mutation: evaluating a candidate states exactly
// which dimensions are satisfied, by which observation, and which remain
// unsatisfied. The report is content-addressed so activation authority can bind
// to the exact evidence set it was derived from.
#ifndef CXF_MODEL_READINESS_HPP
#define CXF_MODEL_READINESS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cxf/model/candidate.hpp"
#include "cxf/model/dependency.hpp"
#include "cxf/model/evidence.hpp"
#include "cxf/support/digest.hpp"
#include "cxf/support/status.hpp"
#include "cxf/types/enums.hpp"
#include "cxf/types/generations.hpp"

namespace cxf {

/// Which dimensions must be satisfied before activation can be authorized.
struct ReadinessPolicy {
  std::array<bool, kEvidenceDimensionCount> required{true, true, true, true, true,
                                                     true, true, true, true, true};

  [[nodiscard]] static ReadinessPolicy all_required() noexcept;
  [[nodiscard]] bool is_required(EvidenceDimension dimension) const noexcept;
  void set_required(EvidenceDimension dimension, bool value) noexcept;
  [[nodiscard]] std::size_t required_count() const noexcept;

  template <typename Ar>
  void visit(Ar& ar) {
    ar("required", required);
  }
};

/// Outcome of evaluating one dimension.
struct DimensionEvaluation {
  EvidenceDimension dimension{EvidenceDimension::kIdentity};
  bool required{true};
  EvidenceVerdict verdict{EvidenceVerdict::kUnknown};
  /// Reason the dimension is not satisfied; kOk exactly when it is satisfied.
  Code code{Code::kOk};
  std::string explanation{};
  /// The observation that decided the dimension, when one exists.
  EvidenceId witness{};
  EvidenceSource witness_source{EvidenceSource::kDeclared};
  ObservationSequence witness_sequence{};
  std::uint32_t live_records{0};
  std::uint32_t stale_records{0};
  std::uint32_t contradictory_records{0};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("dimension", dimension);
    ar("required", required);
    ar("verdict", verdict);
    ar("code", code);
    ar("explanation", explanation);
    ar("witness", witness);
    ar("witness_source", witness_source);
    ar("witness_sequence", witness_sequence);
    ar("live_records", live_records);
    ar("stale_records", stale_records);
    ar("contradictory_records", contradictory_records);
  }
};

/// The complete readiness report for one candidate revision.
struct ReadinessReport {
  CandidateId candidate{};
  Revision revision{};
  IncarnationId incarnation{};
  LifecycleGeneration lifecycle{};
  LifecycleState state{LifecycleState::kDeclared};
  FacilityGenerations generations{};
  DependencyResolution dependencies{};
  bool ready{false};
  /// Furthest chain state the satisfied dimensions support.
  LifecycleState furthest_state{LifecycleState::kDeclared};
  /// Always all ten dimensions, in fixed declaration order.
  std::vector<DimensionEvaluation> dimensions{};
  Timestamp evaluated_at{};
  ObservationSequence observation{};
  /// Content address of this report with the digest field zeroed.
  Digest digest{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("candidate", candidate);
    ar("revision", revision);
    ar("incarnation", incarnation);
    ar("lifecycle", lifecycle);
    ar("state", state);
    ar("generations", generations);
    ar("dependencies", dependencies);
    ar("ready", ready);
    ar("furthest_state", furthest_state);
    ar("dimensions", dimensions);
    ar("evaluated_at", evaluated_at);
    ar("observation", observation);
    ar("digest", digest);
  }
};

/// Evaluate readiness. Pure: it reads the candidate, the evidence, the policy of
/// record and the resolved dependencies, and returns a report. It never
/// mutates its inputs.
[[nodiscard]] ReadinessReport evaluate_readiness(const CommissioningCandidate& candidate,
                                                 const std::vector<EvidenceRecord>& evidence,
                                                 const ReadinessPolicy& policy,
                                                 const FacilityGenerations& current,
                                                 const DependencyResolution& dependencies,
                                                 Timestamp now,
                                                 ObservationSequence observation);

/// Digest over every report field except the digest itself.
[[nodiscard]] Digest compute_report_digest(const ReadinessReport& report);

/// Dimensions that block activation, in fixed dimension order.
[[nodiscard]] std::vector<const DimensionEvaluation*> blocking_dimensions(
    const ReadinessReport& report);

/// Deterministic one-line reason for the first blocker, or an empty string when
/// nothing blocks.
[[nodiscard]] std::string first_blocker_summary(const ReadinessReport& report);

}  // namespace cxf

#endif  // CXF_MODEL_READINESS_HPP
