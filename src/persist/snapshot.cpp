// Commissioning Fabric - atomic snapshot publication.
//
// A snapshot is published by writing a staged file, flushing it, reading it back
// and verifying it, and only then renaming it over the live name. Readers
// therefore never observe a torn or partially written generation: the file is
// only ever the previous snapshot or the new one.
#include "cxf/persist/snapshot.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cxf/persist/record_io.hpp"
#include "cxf/support/fs.hpp"
#include "cxf/support/status.hpp"

namespace cxf {
namespace {

/// Bytes of a published file either decode exactly or the file is unreadable.
/// The decoder's own storage diagnosis is preserved; a buffer-shape diagnosis
/// means the published file does not carry a frame at all.
[[nodiscard]] Status published_failure(const Status& cause, std::string_view what,
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

Status SnapshotStore::publish(std::span<const std::byte> frame) {
  const FrameDecode decoded = decode_frame(frame);
  if (decoded.status != FrameStatus::kOk) {
    // The frame is refused before anything is staged: bytes that do not decode
    // must never replace the generation that does.
    if (decoded.error.failed()) {
      return decoded.error;
    }
    return make_error(Code::kMalformedInput, "snapshot frame does not decode", path_);
  }
  if (decoded.frame.kind != FrameKind::kSnapshot) {
    return make_error(Code::kFieldConflict, "only a snapshot frame may be published as snapshot",
                      path_ + " kind=" + std::string(frame_kind_name(decoded.frame.kind)));
  }
  if (decoded.frame.bytes_consumed != frame.size()) {
    return make_error(Code::kTrailingBytes, "snapshot frame does not consume the whole buffer",
                      path_ + " frame=" + std::to_string(decoded.frame.bytes_consumed) +
                          " buffer=" + std::to_string(frame.size()));
  }
  // Staged, flushed, read back, verified and renamed: the live name changes only
  // once the new generation is durable.
  return fs::write_file_atomic(path_, frame);
}

Outcome<std::optional<DecodedFrame>> SnapshotStore::load(std::uint64_t max_bytes) const {
  const Outcome<std::vector<std::byte>> bytes = fs::read_file(path_, max_bytes);
  if (!bytes.ok()) {
    if (bytes.code() == Code::kStorageUnavailable) {
      // Absence is reported distinctly from failure: nothing has been published.
      return std::optional<DecodedFrame>{};
    }
    return bytes.status();
  }
  const std::span<const std::byte> data(bytes.value());
  FrameDecode decoded = decode_frame(data);
  if (decoded.status != FrameStatus::kOk) {
    return published_failure(decoded.error, "published snapshot does not hold a complete frame",
                             path_);
  }
  if (decoded.frame.kind != FrameKind::kSnapshot) {
    return make_error(Code::kFieldConflict, "published snapshot holds a foreign frame kind",
                      path_ + " kind=" + std::string(frame_kind_name(decoded.frame.kind)));
  }
  if (decoded.frame.bytes_consumed != data.size()) {
    return make_error(Code::kTrailingBytes, "published snapshot holds bytes after its frame",
                      path_ + " frame=" + std::to_string(decoded.frame.bytes_consumed) +
                          " file=" + std::to_string(data.size()));
  }
  return std::optional<DecodedFrame>(std::move(decoded.frame));
}

Outcome<bool> SnapshotStore::exists() const { return fs::exists(path_); }

Outcome<std::uint64_t> SnapshotStore::size() const { return fs::file_size(path_); }

}  // namespace cxf
