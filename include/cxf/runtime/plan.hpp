// Commissioning Fabric - readiness plans.
//
// A plan is the frozen answer to "may this asset enter service now": it records
// the exact candidate revision, incarnation, lifecycle generation, facility
// generations and readiness report that were evaluated together. Activation
// authority is granted against a plan, so a plan that no longer matches current
// state is stale rather than reusable.
#ifndef CXF_RUNTIME_PLAN_HPP
#define CXF_RUNTIME_PLAN_HPP

#include <string>

#include "cxf/model/candidate.hpp"
#include "cxf/model/readiness.hpp"
#include "cxf/model/record.hpp"
#include "cxf/support/clock.hpp"
#include "cxf/support/digest.hpp"
#include "cxf/support/status.hpp"
#include "cxf/types/generations.hpp"
#include "cxf/types/ids.hpp"
#include "cxf/types/timepoint.hpp"

namespace cxf {

struct ReadinessPlan {
  PlanId id{};
  CandidateId candidate{};
  Revision revision{};
  IncarnationId incarnation{};
  LifecycleGeneration lifecycle{};
  FacilityGenerations generations{};
  /// The report the plan was derived from, with its content address.
  ReadinessReport report{};
  Digest report_digest{};
  Timestamp created_at{};
  FreshnessWindow validity{};
  CommitSequence commit{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("id", id);
    ar("candidate", candidate);
    ar("revision", revision);
    ar("incarnation", incarnation);
    ar("lifecycle", lifecycle);
    ar("generations", generations);
    ar("report", report);
    ar("report_digest", report_digest);
    ar("created_at", created_at);
    ar("validity", validity);
    ar("commit", commit);
  }
};

/// Digest over every field of the plan; this is what activation authority binds
/// to, so any change to the plan changes the grant that can be accepted.
[[nodiscard]] Digest compute_plan_digest(const ReadinessPlan& plan);

/// Reason the plan cannot be used for activation, or kOk.
[[nodiscard]] Status validate_plan_usable(const ReadinessPlan& plan,
                                          const CommissioningCandidate& candidate,
                                          const FacilityGenerations& current,
                                          Timestamp now);

inline constexpr Duration kDefaultPlanValidity = Duration::from_seconds(900);

}  // namespace cxf

#endif  // CXF_RUNTIME_PLAN_HPP
