#include "cxf/runtime/plan.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "cxf/codec/archive.hpp"
#include "cxf/support/digest.hpp"

namespace cxf {
namespace {

/// Every plan field that is fixed when the plan is recorded. Commit is excluded:
/// it is assigned by the commit that publishes the plan, and the digest is the
/// value activation authority binds to, so it must not depend on where the plan
/// happens to be stored.
struct PlanDigestBody {
  PlanId id{};
  CandidateId candidate{};
  Revision revision{};
  IncarnationId incarnation{};
  LifecycleGeneration lifecycle{};
  FacilityGenerations generations{};
  ReadinessReport report{};
  Digest report_digest{};
  Timestamp created_at{};
  FreshnessWindow validity{};

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
  }
};

/// Digest of one canonical record body.
[[nodiscard]] Digest digest_of_body(const std::vector<std::byte>& body) {
  DigestBuilder builder;
  builder.update(std::span<const std::byte>(body.data(), body.size()));
  return builder.finish();
}

/// Comma-separated list of the generations that moved, for the error context.
[[nodiscard]] std::string join_names(const std::vector<std::string>& names) {
  std::string out;
  for (const std::string& name : names) {
    if (!out.empty()) {
      out.push_back(',');
    }
    out.append(name);
  }
  return out;
}

}  // namespace

Digest compute_plan_digest(const ReadinessPlan& plan) {
  PlanDigestBody body;
  body.id = plan.id;
  body.candidate = plan.candidate;
  body.revision = plan.revision;
  body.incarnation = plan.incarnation;
  body.lifecycle = plan.lifecycle;
  body.generations = plan.generations;
  body.report = plan.report;
  body.report_digest = plan.report_digest;
  body.created_at = plan.created_at;
  body.validity = plan.validity;
  return digest_of_body(encode_body(body));
}

Status validate_plan_usable(const ReadinessPlan& plan, const CommissioningCandidate& candidate,
                            const FacilityGenerations& current, Timestamp now) {
  // Deterministic order: the plan is unusable for the first reason that
  // applies, and a plan bound to another candidate is not a stale plan but an
  // absent one.
  if (plan.candidate != candidate.id) {
    return make_error(Code::kPlanNotFound, "the plan was recorded for a different candidate",
                      "plan candidate=" + plan.candidate.str() +
                          " candidate=" + candidate.id.str());
  }
  if (plan.revision != candidate.revision || plan.incarnation != candidate.incarnation ||
      plan.lifecycle != candidate.lifecycle) {
    return make_error(Code::kPlanStale,
                      "the plan was recorded for a different candidate version",
                      "plan revision=" + plan.revision.str() +
                          " incarnation=" + plan.incarnation.str() +
                          " lifecycle=" + plan.lifecycle.str() +
                          " candidate revision=" + candidate.revision.str() +
                          " incarnation=" + candidate.incarnation.str() +
                          " lifecycle=" + candidate.lifecycle.str());
  }
  if (plan.generations != current) {
    return make_error(Code::kPlanStale, "facility generations moved after the plan was recorded",
                      "moved=" + join_names(plan.generations.differences(current)));
  }
  if (plan.validity.is_expired(plan.created_at, now)) {
    return make_error(Code::kPlanStale, "the plan is no longer valid",
                      "created_at=" + plan.created_at.to_rfc3339() +
                          " validity=" + plan.validity.str() + " now=" + now.to_rfc3339());
  }
  if (!plan.report.ready) {
    return make_error(Code::kEvidenceMissing,
                      "the plan was derived from a readiness report that is not ready",
                      "plan=" + plan.id.str() + " furthest=" +
                          std::string(state_name(plan.report.furthest_state)));
  }
  return Status::success();
}

}  // namespace cxf
