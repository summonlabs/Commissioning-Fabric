// Commissioning Fabric - durable record vocabulary.
//
// The store is a sequence of whole-record snapshots: each record kind names a
// value that fully replaces the previous value of the same identity. Replay is
// therefore idempotent by construction, and a partially written record can never
// merge with an existing one.
#ifndef CXF_MODEL_RECORD_HPP
#define CXF_MODEL_RECORD_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cxf/model/candidate.hpp"
#include "cxf/support/clock.hpp"
#include "cxf/support/digest.hpp"
#include "cxf/support/status.hpp"
#include "cxf/types/enums.hpp"
#include "cxf/types/generations.hpp"
#include "cxf/types/ids.hpp"

namespace cxf {

/// Kinds of record the durable store holds. Values are part of the format.
enum class RecordKind : std::uint16_t {
  kFabricMeta = 1,
  kFacility = 2,
  kCandidate = 3,
  kEvidence = 4,
  kPlan = 5,
  kAuthority = 6,
  kRequestResult = 7,
  kEvent = 8,
  kIdAllocators = 9,
};

inline constexpr std::size_t kRecordKindCount = 9;

[[nodiscard]] std::string_view record_kind_name(RecordKind kind) noexcept;
[[nodiscard]] Outcome<RecordKind> parse_record_kind(std::string_view text);
[[nodiscard]] constexpr bool enum_value_valid(RecordKind kind) noexcept {
  return static_cast<std::uint16_t>(kind) >= 1 && static_cast<std::uint16_t>(kind) <= 9;
}

/// Durable fabric metadata: the identity of the store incarnation and the
/// commit position that has been published.
struct FabricMeta {
  FormatVersion format{};
  ControlEpoch epoch{};
  IncarnationId incarnation{};
  CommitSequence commit{};
  Timestamp created_at{};
  Timestamp updated_at{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("format", format);
    ar("epoch", epoch);
    ar("incarnation", incarnation);
    ar("commit", commit);
    ar("created_at", created_at);
    ar("updated_at", updated_at);
  }
};

/// Identity allocators, persisted so that a recovered process never reissues an
/// identity that an earlier incarnation already handed out.
struct IdAllocators {
  CandidateId candidate{};
  EvidenceId evidence{};
  AttemptId attempt{};
  PlanId plan{};
  TokenId token{};
  RequestId request{};
  ChangeId change{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("candidate", candidate);
    ar("evidence", evidence);
    ar("attempt", attempt);
    ar("plan", plan);
    ar("token", token);
    ar("request", request);
    ar("change", change);
  }
};

/// The durable outcome of one externally meaningful request. Storing the
/// outcome is what makes a lost response replayable: a repeat of the same
/// request id with the same content returns this result again instead of
/// re-applying the mutation or rejecting it as stale.
struct RequestResult {
  RequestId request{};
  RequestKind kind{RequestKind::kAdmitCandidate};
  /// Content address of the canonical request encoding.
  Digest request_digest{};
  bool accepted{false};
  Code code{Code::kOk};
  std::string detail{};
  /// Content address of the effect the request produced (grant, report, ...).
  Digest effect_digest{};
  CommitSequence commit{};
  Timestamp recorded_at{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("request", request);
    ar("kind", kind);
    ar("request_digest", request_digest);
    ar("accepted", accepted);
    ar("code", code);
    ar("detail", detail);
    ar("effect_digest", effect_digest);
    ar("commit", commit);
    ar("recorded_at", recorded_at);
  }
};

/// One audit event. Events explain what the runtime did; they carry no
/// authority of their own.
struct EventRecord {
  CommitSequence sequence{};
  EventKind kind{EventKind::kFabricOpened};
  Timestamp at{};
  CandidateId candidate{};
  AttemptId attempt{};
  LifecycleGeneration lifecycle{};
  FacilityGenerations generations{};
  std::string detail{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("sequence", sequence);
    ar("kind", kind);
    ar("at", at);
    ar("candidate", candidate);
    ar("attempt", attempt);
    ar("lifecycle", lifecycle);
    ar("generations", generations);
    ar("detail", detail);
  }
};

/// Field checks for the metadata records. A store whose metadata does not
/// validate is reported as corrupt rather than repaired by guessing.
[[nodiscard]] Status validate_fabric_meta(const FabricMeta& meta);
[[nodiscard]] Status validate_request_result(const RequestResult& result);
[[nodiscard]] Status validate_event(const EventRecord& event);

}  // namespace cxf

#endif  // CXF_MODEL_RECORD_HPP
