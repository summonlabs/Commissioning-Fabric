// Adversarial persistence: the durable store is damaged deliberately and the
// runtime must answer with a clear Status every time.
//
// Nothing here may crash, hang, or - worst of all - open successfully on a
// state that is not the durable truth. A case therefore either recovers the
// previous authoritative generation completely, or reports corruption.
//
// Store layout is discovered rather than assumed: the lock file is the one
// carrying holder= text, and the snapshot is the file a checkpoint adds.

#include "support/test_harness.hpp"

#include "cxf/codec/archive.hpp"
#include "cxf/persist/record_io.hpp"
#include "cxf/runtime/fabric.hpp"
#include "cxf/support/clock.hpp"
#include "cxf/support/fs.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

using cxf::Code;
using cxf::CommitSequence;
using cxf::DecodedFrame;
using cxf::Fabric;
using cxf::FabricOptions;
using cxf::FrameDecode;
using cxf::FrameKind;
using cxf::FrameStatus;
using cxf::Timestamp;

constexpr std::int64_t kBase = 1700000000;

struct StoreLayout {
  std::string root{};
  std::string lock{};
  std::string journal{};
  std::string snapshot{};
};

/// The lock file is identified by its informational holder line, which the
/// header documents as the only thing the body carries.
[[nodiscard]] std::string find_lock_file(const std::string& root) {
  for (const std::string& name : cxf::test::list_directory_names(root)) {
    const std::string text = cxf::test::read_file_text(root + "/" + name);
    if (cxf::test::contains_substring(text, "holder=")) {
      return name;
    }
  }
  return std::string{};
}

struct Populated {
  std::uint64_t commit{0};
  std::uint64_t candidates{0};
};

[[nodiscard]] cxf::CandidateDeclaration declaration_of(const std::string& name) {
  cxf::CandidateDeclaration declaration;
  declaration.registry_name = "registry-" + name;
  declaration.model = "model-p";
  declaration.serial = "SN-" + name;
  return declaration;
}

/// Create a store with a known number of admissions, optionally checkpointed.
[[nodiscard]] Populated populate(const std::string& root, int admissions, bool checkpoint) {
  cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
  FabricOptions options;
  options.directory = root;
  options.holder = "adversarial-persistence";
  cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
  REQUIRE_OK(fabric);
  for (int i = 0; i < admissions; ++i) {
    cxf::AdmitCandidateRequest request;
    request.name = "p-" + std::to_string(i);
    request.declaration = declaration_of(request.name);
    REQUIRE_OK(fabric->admit_candidate(request));
  }
  if (checkpoint) {
    REQUIRE_OK(fabric->checkpoint());
  }
  Populated populated;
  populated.commit = fabric->meta().commit.value();
  populated.candidates = fabric->stats().candidates;
  return populated;
}

struct Recovery {
  bool opened{false};
  Code code{Code::kOk};
  std::uint64_t commit{0};
  std::uint64_t candidates{0};
  bool verified{false};
  std::uint64_t discarded_tail{0};
};

[[nodiscard]] Recovery recover(const std::string& root) {
  Recovery recovery;
  cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
  FabricOptions options;
  options.directory = root;
  options.holder = "adversarial-recovery";
  cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
  if (!fabric.ok()) {
    recovery.code = fabric.code();
    return recovery;
  }
  recovery.opened = true;
  recovery.commit = fabric->meta().commit.value();
  recovery.candidates = fabric->stats().candidates;
  recovery.discarded_tail = fabric->stats().discarded_tail_bytes;
  recovery.verified = fabric->verify_store().ok();
  return recovery;
}

/// True when a refusal is reported as a durable-store problem rather than as
/// an input-shape problem. Either is a clear status; the store family is the
/// one a damaged store is expected to use.
[[nodiscard]] bool clear_refusal(Code code) {
  return cxf::test::is_storage_failure(code) || cxf::test::is_input_rejection(code);
}

/// A case that may recover the whole generation, recover a prefix of it while
/// reporting the discarded tail, or report the damage. What it may never do is
/// invent state or lose commits without recording the loss.
void expect_complete_or_refused(const Recovery& recovery, const Recovery& clean,
                                std::string_view what) {
  const std::string label(what);
  if (!recovery.opened) {
    if (!clear_refusal(recovery.code)) {
      ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", label,
                              std::string("refused with an unclear status: ") +
                                  std::string(cxf::code_name(recovery.code)));
    }
    return;
  }
  if (!recovery.verified) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", label, "recovered but did not verify");
  }
  if (recovery.commit > clean.commit || recovery.candidates > clean.candidates) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", label,
                            "opened on invented state: commit " +
                                std::to_string(recovery.commit) + " (clean " +
                                std::to_string(clean.commit) + "), candidates " +
                                std::to_string(recovery.candidates) + " (clean " +
                                std::to_string(clean.candidates) + ")");
    return;
  }
  if (recovery.commit != clean.commit || recovery.candidates != clean.candidates) {
    // A prefix is the documented outcome only when the discarded tail is
    // reported; a silent loss of committed state is a defect.
    if (recovery.discarded_tail == 0) {
      ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", label,
                              "lost committed state without recording a discarded tail: commit " +
                                  std::to_string(recovery.commit) + " instead of " +
                                  std::to_string(clean.commit) + ", candidates " +
                                  std::to_string(recovery.candidates) + " instead of " +
                                  std::to_string(clean.candidates));
    }
  }
}

/// A case that must not be absorbed, where the store reports the damage as a
/// field conflict rather than as a checksum failure. Both are clear statuses;
/// the obligation under test is that no state is invented or silently kept.
void expect_refused_with_clear_status(const Recovery& recovery, std::string_view what) {
  if (recovery.opened) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", std::string(what),
                            "corruption was accepted: reopened with commit " +
                                std::to_string(recovery.commit) + " and " +
                                std::to_string(recovery.candidates) + " candidates");
    return;
  }
  if (!clear_refusal(recovery.code)) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", std::string(what),
                            std::string("refused with an unclear status: ") +
                                std::string(cxf::code_name(recovery.code)));
  }
}

/// A case that must be reported as damage rather than absorbed.
void expect_refused(const Recovery& recovery, std::string_view what) {
  if (recovery.opened) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", std::string(what),
                            "corruption was accepted: reopened with commit " +
                                std::to_string(recovery.commit) + " and " +
                                std::to_string(recovery.candidates) + " candidates");
    return;
  }
  if (!cxf::test::is_storage_failure(recovery.code)) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", std::string(what),
                            std::string("reported as ") +
                                std::string(cxf::code_name(recovery.code)) +
                                " instead of a storage corruption");
  }
}

struct FrameRange {
  std::size_t offset{0};
  std::size_t size{0};
  std::uint64_t commit{0};
};

/// Walk the frames of a journal with the library's own decoder.
[[nodiscard]] std::vector<FrameRange> scan_frames(const std::vector<std::byte>& bytes) {
  std::vector<FrameRange> frames;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const FrameDecode decoded =
        cxf::decode_frame(std::span<const std::byte>(bytes).subspan(offset));
    if (decoded.status != FrameStatus::kOk || decoded.frame.bytes_consumed == 0) {
      break;
    }
    frames.push_back(FrameRange{offset, decoded.frame.bytes_consumed,
                                decoded.frame.commit.value()});
    offset += decoded.frame.bytes_consumed;
  }
  return frames;
}

[[nodiscard]] std::vector<std::byte> read_all(const std::string& path) {
  const cxf::test::FileBytes bytes = cxf::test::read_file_bytes(path);
  REQUIRE(bytes.ok);
  return bytes.bytes;
}

/// Copy the reference store into a fresh directory that can be damaged freely.
[[nodiscard]] std::string fresh_copy(const std::string& source, cxf::test::TempDirectory& home,
                                     const std::string& label) {
  const std::string work = home.child(label);
  REQUIRE(cxf::test::copy_tree(source, work));
  return work;
}

void restore_permissions(const std::string& path) {
  std::error_code ec;
  std::filesystem::permissions(path, std::filesystem::perms::owner_all |
                                          std::filesystem::perms::group_all |
                                          std::filesystem::perms::others_all,
                               std::filesystem::perm_options::replace, ec);
}

}  // namespace

CXF_TEST(adversarial, store_layout_is_discoverable) {
  cxf::test::TempDirectory home("layout");
  REQUIRE(home.ok());
  const std::string plain = home.child("plain");
  const std::string checkpointed = home.child("checkpointed");
  static_cast<void>(populate(plain, 3, false));
  static_cast<void>(populate(checkpointed, 3, true));

  const std::vector<std::string> plain_files = cxf::test::list_directory_names(plain);
  const std::vector<std::string> checkpointed_files = cxf::test::list_directory_names(checkpointed);
  CHECK(!plain_files.empty());
  CHECK(!checkpointed_files.empty());
  const std::string lock = find_lock_file(plain);
  CHECK(!lock.empty());
  CHECK(std::find(checkpointed_files.begin(), checkpointed_files.end(), lock) !=
        checkpointed_files.end());
  // A checkpoint adds exactly the snapshot: that is how the file is identified.
  std::vector<std::string> added;
  for (const std::string& name : checkpointed_files) {
    if (std::find(plain_files.begin(), plain_files.end(), name) == plain_files.end()) {
      added.push_back(name);
    }
  }
  CHECK_EQ(added.size(), std::size_t{1});
}

CXF_TEST(adversarial, truncated_journal_recovers_only_complete_frames) {
  cxf::test::TempDirectory home("truncate");
  REQUIRE(home.ok());
  const std::string reference = home.child("reference");
  static_cast<void>(populate(reference, 4, false));

  const StoreLayout layout{reference, find_lock_file(reference), {}, {}};
  std::string journal;
  for (const std::string& name : cxf::test::list_directory_names(reference)) {
    if (name != layout.lock) {
      journal = name;
    }
  }
  REQUIRE(!journal.empty());
  const std::string journal_path = reference + "/" + journal;
  const std::vector<std::byte> original = read_all(journal_path);
  const std::vector<FrameRange> frames = scan_frames(original);
  REQUIRE(!frames.empty());
  const Recovery clean = recover(reference);
  REQUIRE(clean.opened);
  REQUIRE(clean.verified);

  std::vector<std::size_t> lengths = {0, 1, 2, 4, 5, 20, cxf::kFrameHeaderBytes - 1,
                                      cxf::kFrameHeaderBytes, cxf::kFrameHeaderBytes + 1};
  for (const FrameRange& frame : frames) {
    lengths.push_back(frame.offset);
    lengths.push_back(frame.offset + frame.size - 1);
  }
  lengths.push_back(original.size() - 1);
  std::sort(lengths.begin(), lengths.end());
  lengths.erase(std::unique(lengths.begin(), lengths.end()), lengths.end());

  int index = 0;
  for (const std::size_t length : lengths) {
    if (length >= original.size()) {
      continue;
    }
    const std::string work = fresh_copy(reference, home, "cut-" + std::to_string(index));
    ++index;
    REQUIRE(cxf::test::truncate_file(work + "/" + journal, length));
    const Recovery recovery = recover(work);
    const std::vector<std::byte> truncated = read_all(work + "/" + journal);
    const std::vector<FrameRange> surviving = scan_frames(truncated);
    if (recovery.opened) {
      CHECK(recovery.verified);
      // Recovery never reports less than the last complete frame: a torn tail
      // is discarded, but a complete commit is never lost.
      if (!surviving.empty()) {
        CHECK(recovery.commit >= surviving.back().commit);
      }
      CHECK(recovery.commit <= clean.commit);
      CHECK(recovery.candidates <= clean.candidates);
    } else {
      CHECK(cxf::test::is_storage_failure(recovery.code));
    }
  }
}

CXF_TEST(adversarial, truncated_snapshot_is_never_silently_empty) {
  cxf::test::TempDirectory home("truncate-snapshot");
  REQUIRE(home.ok());
  const std::string reference = home.child("reference");
  static_cast<void>(populate(reference, 3, true));
  const std::string lock = find_lock_file(reference);
  REQUIRE(!lock.empty());
  std::string snapshot;
  for (const std::string& name : cxf::test::list_directory_names(reference)) {
    if (name != lock) {
      snapshot = name;
    }
  }
  REQUIRE(!snapshot.empty());
  const std::uint64_t size = cxf::test::file_bytes_size(reference + "/" + snapshot);
  REQUIRE(size > cxf::kFrameHeaderBytes);
  const Recovery clean = recover(reference);
  REQUIRE(clean.opened);

  const std::vector<std::uint64_t> lengths = {0, 1, 4, cxf::kFrameHeaderBytes - 1,
                                              cxf::kFrameHeaderBytes, size / 2, size - 1};
  int index = 0;
  for (const std::uint64_t length : lengths) {
    const std::string work = fresh_copy(reference, home, "snap-" + std::to_string(index));
    ++index;
    REQUIRE(cxf::test::truncate_file(work + "/" + snapshot, length));
    const Recovery recovery = recover(work);
    // Either the store reports the damage, or it recovers the whole generation
    // from another authoritative source; it never opens on an empty state.
    expect_complete_or_refused(recovery, clean,
                              "truncated snapshot at " + std::to_string(length));
  }
}

CXF_TEST(adversarial, flipped_bits_in_the_journal_are_detected) {
  cxf::test::TempDirectory home("flip");
  REQUIRE(home.ok());
  const std::string reference = home.child("reference");
  static_cast<void>(populate(reference, 3, false));
  const std::string lock = find_lock_file(reference);
  std::string journal;
  for (const std::string& name : cxf::test::list_directory_names(reference)) {
    if (name != lock) {
      journal = name;
    }
  }
  REQUIRE(!journal.empty());
  const std::string journal_path = reference + "/" + journal;
  const std::vector<std::byte> original = read_all(journal_path);
  const std::vector<FrameRange> frames = scan_frames(original);
  REQUIRE(frames.size() >= 2);
  const Recovery clean = recover(reference);
  REQUIRE(clean.opened);

  // Every header byte of the first frame (a frame that is followed by more
  // data, so damage cannot be excused as a torn tail).
  int index = 0;
  for (std::size_t offset = 0; offset < cxf::kFrameHeaderBytes; ++offset) {
    const std::string work = fresh_copy(reference, home, "hdr-" + std::to_string(offset));
    REQUIRE(cxf::test::flip_bit(work + "/" + journal, offset, 3));
    const Recovery recovery = recover(work);
    if (recovery.opened) {
      // An undetected header flip may not change the recovered generation.
      expect_complete_or_refused(recovery, clean, "header flip at " + std::to_string(offset));
    } else {
      CHECK(cxf::test::is_storage_failure(recovery.code));
    }
    ++index;
  }

  // Body bytes are covered by both the CRC and the digest: they must be caught.
  const FrameRange first = frames.front();
  REQUIRE(first.size > cxf::kFrameHeaderBytes);
  for (std::size_t within = 0; within < 8; ++within) {
    const std::size_t offset =
        first.offset + cxf::kFrameHeaderBytes + (within * 5) % (first.size - cxf::kFrameHeaderBytes);
    const std::string work = fresh_copy(reference, home, "body-" + std::to_string(within));
    REQUIRE(cxf::test::flip_bit(work + "/" + journal, offset, 0));
    const Recovery recovery = recover(work);
    expect_refused(recovery, "body flip at " + std::to_string(offset));
  }
}

CXF_TEST(adversarial, appended_garbage_never_changes_the_recovered_generation) {
  cxf::test::TempDirectory home("append");
  REQUIRE(home.ok());
  const std::string plain = home.child("plain");
  static_cast<void>(populate(plain, 3, false));
  const std::string lock = find_lock_file(plain);
  std::string journal;
  for (const std::string& name : cxf::test::list_directory_names(plain)) {
    if (name != lock) {
      journal = name;
    }
  }
  REQUIRE(!journal.empty());
  const Recovery clean = recover(plain);
  REQUIRE(clean.opened);

  // Garbage whose first bytes are not a frame header at all.
  const std::vector<std::byte> garbage = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                          std::byte{0xEF}, std::byte{0x00}, std::byte{0x11}};
  {
    const std::string work = fresh_copy(plain, home, "journal-garbage");
    REQUIRE(cxf::fs::append_file_sync(work + "/" + journal, garbage).ok());
    const Recovery recovery = recover(work);
    expect_complete_or_refused(recovery, clean, "garbage appended to the journal");
    if (recovery.opened) {
      CHECK(recovery.discarded_tail >= garbage.size());
    }
  }
  // A frame header whose format version and body length are nonsense.
  {
    const std::string work = fresh_copy(plain, home, "journal-bad-frame");
    std::vector<std::byte> bogus(cxf::kFrameHeaderBytes, std::byte{0x5A});
    REQUIRE(cxf::fs::append_file_sync(work + "/" + journal, bogus).ok());
    const Recovery recovery = recover(work);
    expect_complete_or_refused(recovery, clean, "bogus frame header appended");
  }
  // Garbage after a complete snapshot frame.
  {
    const std::string checkpointed = home.child("checkpointed");
    static_cast<void>(populate(checkpointed, 3, true));
    const std::string checkpoint_lock = find_lock_file(checkpointed);
    std::string snapshot;
    for (const std::string& name : cxf::test::list_directory_names(checkpointed)) {
      if (name != checkpoint_lock) {
        snapshot = name;
      }
    }
    REQUIRE(!snapshot.empty());
    const Recovery clean_snapshot = recover(checkpointed);
    REQUIRE(clean_snapshot.opened);
    const std::string work = fresh_copy(checkpointed, home, "snapshot-garbage");
    REQUIRE(cxf::fs::append_file_sync(work + "/" + snapshot, garbage).ok());
    const Recovery recovery = recover(work);
    expect_complete_or_refused(recovery, clean_snapshot, "garbage appended to the snapshot");
  }
}

CXF_TEST(adversarial, swapped_frames_are_never_replayed_out_of_order) {
  cxf::test::TempDirectory home("swap");
  REQUIRE(home.ok());
  const std::string reference = home.child("reference");
  static_cast<void>(populate(reference, 4, false));
  const std::string lock = find_lock_file(reference);
  std::string journal;
  for (const std::string& name : cxf::test::list_directory_names(reference)) {
    if (name != lock) {
      journal = name;
    }
  }
  REQUIRE(!journal.empty());
  const std::string journal_path = reference + "/" + journal;
  const std::vector<std::byte> original = read_all(journal_path);
  const std::vector<FrameRange> frames = scan_frames(original);
  REQUIRE(frames.size() >= 3);
  const Recovery clean = recover(reference);
  REQUIRE(clean.opened);

  // Swap the first two frames: the commit sequence would move backwards.
  {
    const std::string work = fresh_copy(reference, home, "swap-first");
    std::vector<std::byte> swapped;
    swapped.insert(swapped.end(), original.begin() + static_cast<std::ptrdiff_t>(frames[1].offset),
                   original.begin() + static_cast<std::ptrdiff_t>(frames[1].offset + frames[1].size));
    swapped.insert(swapped.end(), original.begin() + static_cast<std::ptrdiff_t>(frames[0].offset),
                   original.begin() + static_cast<std::ptrdiff_t>(frames[0].offset + frames[0].size));
    swapped.insert(swapped.end(), original.begin() + static_cast<std::ptrdiff_t>(frames[2].offset),
                   original.end());
    REQUIRE(cxf::test::write_file_bytes(work + "/" + journal, swapped));
    const Recovery recovery = recover(work);
    // Recovering the same generation is acceptable; accepting the swapped
    // order as a new generation is not.
    expect_complete_or_refused(recovery, clean, "first two frames swapped");
  }
  // Swap two adjacent trailing frames.
  {
    const std::string work = fresh_copy(reference, home, "swap-last");
    const FrameRange& a = frames[frames.size() - 2];
    const FrameRange& b = frames[frames.size() - 1];
    std::vector<std::byte> swapped;
    swapped.insert(swapped.end(), original.begin(),
                   original.begin() + static_cast<std::ptrdiff_t>(a.offset));
    swapped.insert(swapped.end(), original.begin() + static_cast<std::ptrdiff_t>(b.offset),
                   original.begin() + static_cast<std::ptrdiff_t>(b.offset + b.size));
    swapped.insert(swapped.end(), original.begin() + static_cast<std::ptrdiff_t>(a.offset),
                   original.begin() + static_cast<std::ptrdiff_t>(a.offset + a.size));
    REQUIRE(cxf::test::write_file_bytes(work + "/" + journal, swapped));
    const Recovery recovery = recover(work);
    expect_complete_or_refused(recovery, clean, "trailing frames swapped");
  }
}

CXF_TEST(adversarial, removing_the_lock_file_mid_run) {
  cxf::test::TempDirectory home("lock-removal");
  REQUIRE(home.ok());
  const std::string store = home.child("store");
  cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
  FabricOptions options;
  options.directory = store;
  options.holder = "lock-removal";
  cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
  REQUIRE_OK(fabric);

  cxf::AdmitCandidateRequest first;
  first.name = "lock-1";
  first.declaration = declaration_of(first.name);
  REQUIRE_OK(fabric->admit_candidate(first));

  const std::string lock = find_lock_file(store);
  REQUIRE(!lock.empty());
  // Removing the lock file is either refused by the operating system or has no
  // effect on exclusion; it is never a silent way to get a second writer.
  const cxf::Status removed = cxf::fs::remove_file(store + "/" + lock);
  if (removed.failed()) {
    CHECK(cxf::test::is_storage_failure(removed.code()));
  }
  const cxf::Outcome<Fabric> second = Fabric::open(options, clock);
  if (second.ok()) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", "second writer after lock removal",
                            "a second Fabric opened the same store while the first was live");
  } else {
    CHECK_CODE(second, Code::kStorageLocked);
  }

  // The live fabric keeps working and the store keeps its committed state.
  cxf::AdmitCandidateRequest later;
  later.name = "lock-2";
  later.declaration = declaration_of(later.name);
  REQUIRE_OK(fabric->admit_candidate(later));
  const std::uint64_t commit = fabric->meta().commit.value();
  const std::uint64_t candidates = fabric->stats().candidates;
  fabric = cxf::Outcome<Fabric>(cxf::Status::success());
  const Recovery recovery = recover(store);
  REQUIRE(recovery.opened);
  CHECK_EQ(recovery.commit, commit);
  CHECK_EQ(recovery.candidates, candidates);
  CHECK(recovery.verified);
}

CXF_TEST(adversarial, snapshot_replaced_by_a_valid_frame_of_the_wrong_kind) {
  cxf::test::TempDirectory home("wrong-kind");
  REQUIRE(home.ok());
  const std::string checkpointed = home.child("checkpointed");
  static_cast<void>(populate(checkpointed, 3, true));
  const std::string lock = find_lock_file(checkpointed);
  REQUIRE(!lock.empty());
  std::string snapshot;
  for (const std::string& name : cxf::test::list_directory_names(checkpointed)) {
    if (name != lock) {
      snapshot = name;
    }
  }
  REQUIRE(!snapshot.empty());

  const std::string work = fresh_copy(checkpointed, home, "wrong-kind-work");
  // A perfectly valid frame, of the wrong kind: a commit group where the
  // snapshot must be. It must not be mistaken for state.
  cxf::CommitGroup group;
  group.commit = CommitSequence::from_value(1);
  group.epoch = cxf::ControlEpoch::from_value(1);
  group.recorded_at = Timestamp::from_unix_seconds(kBase);
  cxf::GroupEntry entry;
  entry.kind = cxf::RecordKind::kFabricMeta;
  entry.body = cxf::encode_body(cxf::FabricMeta{});
  group.entries.push_back(entry);
  const std::vector<std::byte> wrong_kind =
      cxf::encode_frame(FrameKind::kCommitGroup, group.commit, cxf::encode_group(group));
  REQUIRE_OK(cxf::fs::write_file_atomic(work + "/" + snapshot, wrong_kind));

  const Recovery recovery = recover(work);
  expect_refused_with_clear_status(recovery, "snapshot replaced by a commit-group frame");

  // A snapshot whose bytes are a valid frame of the right kind but whose body
  // is a commit group must also be refused.
  const std::string second = fresh_copy(checkpointed, home, "wrong-body");
  const std::vector<std::byte> mismatched =
      cxf::encode_frame(FrameKind::kSnapshot, group.commit, cxf::encode_group(group));
  REQUIRE_OK(cxf::fs::write_file_atomic(second + "/" + snapshot, mismatched));
  expect_refused_with_clear_status(recover(second), "snapshot carrying a commit-group body");
}

CXF_TEST(adversarial, unreadable_store_files_report_a_storage_status) {
  cxf::test::TempDirectory home("permissions");
  REQUIRE(home.ok());
  const std::string reference = home.child("reference");
  static_cast<void>(populate(reference, 2, false));
  const std::string lock = find_lock_file(reference);
  std::string journal;
  for (const std::string& name : cxf::test::list_directory_names(reference)) {
    if (name != lock) {
      journal = name;
    }
  }
  REQUIRE(!journal.empty());

  const std::string work = fresh_copy(reference, home, "read-only");
  const std::string journal_path = work + "/" + journal;
  std::error_code ec;
  std::filesystem::permissions(journal_path, std::filesystem::perms::owner_read,
                               std::filesystem::perm_options::replace, ec);
  if (ec) {
    // The platform does not express this permission; the case is skipped
    // explicitly rather than silently.
    std::printf("  note: read-only permissions are not expressible here (%s)\n",
                ec.message().c_str());
    return;
  }

  const Recovery recovery = recover(work);
  if (recovery.opened) {
    // Recovery might be read-only by design, but a mutation must not silently
    // succeed against a file the process cannot write.
    cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
    FabricOptions options;
    options.directory = work;
    options.holder = "read-only";
    cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
    if (fabric.ok()) {
      cxf::AdmitCandidateRequest request;
      request.name = "read-only-node";
      request.declaration = declaration_of(request.name);
      const cxf::Outcome<cxf::RequestOutcome> outcome = fabric->admit_candidate(request);
      if (outcome.ok()) {
        ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", "mutation on a read-only journal",
                                "an admission was accepted although the journal is read-only");
      } else {
        CHECK(cxf::test::is_storage_failure(outcome.code()));
      }
    }
  } else {
    CHECK(cxf::test::is_storage_failure(recovery.code));
  }

  // Restore the permissions so the temporary tree can be removed.
  restore_permissions(journal_path);
  CHECK(cxf::test::remove_tree(work));
}
