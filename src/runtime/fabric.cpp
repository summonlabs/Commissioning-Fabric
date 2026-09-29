#include "cxf/runtime/fabric.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cxf/codec/archive.hpp"
#include "cxf/model/dependency.hpp"
#include "cxf/model/evidence.hpp"
#include "cxf/model/lifecycle.hpp"
#include "cxf/model/readiness.hpp"
#include "cxf/model/record.hpp"
#include "cxf/persist/journal.hpp"
#include "cxf/persist/record_io.hpp"
#include "cxf/persist/snapshot.hpp"
#include "cxf/runtime/authority.hpp"
#include "cxf/runtime/event.hpp"
#include "cxf/runtime/explain.hpp"
#include "cxf/runtime/plan.hpp"
#include "cxf/support/digest.hpp"
#include "cxf/support/fs.hpp"
#include "cxf/support/lock_file.hpp"
#include "cxf/support/serial.hpp"
#include "cxf/support/status.hpp"
#include "cxf/support/text.hpp"
#include "cxf/types/enums.hpp"
#include "cxf/types/generations.hpp"
#include "cxf/types/ids.hpp"
#include "cxf/types/timepoint.hpp"

#if defined(_WIN32)
// The two Win32 entry points used by the fault-injection path are declared
// directly: including <windows.h> would leak its macros into everything that
// consumes this translation unit, and the runtime needs nothing else from it.
extern "C" {
__declspec(dllimport) void* __stdcall GetCurrentProcess(void);
__declspec(dllimport) int __stdcall TerminateProcess(void* process, unsigned int exit_code);
}
#endif

namespace cxf {
namespace {

// ---------------------------------------------------------------------------
// Fault injection.
// ---------------------------------------------------------------------------

/// Fault injection for the durability tests: end this process at the configured
/// crash point without unwinding and without running a single destructor, so
/// crash consistency is proven with real process death rather than with a
/// simulated exception. This is the only place in the runtime that terminates
/// its own process.
[[noreturn]] void terminate_for_fault_injection() noexcept {
#if defined(_WIN32)
  static_cast<void>(TerminateProcess(GetCurrentProcess(), 97u));
#endif
  std::_Exit(97);
}

// ---------------------------------------------------------------------------
// Small value helpers.
// ---------------------------------------------------------------------------

inline constexpr std::uint64_t kMaxCounterValue = 0xFFFFFFFFFFFFFFFFull;
inline constexpr std::uint64_t kMaxJournalReadBytes = 512ull * 1024ull * 1024ull;

/// Canonical spelling of "no digest". A durable digest field must always carry
/// a canonical 64-character spelling, because a non-canonical one is written as
/// an unreadable field and would make the record undecodable.
[[nodiscard]] Digest zero_digest() {
  const std::array<std::byte, Digest::kBytes> bytes{};
  return Digest::from_bytes(std::span<const std::byte>(bytes.data(), bytes.size()));
}

/// Digest of one canonical record body.
template <typename T>
[[nodiscard]] Digest digest_of(const T& value) {
  const std::vector<std::byte> body = encode_body(value);
  DigestBuilder builder;
  builder.update(std::span<const std::byte>(body.data(), body.size()));
  return builder.finish();
}

[[nodiscard]] Status corrupt(std::string message, std::string context = {}) {
  return make_error(Code::kStorageCorrupt, std::move(message), std::move(context));
}

[[nodiscard]] std::string join(const std::vector<std::string>& parts, char separator) {
  std::string out;
  for (const std::string& part : parts) {
    if (!out.empty()) {
      out.push_back(separator);
    }
    out.append(part);
  }
  return out;
}

/// Largest prefix length of the text that is at most limit bytes and ends on a
/// code-point boundary, so truncation never produces invalid UTF-8.
[[nodiscard]] std::size_t utf8_prefix_length(std::string_view text,
                                             std::size_t limit) noexcept {
  std::size_t at = 0;
  std::size_t complete = 0;
  while (at < text.size() && at < limit) {
    const auto lead = static_cast<unsigned char>(text[at]);
    std::size_t width = 1;
    if (lead >= 0xF0u) {
      width = 4;
    } else if (lead >= 0xE0u) {
      width = 3;
    } else if (lead >= 0xC0u) {
      width = 2;
    }
    if (at + width > text.size() || at + width > limit) {
      break;
    }
    at += width;
    complete = at;
  }
  return complete;
}

/// Bound a text value for a durable detail field, marking the truncation
/// instead of hiding it.
[[nodiscard]] std::string bounded_text(std::string_view text, std::size_t limit) {
  if (limit == 0) {
    return std::string{};
  }
  if (text.size() <= limit) {
    return std::string(text);
  }
  constexpr std::string_view kMarker = "...";
  const std::size_t keep = limit > kMarker.size() ? limit - kMarker.size() : limit;
  std::string out(text.substr(0, utf8_prefix_length(text, keep)));
  if (limit > kMarker.size()) {
    out.append(kMarker);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Canonical request bodies: the durable request digest is taken over exactly
// these fields, so the same request content always maps to the same key.
// ---------------------------------------------------------------------------

struct AdmitCandidateRequestBody {
  RequestId request{};
  std::string name{};
  CandidateDeclaration declaration{};
  std::vector<DependencyRef> dependencies{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("request", request);
    ar("name", name);
    ar("declaration", declaration);
    ar("dependencies", dependencies);
  }
};

struct BindIdentityRequestBody {
  RequestId request{};
  std::string candidate{};
  EvidenceId evidence{};
  AssetIdentity identity{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("request", request);
    ar("candidate", candidate);
    ar("evidence", evidence);
    ar("identity", identity);
  }
};

struct BindPlacementRequestBody {
  RequestId request{};
  std::string candidate{};
  EvidenceId evidence{};
  PlacementBinding placement{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("request", request);
    ar("candidate", candidate);
    ar("evidence", evidence);
    ar("placement", placement);
  }
};

struct SubmitEvidenceRequestBody {
  RequestId request{};
  std::string candidate{};
  EvidenceDimension dimension{EvidenceDimension::kIdentity};
  std::string subject{};
  EvidenceVerdict verdict{EvidenceVerdict::kUnknown};
  EvidenceSource source{EvidenceSource::kDeclared};
  std::string source_name{};
  std::string detail{};
  Timestamp observed_at{};
  FreshnessWindow freshness{};
  PlacementBinding placement{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("request", request);
    ar("candidate", candidate);
    ar("dimension", dimension);
    ar("subject", subject);
    ar("verdict", verdict);
    ar("source", source);
    ar("source_name", source_name);
    ar("detail", detail);
    ar("observed_at", observed_at);
    ar("freshness", freshness);
    ar("placement", placement);
  }
};

/// The observation body as submitted: this is what makes two submissions of the
/// same content recognisable as duplicates.
struct EvidencePayloadBody {
  EvidenceDimension dimension{EvidenceDimension::kIdentity};
  std::string subject{};
  EvidenceVerdict verdict{EvidenceVerdict::kUnknown};
  EvidenceSource source{EvidenceSource::kDeclared};
  std::string source_name{};
  std::string detail{};
  Timestamp observed_at{};
  FreshnessWindow freshness{};
  PlacementBinding placement{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("dimension", dimension);
    ar("subject", subject);
    ar("verdict", verdict);
    ar("source", source);
    ar("source_name", source_name);
    ar("detail", detail);
    ar("observed_at", observed_at);
    ar("freshness", freshness);
    ar("placement", placement);
  }
};

struct EvaluateReadinessRequestBody {
  RequestId request{};
  std::string candidate{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("request", request);
    ar("candidate", candidate);
  }
};

struct AuthorizeActivationRequestBody {
  RequestId request{};
  std::string candidate{};
  PlanId plan{};
  Digest plan_digest{};
  FreshnessWindow validity{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("request", request);
    ar("candidate", candidate);
    ar("plan", plan);
    ar("plan_digest", plan_digest);
    ar("validity", validity);
  }
};

struct ReportActivationRequestBody {
  RequestId request{};
  std::string candidate{};
  TokenId token{};
  Digest binding{};
  ActivationResultKind result{ActivationResultKind::kDeferred};
  std::string detail{};
  Timestamp observed_at{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("request", request);
    ar("candidate", candidate);
    ar("token", token);
    ar("binding", binding);
    ar("result", result);
    ar("detail", detail);
    ar("observed_at", observed_at);
  }
};

struct QuarantineRequestBody {
  RequestId request{};
  std::string candidate{};
  QuarantineReason reason{QuarantineReason::kOperatorRequest};
  std::string detail{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("request", request);
    ar("candidate", candidate);
    ar("reason", reason);
    ar("detail", detail);
  }
};

struct ReleaseQuarantineRequestBody {
  RequestId request{};
  std::string candidate{};
  std::string detail{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("request", request);
    ar("candidate", candidate);
    ar("detail", detail);
  }
};

struct CancelRequestBody {
  RequestId request{};
  std::string candidate{};
  std::string detail{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("request", request);
    ar("candidate", candidate);
    ar("detail", detail);
  }
};

struct FacilityChangeRequestBody {
  RequestId request{};
  std::optional<TopologyGeneration> topology{};
  std::optional<PowerGeneration> power{};
  std::optional<CoolingGeneration> cooling{};
  std::optional<NetworkGeneration> network{};
  std::optional<PolicyGeneration> policy{};
  std::optional<DependencyGeneration> dependency{};
  std::optional<FirmwareGeneration> firmware{};
  std::optional<HardwareGeneration> hardware{};
  std::vector<DependencyFact> dependency_facts{};
  std::string detail{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("request", request);
    ar("topology", topology);
    ar("power", power);
    ar("cooling", cooling);
    ar("network", network);
    ar("policy", policy);
    ar("dependency", dependency);
    ar("firmware", firmware);
    ar("hardware", hardware);
    ar("dependency_facts", dependency_facts);
    ar("detail", detail);
  }
};

/// Content address of the effect an accepted request produced. It is uniform so
/// that two different requests can never claim the same effect, and it carries
/// the identity and the content address of whatever the request produced.
struct EffectBody {
  RequestKind kind{RequestKind::kAdmitCandidate};
  RequestId request{};
  CandidateId candidate{};
  LifecycleState state{LifecycleState::kDeclared};
  Revision revision{};
  LifecycleGeneration lifecycle{};
  IncarnationId incarnation{};
  std::uint64_t primary{0};
  Digest content{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("kind", kind);
    ar("request", request);
    ar("candidate", candidate);
    ar("state", state);
    ar("revision", revision);
    ar("lifecycle", lifecycle);
    ar("incarnation", incarnation);
    ar("primary", primary);
    ar("content", content);
  }
};

[[nodiscard]] Digest effect_digest_of(RequestKind kind, RequestId request, CandidateId candidate,
                                      LifecycleState state, Revision revision,
                                      LifecycleGeneration lifecycle, IncarnationId incarnation,
                                      std::uint64_t primary, const Digest& content) {
  EffectBody body;
  body.kind = kind;
  body.request = request;
  body.candidate = candidate;
  body.state = state;
  body.revision = revision;
  body.lifecycle = lifecycle;
  body.incarnation = incarnation;
  body.primary = primary;
  body.content = content;
  return digest_of(body);
}

// ---------------------------------------------------------------------------
// Domain comparisons and checks.
// ---------------------------------------------------------------------------

[[nodiscard]] bool same_identity(const AssetIdentity& a, const AssetIdentity& b) {
  return a.asset == b.asset && a.registry_name == b.registry_name && a.model == b.model &&
         a.serial == b.serial && a.hardware == b.hardware && a.firmware == b.firmware;
}

[[nodiscard]] bool same_placement(const PlacementBinding& a, const PlacementBinding& b) {
  return a.site == b.site && a.rack == b.rack && a.position == b.position &&
         a.topology == b.topology;
}

[[nodiscard]] bool same_fact(const DependencyFact& a, const DependencyFact& b) {
  return a.kind == b.kind && a.name == b.name && a.generation == b.generation;
}

[[nodiscard]] bool same_facts(const std::vector<DependencyFact>& a,
                              const std::vector<DependencyFact>& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (!same_fact(a[i], b[i])) {
      return false;
    }
  }
  return true;
}

/// Dependency facts are held in a fixed (kind, name) order so that neither the
/// merge nor the rendering depends on the order a request happened to use.
[[nodiscard]] bool fact_before(const DependencyFact& a, const DependencyFact& b) {
  if (a.kind != b.kind) {
    return a.kind < b.kind;
  }
  return a.name < b.name;
}

/// True when a commissioned asset already carries the declared registry name or
/// serial, in either its declaration or its bound identity.
[[nodiscard]] bool declared_asset_conflicts(const CandidateDeclaration& declared,
                                            const CommissioningCandidate& existing) {
  if (!declared.registry_name.empty() &&
      (existing.declaration.registry_name == declared.registry_name ||
       (existing.identity.has_value() &&
        existing.identity->registry_name == declared.registry_name))) {
    return true;
  }
  if (!declared.serial.empty() &&
      (existing.declaration.serial == declared.serial ||
       (existing.identity.has_value() && existing.identity->serial == declared.serial))) {
    return true;
  }
  return false;
}

/// The evidence discipline shared by the binding mutations: the record must
/// exist, address this candidate and dimension, assert the condition, carry at
/// least imported provenance, and it must still be live.
[[nodiscard]] Status require_live_evidence(const EvidenceRecord* record, CandidateId candidate,
                                           EvidenceDimension dimension,
                                           const FacilityGenerations& current, Timestamp now) {
  if (record == nullptr || record->candidate != candidate || record->dimension != dimension ||
      record->verdict != EvidenceVerdict::kSatisfied ||
      source_authority(record->source) < source_authority(EvidenceSource::kImported)) {
    return make_error(Code::kEvidenceMissing,
                      "no live satisfied attestation of the required dimension was supplied",
                      "candidate=" + candidate.str() + " dimension=" +
                          std::string(dimension_name(dimension)));
  }
  const Liveness liveness = liveness_of(*record, current, now);
  if (liveness == Liveness::kExpired) {
    return make_error(Code::kEvidenceStale, "the supplied attestation is no longer live",
                      "evidence=" + record->id.str());
  }
  if (liveness == Liveness::kGenerationMismatch) {
    return make_error(Code::kEvidenceGenerationMismatch,
                      "the supplied attestation was observed under other generations",
                      "evidence=" + record->id.str() + " moved=" +
                          join(record->generations.differences(current), ','));
  }
  return Status::success();
}

/// A wholly zero placement means "not applicable to this observation" (the
/// same rule the evidence validator applies); anything else must be a complete
/// binding.
/// A state the runtime treats as closed to further decisions. Quarantine is
/// deliberately not one of them: it is an operator decision that can be
/// released, superseded by another quarantine, or ended by cancellation, which
/// is exactly what the state machine permits.
[[nodiscard]] bool is_closed_state(LifecycleState state) noexcept {
  return state == LifecycleState::kCommissioned || state == LifecycleState::kFailed ||
         state == LifecycleState::kCancelled;
}

[[nodiscard]] bool placement_supplied(const PlacementBinding& placement) {
  return placement.site.value() != 0 || placement.rack.value() != 0;
}

[[nodiscard]] std::vector<DimensionStatus> dimension_statuses(const ReadinessReport& report) {
  std::vector<DimensionStatus> out;
  for (const EvidenceDimension dimension : all_dimensions()) {
    for (const DimensionEvaluation& evaluation : report.dimensions) {
      if (evaluation.dimension != dimension) {
        continue;
      }
      DimensionStatus status;
      status.dimension = evaluation.dimension;
      status.required = evaluation.required;
      status.verdict = evaluation.verdict;
      status.explanation = evaluation.explanation;
      out.push_back(std::move(status));
      break;
    }
  }
  return out;
}

[[nodiscard]] std::vector<std::string> blocker_lines(const ReadinessReport& report) {
  std::vector<std::string> out;
  for (const DimensionEvaluation* blocker : blocking_dimensions(report)) {
    out.push_back(std::string(dimension_name(blocker->dimension)) + ": " + blocker->explanation);
  }
  return out;
}

[[nodiscard]] std::string describe_authority(const ActivationAuthority& authority) {
  std::string note = "token=" + authority.id.str() + " plan=" + authority.plan.str();
  if (authority.validity.allows_all_time()) {
    note.append(" expires=never");
  } else {
    note.append(" expires=");
    note.append((authority.issued_at + authority.validity.window()).to_rfc3339());
  }
  return note;
}

}  // namespace

// ---------------------------------------------------------------------------
// The runtime implementation.
// ---------------------------------------------------------------------------

struct Fabric::Impl {
  FabricOptions options{};
  Clock* clock{nullptr};
  fs::Path dir{};
  fs::Path snapshot_path{};
  fs::Path journal_path{};
  fs::Path lock_path{};
  LockFile lock{};
  Journal journal{};
  SnapshotStore snapshot{fs::Path{}};
  FabricMeta meta{};
  FacilityRecord facility{};
  IdAllocators allocators{};
  std::vector<CommissioningCandidate> candidates{};
  std::vector<EvidenceRecord> evidence{};
  std::vector<ReadinessPlan> plans{};
  std::vector<ActivationAuthority> authorities{};
  std::vector<RequestResult> requests{};
  EventLog events{};
  StoreStats stats{};
  std::uint64_t commit_index{0};
  bool recovered{false};
  bool allocators_dirty{false};
  bool poisoned{false};
  std::string tail_note{};

  Impl() = default;

  struct ReplayCheck {
    bool replayed{false};
    RequestResult stored{};
  };

  // --- lookups (ascending identity order is the storage order) -------------

  [[nodiscard]] const CommissioningCandidate* candidate_by_name(std::string_view name) const {
    for (const CommissioningCandidate& candidate : candidates) {
      if (candidate.name == name) {
        return &candidate;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const EvidenceRecord* evidence_by_id(EvidenceId id) const {
    for (const EvidenceRecord& record : evidence) {
      if (record.id == id) {
        return &record;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const ReadinessPlan* plan_by_id(PlanId id) const {
    if (id.is_zero()) {
      return nullptr;
    }
    for (const ReadinessPlan& plan : plans) {
      if (plan.id == id) {
        return &plan;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const ReadinessPlan* newest_plan_for(CandidateId id) const {
    const ReadinessPlan* found = nullptr;
    for (const ReadinessPlan& plan : plans) {
      if (plan.candidate != id) {
        continue;
      }
      if (found == nullptr || plan.id > found->id) {
        found = &plan;
      }
    }
    return found;
  }

  [[nodiscard]] const ActivationAuthority* authority_by_id(TokenId id) const {
    if (id.is_zero()) {
      return nullptr;
    }
    for (const ActivationAuthority& authority : authorities) {
      if (authority.id == id) {
        return &authority;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const ActivationAuthority* newest_authority_for(CandidateId id) const {
    const ActivationAuthority* found = nullptr;
    for (const ActivationAuthority& authority : authorities) {
      if (authority.candidate != id) {
        continue;
      }
      if (found == nullptr || authority.id > found->id) {
        found = &authority;
      }
    }
    return found;
  }

  [[nodiscard]] const RequestResult* request_result(RequestId id) const {
    for (const RequestResult& stored : requests) {
      if (stored.request == id) {
        return &stored;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::vector<EvidenceRecord> evidence_for(CandidateId id) const {
    std::vector<EvidenceRecord> selected;
    for (const EvidenceRecord& record : evidence) {
      if (record.candidate == id) {
        selected.push_back(record);
      }
    }
    return selected;
  }

  [[nodiscard]] CandidateSummary summary_of(const CommissioningCandidate& candidate) const {
    CandidateSummary summary;
    summary.id = candidate.id;
    summary.name = candidate.name;
    summary.state = candidate.state;
    summary.lifecycle = candidate.lifecycle;
    summary.incarnation = candidate.incarnation;
    summary.revision = candidate.revision;
    summary.attempt = candidate.attempt.id;
    summary.quarantine_reason = candidate.quarantine_reason;
    summary.last_plan = candidate.last_plan;
    summary.updated_at = candidate.updated_at;
    return summary;
  }

  /// The record one request produced at a given commit. A replayed request
  /// carries its commit, which is what makes its effect recoverable even though
  /// the durable request result does not repeat the produced identities.
  [[nodiscard]] const ReadinessPlan* plan_at_commit(CandidateId id, CommitSequence commit) const {
    for (const ReadinessPlan& plan : plans) {
      if (plan.candidate == id && plan.commit == commit) {
        return &plan;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const ActivationAuthority* authority_at_commit(CandidateId id,
                                                               CommitSequence commit) const {
    for (const ActivationAuthority& authority : authorities) {
      if (authority.candidate == id && authority.commit == commit) {
        return &authority;
      }
    }
    return nullptr;
  }

  /// Merge one supplied generation into the facility record. A generation only
  /// ever moves forward: an older one is refused rather than absorbed.
  template <typename Tag>
  [[nodiscard]] static Status merge_generation(Generation<Tag>& current,
                                               const std::optional<Generation<Tag>>& supplied,
                                               std::string_view name,
                                               std::vector<std::string>& moved) {
    if (!supplied.has_value()) {
      return Status::success();
    }
    if (supplied->value() < current.value()) {
      return make_error(Code::kGenerationRegression,
                        "the supplied generation is older than the one already published",
                        std::string(name) + " current=" + current.str() +
                            " supplied=" + supplied->str());
    }
    if (supplied->value() > current.value()) {
      moved.push_back(std::string(name) + "=" + current.str() + "->" + supplied->str());
      current = *supplied;
    }
    return Status::success();
  }

  // --- retention-bounded merges -------------------------------------------

  void put_candidate(const CommissioningCandidate& value) {
    for (CommissioningCandidate& existing : candidates) {
      if (existing.id == value.id) {
        existing = value;
        return;
      }
    }
    auto position = candidates.begin();
    while (position != candidates.end() && position->id < value.id) {
      ++position;
    }
    candidates.insert(position, value);
  }

  void put_evidence(const EvidenceRecord& value) {
    for (EvidenceRecord& existing : evidence) {
      if (existing.id == value.id) {
        existing = value;
        return;
      }
    }
    auto position = evidence.begin();
    while (position != evidence.end() && position->id < value.id) {
      ++position;
    }
    evidence.insert(position, value);
  }

  void put_plan(const ReadinessPlan& value) {
    bool replaced = false;
    for (ReadinessPlan& existing : plans) {
      if (existing.id == value.id) {
        existing = value;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      auto position = plans.begin();
      while (position != plans.end() && position->id < value.id) {
        ++position;
      }
      plans.insert(position, value);
    }
    // The bound keeps the newest plans of one candidate. Plan identities are
    // allocated in ascending order, so the lowest identities are the oldest.
    std::size_t count = 0;
    for (const ReadinessPlan& plan : plans) {
      if (plan.candidate == value.candidate) {
        ++count;
      }
    }
    while (count > kMaxRetainedPlansPerCandidate) {
      auto oldest = plans.end();
      for (auto it = plans.begin(); it != plans.end(); ++it) {
        if (it->candidate != value.candidate) {
          continue;
        }
        if (oldest == plans.end() || it->id < oldest->id) {
          oldest = it;
        }
      }
      if (oldest == plans.end()) {
        break;
      }
      plans.erase(oldest);
      --count;
    }
  }

  void put_authority(const ActivationAuthority& value) {
    bool replaced = false;
    for (ActivationAuthority& existing : authorities) {
      if (existing.id == value.id) {
        existing = value;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      auto position = authorities.begin();
      while (position != authorities.end() && position->id < value.id) {
        ++position;
      }
      authorities.insert(position, value);
    }
    std::size_t count = 0;
    for (const ActivationAuthority& authority : authorities) {
      if (authority.candidate == value.candidate) {
        ++count;
      }
    }
    while (count > kMaxRetainedAuthoritiesPerCandidate) {
      auto oldest = authorities.end();
      for (auto it = authorities.begin(); it != authorities.end(); ++it) {
        if (it->candidate != value.candidate) {
          continue;
        }
        if (oldest == authorities.end() || it->id < oldest->id) {
          oldest = it;
        }
      }
      if (oldest == authorities.end()) {
        break;
      }
      authorities.erase(oldest);
      --count;
    }
  }

  void put_request(const RequestResult& value) {
    bool replaced = false;
    for (RequestResult& existing : requests) {
      if (existing.request == value.request) {
        existing = value;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      auto position = requests.begin();
      while (position != requests.end() && position->request < value.request) {
        ++position;
      }
      requests.insert(position, value);
    }
    // Request identities are allocated in ascending order, so dropping the
    // smallest retained identity drops the oldest result.
    while (requests.size() > kMaxRetainedRequests) {
      requests.erase(requests.begin());
    }
  }

  // --- entry construction --------------------------------------------------

  template <typename T>
  void push_entry(std::vector<GroupEntry>& entries, RecordKind kind, const T& value) const {
    GroupEntry entry;
    entry.kind = kind;
    entry.body = encode_body(value);
    entries.push_back(std::move(entry));
  }

  [[nodiscard]] EventRecord make_event(EventKind kind, CandidateId candidate, AttemptId attempt,
                                       LifecycleGeneration lifecycle, CommitSequence sequence,
                                       Timestamp at, std::string detail) const {
    EventRecord event;
    event.sequence = sequence;
    event.kind = kind;
    event.at = at;
    event.candidate = candidate;
    event.attempt = attempt;
    event.lifecycle = lifecycle;
    event.generations = facility.generations;
    event.detail = std::move(detail);
    return event;
  }

  [[nodiscard]] Status push_event(std::vector<GroupEntry>& entries,
                                  const EventRecord& event) const {
    const Status valid = validate_event(event);
    if (valid.failed()) {
      return valid;
    }
    push_entry(entries, RecordKind::kEvent, event);
    return Status::success();
  }

  // --- durability -----------------------------------------------------------

  void maybe_crash(CrashPoint point) const {
    if (options.crash_point == CrashPoint::kNone || options.crash_point != point) {
      return;
    }
    if (options.crash_at_commit != commit_index) {
      return;
    }
    terminate_for_fault_injection();
  }

  /// Remove staging files a crashed publisher left behind. A snapshot is
  /// published by renaming the staged file over the live name, so a file that
  /// still carries the staging prefix is never authoritative; it is swept once
  /// the exclusive lock is held, and a file that cannot be removed is left for
  /// the next open rather than turning into a failure to open the store.
  void sweep_staging_files() {
    constexpr std::string_view kStagingPrefix = ".cxf-stage-";
    const Outcome<std::vector<std::string>> names = fs::list_directory(dir);
    if (!names.ok()) {
      return;
    }
    for (const std::string& entry : names.value()) {
      if (entry.size() < kStagingPrefix.size() ||
          entry.compare(0, kStagingPrefix.size(), kStagingPrefix.data(),
                        kStagingPrefix.size()) != 0) {
        continue;
      }
      static_cast<void>(fs::remove_file(fs::join(dir, entry)));
    }
  }

  /// Discard every byte after the offset and rebind the writer to the truncated
  /// end. A commit that was not verified must not leave bytes behind that a
  /// later recovery could read back as an accepted commit.
  [[nodiscard]] Status restore_journal_tail(std::uint64_t offset) {
    Outcome<std::uint64_t> size = fs::file_size(journal_path);
    if (!size.ok()) {
      return size.status();
    }
    if (size.value() > offset) {
      Outcome<fs::File> file = fs::File::open_read_write(journal_path);
      if (!file.ok()) {
        return file.status();
      }
      const Status truncated = file.value().truncate(offset);
      if (truncated.failed()) {
        return truncated;
      }
      const Status flushed = file.value().flush();
      if (flushed.failed()) {
        return flushed;
      }
      const Status closed = file.value().close();
      if (closed.failed()) {
        return closed;
      }
    }
    Outcome<Journal> reopened = Journal::open(journal_path);
    if (!reopened.ok()) {
      return reopened.status();
    }
    journal = std::move(reopened.value());
    return Status::success();
  }

  [[nodiscard]] Status apply_entry(const GroupEntry& entry);

  [[nodiscard]] Status apply_snapshot_group(const CommitGroup& group) {
    // A snapshot is a whole-state record: the target state starts empty so a
    // replayed generation can never merge with a previous one.
    clear_state();
    bool found_meta = false;
    for (const GroupEntry& entry : group.entries) {
      if (entry.kind != RecordKind::kFabricMeta) {
        continue;
      }
      FabricMeta value{};
      const Status decoded = decode_body(entry.body, value);
      if (decoded.failed()) {
        return corrupt("the snapshot metadata failed to decode", decoded.message());
      }
      if (value.format.value() != kStoreFormatVersion) {
        return make_error(Code::kStorageUnsupportedFormat,
                          "the snapshot was written by an unsupported format version",
                          "format=" + value.format.str());
      }
      const Status valid = validate_fabric_meta(value);
      if (valid.failed()) {
        return corrupt("the snapshot metadata is not valid", valid.message());
      }
      if (value.incarnation.is_zero()) {
        return corrupt("the snapshot metadata carries no store incarnation");
      }
      meta = value;
      found_meta = true;
      break;
    }
    if (!found_meta) {
      return corrupt("the snapshot carries no fabric metadata");
    }
    for (const GroupEntry& entry : group.entries) {
      const Status applied = apply_entry(entry);
      if (applied.failed()) {
        return applied;
      }
    }
    if (meta.commit != group.commit) {
      return corrupt("the snapshot does not publish its own commit sequence",
                     "group=" + group.commit.str() + " metadata=" + meta.commit.str());
    }
    return Status::success();
  }

  [[nodiscard]] Status apply_journal_frames(const std::vector<DecodedFrame>& frames,
                                            bool& applied) {
    for (const DecodedFrame& frame : frames) {
      if (frame.kind != FrameKind::kCommitGroup) {
        return corrupt("the journal holds a frame that is not a commit group");
      }
      const CommitSequence sequence = frame.commit;
      if (sequence.is_zero()) {
        return corrupt("the journal holds a commit frame with sequence zero");
      }
      if (sequence <= meta.commit) {
        // Published before the snapshot: it is already part of the state.
        continue;
      }
      if (meta.commit.value() == kMaxCounterValue) {
        return corrupt("the commit sequence cannot advance any further");
      }
      if (sequence.value() != meta.commit.value() + 1u) {
        return corrupt("a commit is missing from the journal",
                       "applied=" + meta.commit.str() + " next=" + sequence.str());
      }
      Outcome<CommitGroup> group = frame.group();
      if (!group.ok()) {
        return corrupt("a journal frame did not decode", group.status().message());
      }
      if (group.value().commit != sequence) {
        return corrupt("a journal frame disagrees with the commit group it carries");
      }
      for (const GroupEntry& entry : group.value().entries) {
        const Status entry_status = apply_entry(entry);
        if (entry_status.failed()) {
          return entry_status;
        }
      }
      if (meta.commit != sequence) {
        return corrupt("a commit group did not publish its own metadata",
                       "group=" + sequence.str() + " metadata=" + meta.commit.str());
      }
      applied = true;
    }
    return Status::success();
  }

  [[nodiscard]] Status apply_snapshot_frame(const DecodedFrame& frame, bool& applied) {
    if (frame.kind != FrameKind::kSnapshot) {
      return corrupt("the snapshot file does not hold a snapshot frame");
    }
    Outcome<CommitGroup> group = frame.group();
    if (!group.ok()) {
      return corrupt("the snapshot frame did not decode", group.status().message());
    }
    if (group.value().commit != frame.commit) {
      return corrupt("the snapshot frame disagrees with the commit group it carries");
    }
    const Status applied_group = apply_snapshot_group(group.value());
    if (applied_group.failed()) {
      return applied_group;
    }
    applied = true;
    return Status::success();
  }

  [[nodiscard]] Status load_durable_state() {
    bool applied = false;
    Outcome<std::optional<DecodedFrame>> loaded =
        snapshot.load(kMaxFrameBodyBytes + kFrameHeaderBytes);
    if (!loaded.ok()) {
      return loaded.status();
    }
    if (loaded.value().has_value()) {
      const Status from_snapshot = apply_snapshot_frame(*loaded.value(), applied);
      if (from_snapshot.failed()) {
        return from_snapshot;
      }
    }
    Outcome<std::uint64_t> snapshot_size = snapshot.size();
    stats.snapshot_bytes = snapshot_size.ok() ? snapshot_size.value() : 0;

    std::vector<DecodedFrame> frames;
    std::uint64_t discarded = 0;
    std::string note;
    const Status read = journal.read_all(frames, discarded, note);
    if (read.failed()) {
      return read;
    }
    stats.discarded_tail_bytes = discarded;
    tail_note = std::move(note);
    const Status replayed = apply_journal_frames(frames, applied);
    if (replayed.failed()) {
      return replayed;
    }
    if (discarded > 0) {
      const std::uint64_t size = journal.bytes();
      const Status restored = restore_journal_tail(size > discarded ? size - discarded : 0);
      if (restored.failed()) {
        return restored;
      }
    }
    if (!applied) {
      // A store with no durable record yet is a new store: it takes its own
      // incarnation and starts before its first commit.
      meta = FabricMeta{};
      meta.format = FormatVersion::from_value(kStoreFormatVersion);
      meta.incarnation = IncarnationId::from_value(1);
      meta.epoch = ControlEpoch::from_value(1);
      meta.commit = CommitSequence{};
      meta.created_at = clock->now();
      meta.updated_at = meta.created_at;
      recovered = false;
    } else {
      recovered = true;
    }
    return Status::success();
  }

  void clear_state() {
    candidates.clear();
    evidence.clear();
    plans.clear();
    authorities.clear();
    requests.clear();
    events.reset({});
    facility = FacilityRecord{};
    allocators = IdAllocators{};
    meta = FabricMeta{};
  }

  void refresh_stats() {
    stats.candidates = candidates.size();
    stats.evidence = evidence.size();
    stats.plans = plans.size();
    stats.authorities = authorities.size();
    stats.retained_requests = requests.size();
    stats.events = events.size();
    stats.journal_bytes = journal.bytes();
    stats.commit = meta.commit;
    stats.epoch = meta.epoch;
    stats.recovered = recovered;
  }

  /// What the open did, for the caller to report. It is deliberately NOT
  /// appended to the event log: the log holds durable audit events only, so a
  /// process-local note must not appear in it with a timestamp that changes per
  /// run and a sequence that duplicates a committed one.
  [[nodiscard]] std::string open_note() const {
    std::string note = recovered ? "recovered commit=" + meta.commit.str()
                                 : std::string("created");
    if (!tail_note.empty()) {
      note.append(" discarded_tail=");
      note.append(bounded_text(tail_note, kMaxDetailBytes / 2));
    }
    return note;
  }

  [[nodiscard]] Status commit(std::vector<GroupEntry> entries) {
    if (poisoned) {
      return corrupt("the durable journal tail could not be discarded; reopen the store");
    }
    const CommitSequence sequence = meta.commit.next();
    ++commit_index;
    const Timestamp now = clock->now();

    FabricMeta next_meta = meta;
    next_meta.commit = sequence;
    next_meta.updated_at = now;
    push_entry(entries, RecordKind::kFabricMeta, next_meta);
    if (allocators_dirty) {
      push_entry(entries, RecordKind::kIdAllocators, allocators);
    }

    CommitGroup group;
    group.commit = sequence;
    group.epoch = meta.epoch;
    group.recorded_at = now;
    group.entries = std::move(entries);

    const std::vector<std::byte> body = encode_group(group);
    const std::vector<std::byte> frame = encode_frame(FrameKind::kCommitGroup, sequence, body);
    if (frame.empty()) {
      return make_error(Code::kInternalError, "the commit frame encoded to no bytes");
    }

    maybe_crash(CrashPoint::kBeforeFlush);

    const std::uint64_t offset = journal.bytes();
    Status status = journal.append(frame);
    if (status.failed()) {
      return discard_failed_commit(offset, status);
    }
    status = journal.flush();
    if (status.failed()) {
      return discard_failed_commit(offset, status);
    }

    maybe_crash(CrashPoint::kAfterFlush);

    status = journal.verify_frame_at(offset, sequence);
    if (status.failed()) {
      return discard_failed_commit(offset, status);
    }

    // Only now may anything become observable: the record is durable and has
    // been read back, so the in-memory state is advanced to match it.
    for (const GroupEntry& entry : group.entries) {
      const Status applied = apply_entry(entry);
      if (applied.failed()) {
        return make_error(Code::kInternalError,
                          "a committed record was refused by the model validators",
                          applied.message());
      }
    }
    meta.commit = sequence;
    allocators_dirty = false;

    maybe_crash(CrashPoint::kAfterPublish);

    refresh_stats();
    if (journal.bytes() > options.snapshot_threshold_bytes) {
      const Status published = checkpoint();
      if (published.failed()) {
        return published;
      }
    }
    return Status::success();
  }

  /// A commit that was not verified is not a commit: the bytes beyond the last
  /// verified prefix are removed and the failure is reported. When the tail
  /// cannot be removed the store refuses further commits, because a later
  /// recovery could otherwise replay a commit the caller was told had failed.
  [[nodiscard]] Status discard_failed_commit(std::uint64_t offset, const Status& cause) {
    const Status restored = restore_journal_tail(offset);
    if (restored.ok()) {
      refresh_stats();
      return cause;
    }
    poisoned = true;
    return make_error(cause.code(), cause.detail(),
                      "the unverified journal tail could not be discarded: " +
                          restored.message());
  }

  [[nodiscard]] Status checkpoint() {
    if (poisoned) {
      return corrupt("the durable journal tail could not be discarded; reopen the store");
    }
    std::vector<GroupEntry> entries;
    push_entry(entries, RecordKind::kFabricMeta, meta);
    push_entry(entries, RecordKind::kFacility, facility);
    for (const CommissioningCandidate& candidate : candidates) {
      push_entry(entries, RecordKind::kCandidate, candidate);
    }
    for (const EvidenceRecord& record : evidence) {
      push_entry(entries, RecordKind::kEvidence, record);
    }
    for (const ReadinessPlan& plan : plans) {
      push_entry(entries, RecordKind::kPlan, plan);
    }
    for (const ActivationAuthority& authority : authorities) {
      push_entry(entries, RecordKind::kAuthority, authority);
    }
    for (const RequestResult& stored : requests) {
      push_entry(entries, RecordKind::kRequestResult, stored);
    }
    for (const EventRecord& event : events.records()) {
      if (event.kind == EventKind::kFabricOpened) {
        continue;  // a process-local note, never part of the durable state
      }
      push_entry(entries, RecordKind::kEvent, event);
    }
    push_entry(entries, RecordKind::kIdAllocators, allocators);

    CommitGroup group;
    group.commit = meta.commit;
    group.epoch = meta.epoch;
    group.recorded_at = meta.updated_at;
    group.entries = std::move(entries);

    const std::vector<std::byte> body = encode_group(group);
    const std::vector<std::byte> frame = encode_frame(FrameKind::kSnapshot, meta.commit, body);
    if (frame.empty()) {
      return make_error(Code::kInternalError, "the snapshot frame encoded to no bytes");
    }
    const Status published = snapshot.publish(frame);
    if (published.failed()) {
      return published;
    }
    const Status reset = journal.reset();
    if (reset.failed()) {
      return reset;
    }
    Outcome<Journal> reopened = Journal::open(journal_path);
    if (!reopened.ok()) {
      return reopened.status();
    }
    journal = std::move(reopened.value());
    Outcome<std::uint64_t> size = snapshot.size();
    stats.snapshot_bytes = size.ok() ? size.value() : 0;
    refresh_stats();
    return Status::success();
  }

  // --- request plumbing -----------------------------------------------------

  /// Resolve the idempotency key of a request whose canonical body is the
  /// supplied struct: an identity the caller supplied is compared against the
  /// durable request results, and a request that arrived without one is given
  /// the next identity the allocator holds. The digest is taken over the
  /// identity the request ends up carrying, so a caller that retries with the
  /// identity the runtime reported is recognised as a replay of that request
  /// rather than as a reuse of the key. A replay is answered before any
  /// staleness check, because the recorded answer is the durable truth about
  /// that request.
  template <typename Body>
  [[nodiscard]] Status resolve_request(RequestId supplied, Body& body, Digest& digest,
                                       ReplayCheck& check) {
    if (supplied.is_zero()) {
      RequestId id = allocators.request.next();
      while (request_result(id) != nullptr) {
        if (id.exhausted()) {
          return make_error(Code::kInternalError, "the request identity space is exhausted");
        }
        id = id.next();
      }
      allocators.request = id;
      allocators_dirty = true;
      body.request = id;
    } else {
      const RequestResult* stored = request_result(supplied);
      if (stored != nullptr) {
        const Digest supplied_digest = digest_of(body);
        if (stored->request_digest != supplied_digest) {
          return make_error(Code::kIdempotencyKeyReuse,
                            "the request identity was already used with different content",
                            "request=" + supplied.str() + " stored kind=" +
                                std::string(request_kind_name(stored->kind)));
        }
        check.replayed = true;
        check.stored = *stored;
      }
    }
    digest = digest_of(body);
    return Status::success();
  }

  [[nodiscard]] Outcome<RequestOutcome> replay_outcome(const RequestResult& stored,
                                                       LifecycleState state) const {
    if (!stored.accepted) {
      return make_error(stored.code, stored.detail, "replayed request=" + stored.request.str());
    }
    RequestOutcome outcome;
    outcome.request = stored.request;
    outcome.kind = stored.kind;
    outcome.request_digest = stored.request_digest;
    outcome.effect_digest = stored.effect_digest;
    outcome.replayed = true;
    outcome.state = state;
    outcome.commit = stored.commit;
    outcome.recorded_at = stored.recorded_at;
    return outcome;
  }

  /// Store the accepted outcome and publish the mutation as one commit group.
  [[nodiscard]] Outcome<RequestOutcome> finish_request(RequestOutcome outcome,
                                                       std::vector<GroupEntry> entries,
                                                       Timestamp now) {
    const CommitSequence sequence = meta.commit.next();
    RequestResult result;
    result.request = outcome.request;
    result.kind = outcome.kind;
    result.request_digest = outcome.request_digest;
    result.accepted = true;
    result.code = Code::kOk;
    result.effect_digest = outcome.effect_digest;
    result.commit = sequence;
    result.recorded_at = now;
    push_entry(entries, RecordKind::kRequestResult, result);

    const Status committed = commit(std::move(entries));
    if (committed.failed()) {
      return committed;
    }
    outcome.commit = sequence;
    outcome.recorded_at = now;
    outcome.replayed = false;
    return outcome;
  }

  /// Store a request whose effect is durable but whose answer is an error. Such
  /// a request must be stored, because recomputing it later would not reproduce
  /// the same answer once the state it changed has moved on.
  [[nodiscard]] Status commit_rejected_effect(RequestId id, RequestKind kind, const Digest& digest,
                                              Code code, std::string detail, const Digest& effect,
                                              std::vector<GroupEntry> entries, Timestamp now) {
    const CommitSequence sequence = meta.commit.next();
    RequestResult result;
    result.request = id;
    result.kind = kind;
    result.request_digest = digest;
    result.accepted = false;
    result.code = code;
    result.detail = bounded_text(detail, kMaxDetailBytes);
    result.effect_digest = effect;
    result.commit = sequence;
    result.recorded_at = now;
    push_entry(entries, RecordKind::kRequestResult, result);

    const Status committed = commit(std::move(entries));
    if (committed.failed()) {
      return committed;
    }
    return make_error(code, result.detail,
                      "recorded request=" + id.str() + " commit=" + sequence.str());
  }

  // --- evaluation -----------------------------------------------------------

  [[nodiscard]] ReadinessReport evaluate_now(const CommissioningCandidate& candidate,
                                             Timestamp now) const {
    const DependencyResolution dependencies = resolve_dependencies(candidate, facility);
    // The enclosing class declares a member of the same name, so the model
    // function is named explicitly here.
    ReadinessReport report =
        ::cxf::evaluate_readiness(candidate, evidence_for(candidate.id), options.policy,
                                 facility.generations, dependencies, now,
                                 ObservationSequence::from_value(meta.commit.value()));
    if (report.digest.empty()) {
      report.digest = compute_report_digest(report);
    }
    return report;
  }

  /// True when the successful activation of this candidate has since been
  /// observed: an acknowledgement is not an effect, so commissioning requires
  /// health evidence the fabric accepted after the activation was reported.
  [[nodiscard]] bool activation_effect_observed(CandidateId id, Timestamp now,
                                                const std::vector<EvidenceRecord>& records) const {
    const ActivationAuthority* succeeded = nullptr;
    for (const ActivationAuthority& authority : authorities) {
      if (authority.candidate != id ||
          authority.last_result != ActivationResultKind::kSucceeded) {
        continue;
      }
      if (succeeded == nullptr || authority.id > succeeded->id) {
        succeeded = &authority;
      }
    }
    if (succeeded == nullptr) {
      return false;
    }
    for (const EvidenceRecord& record : records) {
      if (record.dimension != EvidenceDimension::kHealth) {
        continue;
      }
      if (record.observed_sequence.value() <= succeeded->commit.value()) {
        continue;
      }
      if (liveness_of(record, facility.generations, now) != Liveness::kLive) {
        continue;
      }
      return true;
    }
    return false;
  }

  /// Move a candidate to a target milestone through the state machine. A
  /// candidate already at or beyond the target keeps its state: a binding is
  /// recorded, but the lifecycle is never moved backwards.
  [[nodiscard]] Status advance_candidate(CommissioningCandidate& candidate,
                                         LifecycleState target) {
    if (candidate.state == target) {
      return Status::success();
    }
    if (is_milestone_state(candidate.state) && is_milestone_state(target) &&
        state_rank(candidate.state) > state_rank(target)) {
      return Status::success();
    }
    const Status allowed = require_transition(candidate.state, target);
    if (allowed.failed()) {
      return allowed;
    }
    candidate.state = target;
    candidate.attempt.state = target;
    candidate.lifecycle = candidate.lifecycle.next();
    return Status::success();
  }
};

// ---------------------------------------------------------------------------
// Applying one durable record.
// ---------------------------------------------------------------------------

Status Fabric::Impl::apply_entry(const GroupEntry& entry) {
  switch (entry.kind) {
    case RecordKind::kFabricMeta: {
      FabricMeta value{};
      const Status decoded = decode_body(entry.body, value);
      if (decoded.failed()) {
        return corrupt("a metadata record failed to decode", decoded.message());
      }
      if (value.format.value() != kStoreFormatVersion) {
        return make_error(Code::kStorageUnsupportedFormat,
                          "the store was written by an unsupported format version",
                          "format=" + value.format.str());
      }
      const Status valid = validate_fabric_meta(value);
      if (valid.failed()) {
        return corrupt("a metadata record is not valid", valid.message());
      }
      if (value.incarnation.is_zero()) {
        return corrupt("a metadata record carries no store incarnation");
      }
      if (!meta.incarnation.is_zero() && value.incarnation != meta.incarnation) {
        return corrupt("a metadata record belongs to a different store incarnation",
                       "record=" + value.incarnation.str() + " store=" + meta.incarnation.str());
      }
      if (value.commit < meta.commit) {
        return Status::success();  // an older generation of the same metadata
      }
      meta = value;
      return Status::success();
    }
    case RecordKind::kFacility: {
      FacilityRecord value{};
      const Status decoded = decode_body(entry.body, value);
      if (decoded.failed()) {
        return corrupt("a facility record failed to decode", decoded.message());
      }
      const FacilityGenerations& held = facility.generations;
      const FacilityGenerations& supplied = value.generations;
      const bool regression =
          supplied.topology < held.topology || supplied.power < held.power ||
          supplied.cooling < held.cooling || supplied.network < held.network ||
          supplied.policy < held.policy || supplied.dependency < held.dependency ||
          supplied.firmware < held.firmware || supplied.hardware < held.hardware ||
          supplied.lifecycle < held.lifecycle;
      if (regression) {
        return corrupt("a facility record moves a generation backwards");
      }
      for (const DependencyFact& fact : value.dependencies) {
        if (fact.name.empty()) {
          return corrupt("a dependency fact carries no name");
        }
        for (const DependencyFact& previous : facility.dependencies) {
          if (previous.kind != fact.kind || previous.name != fact.name) {
            continue;
          }
          if (fact.generation < previous.generation) {
            return corrupt("a dependency fact moves a generation backwards",
                           std::string(dependency_kind_name(fact.kind)) + " " + fact.name);
          }
        }
      }
      facility = value;
      return Status::success();
    }
    case RecordKind::kCandidate: {
      CommissioningCandidate value{};
      const Status decoded = decode_body(entry.body, value);
      if (decoded.failed()) {
        return corrupt("a candidate record failed to decode", decoded.message());
      }
      if (value.id.is_zero() || value.name.empty() || value.incarnation.is_zero() ||
          value.lifecycle.is_zero() || value.revision.is_zero() || value.attempt.id.is_zero()) {
        return corrupt("a candidate record is missing its identity or counters",
                       "id=" + value.id.str());
      }
      const Status name_valid = validate_candidate_name(value.name);
      if (name_valid.failed()) {
        return corrupt("a candidate record carries an invalid name", name_valid.message());
      }
      const Status declaration_valid = validate_declaration(value.declaration);
      if (declaration_valid.failed()) {
        return corrupt("a candidate record carries an invalid declaration",
                       declaration_valid.message());
      }
      const Status dependencies_valid = validate_dependencies(value.dependencies);
      if (dependencies_valid.failed()) {
        return corrupt("a candidate record carries invalid dependencies",
                       dependencies_valid.message());
      }
      if (value.identity.has_value()) {
        const Status identity_valid = validate_asset_identity(*value.identity);
        if (identity_valid.failed()) {
          return corrupt("a candidate record carries an invalid identity",
                         identity_valid.message());
        }
      }
      if (value.placement.has_value()) {
        const Status placement_valid = validate_placement(*value.placement);
        if (placement_valid.failed()) {
          return corrupt("a candidate record carries an invalid placement",
                         placement_valid.message());
        }
      }
      if (!value.quarantine_detail.empty()) {
        const Status detail_valid = validate_detail_text(value.quarantine_detail);
        if (detail_valid.failed()) {
          return corrupt("a candidate record carries an invalid quarantine detail",
                         detail_valid.message());
        }
      }
      if (value.last_plan_digest.empty() || value.last_readiness_digest.empty()) {
        return corrupt("a candidate record carries a non-canonical digest",
                       "id=" + value.id.str());
      }
      put_candidate(value);
      return Status::success();
    }
    case RecordKind::kEvidence: {
      EvidenceRecord value{};
      const Status decoded = decode_body(entry.body, value);
      if (decoded.failed()) {
        return corrupt("an evidence record failed to decode", decoded.message());
      }
      if (value.id.is_zero() || value.candidate.is_zero() || value.subject.empty()) {
        return corrupt("an evidence record is missing its identity", "id=" + value.id.str());
      }
      const Status valid = validate_evidence(value);
      if (valid.failed()) {
        return corrupt("an evidence record is not valid", valid.message());
      }
      put_evidence(value);
      return Status::success();
    }
    case RecordKind::kPlan: {
      ReadinessPlan value{};
      const Status decoded = decode_body(entry.body, value);
      if (decoded.failed()) {
        return corrupt("a plan record failed to decode", decoded.message());
      }
      if (value.id.is_zero() || value.candidate.is_zero() ||
          value.report.candidate != value.candidate) {
        return corrupt("a plan record is missing its identity", "id=" + value.id.str());
      }
      if (value.report_digest.empty() || value.report.digest != value.report_digest) {
        return corrupt("a plan record does not carry the digest of its report",
                       "plan=" + value.id.str());
      }
      const Digest recomputed = compute_report_digest(value.report);
      if (recomputed != value.report_digest) {
        return corrupt("a plan record carries a report that does not match its digest",
                       "plan=" + value.id.str());
      }
      put_plan(value);
      return Status::success();
    }
    case RecordKind::kAuthority: {
      ActivationAuthority value{};
      const Status decoded = decode_body(entry.body, value);
      if (decoded.failed()) {
        return corrupt("an authority record failed to decode", decoded.message());
      }
      if (value.id.is_zero() || value.candidate.is_zero() || value.plan.is_zero() ||
          value.plan_digest.empty() || value.incarnation.is_zero() ||
          value.lifecycle.is_zero()) {
        return corrupt("an authority record is missing its binding", "id=" + value.id.str());
      }
      put_authority(value);
      return Status::success();
    }
    case RecordKind::kRequestResult: {
      RequestResult value{};
      const Status decoded = decode_body(entry.body, value);
      if (decoded.failed()) {
        return corrupt("a request result failed to decode", decoded.message());
      }
      const Status valid = validate_request_result(value);
      if (valid.failed()) {
        return corrupt("a request result is not valid", valid.message());
      }
      if (value.request_digest.empty() || value.effect_digest.empty()) {
        return corrupt("a request result carries a non-canonical digest",
                       "request=" + value.request.str());
      }
      put_request(value);
      return Status::success();
    }
    case RecordKind::kEvent: {
      EventRecord value{};
      const Status decoded = decode_body(entry.body, value);
      if (decoded.failed()) {
        return corrupt("an event record failed to decode", decoded.message());
      }
      const Status valid = validate_event(value);
      if (valid.failed()) {
        return corrupt("an event record is not valid", valid.message());
      }
      // Re-applying the same group must not duplicate its events, so an event
      // byte-identical to one already held at the same sequence is absorbed.
      for (auto it = events.records().rbegin(); it != events.records().rend(); ++it) {
        if (it->sequence < value.sequence) {
          break;
        }
        if (it->sequence == value.sequence && encode_body(*it) == encode_body(value)) {
          return Status::success();
        }
      }
      events.append(value);
      return Status::success();
    }
    case RecordKind::kIdAllocators: {
      IdAllocators value{};
      const Status decoded = decode_body(entry.body, value);
      if (decoded.failed()) {
        return corrupt("an allocator record failed to decode", decoded.message());
      }
      const bool regression =
          value.candidate < allocators.candidate || value.evidence < allocators.evidence ||
          value.attempt < allocators.attempt || value.plan < allocators.plan ||
          value.token < allocators.token || value.request < allocators.request ||
          value.change < allocators.change;
      if (regression) {
        return corrupt("an allocator record moves an identity allocator backwards");
      }
      allocators = value;
      return Status::success();
    }
  }
  return corrupt("a durable record carries an unknown kind",
                 std::to_string(static_cast<std::uint16_t>(entry.kind)));
}

// ---------------------------------------------------------------------------
// Crash point vocabulary.
// ---------------------------------------------------------------------------

std::string_view crash_point_name(CrashPoint point) noexcept {
  switch (point) {
    case CrashPoint::kNone: return "none";
    case CrashPoint::kBeforeFlush: return "before_flush";
    case CrashPoint::kAfterFlush: return "after_flush";
    case CrashPoint::kAfterPublish: return "after_publish";
  }
  return "invalid";
}

Outcome<CrashPoint> parse_crash_point(std::string_view text) {
  if (text == "none") {
    return CrashPoint::kNone;
  }
  if (text == "before_flush") {
    return CrashPoint::kBeforeFlush;
  }
  if (text == "after_flush") {
    return CrashPoint::kAfterFlush;
  }
  if (text == "after_publish") {
    return CrashPoint::kAfterPublish;
  }
  return make_error(Code::kFieldOutOfRange, "unknown crash point", std::string(text));
}

// ---------------------------------------------------------------------------
// Lifetime.
// ---------------------------------------------------------------------------

Fabric::Fabric() : impl_(std::make_unique<Impl>()) {}

Fabric::Fabric(Fabric&& other) noexcept : impl_(std::move(other.impl_)) {}

Fabric& Fabric::operator=(Fabric&& other) noexcept {
  if (this != &other) {
    impl_ = std::move(other.impl_);
  }
  return *this;
}

Fabric::~Fabric() = default;

Outcome<Fabric> Fabric::open(const FabricOptions& options, Clock& clock) {
  if (options.directory.empty()) {
    return make_error(Code::kFieldMissing, "a store directory is required");
  }
  const Status path_valid = fs::validate_path(options.directory);
  if (path_valid.failed()) {
    return path_valid;
  }

  Fabric fabric;
  Impl& state = *fabric.impl_;
  state.options = options;
  state.clock = &clock;
  state.dir = options.directory;
  state.snapshot_path = fs::join(state.dir, "fabric.snapshot");
  state.journal_path = fs::join(state.dir, "fabric.journal");
  state.lock_path = fs::join(state.dir, "fabric.lock");
  state.snapshot = SnapshotStore(state.snapshot_path);

  Outcome<bool> directory_exists = fs::exists(state.dir);
  if (!directory_exists.ok()) {
    return directory_exists.status();
  }
  if (!directory_exists.value()) {
    if (!options.create_if_missing) {
      return make_error(Code::kStorageUnavailable, "the store directory does not exist",
                        state.dir);
    }
    const Status created = fs::ensure_directory(state.dir);
    if (created.failed()) {
      return created;
    }
  } else {
    Outcome<bool> is_directory = fs::is_directory(state.dir);
    if (!is_directory.ok()) {
      return is_directory.status();
    }
    if (!is_directory.value()) {
      return make_error(Code::kStorageUnavailable, "the store path is not a directory",
                        state.dir);
    }
  }

  Outcome<LockFile> lock = LockFile::acquire(state.lock_path, options.holder);
  if (!lock.ok()) {
    return lock.status();
  }
  state.lock = std::move(lock.value());

  // With the exclusive lock held no other process can be publishing, so any
  // staging file in the store directory is debris from a process that died
  // between staging and renaming.
  state.sweep_staging_files();

  Outcome<Journal> journal = Journal::open(state.journal_path);
  if (!journal.ok()) {
    return journal.status();
  }
  state.journal = std::move(journal.value());

  const Status loaded = state.load_durable_state();
  if (loaded.failed()) {
    return loaded;
  }

  // Control epoch. The durable rules are:
  //   * the epoch a commit group carries is the epoch of the process that wrote
  //     it, and the committed metadata is what the store reports until a later
  //     commit publishes a new one;
  //   * an epoch never decreases across recovery: a recovered store advances the
  //     recorded epoch before its next commit, while a store that is only ever
  //     read keeps reporting the recorded value (the advance is in memory only
  //     and is published by the next commit, never by a write of its own);
  //   * a store created by this open starts at epoch one, which is exactly the
  //     epoch its first commit will publish, because there is no earlier tenure
  //     for it to fence out.
  if (state.recovered) {
    state.meta.epoch = state.meta.epoch.next();
  }
  // The open note is reported to the caller through the store statistics and the
  // CLI log line; it is not part of the durable event log.
  static_cast<void>(state.open_note());
  state.refresh_stats();
  return fabric;
}

const FabricMeta& Fabric::meta() const noexcept { return impl_->meta; }

const FacilityRecord& Fabric::facility() const noexcept { return impl_->facility; }

const StoreStats& Fabric::stats() const noexcept { return impl_->stats; }

const FabricOptions& Fabric::options() const noexcept { return impl_->options; }

// ---------------------------------------------------------------------------
// Mutations.
// ---------------------------------------------------------------------------

Outcome<RequestOutcome> Fabric::admit_candidate(const AdmitCandidateRequest& request) {
  Impl& state = *impl_;
  AdmitCandidateRequestBody body;
  body.request = request.request;
  body.name = request.name;
  body.declaration = request.declaration;
  body.dependencies = request.dependencies;
  Digest digest{};

  if (request.name.empty()) {
    return make_error(Code::kFieldMissing, "a candidate name is required");
  }
  Status valid = validate_candidate_name(request.name);
  if (valid.failed()) {
    return valid;
  }
  valid = validate_declaration(request.declaration);
  if (valid.failed()) {
    return valid;
  }
  valid = validate_dependencies(request.dependencies);
  if (valid.failed()) {
    return valid;
  }

  Impl::ReplayCheck check;
  valid = state.resolve_request(request.request, body, digest, check);
  if (valid.failed()) {
    return valid;
  }
  const RequestId id = body.request;
  if (check.replayed) {
    const CommissioningCandidate* existing = state.candidate_by_name(request.name);
    return state.replay_outcome(check.stored,
                                existing != nullptr ? existing->state
                                                    : LifecycleState::kDeclared);
  }

  if (state.candidate_by_name(request.name) != nullptr) {
    return make_error(Code::kCandidateAlreadyExists,
                      "a candidate with that name is already recorded", request.name);
  }
  for (const CommissioningCandidate& existing : state.candidates) {
    if (existing.state != LifecycleState::kCommissioned) {
      continue;
    }
    if (declared_asset_conflicts(request.declaration, existing)) {
      return make_error(Code::kAssetAlreadyCommissioned,
                        "the declared asset is already commissioned as another candidate",
                        existing.name);
    }
  }

  const Timestamp now = state.clock->now();
  const CommitSequence sequence = state.meta.commit.next();
  CommissioningCandidate created;
  created.id = state.allocators.candidate.next();
  state.allocators.candidate = created.id;
  state.allocators_dirty = true;
  created.name = request.name;
  created.state = LifecycleState::kDeclared;
  created.lifecycle = LifecycleGeneration::from_value(1);
  created.incarnation = IncarnationId::from_value(1);
  created.revision = Revision::from_value(1);
  created.generations = state.facility.generations;
  created.declaration = request.declaration;
  // A candidate that has no plan yet still carries canonical "no digest"
  // spellings, because a durable digest field always has to be decodable.
  created.last_plan_digest = zero_digest();
  created.last_readiness_digest = zero_digest();
  created.dependencies = request.dependencies;
  created.attempt.id = state.allocators.attempt.next();
  state.allocators.attempt = created.attempt.id;
  created.attempt.generation = created.lifecycle;
  created.attempt.state = LifecycleState::kDeclared;
  created.attempt.opened_at = now;
  created.admitted_at = now;
  created.updated_at = now;
  created.last_commit = sequence;

  std::vector<GroupEntry> entries;
  state.push_entry(entries, RecordKind::kCandidate, created);
  const EventRecord admitted =
      state.make_event(EventKind::kCandidateAdmitted, created.id, created.attempt.id,
                       created.lifecycle, sequence, now,
                       "candidate=" + created.name + " id=" + created.id.str());
  valid = state.push_event(entries, admitted);
  if (valid.failed()) {
    return valid;
  }

  RequestOutcome outcome;
  outcome.request = id;
  outcome.kind = RequestKind::kAdmitCandidate;
  outcome.request_digest = digest;
  outcome.effect_digest =
      effect_digest_of(RequestKind::kAdmitCandidate, id, created.id, created.state,
                       created.revision, created.lifecycle, created.incarnation,
                       created.id.value(), zero_digest());
  outcome.state = created.state;
  return state.finish_request(outcome, std::move(entries), now);
}

Outcome<RequestOutcome> Fabric::bind_identity(const BindIdentityRequest& request) {
  Impl& state = *impl_;
  BindIdentityRequestBody body;
  body.request = request.request;
  body.candidate = request.candidate;
  body.evidence = request.evidence;
  body.identity = request.identity;
  Digest digest{};

  if (request.candidate.empty()) {
    return make_error(Code::kFieldMissing, "a candidate name is required");
  }
  Status valid = validate_asset_identity(request.identity);
  if (valid.failed()) {
    return valid;
  }

  Impl::ReplayCheck check;
  valid = state.resolve_request(request.request, body, digest, check);
  if (valid.failed()) {
    return valid;
  }
  const RequestId id = body.request;
  const CommissioningCandidate* found = state.candidate_by_name(request.candidate);
  if (check.replayed) {
    return state.replay_outcome(check.stored,
                                found != nullptr ? found->state : LifecycleState::kDeclared);
  }
  if (found == nullptr) {
    return make_error(Code::kCandidateNotFound, "no such commissioning candidate",
                      request.candidate);
  }
  // A declared or quarantined candidate may have its identity bound, and a
  // candidate further along the chain may re-attest the identity it already
  // carries: only a failed or cancelled candidate is refused.
  if (!is_milestone_state(found->state) && found->state != LifecycleState::kQuarantined) {
    return make_error(Code::kStateNotAllowed,
                      "the candidate no longer accepts an identity binding",
                      std::string(state_name(found->state)));
  }

  const Timestamp now = state.clock->now();
  const EvidenceRecord* attestation = state.evidence_by_id(request.evidence);
  valid = require_live_evidence(attestation, found->id, EvidenceDimension::kIdentity,
                                state.facility.generations, now);
  if (valid.failed()) {
    return valid;
  }
  if (found->identity.has_value() && !same_identity(*found->identity, request.identity)) {
    return make_error(Code::kAssetIdentityConflict,
                      "the candidate already carries a different bound identity",
                      "candidate=" + found->name + " asset=" + found->identity->asset.str());
  }

  const CommitSequence sequence = state.meta.commit.next();
  CommissioningCandidate updated = *found;
  const bool was_quarantined = updated.state == LifecycleState::kQuarantined;
  valid = state.advance_candidate(updated, LifecycleState::kIdentified);
  if (valid.failed()) {
    return valid;
  }
  if (was_quarantined) {
    // The record may not claim to be quarantined while its state says otherwise.
    updated.quarantine_reason = QuarantineReason::kNone;
    updated.quarantine_detail.clear();
  }
  updated.identity = request.identity;
  updated.revision = updated.revision.next();
  updated.generations = state.facility.generations;
  updated.updated_at = now;
  updated.last_commit = sequence;

  std::vector<GroupEntry> entries;
  state.push_entry(entries, RecordKind::kCandidate, updated);
  const EventRecord bound =
      state.make_event(EventKind::kIdentityBound, updated.id, updated.attempt.id,
                       updated.lifecycle, sequence, now,
                       "candidate=" + updated.name + " asset=" + request.identity.asset.str() +
                           " evidence=" + request.evidence.str());
  valid = state.push_event(entries, bound);
  if (valid.failed()) {
    return valid;
  }

  RequestOutcome outcome;
  outcome.request = id;
  outcome.kind = RequestKind::kBindIdentity;
  outcome.request_digest = digest;
  outcome.effect_digest = effect_digest_of(
      RequestKind::kBindIdentity, id, updated.id, updated.state, updated.revision,
      updated.lifecycle, updated.incarnation, request.identity.asset.value(), attestation->payload);
  outcome.state = updated.state;
  return state.finish_request(outcome, std::move(entries), now);
}

Outcome<RequestOutcome> Fabric::bind_placement(const BindPlacementRequest& request) {
  Impl& state = *impl_;
  BindPlacementRequestBody body;
  body.request = request.request;
  body.candidate = request.candidate;
  body.evidence = request.evidence;
  body.placement = request.placement;
  Digest digest{};

  if (request.candidate.empty()) {
    return make_error(Code::kFieldMissing, "a candidate name is required");
  }
  Status valid = validate_placement(request.placement);
  if (valid.failed()) {
    return valid;
  }

  Impl::ReplayCheck check;
  valid = state.resolve_request(request.request, body, digest, check);
  if (valid.failed()) {
    return valid;
  }
  const RequestId id = body.request;
  const CommissioningCandidate* found = state.candidate_by_name(request.candidate);
  if (check.replayed) {
    return state.replay_outcome(check.stored,
                                found != nullptr ? found->state : LifecycleState::kDeclared);
  }
  if (found == nullptr) {
    return make_error(Code::kCandidateNotFound, "no such commissioning candidate",
                      request.candidate);
  }
  if (!is_milestone_state(found->state) ||
      state_rank(found->state) < state_rank(LifecycleState::kIdentified)) {
    return make_error(Code::kStateNotAllowed,
                      "placement can only be bound once the identity is bound",
                      std::string(state_name(found->state)));
  }

  const Timestamp now = state.clock->now();
  const EvidenceRecord* attestation = state.evidence_by_id(request.evidence);
  valid = require_live_evidence(attestation, found->id, EvidenceDimension::kPlacement,
                                state.facility.generations, now);
  if (valid.failed()) {
    return valid;
  }
  bool rebound = false;
  if (found->placement.has_value() && !same_placement(*found->placement, request.placement)) {
    // A placement may only be re-bound when the topology it was read from has
    // moved: otherwise the two bindings contradict each other.
    if (found->placement->topology == state.facility.generations.topology) {
      return make_error(Code::kFieldConflict,
                        "a different placement is already bound under the current topology",
                        "candidate=" + found->name);
    }
    rebound = true;
  }

  const CommitSequence sequence = state.meta.commit.next();
  CommissioningCandidate updated = *found;
  valid = state.advance_candidate(updated, LifecycleState::kLocated);
  if (valid.failed()) {
    return valid;
  }
  updated.placement = request.placement;
  updated.revision = updated.revision.next();
  updated.generations = state.facility.generations;
  updated.updated_at = now;
  updated.last_commit = sequence;

  std::vector<GroupEntry> entries;
  state.push_entry(entries, RecordKind::kCandidate, updated);
  std::string detail = "candidate=" + updated.name + " site=" +
                       request.placement.site.str() + " rack=" +
                       request.placement.rack.str() + " topology=" +
                       request.placement.topology.str();
  if (rebound) {
    detail.append(" rebound=true");
  }
  const EventRecord bound = state.make_event(EventKind::kPlacementBound, updated.id,
                                             updated.attempt.id, updated.lifecycle, sequence, now,
                                             bounded_text(detail, kMaxDetailBytes));
  valid = state.push_event(entries, bound);
  if (valid.failed()) {
    return valid;
  }

  RequestOutcome outcome;
  outcome.request = id;
  outcome.kind = RequestKind::kBindPlacement;
  outcome.request_digest = digest;
  outcome.effect_digest = effect_digest_of(
      RequestKind::kBindPlacement, id, updated.id, updated.state, updated.revision,
      updated.lifecycle, updated.incarnation, request.placement.rack.value(),
      attestation->payload);
  outcome.state = updated.state;
  return state.finish_request(outcome, std::move(entries), now);
}

Outcome<RequestOutcome> Fabric::submit_evidence(const SubmitEvidenceRequest& request) {
  Impl& state = *impl_;
  SubmitEvidenceRequestBody body;
  body.request = request.request;
  body.candidate = request.candidate;
  body.dimension = request.dimension;
  body.subject = request.subject;
  body.verdict = request.verdict;
  body.source = request.source;
  body.source_name = request.source_name;
  body.detail = request.detail;
  body.observed_at = request.observed_at;
  body.freshness = request.freshness;
  body.placement = request.placement;
  Digest digest{};

  if (request.candidate.empty()) {
    return make_error(Code::kFieldMissing, "a candidate name is required");
  }
  if (!enum_value_valid(request.dimension)) {
    return make_error(Code::kEvidenceUnknownDimension, "the evidence dimension is not declared");
  }
  if (!enum_value_valid(request.verdict) || !enum_value_valid(request.source)) {
    return make_error(Code::kFieldOutOfRange, "the evidence verdict or source is not declared");
  }
  if (request.subject.empty()) {
    return make_error(Code::kFieldMissing, "an evidence subject is required");
  }
  if (request.subject.size() > kMaxTextFieldBytes || request.source_name.size() > kMaxTextFieldBytes) {
    return make_error(Code::kFieldTooLong, "an evidence text field exceeds its bound");
  }
  Status valid;
  if (!request.detail.empty()) {
    valid = validate_detail_text(request.detail);
    if (valid.failed()) {
      return valid;
    }
  }
  if (placement_supplied(request.placement)) {
    valid = validate_placement(request.placement);
    if (valid.failed()) {
      return valid;
    }
  }

  Impl::ReplayCheck check;
  valid = state.resolve_request(request.request, body, digest, check);
  if (valid.failed()) {
    return valid;
  }
  const RequestId id = body.request;
  const CommissioningCandidate* found = state.candidate_by_name(request.candidate);
  if (check.replayed) {
    return state.replay_outcome(check.stored,
                                found != nullptr ? found->state : LifecycleState::kDeclared);
  }
  if (found == nullptr) {
    return make_error(Code::kCandidateNotFound, "no such commissioning candidate",
                      request.candidate);
  }
  if (is_closed_state(found->state)) {
    return make_error(Code::kCandidateTerminal, "the candidate no longer accepts evidence",
                      std::string(state_name(found->state)));
  }

  const Timestamp now = state.clock->now();
  const std::vector<EvidenceRecord> held = state.evidence_for(found->id);
  if (held.size() >= kMaxEvidencePerCandidate) {
    return make_error(Code::kFieldTooLong,
                      "the candidate has reached the evidence bound for one candidate",
                      "candidate=" + found->name);
  }

  EvidencePayloadBody payload_body;
  payload_body.dimension = request.dimension;
  payload_body.subject = request.subject;
  payload_body.verdict = request.verdict;
  payload_body.source = request.source;
  payload_body.source_name = request.source_name;
  payload_body.detail = request.detail;
  payload_body.observed_at = request.observed_at;
  payload_body.freshness = request.freshness;
  payload_body.placement = request.placement;
  const Digest payload = digest_of(payload_body);

  for (const EvidenceRecord& record : held) {
    if (record.dimension == request.dimension && record.subject == request.subject &&
        record.payload == payload) {
      return make_error(Code::kEvidenceDuplicateDigest,
                        "identical evidence content is already recorded",
                        "evidence=" + record.id.str());
    }
  }

  // Contradictory live evidence is not resolved by preferring one record: the
  // observation is recorded and the candidate is quarantined.
  if (source_authority(request.source) >= source_authority(EvidenceSource::kImported)) {
    for (const EvidenceRecord& record : held) {
      if (record.dimension != request.dimension || record.subject != request.subject) {
        continue;
      }
      if (source_authority(record.source) < source_authority(EvidenceSource::kImported)) {
        continue;
      }
      if (liveness_of(record, state.facility.generations, now) != Liveness::kLive) {
        continue;
      }
      const bool opposed =
          (request.verdict == EvidenceVerdict::kSatisfied &&
           record.verdict == EvidenceVerdict::kUnsatisfied) ||
          (request.verdict == EvidenceVerdict::kUnsatisfied &&
           record.verdict == EvidenceVerdict::kSatisfied);
      if (!opposed) {
        continue;
      }
      const CommitSequence sequence = state.meta.commit.next();
      EvidenceRecord contradiction;
      contradiction.id = state.allocators.evidence.next();
      state.allocators.evidence = contradiction.id;
      state.allocators_dirty = true;
      contradiction.candidate = found->id;
      contradiction.attempt = found->attempt.id;
      contradiction.dimension = request.dimension;
      contradiction.subject = request.subject;
      contradiction.verdict = request.verdict;
      contradiction.source = request.source;
      contradiction.source_name = request.source_name;
      contradiction.detail = request.detail;
      contradiction.observed_at = request.observed_at;
      contradiction.freshness = request.freshness;
      contradiction.observed_sequence = ObservationSequence::from_value(sequence.value());
      contradiction.generations = state.facility.generations;
      contradiction.placement = request.placement;
      contradiction.payload = payload;
      contradiction.commit = sequence;
      if (request.observed_at > now) {
        return make_error(Code::kFieldOutOfRange,
                          "evidence cannot be observed in the future",
                          "observed_at=" + request.observed_at.to_rfc3339());
      }
      valid = validate_evidence(contradiction);
      if (valid.failed()) {
        return valid;
      }

      CommissioningCandidate quarantined = *found;
      quarantined.state = LifecycleState::kQuarantined;
      quarantined.lifecycle = quarantined.lifecycle.next();
      quarantined.revision = quarantined.revision.next();
      quarantined.quarantine_reason = QuarantineReason::kContradictoryEvidence;
      quarantined.quarantine_detail =
          bounded_text("live evidence " + record.id.str() + " and " + contradiction.id.str() +
                           " contradict each other on " +
                           std::string(dimension_name(request.dimension)) + " subject=" +
                           bounded_text(request.subject, kMaxNameBytes),
                       kMaxDetailBytes);
      quarantined.attempt.state = LifecycleState::kQuarantined;
      quarantined.generations = state.facility.generations;
      quarantined.updated_at = now;
      quarantined.last_commit = sequence;

      std::vector<GroupEntry> entries;
      state.push_entry(entries, RecordKind::kEvidence, contradiction);
      state.push_entry(entries, RecordKind::kCandidate, quarantined);
      const EventRecord recorded =
          state.make_event(EventKind::kEvidenceRecorded, quarantined.id, quarantined.attempt.id,
                           quarantined.lifecycle, sequence, now,
                           bounded_text("evidence=" + contradiction.id.str() + " dimension=" +
                                            std::string(dimension_name(request.dimension)) +
                                            " verdict=" +
                                            std::string(verdict_name(request.verdict)) +
                                            " subject=" +
                                            bounded_text(request.subject, kMaxNameBytes) +
                                            " contradictory_with=" + record.id.str(),
                                        kMaxDetailBytes));
      valid = state.push_event(entries, recorded);
      if (valid.failed()) {
        return valid;
      }
      const Digest effect = effect_digest_of(
          RequestKind::kSubmitEvidence, id, quarantined.id, quarantined.state,
          quarantined.revision, quarantined.lifecycle, quarantined.incarnation,
          contradiction.id.value(), payload);
      return state.commit_rejected_effect(
          id, RequestKind::kSubmitEvidence, digest, Code::kEvidenceContradictory,
          "live evidence " + record.id.str() + " contradicts evidence " +
              contradiction.id.str() + " for dimension " +
              std::string(dimension_name(request.dimension)),
          effect, std::move(entries), now);
    }
  }

  if (request.observed_at > now) {
    return make_error(Code::kFieldOutOfRange, "evidence cannot be observed in the future",
                      "observed_at=" + request.observed_at.to_rfc3339());
  }

  const CommitSequence sequence = state.meta.commit.next();
  EvidenceRecord record;
  record.id = state.allocators.evidence.next();
  state.allocators.evidence = record.id;
  state.allocators_dirty = true;
  record.candidate = found->id;
  record.attempt = found->attempt.id;
  record.dimension = request.dimension;
  record.subject = request.subject;
  record.verdict = request.verdict;
  record.source = request.source;
  record.source_name = request.source_name;
  record.detail = request.detail;
  record.observed_at = request.observed_at;
  // A zero freshness window is the request's "not supplied" spelling; the
  // configured default is what the operator asked the runtime to apply.
  record.freshness = request.freshness == FreshnessWindow::of(Duration{})
                         ? state.options.default_evidence_freshness
                         : request.freshness;
  record.observed_sequence = ObservationSequence::from_value(sequence.value());
  record.generations = state.facility.generations;
  record.placement = request.placement;
  record.payload = payload;
  record.commit = sequence;
  valid = validate_evidence(record);
  if (valid.failed()) {
    return valid;
  }

  std::vector<GroupEntry> entries;
  state.push_entry(entries, RecordKind::kEvidence, record);
  const EventRecord recorded =
      state.make_event(EventKind::kEvidenceRecorded, record.candidate, record.attempt,
                       found->lifecycle, sequence, now,
                       bounded_text("evidence=" + record.id.str() + " dimension=" +
                                        std::string(dimension_name(record.dimension)) +
                                        " verdict=" + std::string(verdict_name(record.verdict)) +
                                        " source=" + std::string(source_name(record.source)) +
                                        " subject=" +
                                        bounded_text(record.subject, kMaxNameBytes),
                                    kMaxDetailBytes));
  valid = state.push_event(entries, recorded);
  if (valid.failed()) {
    return valid;
  }

  RequestOutcome outcome;
  outcome.request = id;
  outcome.kind = RequestKind::kSubmitEvidence;
  outcome.request_digest = digest;
  outcome.effect_digest = effect_digest_of(
      RequestKind::kSubmitEvidence, id, record.candidate, found->state, found->revision,
      found->lifecycle, found->incarnation, record.id.value(), payload);
  outcome.evidence = record.id;
  outcome.state = found->state;
  return state.finish_request(outcome, std::move(entries), now);
}

Outcome<ReadinessResult> Fabric::evaluate_readiness(const EvaluateReadinessRequest& request) {
  Impl& state = *impl_;
  EvaluateReadinessRequestBody body;
  body.request = request.request;
  body.candidate = request.candidate;
  Digest digest{};

  if (request.candidate.empty()) {
    return make_error(Code::kFieldMissing, "a candidate name is required");
  }

  Impl::ReplayCheck check;
  Status valid = state.resolve_request(request.request, body, digest, check);
  if (valid.failed()) {
    return valid;
  }
  const RequestId id = body.request;
  const CommissioningCandidate* found = state.candidate_by_name(request.candidate);
  if (check.replayed) {
    const Outcome<RequestOutcome> outcome =
        state.replay_outcome(check.stored, found != nullptr ? found->state
                                                           : LifecycleState::kDeclared);
    if (!outcome.ok()) {
      return outcome.status();
    }
    ReadinessResult replay;
    replay.outcome = outcome.value();
    if (found != nullptr) {
      // The plan this request recorded is identified by the commit the stored
      // result names, so the replay repeats the plan digest the caller needs to
      // authorize activation.
      const ReadinessPlan* recorded = state.plan_at_commit(found->id, check.stored.commit);
      if (recorded != nullptr) {
        replay.plan = recorded->id;
        replay.plan_digest = compute_plan_digest(*recorded);
        replay.report = recorded->report;
      }
    }
    return replay;
  }
  if (found == nullptr) {
    return make_error(Code::kCandidateNotFound, "no such commissioning candidate",
                      request.candidate);
  }
  // A quarantined candidate is fenced out of commissioning until the quarantine
  // is explicitly released, so readiness is not evaluated for it; Failed,
  // Cancelled and Commissioned are closed for the same reason.
  if (is_terminal_state(found->state)) {
    return make_error(Code::kCandidateTerminal, "readiness cannot be evaluated in this state",
                      std::string(state_name(found->state)));
  }

  const Timestamp now = state.clock->now();
  const CommitSequence sequence = state.meta.commit.next();
  const std::vector<EvidenceRecord> held = state.evidence_for(found->id);
  const ReadinessReport report = state.evaluate_now(*found, now);

  CommissioningCandidate updated = *found;
  LifecycleState target = updated.state;
  if (updated.state == LifecycleState::kActivating) {
    // Commissioning is the observed effect of a successful activation, never a
    // readiness verdict on its own: the report must be ready and health must
    // have been observed after the activation was reported.
    if (report.ready && state.activation_effect_observed(updated.id, now, held)) {
      target = LifecycleState::kCommissioned;
    }
  } else if (is_milestone_state(report.furthest_state) &&
             state_rank(updated.state) < state_rank(LifecycleState::kHealthValidated) &&
             state_rank(report.furthest_state) > state_rank(updated.state)) {
    // Readiness walks the validation chain and never hands out activation
    // authority. The evaluator already stops its reachable prefix at
    // HealthValidated; the ceiling is repeated here so a report can never talk a
    // candidate past the gate that authorization is supposed to open.
    target = state_rank(report.furthest_state) > state_rank(LifecycleState::kHealthValidated)
                 ? LifecycleState::kHealthValidated
                 : report.furthest_state;
  }
  const bool moved = target != updated.state;
  if (moved) {
    valid = require_transition(updated.state, target);
    if (valid.failed()) {
      return valid;
    }
    updated.state = target;
    updated.attempt.state = target;
    updated.lifecycle = updated.lifecycle.next();
    if (target == LifecycleState::kCommissioned) {
      updated.attempt.closed_at = now;
    }
  }

  ReadinessPlan plan;
  plan.id = state.allocators.plan.next();
  state.allocators.plan = plan.id;
  state.allocators_dirty = true;
  plan.candidate = updated.id;
  plan.generations = state.facility.generations;
  plan.report = report;
  plan.report_digest = report.digest;
  plan.created_at = now;
  plan.validity = state.options.default_plan_validity;
  plan.commit = sequence;

  // The plan is bound to the version the evaluation produced, which is the
  // version an authorization may be granted against.
  updated.revision = updated.revision.next();
  updated.generations = state.facility.generations;
  updated.last_plan = plan.id;
  updated.last_readiness_digest = report.digest;
  updated.updated_at = now;
  updated.last_commit = sequence;
  plan.revision = updated.revision;
  plan.incarnation = updated.incarnation;
  plan.lifecycle = updated.lifecycle;
  updated.last_plan_digest = compute_plan_digest(plan);

  std::vector<GroupEntry> entries;
  state.push_entry(entries, RecordKind::kPlan, plan);
  state.push_entry(entries, RecordKind::kCandidate, updated);
  const EventRecord evaluated =
      state.make_event(EventKind::kReadinessEvaluated, updated.id, updated.attempt.id,
                       updated.lifecycle, sequence, now,
                       "plan=" + plan.id.str() + " ready=" + (report.ready ? "true" : "false") +
                           " furthest=" + std::string(state_name(report.furthest_state)));
  valid = state.push_event(entries, evaluated);
  if (valid.failed()) {
    return valid;
  }
  if (moved && target == LifecycleState::kCommissioned) {
    const EventRecord commissioned =
        state.make_event(EventKind::kCommissioned, updated.id, updated.attempt.id,
                         updated.lifecycle, sequence, now,
                         "plan=" + plan.id.str() + " health_observed=true");
    valid = state.push_event(entries, commissioned);
    if (valid.failed()) {
      return valid;
    }
  }

  RequestOutcome outcome;
  outcome.request = id;
  outcome.kind = RequestKind::kEvaluateReadiness;
  outcome.request_digest = digest;
  outcome.effect_digest =
      effect_digest_of(RequestKind::kEvaluateReadiness, id, updated.id, updated.state,
                       updated.revision, updated.lifecycle, updated.incarnation,
                       plan.id.value(), report.digest);
  outcome.plan = plan.id;
  outcome.state = updated.state;
  const Outcome<RequestOutcome> stored =
      state.finish_request(outcome, std::move(entries), now);
  if (!stored.ok()) {
    return stored.status();
  }
  ReadinessResult result;
  result.outcome = stored.value();
  result.plan = plan.id;
  result.plan_digest = updated.last_plan_digest;
  result.report = report;
  return result;
}

Outcome<ActivationGrant> Fabric::authorize_activation(
    const AuthorizeActivationRequest& request) {
  Impl& state = *impl_;
  AuthorizeActivationRequestBody body;
  body.request = request.request;
  body.candidate = request.candidate;
  body.plan = request.plan;
  body.plan_digest = request.plan_digest;
  body.validity = request.validity;
  Digest digest{};

  if (request.candidate.empty()) {
    return make_error(Code::kFieldMissing, "a candidate name is required");
  }

  Impl::ReplayCheck check;
  Status valid = state.resolve_request(request.request, body, digest, check);
  if (valid.failed()) {
    return valid;
  }
  const RequestId id = body.request;
  const CommissioningCandidate* found = state.candidate_by_name(request.candidate);
  if (check.replayed) {
    const Outcome<RequestOutcome> outcome =
        state.replay_outcome(check.stored, found != nullptr ? found->state
                                                           : LifecycleState::kDeclared);
    if (!outcome.ok()) {
      return outcome.status();
    }
    ActivationGrant replay;
    replay.outcome = outcome.value();
    if (found != nullptr) {
      const ActivationAuthority* recorded =
          state.authority_at_commit(found->id, check.stored.commit);
      if (recorded != nullptr) {
        replay.token = recorded->id;
        replay.plan = recorded->plan;
        replay.candidate = recorded->candidate;
        replay.binding = compute_authority_binding(*recorded);
        replay.issued_at = recorded->issued_at;
        replay.validity = recorded->validity;
      }
    }
    return replay;
  }
  if (found == nullptr) {
    return make_error(Code::kCandidateNotFound, "no such commissioning candidate",
                      request.candidate);
  }
  if (found->state != LifecycleState::kHealthValidated &&
      found->state != LifecycleState::kActivationAuthorized) {
    return make_error(Code::kStateNotAllowed,
                      "activation can only be authorized from health_validated or "
                      "activation_authorized",
                      std::string(state_name(found->state)));
  }

  const ReadinessPlan* plan = state.plan_by_id(request.plan);
  if (plan == nullptr || plan->candidate != found->id) {
    return make_error(Code::kPlanNotFound, "no such readiness plan for this candidate",
                      "plan=" + request.plan.str());
  }
  const Digest plan_digest = compute_plan_digest(*plan);
  if (plan_digest != request.plan_digest) {
    return make_error(Code::kPlanStale, "the supplied plan digest does not match the record",
                      "plan=" + plan->id.str());
  }
  const Timestamp now = state.clock->now();
  const Status usable = validate_plan_usable(*plan, *found, state.facility.generations, now);
  if (usable.failed()) {
    if (usable.code() == Code::kEvidenceMissing) {
      return usable;
    }
    return make_error(Code::kPlanStale, usable.detail(), usable.context());
  }

  const CommitSequence sequence = state.meta.commit.next();
  CommissioningCandidate updated = *found;
  if (updated.state == LifecycleState::kHealthValidated) {
    valid = require_transition(updated.state, LifecycleState::kActivationAuthorized);
    if (valid.failed()) {
      return valid;
    }
    updated.state = LifecycleState::kActivationAuthorized;
    updated.attempt.state = updated.state;
  }
  updated.lifecycle = updated.lifecycle.next();
  updated.revision = updated.revision.next();
  updated.generations = state.facility.generations;
  updated.updated_at = now;
  updated.last_commit = sequence;

  ActivationAuthority authority;
  authority.id = state.allocators.token.next();
  state.allocators.token = authority.id;
  state.allocators_dirty = true;
  authority.plan = plan->id;
  authority.plan_digest = plan_digest;
  authority.candidate = updated.id;
  // The token is bound to the version the candidate reaches when it becomes
  // authorized, which is the version a report is checked against.
  authority.revision = updated.revision;
  authority.incarnation = updated.incarnation;
  authority.lifecycle = updated.lifecycle;
  authority.generations = state.facility.generations;
  authority.attempt = updated.attempt.id;
  authority.issued_at = now;
  authority.validity =
      (request.validity.allows_all_time() || !request.validity.window().is_zero())
          ? request.validity
          : state.options.default_authority_validity;
  authority.consumed = false;
  authority.last_result = ActivationResultKind::kDeferred;  // no report yet
  authority.commit = sequence;

  std::vector<GroupEntry> entries;
  state.push_entry(entries, RecordKind::kAuthority, authority);
  state.push_entry(entries, RecordKind::kCandidate, updated);
  const EventRecord authorized =
      state.make_event(EventKind::kActivationAuthorized, updated.id, updated.attempt.id,
                       updated.lifecycle, sequence, now,
                       "token=" + authority.id.str() + " plan=" + plan->id.str() + " revision=" +
                           updated.revision.str());
  valid = state.push_event(entries, authorized);
  if (valid.failed()) {
    return valid;
  }

  RequestOutcome outcome;
  outcome.request = id;
  outcome.kind = RequestKind::kAuthorizeActivation;
  outcome.request_digest = digest;
  outcome.effect_digest =
      effect_digest_of(RequestKind::kAuthorizeActivation, id, updated.id, updated.state,
                       updated.revision, updated.lifecycle, updated.incarnation,
                       authority.id.value(), compute_authority_binding(authority));
  outcome.token = authority.id;
  outcome.plan = plan->id;
  outcome.state = updated.state;
  const Outcome<RequestOutcome> stored =
      state.finish_request(outcome, std::move(entries), now);
  if (!stored.ok()) {
    return stored.status();
  }
  ActivationGrant grant;
  grant.outcome = stored.value();
  grant.token = authority.id;
  grant.plan = plan->id;
  grant.candidate = updated.id;
  grant.binding = compute_authority_binding(authority);
  grant.issued_at = authority.issued_at;
  grant.validity = authority.validity;
  return grant;
}

Outcome<RequestOutcome> Fabric::report_activation(const ReportActivationRequest& request) {
  Impl& state = *impl_;
  ReportActivationRequestBody body;
  body.request = request.request;
  body.candidate = request.candidate;
  body.token = request.token;
  body.binding = request.binding;
  body.result = request.result;
  body.detail = request.detail;
  body.observed_at = request.observed_at;
  Digest digest{};

  if (request.candidate.empty()) {
    return make_error(Code::kFieldMissing, "a candidate name is required");
  }
  if (!enum_value_valid(request.result)) {
    return make_error(Code::kFieldOutOfRange, "the activation result is not declared");
  }
  Status valid;
  if (!request.detail.empty()) {
    valid = validate_detail_text(request.detail);
    if (valid.failed()) {
      return valid;
    }
  }

  Impl::ReplayCheck check;
  valid = state.resolve_request(request.request, body, digest, check);
  if (valid.failed()) {
    return valid;
  }
  const RequestId id = body.request;
  const CommissioningCandidate* found = state.candidate_by_name(request.candidate);
  if (check.replayed) {
    return state.replay_outcome(check.stored,
                                found != nullptr ? found->state : LifecycleState::kDeclared);
  }
  if (found == nullptr) {
    return make_error(Code::kCandidateNotFound, "no such commissioning candidate",
                      request.candidate);
  }

  const Timestamp now = state.clock->now();
  const ActivationAuthority* held = state.authority_by_id(request.token);
  if (held == nullptr || held->candidate != found->id) {
    return make_error(Code::kAuthorityTokenInvalid,
                      "no activation authority with that identity was issued for this candidate",
                      "token=" + request.token.str());
  }
  if (compute_authority_binding(*held) != request.binding) {
    return make_error(Code::kAuthorityTokenInvalid,
                      "the supplied binding is not the binding of the stored grant",
                      "token=" + request.token.str());
  }
  // Revalidation is the fence: generation movement, supersession, consumption
  // and expiry are all decided against live state here.
  valid = validate_authority(*held, *found, state.facility.generations, now);
  if (valid.failed()) {
    return valid;
  }

  const CommitSequence sequence = state.meta.commit.next();
  ActivationAuthority authority = *held;
  CommissioningCandidate updated = *found;
  EventKind kind = EventKind::kActivationReported;
  std::string detail;

  switch (request.result) {
    case ActivationResultKind::kDeferred:
      authority.last_result = ActivationResultKind::kDeferred;
      authority.deferred_reports += 1;
      detail = "result=deferred token=" + authority.id.str() + " deferred_reports=" +
               to_dec(authority.deferred_reports);
      break;
    case ActivationResultKind::kFailed:
      authority.consumed = true;
      authority.consumed_at = request.observed_at;
      authority.last_result = ActivationResultKind::kFailed;
      valid = require_transition(updated.state, LifecycleState::kFailed);
      if (valid.failed()) {
        return valid;
      }
      updated.state = LifecycleState::kFailed;
      updated.attempt.state = LifecycleState::kFailed;
      updated.attempt.closed_at = now;
      updated.lifecycle = updated.lifecycle.next();
      kind = EventKind::kFailed;
      detail = "result=failed token=" + authority.id.str() +
               " observed_at=" + request.observed_at.to_rfc3339();
      break;
    case ActivationResultKind::kSucceeded:
      authority.consumed = true;
      authority.consumed_at = request.observed_at;
      authority.last_result = ActivationResultKind::kSucceeded;
      valid = require_transition(updated.state, LifecycleState::kActivating);
      if (valid.failed()) {
        return valid;
      }
      updated.state = LifecycleState::kActivating;
      updated.attempt.state = LifecycleState::kActivating;
      updated.lifecycle = updated.lifecycle.next();
      detail = "result=succeeded token=" + authority.id.str() +
               " observed_at=" + request.observed_at.to_rfc3339();
      break;
  }
  if (!request.detail.empty()) {
    detail.append(" detail=");
    detail.append(bounded_text(request.detail, kMaxDetailBytes / 2));
  }
  authority.commit = sequence;
  updated.revision = updated.revision.next();
  updated.generations = state.facility.generations;
  updated.updated_at = now;
  updated.last_commit = sequence;

  std::vector<GroupEntry> entries;
  state.push_entry(entries, RecordKind::kAuthority, authority);
  state.push_entry(entries, RecordKind::kCandidate, updated);
  const EventRecord reported = state.make_event(kind, updated.id, updated.attempt.id,
                                                updated.lifecycle, sequence, now, detail);
  valid = state.push_event(entries, reported);
  if (valid.failed()) {
    return valid;
  }

  RequestOutcome outcome;
  outcome.request = id;
  outcome.kind = RequestKind::kReportActivation;
  outcome.request_digest = digest;
  outcome.effect_digest =
      effect_digest_of(RequestKind::kReportActivation, id, updated.id, updated.state,
                       updated.revision, updated.lifecycle, updated.incarnation,
                       authority.id.value(), compute_authority_binding(authority));
  outcome.token = authority.id;
  outcome.state = updated.state;
  return state.finish_request(outcome, std::move(entries), now);
}

Outcome<RequestOutcome> Fabric::quarantine(const QuarantineRequest& request) {
  Impl& state = *impl_;
  QuarantineRequestBody body;
  body.request = request.request;
  body.candidate = request.candidate;
  body.reason = request.reason;
  body.detail = request.detail;
  Digest digest{};

  if (request.candidate.empty()) {
    return make_error(Code::kFieldMissing, "a candidate name is required");
  }
  if (request.reason == QuarantineReason::kNone || !enum_value_valid(request.reason)) {
    return make_error(Code::kFieldOutOfRange, "a quarantine reason must be supplied");
  }
  Status valid;
  if (!request.detail.empty()) {
    valid = validate_detail_text(request.detail);
    if (valid.failed()) {
      return valid;
    }
  }

  Impl::ReplayCheck check;
  valid = state.resolve_request(request.request, body, digest, check);
  if (valid.failed()) {
    return valid;
  }
  const RequestId id = body.request;
  const CommissioningCandidate* found = state.candidate_by_name(request.candidate);
  if (check.replayed) {
    return state.replay_outcome(check.stored,
                                found != nullptr ? found->state : LifecycleState::kDeclared);
  }
  if (found == nullptr) {
    return make_error(Code::kCandidateNotFound, "no such commissioning candidate",
                      request.candidate);
  }
  if (is_closed_state(found->state)) {
    return make_error(Code::kCandidateTerminal, "the candidate is already in a terminal state",
                      std::string(state_name(found->state)));
  }

  const Timestamp now = state.clock->now();
  const CommitSequence sequence = state.meta.commit.next();
  CommissioningCandidate updated = *found;
  updated.state = LifecycleState::kQuarantined;
  updated.lifecycle = updated.lifecycle.next();
  updated.revision = updated.revision.next();
  updated.quarantine_reason = request.reason;
  updated.quarantine_detail = request.detail;
  updated.attempt.state = LifecycleState::kQuarantined;
  updated.generations = state.facility.generations;
  updated.updated_at = now;
  updated.last_commit = sequence;

  std::vector<GroupEntry> entries;
  state.push_entry(entries, RecordKind::kCandidate, updated);
  const EventRecord quarantined = state.make_event(
      EventKind::kQuarantined, updated.id, updated.attempt.id, updated.lifecycle, sequence, now,
      bounded_text("reason=" + std::string(quarantine_reason_name(request.reason)) + " detail=" +
                       bounded_text(request.detail, kMaxDetailBytes / 2),
                   kMaxDetailBytes));
  valid = state.push_event(entries, quarantined);
  if (valid.failed()) {
    return valid;
  }

  RequestOutcome outcome;
  outcome.request = id;
  outcome.kind = RequestKind::kQuarantine;
  outcome.request_digest = digest;
  outcome.effect_digest =
      effect_digest_of(RequestKind::kQuarantine, id, updated.id, updated.state,
                       updated.revision, updated.lifecycle, updated.incarnation,
                       static_cast<std::uint64_t>(request.reason), zero_digest());
  outcome.state = updated.state;
  return state.finish_request(outcome, std::move(entries), now);
}

Outcome<RequestOutcome> Fabric::release_quarantine(const ReleaseQuarantineRequest& request) {
  Impl& state = *impl_;
  ReleaseQuarantineRequestBody body;
  body.request = request.request;
  body.candidate = request.candidate;
  body.detail = request.detail;
  Digest digest{};

  if (request.candidate.empty()) {
    return make_error(Code::kFieldMissing, "a candidate name is required");
  }
  Status valid;
  if (!request.detail.empty()) {
    valid = validate_detail_text(request.detail);
    if (valid.failed()) {
      return valid;
    }
  }

  Impl::ReplayCheck check;
  valid = state.resolve_request(request.request, body, digest, check);
  if (valid.failed()) {
    return valid;
  }
  const RequestId id = body.request;
  const CommissioningCandidate* found = state.candidate_by_name(request.candidate);
  if (check.replayed) {
    return state.replay_outcome(check.stored,
                                found != nullptr ? found->state : LifecycleState::kDeclared);
  }
  if (found == nullptr) {
    return make_error(Code::kCandidateNotFound, "no such commissioning candidate",
                      request.candidate);
  }
  if (found->state != LifecycleState::kQuarantined) {
    return make_error(Code::kStateNotAllowed, "the candidate is not quarantined",
                      std::string(state_name(found->state)));
  }

  const Timestamp now = state.clock->now();
  const CommitSequence sequence = state.meta.commit.next();
  CommissioningCandidate updated = *found;
  Attempt closed = updated.attempt;
  if (closed.closed_at == Timestamp{}) {
    closed.closed_at = now;
  }
  if (closed.note.empty() && !request.detail.empty()) {
    closed.note = bounded_text(request.detail, kMaxDetailBytes);
  }
  updated.history.push_back(closed);

  // Releasing quarantine opens a new attempt and a new incarnation: the earlier
  // attempt's evidence is not inherited as fresh authority.
  updated.lifecycle = updated.lifecycle.next();
  updated.incarnation = updated.incarnation.next();
  updated.attempt = Attempt{};
  updated.attempt.id = state.allocators.attempt.next();
  state.allocators.attempt = updated.attempt.id;
  state.allocators_dirty = true;
  updated.attempt.generation = updated.lifecycle;
  updated.attempt.state = kQuarantineReleaseState;
  updated.attempt.opened_at = now;
  updated.state = kQuarantineReleaseState;
  updated.revision = updated.revision.next();
  updated.quarantine_reason = QuarantineReason::kNone;
  updated.quarantine_detail.clear();
  updated.generations = state.facility.generations;
  updated.updated_at = now;
  updated.last_commit = sequence;

  std::vector<GroupEntry> entries;
  state.push_entry(entries, RecordKind::kCandidate, updated);
  const EventRecord released = state.make_event(
      EventKind::kQuarantineReleased, updated.id, updated.attempt.id, updated.lifecycle, sequence,
      now,
      bounded_text("attempt=" + updated.attempt.id.str() + " detail=" +
                       bounded_text(request.detail, kMaxDetailBytes / 2),
                   kMaxDetailBytes));
  valid = state.push_event(entries, released);
  if (valid.failed()) {
    return valid;
  }
  const EventRecord opened =
      state.make_event(EventKind::kAttemptOpened, updated.id, updated.attempt.id,
                       updated.lifecycle, sequence, now,
                       "incarnation=" + updated.incarnation.str() + " attempt=" +
                           updated.attempt.id.str());
  valid = state.push_event(entries, opened);
  if (valid.failed()) {
    return valid;
  }

  RequestOutcome outcome;
  outcome.request = id;
  outcome.kind = RequestKind::kReleaseQuarantine;
  outcome.request_digest = digest;
  outcome.effect_digest =
      effect_digest_of(RequestKind::kReleaseQuarantine, id, updated.id, updated.state,
                       updated.revision, updated.lifecycle, updated.incarnation,
                       updated.attempt.id.value(), zero_digest());
  outcome.state = updated.state;
  return state.finish_request(outcome, std::move(entries), now);
}

Outcome<RequestOutcome> Fabric::cancel(const CancelRequest& request) {
  Impl& state = *impl_;
  CancelRequestBody body;
  body.request = request.request;
  body.candidate = request.candidate;
  body.detail = request.detail;
  Digest digest{};

  if (request.candidate.empty()) {
    return make_error(Code::kFieldMissing, "a candidate name is required");
  }
  Status valid;
  if (!request.detail.empty()) {
    valid = validate_detail_text(request.detail);
    if (valid.failed()) {
      return valid;
    }
  }

  Impl::ReplayCheck check;
  valid = state.resolve_request(request.request, body, digest, check);
  if (valid.failed()) {
    return valid;
  }
  const RequestId id = body.request;
  const CommissioningCandidate* found = state.candidate_by_name(request.candidate);
  if (check.replayed) {
    return state.replay_outcome(check.stored,
                                found != nullptr ? found->state : LifecycleState::kDeclared);
  }
  if (found == nullptr) {
    return make_error(Code::kCandidateNotFound, "no such commissioning candidate",
                      request.candidate);
  }
  if (is_closed_state(found->state)) {
    return make_error(Code::kCandidateTerminal, "the candidate is already in a terminal state",
                      std::string(state_name(found->state)));
  }

  const Timestamp now = state.clock->now();
  const CommitSequence sequence = state.meta.commit.next();
  CommissioningCandidate updated = *found;
  valid = require_transition(updated.state, LifecycleState::kCancelled);
  if (valid.failed()) {
    return valid;
  }
  updated.state = LifecycleState::kCancelled;
  updated.attempt.state = LifecycleState::kCancelled;
  updated.attempt.closed_at = now;
  updated.lifecycle = updated.lifecycle.next();
  updated.revision = updated.revision.next();
  updated.generations = state.facility.generations;
  updated.updated_at = now;
  updated.last_commit = sequence;

  std::vector<GroupEntry> entries;
  state.push_entry(entries, RecordKind::kCandidate, updated);
  const EventRecord cancelled = state.make_event(
      EventKind::kCancelled, updated.id, updated.attempt.id, updated.lifecycle, sequence, now,
      bounded_text("detail=" + bounded_text(request.detail, kMaxDetailBytes / 2),
                   kMaxDetailBytes));
  valid = state.push_event(entries, cancelled);
  if (valid.failed()) {
    return valid;
  }

  RequestOutcome outcome;
  outcome.request = id;
  outcome.kind = RequestKind::kCancel;
  outcome.request_digest = digest;
  outcome.effect_digest =
      effect_digest_of(RequestKind::kCancel, id, updated.id, updated.state, updated.revision,
                       updated.lifecycle, updated.incarnation, updated.id.value(), zero_digest());
  outcome.state = updated.state;
  return state.finish_request(outcome, std::move(entries), now);
}

Outcome<RequestOutcome> Fabric::record_facility_change(const FacilityChangeRequest& request) {
  Impl& state = *impl_;
  FacilityChangeRequestBody body;
  body.request = request.request;
  body.topology = request.topology;
  body.power = request.power;
  body.cooling = request.cooling;
  body.network = request.network;
  body.policy = request.policy;
  body.dependency = request.dependency;
  body.firmware = request.firmware;
  body.hardware = request.hardware;
  body.dependency_facts = request.dependency_facts;
  body.detail = request.detail;
  Digest digest{};

  Status valid;
  if (!request.detail.empty()) {
    valid = validate_detail_text(request.detail);
    if (valid.failed()) {
      return valid;
    }
  }
  for (const DependencyFact& fact : request.dependency_facts) {
    if (fact.name.empty()) {
      return make_error(Code::kFieldMissing, "a dependency fact needs a name");
    }
    if (fact.name.size() > kMaxTextFieldBytes) {
      return make_error(Code::kFieldTooLong, "a dependency fact name exceeds its bound");
    }
    if (!enum_value_valid(fact.kind)) {
      return make_error(Code::kFieldOutOfRange, "a dependency fact kind is not declared");
    }
  }

  Impl::ReplayCheck check;
  valid = state.resolve_request(request.request, body, digest, check);
  if (valid.failed()) {
    return valid;
  }
  const RequestId id = body.request;
  if (check.replayed) {
    return state.replay_outcome(check.stored, LifecycleState::kDeclared);
  }

  FacilityRecord merged = state.facility;
  std::vector<std::string> moved;
  valid = Impl::merge_generation(merged.generations.topology, request.topology, "topology",
                                 moved);
  if (valid.failed()) {
    return valid;
  }
  valid = Impl::merge_generation(merged.generations.power, request.power, "power", moved);
  if (valid.failed()) {
    return valid;
  }
  valid = Impl::merge_generation(merged.generations.cooling, request.cooling, "cooling", moved);
  if (valid.failed()) {
    return valid;
  }
  valid = Impl::merge_generation(merged.generations.network, request.network, "network", moved);
  if (valid.failed()) {
    return valid;
  }
  valid = Impl::merge_generation(merged.generations.policy, request.policy, "policy", moved);
  if (valid.failed()) {
    return valid;
  }
  valid =
      Impl::merge_generation(merged.generations.dependency, request.dependency, "dependency",
                             moved);
  if (valid.failed()) {
    return valid;
  }
  valid = Impl::merge_generation(merged.generations.firmware, request.firmware, "firmware",
                                 moved);
  if (valid.failed()) {
    return valid;
  }
  valid = Impl::merge_generation(merged.generations.hardware, request.hardware, "hardware",
                                 moved);
  if (valid.failed()) {
    return valid;
  }

  std::size_t facts_added = 0;
  std::size_t facts_updated = 0;
  for (const DependencyFact& fact : request.dependency_facts) {
    bool known = false;
    for (DependencyFact& existing : merged.dependencies) {
      if (existing.kind != fact.kind || existing.name != fact.name) {
        continue;
      }
      known = true;
      if (fact.generation < existing.generation) {
        return make_error(Code::kGenerationRegression,
                          "a dependency fact is older than the one already published",
                          std::string(dependency_kind_name(fact.kind)) + " " + fact.name +
                              " current=" + existing.generation.str() +
                              " supplied=" + fact.generation.str());
      }
      if (fact.generation > existing.generation) {
        existing.generation = fact.generation;
        ++facts_updated;
      }
      break;
    }
    if (known) {
      continue;
    }
    auto position = merged.dependencies.begin();
    while (position != merged.dependencies.end() && fact_before(*position, fact)) {
      ++position;
    }
    merged.dependencies.insert(position, fact);
    ++facts_added;
  }

  const bool changed = merged.generations != state.facility.generations ||
                       !same_facts(merged.dependencies, state.facility.dependencies);
  const Timestamp now = state.clock->now();
  const CommitSequence sequence = state.meta.commit.next();

  std::vector<GroupEntry> entries;
  state.push_entry(entries, RecordKind::kFacility, merged);
  std::string detail = moved.empty() ? std::string("no generation moved") : join(moved, ' ');
  if (facts_added > 0 || facts_updated > 0) {
    detail.append(" facts_added=" + to_dec(facts_added) + " facts_updated=" +
                  to_dec(facts_updated));
  }
  const EventRecord facility_changed =
      state.make_event(EventKind::kFacilityChanged, CandidateId{}, AttemptId{},
                       LifecycleGeneration{}, sequence, now,
                       bounded_text(detail, kMaxDetailBytes));
  valid = state.push_event(entries, facility_changed);
  if (valid.failed()) {
    return valid;
  }

  RequestOutcome outcome;
  outcome.request = id;
  outcome.kind = RequestKind::kRecordFacilityChange;
  outcome.request_digest = digest;
  outcome.effect_digest = effect_digest_of(
      RequestKind::kRecordFacilityChange, id, CandidateId{}, LifecycleState::kDeclared,
      Revision{}, LifecycleGeneration{}, IncarnationId{},
      merged.generations.topology.value(), zero_digest());
  outcome.facility_changed = changed;
  outcome.state = LifecycleState::kDeclared;
  return state.finish_request(outcome, std::move(entries), now);
}

// ---------------------------------------------------------------------------
// Queries.
// ---------------------------------------------------------------------------

Outcome<CandidateView> Fabric::view(std::string_view candidate_name) const {
  const Impl& state = *impl_;
  const CommissioningCandidate* found = state.candidate_by_name(candidate_name);
  if (found == nullptr) {
    return make_error(Code::kCandidateNotFound, "no such commissioning candidate",
                      std::string(candidate_name));
  }
  CandidateView result;
  result.candidate = *found;
  result.evidence = state.evidence_for(found->id);
  const ReadinessPlan* plan = state.plan_by_id(found->last_plan);
  if (plan == nullptr) {
    plan = state.newest_plan_for(found->id);
  }
  if (plan != nullptr) {
    result.plan = *plan;
  }
  const ActivationAuthority* authority = state.newest_authority_for(found->id);
  if (authority != nullptr) {
    result.authority = *authority;
  }
  result.events = state.events.for_candidate(found->id, kMaxRetainedEvents);
  return result;
}

Outcome<std::vector<CandidateSummary>> Fabric::list_candidates() const {
  const Impl& state = *impl_;
  std::vector<CandidateSummary> summaries;
  summaries.reserve(state.candidates.size());
  for (const CommissioningCandidate& candidate : state.candidates) {
    summaries.push_back(state.summary_of(candidate));
  }
  return summaries;
}

Outcome<ReadinessReport> Fabric::preview_readiness(std::string_view candidate_name) const {
  const Impl& state = *impl_;
  const CommissioningCandidate* found = state.candidate_by_name(candidate_name);
  if (found == nullptr) {
    return make_error(Code::kCandidateNotFound, "no such commissioning candidate",
                      std::string(candidate_name));
  }
  // A preview is the same evaluation as a recorded one: it reads the same
  // evidence under the same generations, and records nothing.
  return state.evaluate_now(*found, state.clock->now());
}

Outcome<StatusView> Fabric::status(std::string_view candidate_name) const {
  const Impl& state = *impl_;
  const CommissioningCandidate* found = state.candidate_by_name(candidate_name);
  if (found == nullptr) {
    return make_error(Code::kCandidateNotFound, "no such commissioning candidate",
                      std::string(candidate_name));
  }
  StatusView view;
  view.summary = state.summary_of(*found);
  view.plan = found->last_plan;
  view.readiness_digest = found->last_readiness_digest;

  const ReadinessPlan* plan = state.plan_by_id(found->last_plan);
  if (plan == nullptr) {
    plan = state.newest_plan_for(found->id);
  }
  ReadinessReport report;
  if (plan != nullptr) {
    report = plan->report;
    view.plan = plan->id;
    view.fenced_generations = state.facility.generations.differences(plan->generations);
  } else {
    report = state.evaluate_now(*found, state.clock->now());
    view.readiness_digest = report.digest;
  }
  view.ready = report.ready;
  view.furthest_state = report.furthest_state;
  view.dimensions = dimension_statuses(report);
  view.blockers = blocker_lines(report);

  if (found->state == LifecycleState::kQuarantined) {
    view.quarantine_reason = std::string(quarantine_reason_name(found->quarantine_reason));
    view.quarantine_detail = found->quarantine_detail;
  }

  const ActivationAuthority* authority = state.newest_authority_for(found->id);
  if (authority == nullptr) {
    view.authority_outstanding = false;
    view.authority_valid = false;
    view.authority_note = "no activation authority has been issued for this candidate";
  } else {
    const Status valid =
        validate_authority(*authority, *found, state.facility.generations, state.clock->now());
    view.authority_outstanding = !authority->consumed;
    view.authority_valid = valid.ok();
    view.authority_note = valid.ok() ? describe_authority(*authority) : valid.message();
  }
  return view;
}

Outcome<std::string> Fabric::explain(std::string_view candidate_name) const {
  const Outcome<StatusView> view = status(candidate_name);
  if (!view.ok()) {
    return view.status();
  }
  return render_status(view.value());
}

Outcome<std::vector<EventRecord>> Fabric::recent_events(std::size_t limit) const {
  const Impl& state = *impl_;
  std::vector<EventRecord> selected;
  const std::vector<EventRecord>& held = state.events.records();
  const std::size_t begin = held.size() > limit ? held.size() - limit : 0;
  selected.reserve(held.size() - begin);
  for (std::size_t i = begin; i < held.size(); ++i) {
    selected.push_back(held[i]);
  }
  return selected;
}

// ---------------------------------------------------------------------------
// Durability.
// ---------------------------------------------------------------------------

Status Fabric::verify_store() const {
  const Impl& state = *impl_;
  Impl scratch;
  scratch.snapshot_path = state.snapshot_path;
  scratch.snapshot = SnapshotStore(state.snapshot_path);

  bool applied = false;
  Outcome<std::optional<DecodedFrame>> loaded =
      scratch.snapshot.load(kMaxFrameBodyBytes + kFrameHeaderBytes);
  if (!loaded.ok()) {
    return loaded.status();
  }
  if (loaded.value().has_value()) {
    const Status from_snapshot = scratch.apply_snapshot_frame(*loaded.value(), applied);
    if (from_snapshot.failed()) {
      return from_snapshot;
    }
  }

  // The journal is read as bytes rather than through the live writer: a query
  // must not create, extend or reposition any file.
  Outcome<std::vector<std::byte>> bytes =
      fs::read_file(state.journal_path, kMaxJournalReadBytes);
  if (!bytes.ok()) {
    return bytes.status();
  }
  std::vector<DecodedFrame> frames;
  std::size_t offset = 0;
  const std::span<const std::byte> data(bytes.value().data(), bytes.value().size());
  while (offset < data.size()) {
    const FrameDecode decoded = decode_frame(data.subspan(offset));
    if (decoded.status == FrameStatus::kIncomplete) {
      break;  // a torn tail is not a record
    }
    if (decoded.status == FrameStatus::kInvalid) {
      return corrupt("the journal holds an invalid frame", decoded.error.message());
    }
    if (decoded.frame.bytes_consumed == 0) {
      return corrupt("a journal frame consumed no bytes");
    }
    frames.push_back(decoded.frame);
    offset += decoded.frame.bytes_consumed;
  }
  const Status replayed = scratch.apply_journal_frames(frames, applied);
  if (replayed.failed()) {
    return replayed;
  }

  if (!applied) {
    if (!state.meta.commit.is_zero()) {
      return corrupt("the store holds no durable record of the live commit",
                     "live=" + state.meta.commit.str());
    }
    return Status::success();
  }
  if (scratch.meta.commit != state.meta.commit) {
    return corrupt("the durable store disagrees with the live commit sequence",
                   "durable=" + scratch.meta.commit.str() + " live=" + state.meta.commit.str());
  }
  if (scratch.meta.incarnation != state.meta.incarnation) {
    return corrupt("the durable store belongs to another incarnation",
                   "durable=" + scratch.meta.incarnation.str() +
                       " live=" + state.meta.incarnation.str());
  }
  return Status::success();
}

Status Fabric::checkpoint() { return impl_->checkpoint(); }

}  // namespace cxf

