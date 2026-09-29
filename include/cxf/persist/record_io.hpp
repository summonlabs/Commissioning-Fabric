// Commissioning Fabric - durable frame format.
//
// Every durable byte the runtime writes is one framed record. The frame carries
// the format version, the frame kind, the commit sequence it belongs to, the
// body length, a CRC-32C and a 256-bit digest of the body. Decoding validates
// all of them: a frame whose declared length, checksum, digest, reserved fields
// or format version disagree with its bytes is rejected rather than partially
// used.
#ifndef CXF_PERSIST_RECORD_IO_HPP
#define CXF_PERSIST_RECORD_IO_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cxf/model/record.hpp"
#include "cxf/support/clock.hpp"
#include "cxf/support/fs.hpp"
#include "cxf/support/status.hpp"
#include "cxf/types/generations.hpp"

namespace cxf {

/// Frame kinds. Values are part of the durable format.
enum class FrameKind : std::uint16_t {
  kCommitGroup = 1,
  kSnapshot = 2,
};

[[nodiscard]] std::string_view frame_kind_name(FrameKind kind) noexcept;
[[nodiscard]] Outcome<FrameKind> parse_frame_kind(std::string_view text);
[[nodiscard]] constexpr bool enum_value_valid(FrameKind kind) noexcept {
  return kind == FrameKind::kCommitGroup || kind == FrameKind::kSnapshot;
}

inline constexpr std::uint32_t kFrameMagic = 0x31465843u;  // "CXF1"
inline constexpr std::size_t kFrameHeaderBytes = 4 + 2 + 2 + 2 + 8 + 4 + 4 + 32;  // 58
inline constexpr std::uint32_t kMaxFrameBodyBytes = 64u * 1024u * 1024u;

/// One record inside a commit group: its kind and its canonical body.
struct GroupEntry {
  RecordKind kind{RecordKind::kFabricMeta};
  std::vector<std::byte> body{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("kind", kind);
    ar("body", body);
  }
};

/// The unit of atomic publication: all entries of one accepted mutation are
/// written as a single frame, so a torn write can never publish half of it.
struct CommitGroup {
  CommitSequence commit{};
  ControlEpoch epoch{};
  Timestamp recorded_at{};
  std::vector<GroupEntry> entries{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("commit", commit);
    ar("epoch", epoch);
    ar("recorded_at", recorded_at);
    ar("entries", entries);
  }
};

/// Encode a frame: header followed by the body bytes.
[[nodiscard]] std::vector<std::byte> encode_frame(FrameKind kind, CommitSequence commit,
                                                  std::span<const std::byte> body);

/// A decoded frame, with the number of bytes it occupied.
struct DecodedFrame {
  FrameKind kind{FrameKind::kCommitGroup};
  CommitSequence commit{};
  std::vector<std::byte> body{};
  std::size_t bytes_consumed{0};

  /// Decode the group carried by this frame.
  [[nodiscard]] Outcome<CommitGroup> group() const;
};

/// Outcome of attempting to decode one frame at the front of a buffer.
enum class FrameStatus : std::uint8_t {
  kOk = 0,
  /// Fewer bytes are present than the frame declares: a torn tail, not a
  /// corruption, because the writer never publishes a partial frame.
  kIncomplete = 1,
  /// The bytes are present but invalid: format, checksum, digest or reserved
  /// fields disagree.
  kInvalid = 2,
};

struct FrameDecode {
  FrameStatus status{FrameStatus::kOk};
  DecodedFrame frame{};
  Status error{};
};

/// Decode one frame from the front of the buffer.
[[nodiscard]] FrameDecode decode_frame(std::span<const std::byte> data);

/// Canonically encode a commit group (used by both the journal and the
/// snapshot).
[[nodiscard]] std::vector<std::byte> encode_group(const CommitGroup& group);

/// Decode a commit group body, rejecting trailing bytes.
[[nodiscard]] Outcome<CommitGroup> decode_group(std::span<const std::byte> body);

}  // namespace cxf

#endif  // CXF_PERSIST_RECORD_IO_HPP
