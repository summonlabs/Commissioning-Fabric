// Commissioning Fabric - atomic snapshot publication.
//
// A snapshot is published by writing a staged file, flushing it, reading it
// back and verifying it, and only then renaming it over the live name. Until
// the rename happens the previous snapshot is still the authoritative
// generation; after it, the new one is. Readers therefore never observe a torn
// or partially written generation.
#ifndef CXF_PERSIST_SNAPSHOT_HPP
#define CXF_PERSIST_SNAPSHOT_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cxf/persist/record_io.hpp"
#include "cxf/support/fs.hpp"
#include "cxf/support/status.hpp"

namespace cxf {

class SnapshotStore {
 public:
  explicit SnapshotStore(fs::Path path) : path_(std::move(path)) {}

  [[nodiscard]] const fs::Path& path() const noexcept { return path_; }

  /// Write, flush, verify and publish the frame. Returns only once the frame is
  /// durable under the live name.
  [[nodiscard]] Status publish(std::span<const std::byte> frame);

  /// Load the published snapshot. Reports absence distinctly from failure.
  [[nodiscard]] Outcome<std::optional<DecodedFrame>> load(std::uint64_t max_bytes) const;

  [[nodiscard]] Outcome<bool> exists() const;
  [[nodiscard]] Outcome<std::uint64_t> size() const;

 private:
  fs::Path path_{};
};

}  // namespace cxf

#endif  // CXF_PERSIST_SNAPSHOT_HPP
