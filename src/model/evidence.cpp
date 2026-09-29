// Commissioning Fabric - generation-bound evidence.
//
// A record is only ever an observation. Liveness is decided against the current
// facility generations and the evaluator's clock; a record that fails either
// test is retained for audit but never counted as live readiness. Selection
// between live observations is a strict total order so evaluation never depends
// on the order records happen to be stored in.
#include "cxf/model/evidence.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "cxf/support/status.hpp"
#include "cxf/support/text.hpp"
#include "cxf/types/enums.hpp"
#include "cxf/types/ids.hpp"
#include "cxf/types/timepoint.hpp"

namespace cxf {
namespace {

/// Bounded producer name. It is descriptive text, not an identifier, and may be
/// absent: the provenance that carries authority is the typed source, not the
/// spelling of the producer.
[[nodiscard]] Status check_source_name(std::string_view text) {
  if (text.empty()) {
    return Status::success();
  }
  if (text.size() > kMaxDetailBytes) {
    return make_error(Code::kFieldTooLong, "source name is longer than allowed",
                      "bytes=" + std::to_string(text.size()));
  }
  if (!is_valid_utf8(text)) {
    return make_error(Code::kFieldInvalidUtf8, "source name is not well-formed UTF-8",
                      "source_name");
  }
  if (!is_valid_display_text(text)) {
    return make_error(Code::kMalformedInput,
                      "source name contains a control character", "source_name");
  }
  return Status::success();
}

}  // namespace

std::string_view liveness_name(Liveness liveness) noexcept {
  switch (liveness) {
    case Liveness::kLive: return "live";
    case Liveness::kExpired: return "expired";
    case Liveness::kGenerationMismatch: return "generation_mismatch";
  }
  return "invalid";
}

EvidenceKey key_of(const EvidenceRecord& record) {
  EvidenceKey key;
  key.dimension = record.dimension;
  key.subject = record.subject;
  return key;
}

Liveness liveness_of(const EvidenceRecord& record, const FacilityGenerations& current,
                     Timestamp now) noexcept {
  // The generation test comes first: an observation of a facility revision that
  // has since moved is not merely old, it describes a world that no longer
  // exists. Reporting it as expired would understate what happened.
  if (record.generations != current) {
    return Liveness::kGenerationMismatch;
  }
  if (record.freshness.is_expired(record.observed_at, now)) {
    return Liveness::kExpired;
  }
  return Liveness::kLive;
}

bool outranks(const EvidenceRecord& challenger, const EvidenceRecord& incumbent) noexcept {
  const std::uint8_t challenger_authority = source_authority(challenger.source);
  const std::uint8_t incumbent_authority = source_authority(incumbent.source);
  if (challenger_authority != incumbent_authority) {
    // Stronger provenance wins: a signed bundle outranks an owning system of
    // record, which outranks a bare assertion.
    return challenger_authority > incumbent_authority;
  }
  if (challenger.observed_at != incumbent.observed_at) {
    return challenger.observed_at > incumbent.observed_at;
  }
  if (challenger.observed_sequence != incumbent.observed_sequence) {
    return challenger.observed_sequence > incumbent.observed_sequence;
  }
  // Final tie-break: the lower record id wins. Two distinct records always
  // differ here, so the relation is a strict total order over distinct records
  // and the winner does not depend on iteration order.
  return challenger.id < incumbent.id;
}

Status validate_evidence(const EvidenceRecord& record) {
  if (record.id.is_zero()) {
    return make_error(Code::kFieldOutOfRange, "evidence requires a non-zero evidence id",
                      "id=0");
  }
  if (record.candidate.is_zero()) {
    return make_error(Code::kFieldOutOfRange, "evidence requires a non-zero candidate id",
                      "candidate=0");
  }
  // record.attempt may be zero: evidence that predates the attempt it will be
  // attributed to is honest about having no attempt yet, and is never assigned
  // one by guessing.
  if (!enum_value_valid(record.dimension)) {
    return make_error(Code::kFieldOutOfRange,
                      "evidence dimension is outside the declared domain",
                      "dimension=" + std::to_string(static_cast<unsigned>(record.dimension)));
  }
  if (record.subject.empty()) {
    return make_error(Code::kFieldEmpty, "evidence requires a subject and must not be empty",
                      "subject");
  }
  if (record.subject.size() > kMaxNameBytes) {
    return make_error(Code::kFieldTooLong,
                      "evidence subject is longer than allowed",
                      "subject bytes=" + std::to_string(record.subject.size()));
  }
  if (!is_valid_token(record.subject)) {
    return make_error(Code::kFieldOutOfRange,
                      "evidence subject must be a token of [A-Za-z0-9._:-] characters",
                      "subject=" + escape_for_output(record.subject));
  }
  if (!enum_value_valid(record.verdict)) {
    return make_error(Code::kFieldOutOfRange,
                      "evidence verdict is outside the declared domain",
                      "verdict=" + std::to_string(static_cast<unsigned>(record.verdict)));
  }
  if (!enum_value_valid(record.source)) {
    return make_error(Code::kFieldOutOfRange,
                      "evidence source is outside the declared domain",
                      "source=" + std::to_string(static_cast<unsigned>(record.source)));
  }
  Status status = check_source_name(record.source_name);
  if (status.failed()) {
    return status;
  }
  status = validate_detail_text(record.detail);
  if (status.failed()) {
    return status;
  }
  // An unknown verdict with no payload is honest: the observer saw something it
  // could not decide. A claimed verdict without a content address is not
  // evidence at all, so it is rejected instead of being trusted.
  if (record.verdict != EvidenceVerdict::kUnknown &&
      (record.payload.empty() || record.payload.is_zero())) {
    return make_error(Code::kFieldMissing,
                      "a claimed evidence verdict requires a non-zero payload digest",
                      std::string("verdict=") + std::string(verdict_name(record.verdict)));
  }
  if (record.observed_sequence.is_zero()) {
    return make_error(Code::kFieldOutOfRange,
                      "evidence requires a non-zero observation sequence",
                      "observed_sequence=0");
  }
  // A wholly zero placement means "not applicable to this observation"; any
  // partially filled placement must be a complete, valid binding.
  if (!record.placement.site.is_zero() || !record.placement.rack.is_zero()) {
    status = validate_placement(record.placement);
    if (status.failed()) {
      return status;
    }
  }
  return Status::success();
}

}  // namespace cxf
