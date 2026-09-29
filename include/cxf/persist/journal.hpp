// Commissioning Fabric - append-only commit journal.
//
// The journal is the authority for durability between snapshots. A commit is
// durable only after its frame has been flushed to the device and read back
// successfully; the durable fence advances only after that verification. On
// recovery, a final frame that is incomplete is a torn write and is discarded
// with a note, while an invalid frame that is followed by more data is
// corruption and stops recovery.
#ifndef CXF_PERSIST_JOURNAL_HPP
#define CXF_PERSIST_JOURNAL_HPP

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cxf/persist/record_io.hpp"
#include "cxf/support/fs.hpp"
#include "cxf/support/status.hpp"

namespace cxf {

/// Appending writer over one journal file.
class Journal {
 public:
  Journal() = default;
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;
  Journal(Journal&&) noexcept;
  Journal& operator=(Journal&&) noexcept;
  ~Journal();

  /// Open (creating when absent) and position the writer at the end.
  [[nodiscard]] static Outcome<Journal> open(const fs::Path& path);

  [[nodiscard]] const fs::Path& path() const noexcept { return path_; }
  [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
  [[nodiscard]] bool valid() const noexcept { return file_.valid(); }

  /// Append bytes at the end of the file. Does not flush.
  [[nodiscard]] Status append(std::span<const std::byte> bytes);

  /// Flush file data and metadata to the device.
  [[nodiscard]] Status flush();

  /// Read back the frame that starts at offset and verify it decodes exactly to
  /// the expected commit sequence.
  [[nodiscard]] Status verify_frame_at(std::uint64_t offset, CommitSequence expected) const;

  /// Reset the journal to zero length. Used only after a snapshot has been
  /// published and a subsequent commit has been written.
  [[nodiscard]] Status reset();

  /// Read every frame from the file, classifying a trailing torn frame.
  [[nodiscard]] Status read_all(std::vector<DecodedFrame>& frames,
                                std::uint64_t& discarded_tail_bytes,
                                std::string& tail_note) const;

 private:
  fs::Path path_{};
  fs::File file_{};
  std::uint64_t bytes_{0};
};

}  // namespace cxf

#endif  // CXF_PERSIST_JOURNAL_HPP
