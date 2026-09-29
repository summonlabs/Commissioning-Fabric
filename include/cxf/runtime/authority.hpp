// Commissioning Fabric - activation authority tokens.
//
// A token is the runtime's bounded permission to ask the power system to bring
// one asset into service. It binds the exact candidate revision, incarnation,
// lifecycle generation, facility generations, attempt and plan digest it was
// granted for. Any relevant change fences it: the token is revalidated against
// live state immediately before it is used, and it is single-use.
#ifndef CXF_RUNTIME_AUTHORITY_HPP
#define CXF_RUNTIME_AUTHORITY_HPP

#include <cstdint>
#include <string>

#include "cxf/model/candidate.hpp"
#include "cxf/model/record.hpp"
#include "cxf/runtime/plan.hpp"
#include "cxf/support/clock.hpp"
#include "cxf/support/digest.hpp"
#include "cxf/support/status.hpp"
#include "cxf/types/generations.hpp"
#include "cxf/types/ids.hpp"
#include "cxf/types/timepoint.hpp"

namespace cxf {

/// What a token authorises, and exactly what it was derived from.
struct ActivationAuthority {
  TokenId id{};
  PlanId plan{};
  Digest plan_digest{};
  CandidateId candidate{};
  Revision revision{};
  IncarnationId incarnation{};
  LifecycleGeneration lifecycle{};
  FacilityGenerations generations{};
  AttemptId attempt{};
  Timestamp issued_at{};
  FreshnessWindow validity{};
  bool consumed{false};
  Timestamp consumed_at{};
  ActivationResultKind last_result{ActivationResultKind::kDeferred};
  std::uint32_t deferred_reports{0};
  CommitSequence commit{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("id", id);
    ar("plan", plan);
    ar("plan_digest", plan_digest);
    ar("candidate", candidate);
    ar("revision", revision);
    ar("incarnation", incarnation);
    ar("lifecycle", lifecycle);
    ar("generations", generations);
    ar("attempt", attempt);
    ar("issued_at", issued_at);
    ar("validity", validity);
    ar("consumed", consumed);
    ar("consumed_at", consumed_at);
    ar("last_result", last_result);
    ar("deferred_reports", deferred_reports);
    ar("commit", commit);
  }
};

/// The fencing value of a grant: a digest over the binding fields only. A
/// caller returns it with the activation result, which proves it is acting on
/// the exact grant that was issued rather than a doctored one.
[[nodiscard]] Digest compute_authority_binding(const ActivationAuthority& authority);

/// Revalidate a grant against live state immediately before use. Detects a
/// tampered binding, a superseded candidate revision, a moved lifecycle
/// generation, advanced facility generations, expiry and prior consumption.
[[nodiscard]] Status validate_authority(const ActivationAuthority& authority,
                                        const CommissioningCandidate& candidate,
                                        const FacilityGenerations& current,
                                        Timestamp now);

inline constexpr Duration kDefaultAuthorityValidity = Duration::from_seconds(300);

}  // namespace cxf

#endif  // CXF_RUNTIME_AUTHORITY_HPP
