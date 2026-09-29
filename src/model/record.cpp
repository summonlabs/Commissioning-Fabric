// Commissioning Fabric - durable record vocabulary.
//
// The record kind names and the metadata checks are part of the durable format:
// the nine kinds have exactly one lowercase spelling each, and a record whose
// metadata does not validate is reported as corrupt rather than repaired by
// guessing. An absent identity, digest or instant is never read as a present
// one.
#include "cxf/model/record.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "cxf/model/candidate.hpp"
#include "cxf/support/clock.hpp"
#include "cxf/support/status.hpp"
#include "cxf/types/enums.hpp"
#include "cxf/types/generations.hpp"
#include "cxf/types/ids.hpp"

namespace cxf {

std::string_view record_kind_name(RecordKind kind) noexcept {
  switch (kind) {
    case RecordKind::kFabricMeta: return "fabric_meta";
    case RecordKind::kFacility: return "facility";
    case RecordKind::kCandidate: return "candidate";
    case RecordKind::kEvidence: return "evidence";
    case RecordKind::kPlan: return "plan";
    case RecordKind::kAuthority: return "authority";
    case RecordKind::kRequestResult: return "request_result";
    case RecordKind::kEvent: return "event";
    case RecordKind::kIdAllocators: return "id_allocators";
  }
  return "invalid";
}

Outcome<RecordKind> parse_record_kind(std::string_view text) {
  static const std::array<std::pair<std::string_view, RecordKind>, kRecordKindCount> kTable = {
      {{"fabric_meta", RecordKind::kFabricMeta},
       {"facility", RecordKind::kFacility},
       {"candidate", RecordKind::kCandidate},
       {"evidence", RecordKind::kEvidence},
       {"plan", RecordKind::kPlan},
       {"authority", RecordKind::kAuthority},
       {"request_result", RecordKind::kRequestResult},
       {"event", RecordKind::kEvent},
       {"id_allocators", RecordKind::kIdAllocators}}};
  for (const auto& entry : kTable) {
    if (entry.first == text) {
      return entry.second;
    }
  }
  return make_error(Code::kFieldOutOfRange, "unknown record kind", std::string(text));
}

Status validate_fabric_meta(const FabricMeta& meta) {
  if (meta.format.is_zero()) {
    return make_error(Code::kFieldOutOfRange,
                      "fabric metadata requires a non-zero format version", "format=0");
  }
  if (meta.incarnation.is_zero()) {
    // Without an incarnation a recovered store cannot tell its own records from
    // those of a previous process, so a zero value is refused rather than
    // treated as "the first incarnation".
    return make_error(Code::kFieldOutOfRange,
                      "fabric metadata requires a non-zero incarnation", "incarnation=0");
  }
  // created_at and updated_at are recorded instants, not an ordering authority.
  // A store written by a process whose clock differs from, or stepped backwards
  // relative to, the clock of the store's creator may legitimately hold
  // updated_at earlier than created_at, and it must still be recoverable: the
  // durable ordering authority is the commit sequence, which only moves forward.
  // Both instants are still required, so an absent one is never read as present.
  if (meta.created_at == Timestamp::epoch() || meta.updated_at == Timestamp::epoch()) {
    return make_error(Code::kFieldOutOfRange,
                      "fabric metadata requires created and updated instants");
  }
  // epoch and commit may be zero: a freshly created store has published nothing
  // yet, and that is recorded as zero rather than as an invented position.
  return Status::success();
}

Status validate_request_result(const RequestResult& result) {
  if (result.request.is_zero()) {
    return make_error(Code::kFieldOutOfRange,
                      "request result requires a non-zero request id", "request=0");
  }
  if (!enum_value_valid(result.kind)) {
    return make_error(Code::kFieldOutOfRange,
                      "request kind is outside the declared domain",
                      "kind=" + std::to_string(static_cast<unsigned>(result.kind)));
  }
  if (result.request_digest.empty()) {
    return make_error(Code::kFieldMissing,
                      "request result requires the digest of the request it answers",
                      "request_digest");
  }
  const Status detail = validate_detail_text(result.detail);
  if (detail.failed()) {
    return detail;
  }
  if (result.commit.is_zero()) {
    // The stored outcome must name the commit that published it, otherwise a
    // replay could not tell whether the effect was ever made durable.
    return make_error(Code::kFieldOutOfRange,
                      "request result requires a non-zero commit sequence", "commit=0");
  }
  if (result.recorded_at == Timestamp::epoch()) {
    return make_error(Code::kFieldOutOfRange,
                      "request result requires a recorded instant", "recorded_at=0");
  }
  return Status::success();
}

Status validate_event(const EventRecord& event) {
  if (event.sequence.is_zero()) {
    return make_error(Code::kFieldOutOfRange,
                      "event requires a non-zero commit sequence", "sequence=0");
  }
  if (!enum_value_valid(event.kind)) {
    return make_error(Code::kFieldOutOfRange,
                      "event kind is outside the declared domain",
                      "kind=" + std::to_string(static_cast<unsigned>(event.kind)));
  }
  if (event.at == Timestamp::epoch()) {
    return make_error(Code::kFieldOutOfRange, "event requires an instant", "at=0");
  }
  return validate_detail_text(event.detail);
}

}  // namespace cxf
