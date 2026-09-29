// Proof obligations for the durable format: the framed record, the commit
// group, the append-only journal and the atomic snapshot.
//
// The frame header layout is not spelled out by the public header (only its
// total size and the fields it must carry), so this file derives the field
// offsets from encode_frame itself and then proves that every field is
// validated. A frame that decodes to something other than what was encoded, or
// that accepts a corrupted header, is a durability defect.

#include "support/test_harness.hpp"

#include "cxf/codec/archive.hpp"
#include "cxf/persist/journal.hpp"
#include "cxf/persist/record_io.hpp"
#include "cxf/persist/snapshot.hpp"
#include "cxf/support/crc32c.hpp"
#include "cxf/support/digest.hpp"
#include "cxf/support/fs.hpp"
#include "cxf/runtime/fabric.hpp"
#include "cxf/support/serial.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using cxf::Code;
using cxf::CommitGroup;
using cxf::CommitSequence;
using cxf::ControlEpoch;
using cxf::DecodedFrame;
using cxf::Digest;
using cxf::DigestBuilder;
using cxf::FrameDecode;
using cxf::FrameKind;
using cxf::FrameStatus;
using cxf::GroupEntry;
using cxf::Journal;
using cxf::RecordKind;
using cxf::SnapshotStore;
using cxf::Timestamp;

// ---------------------------------------------------------------------------
// Small byte helpers.
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<std::byte> sample_body(std::size_t size, std::uint8_t seed) {
  std::vector<std::byte> body(size);
  for (std::size_t i = 0; i < size; ++i) {
    body[i] = static_cast<std::byte>(static_cast<std::uint8_t>(seed + i * 7u));
  }
  return body;
}

[[nodiscard]] std::vector<std::byte> u32_bytes(std::uint32_t value, bool little_endian) {
  std::vector<std::byte> out(4);
  for (unsigned i = 0; i < 4; ++i) {
    const unsigned shift = little_endian ? 8u * i : 8u * (3u - i);
    out[i] = static_cast<std::byte>((value >> shift) & 0xFFu);
  }
  return out;
}

[[nodiscard]] std::vector<std::size_t> find_sequence(std::span<const std::byte> haystack,
                                                     std::span<const std::byte> needle) {
  std::vector<std::size_t> hits;
  if (needle.empty() || haystack.size() < needle.size()) {
    return hits;
  }
  for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool match = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      if (haystack[i + j] != needle[j]) {
        match = false;
        break;
      }
    }
    if (match) {
      hits.push_back(i);
    }
  }
  return hits;
}

[[nodiscard]] std::vector<std::size_t> differing_positions(std::span<const std::byte> a,
                                                          std::span<const std::byte> b) {
  std::vector<std::size_t> positions;
  const std::size_t limit = a.size() < b.size() ? a.size() : b.size();
  for (std::size_t i = 0; i < limit; ++i) {
    if (a[i] != b[i]) {
      positions.push_back(i);
    }
  }
  return positions;
}

/// Locate the documented frame fields inside the header produced by
/// encode_frame. Every field the format documents must be findable; a missing
/// field fails the test rather than being skipped silently.
struct FrameLayout {
  std::vector<std::size_t> magic{};
  std::vector<std::size_t> kind{};
  std::vector<std::size_t> commit{};
  std::vector<std::size_t> length{};
  std::vector<std::size_t> crc{};
  std::vector<std::size_t> digest{};
  std::vector<std::size_t> other{};
  bool length_little_endian{true};
  bool magic_little_endian{true};
};

[[nodiscard]] FrameLayout probe_layout() {
  const std::vector<std::byte> body = sample_body(32, 0x11);
  const std::vector<std::byte> other_body = sample_body(48, 0x11);
  const std::vector<std::byte> commit_one =
      cxf::encode_frame(FrameKind::kCommitGroup, CommitSequence::from_value(1), body);
  const std::vector<std::byte> commit_one_snapshot =
      cxf::encode_frame(FrameKind::kSnapshot, CommitSequence::from_value(1), body);
  const std::vector<std::byte> commit_two =
      cxf::encode_frame(FrameKind::kCommitGroup, CommitSequence::from_value(2), body);
  const std::vector<std::byte> longer_body =
      cxf::encode_frame(FrameKind::kCommitGroup, CommitSequence::from_value(1), other_body);
  REQUIRE(commit_one.size() == cxf::kFrameHeaderBytes + body.size());

  const std::span<const std::byte> header(commit_one.data(), cxf::kFrameHeaderBytes);
  FrameLayout layout;

  // Magic: the documented constant, in whichever byte order the format uses.
  const std::vector<std::byte> magic_le = u32_bytes(cxf::kFrameMagic, true);
  const std::vector<std::byte> magic_be = u32_bytes(cxf::kFrameMagic, false);
  layout.magic = find_sequence(header, magic_le);
  if (!layout.magic.empty()) {
    layout.magic_little_endian = true;
  } else {
    layout.magic = find_sequence(header, magic_be);
    layout.magic_little_endian = false;
  }
  REQUIRE(!layout.magic.empty());

  // Kind: the bytes that differ between a commit group and a snapshot frame
  // carrying the same commit and body.
  layout.kind = differing_positions(commit_one, commit_one_snapshot);
  REQUIRE(!layout.kind.empty());

  // Commit: probe with a value that differs in every byte so the whole field
  // (not just its least significant byte) is identified. The commit is not
  // covered by the body CRC or digest, so it must not be poked by the
  // header-validation sweep.
  const std::vector<std::byte> wide_commit = cxf::encode_frame(
      FrameKind::kCommitGroup, CommitSequence::from_value(0x0102030405060708ull), body);
  layout.commit = differing_positions(commit_one, wide_commit);
  const std::vector<std::size_t> narrow_commit = differing_positions(commit_one, commit_two);
  REQUIRE(!layout.commit.empty());
  REQUIRE(!narrow_commit.empty());
  for (const std::size_t at : narrow_commit) {
    CHECK(std::find(layout.commit.begin(), layout.commit.end(), at) != layout.commit.end());
  }

  // CRC-32C of the body and the 256-bit digest of the body both live in the
  // header; both are searched in either byte order where that applies.
  const std::vector<std::byte> crc_le =
      u32_bytes(cxf::Crc32c::compute(body), true);
  const std::vector<std::byte> crc_be =
      u32_bytes(cxf::Crc32c::compute(body), false);
  layout.crc = find_sequence(header, crc_le);
  if (layout.crc.empty()) {
    layout.crc = find_sequence(header, crc_be);
  }
  const Digest digest = DigestBuilder::of(body);
  std::vector<std::byte> digest_bytes;
  digest_bytes.reserve(Digest::kBytes);
  for (std::size_t i = 0; i + 1 < digest.hex().size(); i += 2) {
    const auto hi = static_cast<std::uint8_t>(digest.hex()[i] >= 'a'
                                                  ? digest.hex()[i] - 'a' + 10
                                                  : digest.hex()[i] - '0');
    const auto lo = static_cast<std::uint8_t>(digest.hex()[i + 1] >= 'a'
                                                  ? digest.hex()[i + 1] - 'a' + 10
                                                  : digest.hex()[i + 1] - '0');
    digest_bytes.push_back(static_cast<std::byte>(static_cast<std::uint8_t>((hi << 4u) | lo)));
  }
  layout.digest = find_sequence(header, digest_bytes);

  // Body length: the declared size, at a position that also changes when the
  // body size changes.
  const std::vector<std::size_t> body_moved = differing_positions(commit_one, longer_body);
  for (const bool little : {true, false}) {
    const std::vector<std::byte> encoded = u32_bytes(static_cast<std::uint32_t>(body.size()), little);
    for (const std::size_t hit : find_sequence(header, encoded)) {
      const bool moved = std::find(body_moved.begin(), body_moved.end(), hit) != body_moved.end();
      if (moved) {
        layout.length = {hit};
        layout.length_little_endian = little;
        break;
      }
    }
    if (!layout.length.empty()) {
      break;
    }
  }

  // Everything left over is a field the format documents as reserved or as the
  // format version; both must be validated.
  std::vector<bool> known(cxf::kFrameHeaderBytes, false);
  const auto mark = [&known](const std::vector<std::size_t>& positions, std::size_t width) {
    for (const std::size_t at : positions) {
      for (std::size_t i = 0; i < width && at + i < known.size(); ++i) {
        known[at + i] = true;
      }
    }
  };
  mark(layout.magic, 4);
  mark(layout.kind, 1);
  mark(layout.commit, 1);
  mark(layout.crc, 4);
  mark(layout.digest, 32);
  mark(layout.length, 4);
  for (std::size_t i = 0; i < known.size(); ++i) {
    if (!known[i]) {
      layout.other.push_back(i);
    }
  }
  return layout;
}

/// Decode one frame from exactly the bytes of a frame (no trailing data).
[[nodiscard]] FrameDecode decode_exact(const std::vector<std::byte>& frame) {
  return cxf::decode_frame(std::span<const std::byte>(frame));
}

// ---------------------------------------------------------------------------
// Frame round trip.
// ---------------------------------------------------------------------------

CXF_TEST(unit, frame_round_trip) {
  const std::vector<std::byte> body = sample_body(64, 0x5A);
  const std::vector<std::byte> encoded =
      cxf::encode_frame(FrameKind::kCommitGroup, CommitSequence::from_value(9), body);
  CHECK_EQ(encoded.size(), cxf::kFrameHeaderBytes + body.size());

  const FrameDecode decoded = decode_exact(encoded);
  CHECK_EQ(decoded.status, FrameStatus::kOk);
  CHECK(decoded.error.ok());
  CHECK_EQ(decoded.frame.kind, FrameKind::kCommitGroup);
  CHECK_EQ(decoded.frame.commit, CommitSequence::from_value(9));
  CHECK_EQ(decoded.frame.bytes_consumed, encoded.size());
  CHECK_EQ(decoded.frame.body.size(), body.size());
  CHECK(std::equal(decoded.frame.body.begin(), decoded.frame.body.end(), body.begin()));

  // A snapshot frame is a different kind with the same body.
  const std::vector<std::byte> snapshot =
      cxf::encode_frame(FrameKind::kSnapshot, CommitSequence::from_value(3), body);
  const FrameDecode snapshot_decoded = decode_exact(snapshot);
  CHECK_EQ(snapshot_decoded.status, FrameStatus::kOk);
  CHECK_EQ(snapshot_decoded.frame.kind, FrameKind::kSnapshot);
  CHECK_EQ(snapshot_decoded.frame.commit, CommitSequence::from_value(3));

  // Trailing bytes belong to the next frame: decoding one frame consumes
  // exactly one frame and leaves the rest.
  std::vector<std::byte> two = encoded;
  two.insert(two.end(), snapshot.begin(), snapshot.end());
  const FrameDecode first = cxf::decode_frame(std::span<const std::byte>(two));
  CHECK_EQ(first.status, FrameStatus::kOk);
  CHECK_EQ(first.frame.bytes_consumed, encoded.size());
  const FrameDecode second = cxf::decode_frame(
      std::span<const std::byte>(two).subspan(first.frame.bytes_consumed));
  CHECK_EQ(second.status, FrameStatus::kOk);
  CHECK_EQ(second.frame.kind, FrameKind::kSnapshot);
  CHECK_EQ(second.frame.bytes_consumed, snapshot.size());
}

CXF_TEST(unit, frame_layout_is_the_documented_one) {
  const FrameLayout layout = probe_layout();
  // The documented fields are all present and inside the header.
  CHECK(!layout.magic.empty());
  CHECK_EQ(layout.magic.front(), std::size_t{0});
  CHECK(!layout.kind.empty());
  CHECK(!layout.commit.empty());
  CHECK(!layout.length.empty());
  CHECK_EQ(layout.crc.size(), std::size_t{1});
  CHECK_EQ(layout.digest.size(), std::size_t{1});
  for (const std::size_t at : {layout.crc.front(), layout.digest.front(), layout.length.front()}) {
    CHECK(at < cxf::kFrameHeaderBytes);
  }
  // The header is exactly the documented size: no field is written outside it.
  const std::vector<std::byte> body = sample_body(8, 0x01);
  const std::vector<std::byte> frame =
      cxf::encode_frame(FrameKind::kCommitGroup, CommitSequence::from_value(1), body);
  CHECK_EQ(frame.size(), cxf::kFrameHeaderBytes + body.size());
  CHECK(std::equal(frame.begin() + static_cast<std::ptrdiff_t>(cxf::kFrameHeaderBytes),
                   frame.end(), body.begin()));
}

CXF_TEST(unit, frame_rejects_a_short_or_truncated_buffer) {
  const std::vector<std::byte> body = sample_body(32, 0x22);
  const std::vector<std::byte> frame =
      cxf::encode_frame(FrameKind::kCommitGroup, CommitSequence::from_value(4), body);

  // Nothing at all, and every prefix of the header, is an incomplete frame.
  CHECK_EQ(cxf::decode_frame(std::span<const std::byte>()).status, FrameStatus::kIncomplete);
  for (const std::size_t length : {std::size_t{1}, std::size_t{4}, std::size_t{12},
                                   cxf::kFrameHeaderBytes - 1}) {
    const FrameDecode decoded =
        cxf::decode_frame(std::span<const std::byte>(frame).first(length));
    CHECK_EQ(decoded.status, FrameStatus::kIncomplete);
    CHECK(decoded.error.failed());
  }
  // A complete header with a truncated body is a torn write, not corruption.
  const FrameDecode truncated =
      cxf::decode_frame(std::span<const std::byte>(frame).first(frame.size() - 1));
  CHECK_EQ(truncated.status, FrameStatus::kIncomplete);
  CHECK(truncated.error.failed());
  const FrameDecode header_only =
      cxf::decode_frame(std::span<const std::byte>(frame).first(cxf::kFrameHeaderBytes));
  CHECK_EQ(header_only.status, FrameStatus::kIncomplete);
}

CXF_TEST(unit, frame_validates_every_header_field) {
  const FrameLayout layout = probe_layout();
  const std::vector<std::byte> body = sample_body(32, 0x33);
  const std::vector<std::byte> good =
      cxf::encode_frame(FrameKind::kCommitGroup, CommitSequence::from_value(6), body);
  REQUIRE(decode_exact(good).status == FrameStatus::kOk);

  // A corrupted frame must be refused. The frame decoder names the field that
  // disagrees (an input-shape status: an out-of-domain format version or kind, a
  // non-zero reserved field, a length outside the bound) or reports the
  // integrity failure as a storage problem; either way it never returns a value.
  // The store-level mapping of every one of these to a storage corruption is
  // proven in adversarial_persistence.
  const auto expect_invalid = [&good](std::size_t offset, std::uint8_t value,
                                      std::string_view what) {
    std::vector<std::byte> corrupted = good;
    corrupted[offset] = static_cast<std::byte>(value);
    const FrameDecode decoded = decode_exact(corrupted);
    if (decoded.status != FrameStatus::kInvalid) {
      ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK_EQ",
                              std::string(what) + " corrupted at offset " +
                                  std::to_string(offset),
                              "expected Invalid, got " +
                                  std::to_string(static_cast<int>(decoded.status)));
    } else if (!cxf::test::is_storage_failure(decoded.error.code()) &&
               !cxf::test::is_input_rejection(decoded.error.code())) {
      ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", std::string(what),
                              std::string("invalid frame reported as ") +
                                  std::string(cxf::code_name(decoded.error.code())));
    }
  };

  // Bad magic.
  expect_invalid(layout.magic.front(), 0xFFu, "magic");
  // Bad kind: zero is not a declared frame kind.
  expect_invalid(layout.kind.front(), 0x00u, "kind");
  // Bad commit: all-ones is a different commit, which is only detectable by the
  // digest of the group; the frame must at least not claim a different one.
  // Corrupted CRC and digest.
  expect_invalid(layout.crc.front(), static_cast<std::uint8_t>(
                                         static_cast<std::uint8_t>(good[layout.crc.front()]) ^ 0x01u),
                 "crc");
  expect_invalid(layout.digest.front(),
                 static_cast<std::uint8_t>(
                     static_cast<std::uint8_t>(good[layout.digest.front()]) ^ 0x80u),
                 "digest");
  // Corrupted body byte.
  expect_invalid(cxf::kFrameHeaderBytes + 3,
                 static_cast<std::uint8_t>(
                     static_cast<std::uint8_t>(good[cxf::kFrameHeaderBytes + 3]) ^ 0xFFu),
                 "body");
  // Format version and reserved fields: every header byte that is not part of a
  // documented, value-carrying field must be validated.
  for (const std::size_t offset : layout.other) {
    expect_invalid(offset, 0xFFu, "reserved or format version");
  }
  // A declared body length above the documented bound is refused even when the
  // bytes are actually present.
  {
    std::vector<std::byte> oversized = good;
    oversized.resize(cxf::kFrameHeaderBytes + cxf::kMaxFrameBodyBytes + 1, std::byte{0});
    const std::vector<std::byte> encoded_length = u32_bytes(
        static_cast<std::uint32_t>(cxf::kMaxFrameBodyBytes + 1), layout.length_little_endian);
    for (std::size_t i = 0; i < encoded_length.size(); ++i) {
      oversized[layout.length.front() + i] = encoded_length[i];
    }
    const FrameDecode decoded = decode_exact(oversized);
    CHECK_EQ(decoded.status, FrameStatus::kInvalid);
    CHECK(cxf::test::is_storage_failure(decoded.error.code()) ||
          cxf::test::is_input_rejection(decoded.error.code()));
  }
  // A length below the real body size is rejected too: the digest would not
  // cover the whole body.
  {
    std::vector<std::byte> shortened = good;
    const std::vector<std::byte> encoded_length =
        u32_bytes(static_cast<std::uint32_t>(body.size() - 1), layout.length_little_endian);
    for (std::size_t i = 0; i < encoded_length.size(); ++i) {
      shortened[layout.length.front() + i] = encoded_length[i];
    }
    CHECK_EQ(decode_exact(shortened).status, FrameStatus::kInvalid);
  }
}

// ---------------------------------------------------------------------------
// Commit groups.
// ---------------------------------------------------------------------------

[[nodiscard]] CommitGroup sample_group(CommitSequence commit) {
  CommitGroup group;
  group.commit = commit;
  group.epoch = ControlEpoch::from_value(2);
  group.recorded_at = Timestamp::from_unix_seconds(1700000000);

  cxf::FabricMeta meta;
  meta.format = cxf::FormatVersion::from_value(cxf::kStoreFormatVersion);
  meta.epoch = ControlEpoch::from_value(2);
  meta.incarnation = cxf::IncarnationId::from_value(7);
  meta.commit = commit;
  meta.created_at = Timestamp::from_unix_seconds(1699999999);
  meta.updated_at = Timestamp::from_unix_seconds(1700000000);
  GroupEntry meta_entry;
  meta_entry.kind = RecordKind::kFabricMeta;
  meta_entry.body = cxf::encode_body(meta);
  group.entries.push_back(meta_entry);

  GroupEntry blob_entry;
  blob_entry.kind = RecordKind::kEvidence;
  blob_entry.body = sample_body(24, 0x77);
  group.entries.push_back(blob_entry);
  return group;
}

CXF_TEST(unit, commit_group_round_trip) {
  const CommitGroup group = sample_group(CommitSequence::from_value(5));
  const std::vector<std::byte> encoded = cxf::encode_group(group);
  CHECK(!encoded.empty());

  const cxf::Outcome<CommitGroup> decoded = cxf::decode_group(encoded);
  REQUIRE_OK(decoded);
  CHECK_EQ(decoded->commit, group.commit);
  CHECK_EQ(decoded->epoch, group.epoch);
  CHECK_EQ(decoded->recorded_at, group.recorded_at);
  CHECK_EQ(decoded->entries.size(), group.entries.size());
  for (std::size_t i = 0; i < group.entries.size(); ++i) {
    CHECK_EQ(decoded->entries[i].kind, group.entries[i].kind);
    CHECK_EQ(decoded->entries[i].body.size(), group.entries[i].body.size());
    CHECK(std::equal(decoded->entries[i].body.begin(), decoded->entries[i].body.end(),
                     group.entries[i].body.begin()));
  }

  // Encoding is canonical: the same group encodes to the same bytes.
  CHECK_EQ(cxf::encode_group(group), encoded);

  // Trailing bytes after a complete group are refused.
  std::vector<std::byte> with_trailing = encoded;
  with_trailing.push_back(std::byte{0x00});
  CHECK_CODE(cxf::decode_group(with_trailing), Code::kTrailingBytes);
  // A truncated group is refused as malformed input.
  CHECK(!cxf::decode_group(std::span<const std::byte>(encoded).first(encoded.size() - 1)).ok());
  CHECK(!cxf::decode_group(std::span<const std::byte>()).ok());

  // An empty group is a legitimate value: a commit that carries no record.
  CommitGroup empty;
  empty.commit = CommitSequence::from_value(1);
  const cxf::Outcome<CommitGroup> empty_decoded = cxf::decode_group(cxf::encode_group(empty));
  REQUIRE_OK(empty_decoded);
  CHECK(empty_decoded->entries.empty());
}

CXF_TEST(unit, frame_group_agrees_with_the_frame_commit) {
  const CommitGroup group = sample_group(CommitSequence::from_value(11));
  const std::vector<std::byte> body = cxf::encode_group(group);

  const std::vector<std::byte> matching =
      cxf::encode_frame(FrameKind::kCommitGroup, group.commit, body);
  const FrameDecode decoded = decode_exact(matching);
  REQUIRE(decoded.status == FrameStatus::kOk);
  const cxf::Outcome<CommitGroup> carried = decoded.frame.group();
  REQUIRE_OK(carried);
  CHECK_EQ(carried->commit, group.commit);
  CHECK_EQ(carried->entries.size(), group.entries.size());

  // A frame whose commit disagrees with the group it carries must be refused:
  // otherwise a torn or swapped frame could publish the wrong commit number.
  const std::vector<std::byte> mismatched =
      cxf::encode_frame(FrameKind::kCommitGroup,
                        CommitSequence::from_value(group.commit.value() + 1), body);
  const FrameDecode mismatched_decoded = decode_exact(mismatched);
  REQUIRE(mismatched_decoded.status == FrameStatus::kOk);
  const cxf::Outcome<CommitGroup> refused = mismatched_decoded.frame.group();
  CHECK(!refused.ok());
  CHECK(cxf::test::is_input_rejection(refused.code()) ||
        cxf::test::is_storage_failure(refused.code()));

  // group() decodes the body the frame carries, whatever the frame kind; a
  // body that is not a canonical commit group is refused rather than guessed
  // at. (A store that finds a wrong-kind frame where a snapshot belongs is
  // refused at the store level; adversarial_persistence proves that.)
  const std::vector<std::byte> not_a_group = sample_body(24, 0x99);
  const std::vector<std::byte> snapshot =
      cxf::encode_frame(FrameKind::kSnapshot, group.commit, not_a_group);
  const FrameDecode snapshot_decoded = decode_exact(snapshot);
  REQUIRE(snapshot_decoded.status == FrameStatus::kOk);
  CHECK(!snapshot_decoded.frame.group().ok());
}

// ---------------------------------------------------------------------------
// Journal.
// ---------------------------------------------------------------------------

/// A journal frame carrying a real commit group: the journal verifies that the
/// body decodes to a group whose commit matches the frame, so raw bytes are not
/// a frame the journal would ever have written.
[[nodiscard]] std::vector<std::byte> frame_bytes(std::uint64_t commit, std::uint8_t seed) {
  CommitGroup group = sample_group(CommitSequence::from_value(commit));
  group.entries.back().body = sample_body(40, seed);
  return cxf::encode_frame(FrameKind::kCommitGroup, CommitSequence::from_value(commit),
                           cxf::encode_group(group));
}

CXF_TEST(unit, journal_append_verify_and_read_all) {
  cxf::test::TempDirectory directory("journal");
  REQUIRE(directory.ok());
  const std::string path = directory.child("journal.cxf");

  auto journal = Journal::open(path);
  REQUIRE_OK(journal);
  CHECK(cxf::test::path_exists(path));
  CHECK_EQ(journal->bytes(), 0u);

  const std::vector<std::byte> first = frame_bytes(1, 0x01);
  const std::vector<std::byte> second = frame_bytes(2, 0x80);
  REQUIRE_OK(journal->append(first));
  const std::uint64_t second_offset = journal->bytes();
  CHECK_EQ(second_offset, first.size());
  REQUIRE_OK(journal->append(second));
  REQUIRE_OK(journal->flush());
  CHECK_EQ(cxf::test::file_bytes_size(path), first.size() + second.size());
  CHECK_EQ(journal->bytes(), first.size() + second.size());

  // Read back each frame and check it decodes to exactly the expected commit.
  CHECK_CODE(journal->verify_frame_at(0, CommitSequence::from_value(1)), Code::kOk);
  CHECK_CODE(journal->verify_frame_at(second_offset, CommitSequence::from_value(2)), Code::kOk);
  // The wrong expectation is a failure, not a shrug: this is the check the
  // commit path performs before it publishes.
  CHECK(journal->verify_frame_at(0, CommitSequence::from_value(2)).failed());
  CHECK(journal->verify_frame_at(second_offset, CommitSequence::from_value(1)).failed());
  CHECK(journal->verify_frame_at(second_offset + 1, CommitSequence::from_value(2)).failed());

  std::vector<DecodedFrame> frames;
  std::uint64_t discarded = 0;
  std::string note;
  CHECK_CODE(journal->read_all(frames, discarded, note), Code::kOk);
  CHECK_EQ(frames.size(), std::size_t{2});
  CHECK_EQ(frames[0].commit, CommitSequence::from_value(1));
  CHECK_EQ(frames[1].commit, CommitSequence::from_value(2));
  CHECK_EQ(discarded, 0u);
  CHECK(note.empty());

  // Reset truncates the journal to nothing, after which it is empty again.
  REQUIRE_OK(journal->reset());
  CHECK_EQ(journal->bytes(), 0u);
  CHECK_EQ(cxf::test::file_bytes_size(path), 0u);
  std::vector<DecodedFrame> after_reset;
  discarded = 0;
  note.clear();
  CHECK_CODE(journal->read_all(after_reset, discarded, note), Code::kOk);
  CHECK(after_reset.empty());
  CHECK_EQ(discarded, 0u);
}

CXF_TEST(unit, journal_reads_an_absent_or_empty_file) {
  cxf::test::TempDirectory directory("journal-empty");
  REQUIRE(directory.ok());
  // Opening creates the file; reading it back yields no frames and no note.
  auto journal = Journal::open(directory.child("journal.cxf"));
  REQUIRE_OK(journal);
  std::vector<DecodedFrame> frames;
  std::uint64_t discarded = 0;
  std::string note;
  CHECK_CODE(journal->read_all(frames, discarded, note), Code::kOk);
  CHECK(frames.empty());
  CHECK_EQ(discarded, 0u);
  CHECK(note.empty());
}

CXF_TEST(unit, journal_discards_a_torn_tail) {
  cxf::test::TempDirectory directory("journal-torn");
  REQUIRE(directory.ok());
  const std::string path = directory.child("journal.cxf");
  const std::vector<std::byte> complete = frame_bytes(1, 0x10);
  const std::vector<std::byte> next = frame_bytes(2, 0x20);

  // One complete frame followed by the first bytes of another: the tail is a
  // torn write and is discarded with a note, not reported as corruption.
  std::vector<std::byte> content = complete;
  content.insert(content.end(), next.begin(), next.begin() + 9);
  REQUIRE(cxf::test::write_file_bytes(path, content));
  {
    auto journal = Journal::open(path);
    REQUIRE_OK(journal);
    std::vector<DecodedFrame> frames;
    std::uint64_t discarded = 0;
    std::string note;
    CHECK_CODE(journal->read_all(frames, discarded, note), Code::kOk);
    CHECK_EQ(frames.size(), std::size_t{1});
    CHECK_EQ(frames[0].commit, CommitSequence::from_value(1));
    CHECK_EQ(discarded, 9u);
    CHECK(!note.empty());
  }
  // A header-only torn tail is discarded as well.
  content = complete;
  content.insert(content.end(), next.begin(), next.begin() + cxf::kFrameHeaderBytes);
  REQUIRE(cxf::test::write_file_bytes(path, content));
  {
    auto journal = Journal::open(path);
    REQUIRE_OK(journal);
    std::vector<DecodedFrame> frames;
    std::uint64_t discarded = 0;
    std::string note;
    CHECK_CODE(journal->read_all(frames, discarded, note), Code::kOk);
    CHECK_EQ(frames.size(), std::size_t{1});
    CHECK_EQ(discarded, cxf::kFrameHeaderBytes);
    CHECK(!note.empty());
  }
}

CXF_TEST(unit, journal_reports_corruption_before_valid_frames) {
  cxf::test::TempDirectory directory("journal-corrupt");
  REQUIRE(directory.ok());
  const std::string path = directory.child("journal.cxf");
  const std::vector<std::byte> first = frame_bytes(1, 0x30);
  std::vector<std::byte> corrupt = frame_bytes(2, 0x40);
  const std::vector<std::byte> third = frame_bytes(3, 0x50);
  // Corrupt a body byte of the middle frame: the bytes are present, so this is
  // corruption rather than a torn tail, even though a valid frame follows.
  corrupt[cxf::kFrameHeaderBytes + 7] =
      static_cast<std::byte>(static_cast<std::uint8_t>(corrupt[cxf::kFrameHeaderBytes + 7]) ^ 0x01u);

  std::vector<std::byte> content = first;
  content.insert(content.end(), corrupt.begin(), corrupt.end());
  content.insert(content.end(), third.begin(), third.end());
  REQUIRE(cxf::test::write_file_bytes(path, content));

  auto journal = Journal::open(path);
  REQUIRE_OK(journal);
  std::vector<DecodedFrame> frames;
  std::uint64_t discarded = 0;
  std::string note;
  const cxf::Status status = journal->read_all(frames, discarded, note);
  CHECK(status.failed());
  CHECK(cxf::test::is_storage_failure(status.code()));
  CHECK_EQ(status.code(), Code::kStorageCorrupt);
  // A frame that is corrupted must not verify either.
  CHECK(journal->verify_frame_at(first.size(), CommitSequence::from_value(2)).failed());
}

// ---------------------------------------------------------------------------
// Snapshot store.
// ---------------------------------------------------------------------------

CXF_TEST(unit, snapshot_absence_is_not_a_failure) {
  cxf::test::TempDirectory directory("snapshot-absent");
  REQUIRE(directory.ok());
  SnapshotStore store(directory.child("snapshot.cxf"));
  CHECK_EQ(store.path(), directory.child("snapshot.cxf"));
  const cxf::Outcome<bool> exists = store.exists();
  REQUIRE_OK(exists);
  CHECK(!exists.value());
  const cxf::Outcome<std::optional<DecodedFrame>> loaded = store.load(1u << 20);
  REQUIRE_OK(loaded);
  CHECK(!loaded->has_value());
}

CXF_TEST(unit, snapshot_publish_then_load) {
  cxf::test::TempDirectory directory("snapshot-publish");
  REQUIRE(directory.ok());
  const std::string path = directory.child("snapshot.cxf");
  SnapshotStore store(path);

  const std::vector<std::byte> body = sample_body(96, 0x60);
  const std::vector<std::byte> frame =
      cxf::encode_frame(FrameKind::kSnapshot, CommitSequence::from_value(12), body);
  REQUIRE_OK(store.publish(frame));

  const cxf::Outcome<bool> exists = store.exists();
  REQUIRE_OK(exists);
  CHECK(exists.value());
  const cxf::Outcome<std::uint64_t> size = store.size();
  REQUIRE_OK(size);
  CHECK_EQ(size.value(), static_cast<std::uint64_t>(frame.size()));

  const cxf::Outcome<std::optional<DecodedFrame>> loaded = store.load(1u << 20);
  REQUIRE_OK(loaded);
  REQUIRE(loaded->has_value());
  CHECK_EQ(loaded->value().kind, FrameKind::kSnapshot);
  CHECK_EQ(loaded->value().commit, CommitSequence::from_value(12));
  CHECK_EQ(loaded->value().body.size(), body.size());
  CHECK(std::equal(loaded->value().body.begin(), loaded->value().body.end(), body.begin()));

  // The whole frame was published, not a body: the file starts with the frame
  // header that encodes it.
  const cxf::test::FileBytes raw = cxf::test::read_file_bytes(path);
  REQUIRE(raw.ok);
  CHECK_EQ(raw.bytes.size(), frame.size());
  CHECK_EQ(cxf::decode_frame(std::span<const std::byte>(raw.bytes)).status, FrameStatus::kOk);

  // No staging file survives a successful publication.
  for (const std::string& name : cxf::test::list_directory_names(directory.path())) {
    CHECK(!cxf::test::contains_substring(name, "stage"));
  }
}

CXF_TEST(unit, snapshot_publish_replaces_the_previous_generation) {
  cxf::test::TempDirectory directory("snapshot-replace");
  REQUIRE(directory.ok());
  const std::string path = directory.child("snapshot.cxf");
  SnapshotStore store(path);

  const std::vector<std::byte> first =
      cxf::encode_frame(FrameKind::kSnapshot, CommitSequence::from_value(1), sample_body(16, 0x10));
  const std::vector<std::byte> second =
      cxf::encode_frame(FrameKind::kSnapshot, CommitSequence::from_value(2), sample_body(32, 0x20));
  REQUIRE_OK(store.publish(first));
  REQUIRE_OK(store.publish(second));

  // The live name carries the second generation, and only it.
  const cxf::Outcome<std::optional<DecodedFrame>> loaded = store.load(1u << 20);
  REQUIRE_OK(loaded);
  REQUIRE(loaded->has_value());
  CHECK_EQ(loaded->value().commit, CommitSequence::from_value(2));
  CHECK_EQ(loaded->value().body.size(), std::size_t{32});
  const cxf::Outcome<std::uint64_t> size = store.size();
  REQUIRE_OK(size);
  CHECK_EQ(size.value(), static_cast<std::uint64_t>(second.size()));
}

CXF_TEST(unit, snapshot_rejects_a_corrupted_file) {
  cxf::test::TempDirectory directory("snapshot-corrupt");
  REQUIRE(directory.ok());
  const std::string path = directory.child("snapshot.cxf");
  SnapshotStore store(path);

  const std::vector<std::byte> frame =
      cxf::encode_frame(FrameKind::kSnapshot, CommitSequence::from_value(3), sample_body(48, 0x30));
  REQUIRE_OK(store.publish(frame));

  // A flipped body byte no longer matches the CRC and digest under the live
  // name, so the load must fail rather than return the altered state.
  REQUIRE(cxf::test::flip_bit(path, cxf::kFrameHeaderBytes + 5, 4));
  const cxf::Outcome<std::optional<DecodedFrame>> corrupted = store.load(1u << 20);
  CHECK(!corrupted.ok());
  CHECK(cxf::test::is_storage_failure(corrupted.code()));

  // A torn snapshot (a prefix of the published frame) is refused too.
  REQUIRE(cxf::test::truncate_file(path, frame.size() - 1));
  const cxf::Outcome<std::optional<DecodedFrame>> torn = store.load(1u << 20);
  CHECK(!torn.ok());

  // A file larger than the accepted bound is refused by size, not read whole.
  REQUIRE_OK(store.publish(frame));
  const cxf::Outcome<std::optional<DecodedFrame>> too_large = store.load(4);
  CHECK(!too_large.ok());
}

}  // namespace
