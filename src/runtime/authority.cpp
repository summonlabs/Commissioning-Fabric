#include "cxf/runtime/authority.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "cxf/codec/archive.hpp"
#include "cxf/support/digest.hpp"
#include "cxf/support/text.hpp"

namespace cxf {
namespace {

/// The fencing fields of a grant, and nothing else.
///
/// The digest deliberately excludes consumed, consumed_at, last_result,
/// deferred_reports and commit: recording a deferral rewrites those fields, and
/// the caller must be able to return the same binding until the token is
/// consumed.
struct AuthorityBindingBody {
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

Digest compute_authority_binding(const ActivationAuthority& authority) {
  AuthorityBindingBody body;
  body.id = authority.id;
  body.plan = authority.plan;
  body.plan_digest = authority.plan_digest;
  body.candidate = authority.candidate;
  body.revision = authority.revision;
  body.incarnation = authority.incarnation;
  body.lifecycle = authority.lifecycle;
  body.generations = authority.generations;
  body.attempt = authority.attempt;
  body.issued_at = authority.issued_at;
  body.validity = authority.validity;
  return digest_of_body(encode_body(body));
}

Status validate_authority(const ActivationAuthority& authority,
                          const CommissioningCandidate& candidate,
                          const FacilityGenerations& current, Timestamp now) {
  // Deterministic order: the checks are resolved to the lowest-numbered
  // applicable reason, so the same stale token always reports the same code.
  if (authority.candidate != candidate.id) {
    return make_error(Code::kAuthorityTokenInvalid,
                      "the token was issued for a different candidate",
                      "token candidate=" + authority.candidate.str() +
                          " candidate=" + candidate.id.str());
  }
  if (authority.incarnation != candidate.incarnation) {
    return make_error(Code::kAuthorityTokenInvalid,
                      "the token was issued for a different candidate incarnation",
                      "token incarnation=" + authority.incarnation.str() +
                          " candidate incarnation=" + candidate.incarnation.str());
  }
  if (authority.lifecycle != candidate.lifecycle) {
    return make_error(Code::kFencingTokenStale,
                      "the candidate lifecycle generation moved after the token was issued",
                      "token lifecycle=" + authority.lifecycle.str() +
                          " candidate lifecycle=" + candidate.lifecycle.str());
  }
  if (authority.generations != current) {
    return make_error(Code::kFencingTokenStale,
                      "facility generations moved after the token was issued",
                      "moved=" + join_names(authority.generations.differences(current)));
  }
  // The lifecycle generation is the strict fence; the revision only has to be a
  // revision the candidate actually reached.
  if (authority.revision > candidate.revision) {
    return make_error(Code::kAuthorityTokenInvalid,
                      "the token claims a candidate revision that was never reached",
                      "token revision=" + authority.revision.str() +
                          " candidate revision=" + candidate.revision.str());
  }
  if (authority.consumed) {
    return make_error(Code::kAuthorityTokenConsumed,
                      "the activation authority token was already consumed",
                      "token=" + authority.id.str() +
                          " consumed_at=" + authority.consumed_at.to_rfc3339());
  }
  if (authority.validity.is_expired(authority.issued_at, now)) {
    return make_error(Code::kAuthorityTokenExpired,
                      "the activation authority token is no longer valid",
                      "issued_at=" + authority.issued_at.to_rfc3339() +
                          " validity=" + authority.validity.str() +
                          " now=" + now.to_rfc3339());
  }
  return Status::success();
}

}  // namespace cxf
