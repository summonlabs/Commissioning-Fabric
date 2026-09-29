// Commissioning Fabric - append-only commit journal.
//
// The journal is the authority for durability between snapshots: a commit is
// durable only after its frame has been flushed and read back successfully. A
// torn final frame is discarded with a note; an invalid frame followed by more
// data is corruption and stops recovery.
#include "cxf/persist/journal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cxf/persist/record_io.hpp"
#include "cxf/support/fs.hpp"
#include "cxf/support/serial.hpp"
#include "cxf/support/status.hpp"
#include "cxf/types/generations.hpp"

namespace cxf {
namespace {

/// Largest journal a single recovery reads. A longer journal is reported as too
/// long rather than read into memory without a bound.
constexpr std::uint64_t kMaxJournalReadBytes = 512ull * 1024ull * 1024ull;

// Offset of the declared body length inside the frame header; the header size
// itself is kFrameHeaderBytes. See record_io.cpp for the complete frame layout.
constexpr std::size_t kBodyLengthOffset = 4 + 2 + 2 + 2 + 8;

[[nodiscard]] Status journal_not_open(const fs::Path& path) {
  return make_error(Code::kPreconditionViolated, "journal is not open", path);
}

/// A frame read back from the journal is either exactly what was written or a
/// failure of the durable bytes. The decoder's own storage diagnosis is
/// preserved; a buffer-shape diagnosis is reported as corruption of the store.
[[nodiscard]] Status readback_failure(const Status& cause, std::string_view what,
                                      const std::string& context) {
  const Code code = is_storage_code(cause.code()) ? cause.code() : Code::kStorageCorrupt;
  std::string detail(what);
  if (!cause.detail().empty()) {
    detail += ": ";
    detail += cause.detail();
  }
  return make_error(code, std::move(detail), context);
}

}  // namespace

Journal::Journal(Journal&&) noexcept = default;
Journal& Journal::operator=(Journal&&) noexcept = default;
Journal::~Journal() = default;

Outcome<Journal> Journal::open(const fs::Path& path) {
  Outcome<fs::File> file = fs::File::open_append(path);
  if (!file.ok()) {
    // A journal that cannot be created or opened leaves the store without its
    // durability authority, which is reported as the store being unavailable.
    return make_error(Code::kStorageUnavailable, "journal could not be created or opened",
                      path + " " + file.status().message());
  }
  const Outcome<std::uint64_t> size = file.value().size();
  if (!size.ok()) {
    return size.status();
  }
  Journal journal;
  journal.path_ = path;
  journal.file_ = std::move(file.value());
  journal.bytes_ = size.value();
  return journal;
}

Status Journal::append(std::span<const std::byte> bytes) {
  if (!file_.valid()) {
    return journal_not_open(path_);
  }
  // Write at the tracked end of the journal rather than through File::append.
  // File::append writes at the handle's current position, and File::read_at
  // advances that position on Windows, so a read-back verification in between
  // (which every published commit performs) would make the next append
  // overwrite an existing frame instead of extending the journal.
  const Status written = file_.write_at(bytes_, bytes);
  if (written.failed()) {
    // bytes_ is left at the offset the frame was written from: the commit was
    // never published, and the next append must overwrite the torn bytes.
    return written;
  }
  const Outcome<std::uint64_t> size = file_.size();
  if (!size.ok()) {
    return size.status();
  }
  const std::uint64_t expected = bytes_ + static_cast<std::uint64_t>(bytes.size());
  if (size.value() != expected) {
    // The write reported success but the file is not the length it must be.
    // Record what the file really holds; never claim bytes that did not land.
    bytes_ = size.value();
    return make_error(Code::kStorageIo, "journal append did not extend the file as written",
                      path_ + " expected=" + std::to_string(expected) +
                          " actual=" + std::to_string(size.value()));
  }
  bytes_ = expected;
  return Status::success();
}

Status Journal::flush() {
  if (!file_.valid()) {
    return journal_not_open(path_);
  }
  return file_.flush();
}

Status Journal::verify_frame_at(std::uint64_t offset, CommitSequence expected) const {
  if (!file_.valid()) {
    return journal_not_open(path_);
  }
  const std::string context = path_ + " offset=" + std::to_string(offset);

  std::array<std::byte, kFrameHeaderBytes> header{};
  const Outcome<std::size_t> header_read = file_.read_at(offset, header);
  if (!header_read.ok()) {
    return header_read.status();
  }
  if (header_read.value() != kFrameHeaderBytes) {
    return make_error(Code::kStorageCorrupt, "journal frame header did not read back complete",
                      context + " bytes=" + std::to_string(header_read.value()));
  }

  // The declared body length is read from the header before the body is read,
  // so an implausible length can never drive an unbounded allocation.
  const std::uint32_t body_len = get_le32(header.data() + kBodyLengthOffset);
  if (body_len > kMaxFrameBodyBytes) {
    return make_error(Code::kFieldTooLong, "journal frame declares a body beyond the bound",
                      context + " bytes=" + std::to_string(body_len));
  }

  std::vector<std::byte> frame(kFrameHeaderBytes + static_cast<std::size_t>(body_len));
  std::copy(header.begin(), header.end(), frame.begin());
  if (body_len > 0) {
    const std::span<std::byte> body(frame.data() + kFrameHeaderBytes, body_len);
    const Outcome<std::size_t> body_read = file_.read_at(offset + kFrameHeaderBytes, body);
    if (!body_read.ok()) {
      return body_read.status();
    }
    if (body_read.value() != body_len) {
      return make_error(Code::kStorageCorrupt, "journal frame body did not read back complete",
                        context + " bytes=" + std::to_string(body_read.value()));
    }
  }

  const FrameDecode decoded = decode_frame(frame);
  if (decoded.status != FrameStatus::kOk) {
    return readback_failure(decoded.error, "journal frame did not verify after it was written",
                            context);
  }
  if (decoded.frame.commit != expected) {
    return make_error(Code::kStorageCorrupt, "journal frame carries an unexpected commit",
                      context + " expected=" + expected.str() +
                          " actual=" + decoded.frame.commit.str());
  }
  const Outcome<CommitGroup> group = decoded.frame.group();
  if (!group.ok()) {
    return readback_failure(group.status(), "journal frame body is not a readable commit group",
                            context);
  }
  return Status::success();
}

Status Journal::reset() {
  if (!file_.valid()) {
    return journal_not_open(path_);
  }
  const Status truncated = file_.truncate(0);
  if (truncated.failed()) {
    return truncated;
  }
  const Status flushed = file_.flush();
  if (flushed.failed()) {
    return flushed;
  }
  const Outcome<std::uint64_t> size = file_.size();
  if (!size.ok()) {
    return size.status();
  }
  if (size.value() != 0) {
    // The reset did not take effect: report the file's real extent instead of
    // assuming an empty journal.
    bytes_ = size.value();
    return make_error(Code::kStorageIo, "journal reset did not truncate the file",
                      path_ + " bytes=" + std::to_string(size.value()));
  }
  bytes_ = 0;
  return Status::success();
}

Status Journal::read_all(std::vector<DecodedFrame>& frames, std::uint64_t& discarded_tail_bytes,
                         std::string& tail_note) const {
  if (!file_.valid()) {
    return journal_not_open(path_);
  }
  const Outcome<std::uint64_t> size = file_.size();
  if (!size.ok()) {
    return size.status();
  }
  if (size.value() > kMaxJournalReadBytes) {
    return make_error(Code::kFieldTooLong, "journal exceeds the accepted read bound",
                      path_ + " bytes=" + std::to_string(size.value()));
  }

  const auto total = static_cast<std::size_t>(size.value());
  std::vector<std::byte> data(total);
  if (total != 0) {
    const Outcome<std::size_t> read = file_.read_at(0, std::span<std::byte>(data));
    if (!read.ok()) {
      return read.status();
    }
    if (read.value() != total) {
      return make_error(Code::kStorageCorrupt, "journal did not read back completely",
                        path_ + " bytes=" + std::to_string(read.value()));
    }
  }

  // The out-parameters are written only when the whole journal has been read:
  // a caller that receives an error must not see a partially filled result.
  std::vector<DecodedFrame> decoded_frames;
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::span<const std::byte> remaining(data.data() + offset, data.size() - offset);
    FrameDecode decoded = decode_frame(remaining);
    if (decoded.status == FrameStatus::kOk) {
      const std::size_t consumed = decoded.frame.bytes_consumed;
      decoded_frames.push_back(std::move(decoded.frame));
      offset += consumed;
      continue;
    }
    const auto left = static_cast<std::uint64_t>(data.size() - offset);
    if (decoded.status == FrameStatus::kIncomplete) {
      // The writer publishes a frame only after reading it back, so a frame that
      // is still incomplete at the end of the journal is a torn write, not
      // authority.
      frames = std::move(decoded_frames);
      discarded_tail_bytes = left;
      tail_note = "incomplete frame at end of journal (torn write), discarded";
      return Status::success();
    }
    const auto declared = static_cast<std::uint64_t>(decoded.frame.bytes_consumed);
    if (declared == left && declared > kFrameHeaderBytes) {
      // The frame claims to end exactly at the end of the journal and its bytes
      // do not verify: the shape a torn write of the final frame leaves behind.
      frames = std::move(decoded_frames);
      discarded_tail_bytes = left;
      tail_note = "invalid final frame discarded";
      return Status::success();
    }
    // An invalid frame that is followed by more data is corruption: skipping it
    // would silently drop durable authority.
    return readback_failure(decoded.error, "journal frame did not decode",
                            path_ + " offset=" + std::to_string(offset));
  }

  frames = std::move(decoded_frames);
  discarded_tail_bytes = 0;
  tail_note.clear();
  return Status::success();
}

}  // namespace cxf
