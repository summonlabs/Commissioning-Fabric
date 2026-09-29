// Property obligations: randomised, seeded round trips over the canonical
// encoding, the durable frame and the serial reader.
//
// Every test in this file is a property asserted over many generated values.
// The generator is the harness SplitMix64 stream, so a failure prints the seed
// and the exact command that reproduces it.

#include "support/test_harness.hpp"

#include "cxf/codec/archive.hpp"
#include "cxf/model/candidate.hpp"
#include "cxf/model/evidence.hpp"
#include "cxf/model/readiness.hpp"
#include "cxf/model/record.hpp"
#include "cxf/persist/record_io.hpp"
#include "cxf/runtime/authority.hpp"
#include "cxf/runtime/event.hpp"
#include "cxf/runtime/fabric.hpp"
#include "cxf/runtime/plan.hpp"
#include "cxf/support/serial.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using cxf::ActivationAuthority;
using cxf::Code;
using cxf::CommitGroup;
using cxf::CommitSequence;
using cxf::ControlEpoch;
using cxf::Digest;
using cxf::DigestBuilder;
using cxf::EvidenceRecord;
using cxf::FrameDecode;
using cxf::FrameKind;
using cxf::FrameStatus;
using cxf::GroupEntry;
using cxf::ReadinessPlan;
using cxf::RecordKind;
using cxf::RequestResult;
using cxf::test::Rng;
using cxf::Timestamp;

constexpr int kIterations = 250;

template <typename E>
[[nodiscard]] E random_enum(Rng& rng, std::size_t count) {
  return static_cast<E>(static_cast<std::uint8_t>(rng.below(count)));
}

[[nodiscard]] Digest random_digest(Rng& rng) {
  return DigestBuilder::of(rng.token(rng.inclusive(4, 24)));
}

/// Only declared reason codes may appear in a record: the decoder rejects a
/// numeric value outside the closed domain.
[[nodiscard]] Code random_code(Rng& rng) {
  for (;;) {
    const auto value = static_cast<std::uint16_t>(rng.below(83));
    if (cxf::is_valid_code_value(value)) {
      return static_cast<Code>(value);
    }
  }
}

[[nodiscard]] Timestamp random_time(Rng& rng) {
  return Timestamp::from_unix_seconds(static_cast<std::int64_t>(rng.inclusive(1000000000, 2000000000)));
}

[[nodiscard]] cxf::FreshnessWindow random_window(Rng& rng) {
  if (rng.chance(1, 4)) {
    return cxf::FreshnessWindow::never();
  }
  return cxf::FreshnessWindow::of(
      cxf::Duration::from_seconds(static_cast<std::int64_t>(rng.inclusive(0, 86400))));
}

[[nodiscard]] cxf::FacilityGenerations random_generations(Rng& rng) {
  cxf::FacilityGenerations generations;
  generations.topology = cxf::TopologyGeneration::from_value(rng.below(64));
  generations.power = cxf::PowerGeneration::from_value(rng.below(64));
  generations.cooling = cxf::CoolingGeneration::from_value(rng.below(64));
  generations.network = cxf::NetworkGeneration::from_value(rng.below(64));
  generations.policy = cxf::PolicyGeneration::from_value(rng.below(64));
  generations.dependency = cxf::DependencyGeneration::from_value(rng.below(64));
  generations.firmware = cxf::FirmwareGeneration::from_value(rng.below(64));
  generations.hardware = cxf::HardwareGeneration::from_value(rng.below(64));
  generations.lifecycle = cxf::LifecycleGeneration::from_value(rng.below(64));
  return generations;
}

[[nodiscard]] cxf::PlacementBinding random_placement(Rng& rng) {
  cxf::PlacementBinding placement;
  placement.site = cxf::SiteId::from_value(rng.inclusive(1, 32));
  placement.rack = cxf::RackId::from_value(rng.inclusive(1, 4096));
  placement.position = "U" + std::to_string(rng.inclusive(1, 48));
  placement.topology = cxf::TopologyGeneration::from_value(rng.below(64));
  return placement;
}

[[nodiscard]] cxf::DependencyRef random_dependency(Rng& rng) {
  cxf::DependencyRef ref;
  ref.kind = random_enum<cxf::DependencyKind>(rng, cxf::kDependencyKindCount);
  ref.name = rng.token(rng.inclusive(1, 24));
  ref.expected = cxf::DependencyGeneration::from_value(rng.below(64));
  ref.required = rng.chance(3, 4);
  return ref;
}

[[nodiscard]] cxf::Attempt random_attempt(Rng& rng) {
  cxf::Attempt attempt;
  attempt.id = cxf::AttemptId::from_value(rng.inclusive(1, 1000));
  attempt.generation = cxf::LifecycleGeneration::from_value(rng.below(32));
  attempt.state = random_enum<cxf::LifecycleState>(rng, cxf::kLifecycleStateCount);
  attempt.opened_at = random_time(rng);
  attempt.closed_at = random_time(rng);
  attempt.note = rng.token(rng.below(32));
  return attempt;
}

[[nodiscard]] cxf::CommissioningCandidate random_candidate(Rng& rng) {
  cxf::CommissioningCandidate candidate;
  candidate.id = cxf::CandidateId::from_value(rng.inclusive(1, 100000));
  candidate.name = rng.token(rng.inclusive(1, 40));
  candidate.state = random_enum<cxf::LifecycleState>(rng, cxf::kLifecycleStateCount);
  candidate.lifecycle = cxf::LifecycleGeneration::from_value(rng.below(64));
  candidate.incarnation = cxf::IncarnationId::from_value(rng.below(16));
  candidate.revision = cxf::Revision::from_value(rng.below(1024));
  candidate.generations = random_generations(rng);
  candidate.declaration.registry_name = rng.token(rng.inclusive(1, 32));
  candidate.declaration.model = rng.token(rng.below(32));
  candidate.declaration.serial = rng.token(rng.below(32));
  candidate.declaration.hardware = cxf::HardwareGeneration::from_value(rng.below(32));
  candidate.declaration.firmware = cxf::FirmwareGeneration::from_value(rng.below(32));
  if (rng.chance(1, 2)) {
    cxf::AssetIdentity identity;
    identity.asset = cxf::AssetId::from_value(rng.inclusive(1, 100000));
    identity.registry_name = rng.token(rng.inclusive(1, 32));
    identity.model = rng.token(rng.below(24));
    identity.serial = rng.token(rng.below(24));
    identity.hardware = cxf::HardwareGeneration::from_value(rng.below(32));
    identity.firmware = cxf::FirmwareGeneration::from_value(rng.below(32));
    candidate.identity = identity;
  }
  if (rng.chance(1, 2)) {
    candidate.placement = random_placement(rng);
  }
  const std::size_t dependencies = rng.below(4);
  for (std::size_t i = 0; i < dependencies; ++i) {
    candidate.dependencies.push_back(random_dependency(rng));
  }
  candidate.attempt = random_attempt(rng);
  const std::size_t history = rng.below(3);
  for (std::size_t i = 0; i < history; ++i) {
    candidate.history.push_back(random_attempt(rng));
  }
  candidate.quarantine_reason = random_enum<cxf::QuarantineReason>(rng, 9);
  candidate.quarantine_detail = rng.token(rng.below(24));
  candidate.last_plan = cxf::PlanId::from_value(rng.below(64));
  candidate.last_plan_digest = random_digest(rng);
  candidate.last_readiness_digest = random_digest(rng);
  candidate.admitted_at = random_time(rng);
  candidate.updated_at = random_time(rng);
  candidate.last_commit = CommitSequence::from_value(rng.below(4096));
  return candidate;
}

[[nodiscard]] EvidenceRecord random_evidence(Rng& rng) {
  EvidenceRecord record;
  record.id = cxf::EvidenceId::from_value(rng.inclusive(1, 100000));
  record.candidate = cxf::CandidateId::from_value(rng.inclusive(1, 100000));
  record.attempt = cxf::AttemptId::from_value(rng.below(16));
  record.dimension = random_enum<cxf::EvidenceDimension>(rng, cxf::kEvidenceDimensionCount);
  record.subject = rng.token(rng.inclusive(1, 32));
  record.verdict = random_enum<cxf::EvidenceVerdict>(rng, 3);
  record.source = random_enum<cxf::EvidenceSource>(rng, 3);
  record.source_name = rng.token(rng.below(24));
  record.detail = rng.token(rng.below(48));
  record.observed_at = random_time(rng);
  record.freshness = random_window(rng);
  record.observed_sequence = cxf::ObservationSequence::from_value(rng.below(4096));
  record.generations = random_generations(rng);
  record.placement = random_placement(rng);
  record.payload = random_digest(rng);
  record.commit = CommitSequence::from_value(rng.below(4096));
  return record;
}

[[nodiscard]] cxf::DependencyOutcome random_dependency_outcome(Rng& rng) {
  cxf::DependencyOutcome outcome;
  outcome.ref = random_dependency(rng);
  outcome.known = rng.chance(3, 4);
  outcome.ambiguous = rng.chance(1, 8);
  outcome.actual = cxf::DependencyGeneration::from_value(rng.below(64));
  outcome.detail = rng.token(rng.below(24));
  return outcome;
}

[[nodiscard]] cxf::DimensionEvaluation random_dimension(Rng& rng) {
  cxf::DimensionEvaluation dimension;
  dimension.dimension = random_enum<cxf::EvidenceDimension>(rng, cxf::kEvidenceDimensionCount);
  dimension.required = rng.chance(3, 4);
  dimension.verdict = random_enum<cxf::EvidenceVerdict>(rng, 3);
  dimension.code = rng.chance(1, 4) ? random_code(rng) : Code::kOk;
  dimension.explanation = rng.token(rng.below(40));
  dimension.witness = cxf::EvidenceId::from_value(rng.below(1024));
  dimension.witness_source = random_enum<cxf::EvidenceSource>(rng, 3);
  dimension.witness_sequence = cxf::ObservationSequence::from_value(rng.below(1024));
  dimension.live_records = static_cast<std::uint32_t>(rng.below(64));
  dimension.stale_records = static_cast<std::uint32_t>(rng.below(64));
  dimension.contradictory_records = static_cast<std::uint32_t>(rng.below(8));
  return dimension;
}

[[nodiscard]] cxf::ReadinessReport random_report(Rng& rng) {
  cxf::ReadinessReport report;
  report.candidate = cxf::CandidateId::from_value(rng.inclusive(1, 100000));
  report.revision = cxf::Revision::from_value(rng.below(1024));
  report.incarnation = cxf::IncarnationId::from_value(rng.below(16));
  report.lifecycle = cxf::LifecycleGeneration::from_value(rng.below(64));
  report.state = random_enum<cxf::LifecycleState>(rng, cxf::kLifecycleStateCount);
  report.generations = random_generations(rng);
  const std::size_t outcomes = rng.below(4);
  for (std::size_t i = 0; i < outcomes; ++i) {
    report.dependencies.entries.push_back(random_dependency_outcome(rng));
  }
  report.ready = rng.chance(1, 2);
  report.furthest_state = random_enum<cxf::LifecycleState>(rng, cxf::kMilestoneStateCount);
  const std::size_t dimensions = rng.below(cxf::kEvidenceDimensionCount + 1);
  for (std::size_t i = 0; i < dimensions; ++i) {
    report.dimensions.push_back(random_dimension(rng));
  }
  report.evaluated_at = random_time(rng);
  report.observation = cxf::ObservationSequence::from_value(rng.below(4096));
  report.digest = random_digest(rng);
  return report;
}

[[nodiscard]] ReadinessPlan random_plan(Rng& rng) {
  ReadinessPlan plan;
  plan.id = cxf::PlanId::from_value(rng.inclusive(1, 100000));
  plan.candidate = cxf::CandidateId::from_value(rng.inclusive(1, 100000));
  plan.revision = cxf::Revision::from_value(rng.below(1024));
  plan.incarnation = cxf::IncarnationId::from_value(rng.below(16));
  plan.lifecycle = cxf::LifecycleGeneration::from_value(rng.below(64));
  plan.generations = random_generations(rng);
  plan.report = random_report(rng);
  plan.report_digest = random_digest(rng);
  plan.created_at = random_time(rng);
  plan.validity = random_window(rng);
  plan.commit = CommitSequence::from_value(rng.below(4096));
  return plan;
}

[[nodiscard]] ActivationAuthority random_authority(Rng& rng) {
  ActivationAuthority authority;
  authority.id = cxf::TokenId::from_value(rng.inclusive(1, 100000));
  authority.plan = cxf::PlanId::from_value(rng.inclusive(1, 100000));
  authority.plan_digest = random_digest(rng);
  authority.candidate = cxf::CandidateId::from_value(rng.inclusive(1, 100000));
  authority.revision = cxf::Revision::from_value(rng.below(1024));
  authority.incarnation = cxf::IncarnationId::from_value(rng.below(16));
  authority.lifecycle = cxf::LifecycleGeneration::from_value(rng.below(64));
  authority.generations = random_generations(rng);
  authority.attempt = cxf::AttemptId::from_value(rng.below(16));
  authority.issued_at = random_time(rng);
  authority.validity = random_window(rng);
  authority.consumed = rng.chance(1, 3);
  authority.consumed_at = random_time(rng);
  authority.last_result = random_enum<cxf::ActivationResultKind>(rng, 3);
  authority.deferred_reports = static_cast<std::uint32_t>(rng.below(8));
  authority.commit = CommitSequence::from_value(rng.below(4096));
  return authority;
}

[[nodiscard]] cxf::EventRecord random_event(Rng& rng) {
  cxf::EventRecord event;
  event.sequence = CommitSequence::from_value(rng.below(4096));
  event.kind = random_enum<cxf::EventKind>(rng, 18);
  event.at = random_time(rng);
  event.candidate = cxf::CandidateId::from_value(rng.below(1024));
  event.attempt = cxf::AttemptId::from_value(rng.below(16));
  event.lifecycle = cxf::LifecycleGeneration::from_value(rng.below(64));
  event.generations = random_generations(rng);
  event.detail = rng.token(rng.below(48));
  return event;
}

[[nodiscard]] RequestResult random_request_result(Rng& rng) {
  RequestResult result;
  result.request = cxf::RequestId::from_value(rng.inclusive(1, 100000));
  result.kind = random_enum<cxf::RequestKind>(rng, 11);
  result.request_digest = random_digest(rng);
  result.accepted = rng.chance(1, 2);
  result.code = random_code(rng);
  result.detail = rng.token(rng.below(40));
  result.effect_digest = random_digest(rng);
  result.commit = CommitSequence::from_value(rng.below(4096));
  result.recorded_at = random_time(rng);
  return result;
}

[[nodiscard]] CommitGroup random_group(Rng& rng) {
  CommitGroup group;
  group.commit = CommitSequence::from_value(rng.below(100000));
  group.epoch = ControlEpoch::from_value(rng.below(8));
  group.recorded_at = random_time(rng);
  const std::size_t entries = rng.below(5);
  for (std::size_t i = 0; i < entries; ++i) {
    GroupEntry entry;
    entry.kind = static_cast<RecordKind>(static_cast<std::uint16_t>(1 + rng.below(9)));
    entry.body = rng.bytes(rng.below(40));
    group.entries.push_back(entry);
  }
  return group;
}

/// Encode, decode and re-encode: the canonical bytes must survive unchanged and
/// no field may be silently dropped, reordered or defaulted.
template <typename T>
void check_body_round_trip(const T& value, std::string_view what, int iteration) {
  const std::vector<std::byte> encoded = cxf::encode_body(value);
  T decoded{};
  const cxf::Status status = cxf::decode_body(encoded, decoded);
  if (!status.ok()) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "decode_body",
                            std::string(what) + " iteration " + std::to_string(iteration),
                            status.message());
    return;
  }
  const std::vector<std::byte> again = cxf::encode_body(decoded);
  if (again != encoded) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "encode(decoded) == encoded",
                            std::string(what) + " iteration " + std::to_string(iteration),
                            "sizes " + std::to_string(encoded.size()) + " and " +
                                std::to_string(again.size()));
  }
}

}  // namespace

CXF_TEST(property, records_survive_encode_decode) {
  Rng& rng = cxf::test::rng();
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    check_body_round_trip(random_candidate(rng), "CommissioningCandidate", iteration);
    check_body_round_trip(random_evidence(rng), "EvidenceRecord", iteration);
    check_body_round_trip(random_plan(rng), "ReadinessPlan", iteration);
    check_body_round_trip(random_authority(rng), "ActivationAuthority", iteration);
    check_body_round_trip(random_event(rng), "EventRecord", iteration);
    check_body_round_trip(random_request_result(rng), "RequestResult", iteration);
    check_body_round_trip(random_group(rng), "CommitGroup", iteration);
    check_body_round_trip(random_generations(rng), "FacilityGenerations", iteration);
    check_body_round_trip(random_placement(rng), "PlacementBinding", iteration);
    check_body_round_trip(random_dependency(rng), "DependencyRef", iteration);
  }
}

CXF_TEST(property, group_encoding_matches_the_frame_body) {
  Rng& rng = cxf::test::rng();
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    const CommitGroup group = random_group(rng);
    // The frame body of a commit group is exactly encode_group, so a frame that
    // decodes to a group must agree field for field.
    const std::vector<std::byte> body = cxf::encode_group(group);
    const std::vector<std::byte> direct = cxf::encode_body(group);
    CHECK_EQ(body, direct);

    const std::vector<std::byte> frame =
        cxf::encode_frame(FrameKind::kCommitGroup, group.commit, body);
    const FrameDecode decoded = cxf::decode_frame(std::span<const std::byte>(frame));
    REQUIRE(decoded.status == FrameStatus::kOk);
    const cxf::Outcome<CommitGroup> carried = decoded.frame.group();
    REQUIRE_OK(carried);
    CHECK_EQ(carried->commit, group.commit);
    CHECK_EQ(carried->epoch, group.epoch);
    CHECK_EQ(carried->recorded_at, group.recorded_at);
    CHECK_EQ(carried->entries.size(), group.entries.size());
    CHECK_EQ(cxf::encode_group(*carried), body);
  }
}

CXF_TEST(property, single_bit_frame_mutations_never_decode_silently) {
  Rng& rng = cxf::test::rng();
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    const std::vector<std::byte> body = rng.bytes(static_cast<std::size_t>(rng.below(48)));
    const FrameKind kind = rng.chance(1, 2) ? FrameKind::kCommitGroup : FrameKind::kSnapshot;
    // A commit sequence is 1-based. encode_frame refuses commit zero by
    // returning an empty frame rather than a malformed one, so the generator
    // draws from the domain that actually occurs durably.
    const std::uint64_t commit = rng.inclusive(1, 1000);
    const std::vector<std::byte> frame =
        cxf::encode_frame(kind, CommitSequence::from_value(commit), body);
    REQUIRE(frame.size() == cxf::kFrameHeaderBytes + body.size());
    const std::size_t offset = static_cast<std::size_t>(rng.below(frame.size()));
    const unsigned bit = static_cast<unsigned>(rng.below(8));

    std::vector<std::byte> mutated = frame;
    mutated[offset] = static_cast<std::byte>(static_cast<std::uint8_t>(
        static_cast<std::uint8_t>(mutated[offset]) ^ static_cast<std::uint8_t>(1u << bit)));

    const FrameDecode decoded = cxf::decode_frame(std::span<const std::byte>(mutated));
    if (decoded.status == FrameStatus::kOk) {
      // A successful decode never claims to have consumed more than it was
      // given, and it re-encodes to exactly the bytes that were read.
      CHECK(decoded.frame.bytes_consumed <= mutated.size());
      // A decoded frame must re-encode to exactly the bytes that were read:
      // otherwise the decoder invented a value the bytes do not carry.
      const std::vector<std::byte> reencoded = cxf::encode_frame(
          decoded.frame.kind, decoded.frame.commit, decoded.frame.body);
      CHECK_EQ(reencoded.size(), decoded.frame.bytes_consumed);
      if (reencoded.size() == decoded.frame.bytes_consumed &&
          reencoded.size() <= mutated.size()) {
        CHECK(std::equal(reencoded.begin(), reencoded.end(), mutated.begin()));
      }
    } else {
      CHECK(decoded.error.failed());
      CHECK(decoded.status == FrameStatus::kInvalid || decoded.status == FrameStatus::kIncomplete);
    }
  }
}

CXF_TEST(property, frame_truncations_never_decode_a_complete_frame) {
  Rng& rng = cxf::test::rng();
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    const std::vector<std::byte> body = rng.bytes(static_cast<std::size_t>(rng.below(64)));
    const std::vector<std::byte> frame =
        cxf::encode_frame(FrameKind::kCommitGroup, CommitSequence::from_value(rng.below(1000)),
                          body);
    const std::size_t length = static_cast<std::size_t>(rng.below(frame.size()));
    const FrameDecode decoded =
        cxf::decode_frame(std::span<const std::byte>(frame).first(length));
    // Fewer bytes than the header declares is a torn tail, never a value.
    CHECK_EQ(decoded.status, FrameStatus::kIncomplete);
    CHECK(decoded.error.failed());
    // bytes_consumed is only defined for a successful decode, so the property
    // asserted here is that no value was produced at all.
    CHECK(decoded.frame.body.empty());
  }
}

CXF_TEST(property, truncated_record_bodies_never_decode) {
  Rng& rng = cxf::test::rng();
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    const cxf::CommissioningCandidate candidate = random_candidate(rng);
    const std::vector<std::byte> encoded = cxf::encode_body(candidate);
    if (encoded.empty()) {
      continue;
    }
    const std::size_t length = static_cast<std::size_t>(rng.below(encoded.size()));
    cxf::CommissioningCandidate decoded{};
    const cxf::Status status = cxf::decode_body(std::span<const std::byte>(encoded).first(length),
                                                decoded);
    CHECK(!status.ok());

    // Appending arbitrary bytes to a complete body is trailing data, not a
    // longer record.
    std::vector<std::byte> extended = encoded;
    extended.push_back(std::byte{static_cast<unsigned char>(rng.below(256))});
    cxf::CommissioningCandidate extended_decoded{};
    CHECK(!cxf::decode_body(extended, extended_decoded).ok());
  }
}

CXF_TEST(property, random_bytes_are_never_accepted_as_canonical_records) {
  Rng& rng = cxf::test::rng();
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    const std::vector<std::byte> arbitrary =
        rng.bytes(static_cast<std::size_t>(rng.below(96)));
    cxf::CommissioningCandidate candidate{};
    const cxf::Status status = cxf::decode_body(arbitrary, candidate);
    if (status.ok()) {
      // If arbitrary bytes do decode, the value must be canonical: re-encoding
      // it reproduces exactly those bytes, so no two spellings of one value can
      // exist.
      CHECK_EQ(cxf::encode_body(candidate), arbitrary);
    }
  }
}

CXF_TEST(property, serial_reader_never_reads_past_the_buffer) {
  Rng& rng = cxf::test::rng();
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    const std::vector<std::byte> data = rng.bytes(static_cast<std::size_t>(rng.below(48)));
    const std::span<const std::byte> view(data);
    cxf::SerialReader reader(view);
    for (int access = 0; access < 12; ++access) {
      const std::size_t offset_before = reader.offset();
      const std::size_t remaining_before = reader.remaining();
      CHECK(offset_before <= data.size());
      CHECK_EQ(offset_before + remaining_before, data.size());
      // A field that is present is consumed; a field that is not is refused.
      // Either way the reader never rewinds and never steps past the buffer,
      // however hostile the declared length is.
      bool succeeded = false;
      switch (static_cast<int>(rng.below(9))) {
        case 0:
          succeeded = reader.u8().ok();
          break;
        case 1:
          succeeded = reader.u16().ok();
          break;
        case 2:
          succeeded = reader.u32().ok();
          break;
        case 3:
          succeeded = reader.u64().ok();
          break;
        case 4:
          succeeded = reader.i64().ok();
          break;
        case 5:
          succeeded = reader.boolean().ok();
          break;
        case 6:
          succeeded = reader.text().ok();
          break;
        case 7:
          succeeded = reader.blob().ok();
          break;
        default:
          succeeded = reader.digest_text().ok();
          break;
      }
      // The reader never rewinds, never steps past the buffer, and its
      // remaining count always agrees with the offset it reports.
      CHECK(reader.offset() >= offset_before);
      CHECK(reader.offset() <= data.size());
      CHECK(reader.remaining() <= remaining_before);
      CHECK_EQ(reader.remaining(), data.size() - reader.offset());
      // A successful read always consumed at least one byte; a failed read may
      // legitimately consume nothing (the field simply was not there).
      if (succeeded) {
        CHECK(reader.offset() > offset_before);
      }
    }
  }
}
