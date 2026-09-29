// Commissioning Fabric - generation-bound evidence.
//
// Evidence is an observation, not an authority. Every record names the subject
// it observed, the facility generations it was observed under, when it was
// observed, and how long that observation may be trusted. A record that is
// expired or that was observed under different generations is retained for
// audit but is never counted as live readiness.
#ifndef CXF_MODEL_EVIDENCE_HPP
#define CXF_MODEL_EVIDENCE_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "cxf/model/candidate.hpp"
#include "cxf/support/clock.hpp"
#include "cxf/support/digest.hpp"
#include "cxf/support/status.hpp"
#include "cxf/types/enums.hpp"
#include "cxf/types/generations.hpp"
#include "cxf/types/ids.hpp"
#include "cxf/types/timepoint.hpp"

namespace cxf {

/// What an observation is about: one readiness dimension of one named subject
/// (for example the electrical dimension of power feed "pdu-a").
struct EvidenceKey {
  EvidenceDimension dimension{EvidenceDimension::kIdentity};
  std::string subject{};

  friend bool operator==(const EvidenceKey& a, const EvidenceKey& b) noexcept {
    return a.dimension == b.dimension && a.subject == b.subject;
  }
  friend bool operator!=(const EvidenceKey& a, const EvidenceKey& b) noexcept {
    return !(a == b);
  }
  friend bool operator<(const EvidenceKey& a, const EvidenceKey& b) noexcept {
    if (a.dimension != b.dimension) {
      return a.dimension < b.dimension;
    }
    return a.subject < b.subject;
  }

  template <typename Ar>
  void visit(Ar& ar) {
    ar("dimension", dimension);
    ar("subject", subject);
  }
};

/// Classification of a record against current facility generations and the
/// evaluator's clock. Only kLive may contribute to readiness.
enum class Liveness : std::uint8_t {
  kLive = 0,
  kExpired = 1,
  kGenerationMismatch = 2,
};

[[nodiscard]] std::string_view liveness_name(Liveness liveness) noexcept;

/// One recorded observation.
struct EvidenceRecord {
  EvidenceId id{};
  CandidateId candidate{};
  AttemptId attempt{};
  EvidenceDimension dimension{EvidenceDimension::kIdentity};
  std::string subject{};
  EvidenceVerdict verdict{EvidenceVerdict::kUnknown};
  EvidenceSource source{EvidenceSource::kDeclared};
  std::string source_name{};
  std::string detail{};
  Timestamp observed_at{};
  FreshnessWindow freshness{};
  /// Order in which the fabric accepted the observation. This is the tie-break
  /// that makes selection deterministic when two observations carry the same
  /// authority and timestamp.
  ObservationSequence observed_sequence{};
  FacilityGenerations generations{};
  /// Placement at the time of observation. Zero ids mean "not applicable to
  /// this observation", never "unknown but assumed".
  PlacementBinding placement{};
  /// Content address of the observation body as submitted.
  Digest payload{};
  CommitSequence commit{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("id", id);
    ar("candidate", candidate);
    ar("attempt", attempt);
    ar("dimension", dimension);
    ar("subject", subject);
    ar("verdict", verdict);
    ar("source", source);
    ar("source_name", source_name);
    ar("detail", detail);
    ar("observed_at", observed_at);
    ar("freshness", freshness);
    ar("observed_sequence", observed_sequence);
    ar("generations", generations);
    ar("placement", placement);
    ar("payload", payload);
    ar("commit", commit);
  }
};

[[nodiscard]] EvidenceKey key_of(const EvidenceRecord& record);
[[nodiscard]] Liveness liveness_of(const EvidenceRecord& record,
                                   const FacilityGenerations& current,
                                   Timestamp now) noexcept;

/// Selection precedence between two live observations of the same subject.
/// Order: stronger provenance wins; then the later observation instant; then
/// the higher accepted sequence; then the lower record id. The last two steps
/// guarantee a total order, so evaluation never depends on iteration order.
[[nodiscard]] bool outranks(const EvidenceRecord& challenger,
                            const EvidenceRecord& incumbent) noexcept;

/// Validate every externally controlled field of a record.
[[nodiscard]] Status validate_evidence(const EvidenceRecord& record);

/// Upper bound on the evidence a single candidate may accumulate, so that a
/// hostile or looping producer cannot grow the store without bound.
inline constexpr std::size_t kMaxEvidencePerCandidate = 4096;

}  // namespace cxf

#endif  // CXF_MODEL_EVIDENCE_HPP
