// Commissioning Fabric - durable frame encoding and decoding.
//
// A frame is the unit of atomic publication: a fixed 58-byte little-endian
// header followed by the canonical body. Encoding refuses to produce a frame a
// decoder could not read back; decoding validates every header field, the body
// length, the checksum and the digest before a single body byte is accepted.
#include "cxf/persist/record_io.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cxf/codec/archive.hpp"
#include "cxf/runtime/fabric.hpp"
#include "cxf/support/crc32c.hpp"
#include "cxf/support/digest.hpp"
#include "cxf/support/serial.hpp"
#include "cxf/support/status.hpp"
#include "cxf/types/generations.hpp"

namespace cxf {
namespace {

// Offsets inside the fixed frame header. They are part of the durable format.
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kFormatOffset = 4;
constexpr std::size_t kKindOffset = 6;
constexpr std::size_t kReservedOffset = 8;
constexpr std::size_t kCommitOffset = 10;
constexpr std::size_t kBodyLengthOffset = 18;
constexpr std::size_t kBodyCrc32cOffset = 22;
constexpr std::size_t kBodyDigestOffset = 26;

// The fields must be laid out end to end with no gap, and the digest must end
// exactly where the body begins.
static_assert(kCommitOffset + 8 == kBodyLengthOffset, "body length follows the commit field");
static_assert(kBodyLengthOffset + 4 == kBodyCrc32cOffset, "checksum follows the body length");
static_assert(kBodyCrc32cOffset + 4 == kBodyDigestOffset, "digest follows the checksum");
static_assert(kBodyDigestOffset + Digest::kBytes == kFrameHeaderBytes,
              "frame header offsets must cover exactly kFrameHeaderBytes bytes");

/// Little-endian u16. serial.hpp exposes put_le32/put_le64 only.
void put_le16(std::byte* out, std::uint16_t v) noexcept {
  out[0] = static_cast<std::byte>(v & 0xFFu);
  out[1] = static_cast<std::byte>((v >> 8u) & 0xFFu);
}

[[nodiscard]] std::uint16_t get_le16(const std::byte* in) noexcept {
  const auto lo = static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[0]));
  const auto hi = static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[1]));
  return static_cast<std::uint16_t>(lo | static_cast<std::uint16_t>(hi << 8u));
}

/// Lowercase-hex nibble value, or -1 when the character is not lowercase hex.
[[nodiscard]] int hex_nibble(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

/// Write the 32 raw digest bytes that a canonical 64-character lowercase-hex
/// spelling denotes. A non-canonical spelling is refused, so a miscomputed
/// digest can never be written as a plausible-looking field.
[[nodiscard]] bool put_digest_bytes(std::byte* out, std::string_view hex) noexcept {
  if (hex.size() != Digest::kBytes * 2) {
    return false;
  }
  for (std::size_t i = 0; i < Digest::kBytes; ++i) {
    const int hi = hex_nibble(hex[i * 2]);
    const int lo = hex_nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out[i] = static_cast<std::byte>(static_cast<unsigned>((hi << 4) | lo));
  }
  return true;
}

}  // namespace

std::string_view frame_kind_name(FrameKind kind) noexcept {
  switch (kind) {
    case FrameKind::kCommitGroup: return "commit_group";
    case FrameKind::kSnapshot: return "snapshot";
  }
  return "invalid";
}

Outcome<FrameKind> parse_frame_kind(std::string_view text) {
  if (text == "commit_group") {
    return FrameKind::kCommitGroup;
  }
  if (text == "snapshot") {
    return FrameKind::kSnapshot;
  }
  return make_error(Code::kFieldOutOfRange, "unknown frame kind", std::string(text));
}

std::vector<std::byte> encode_frame(FrameKind kind, CommitSequence commit,
                                    std::span<const std::byte> body) {
  // A frame the decoder would reject is never produced: callers treat the empty
  // result as an encoding failure and refuse to publish the commit.
  //
  // Commit zero is accepted: a snapshot of a store that has published nothing
  // yet is a legitimate generation, and refusing it turned an ordinary
  // "init, then checkpoint" into an internal error.
  if (!enum_value_valid(kind) || body.size() > kMaxFrameBodyBytes) {
    return {};
  }
  const Digest digest = DigestBuilder::of(body);
  std::vector<std::byte> frame(kFrameHeaderBytes + body.size());
  std::byte* const header = frame.data();
  put_le32(header + kMagicOffset, kFrameMagic);
  put_le16(header + kFormatOffset, kStoreFormatVersion);
  put_le16(header + kKindOffset, static_cast<std::uint16_t>(kind));
  put_le16(header + kReservedOffset, 0);
  put_le64(header + kCommitOffset, commit.value());
  put_le32(header + kBodyLengthOffset, static_cast<std::uint32_t>(body.size()));
  put_le32(header + kBodyCrc32cOffset, Crc32c::compute(body));
  if (!put_digest_bytes(header + kBodyDigestOffset, digest.view())) {
    return {};
  }
  std::copy(body.begin(), body.end(),
            frame.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes));
  return frame;
}

Outcome<CommitGroup> DecodedFrame::group() const {
  // Only the two frame kinds that carry a commit group may be decoded as one;
  // any other kind is a conflict rather than a body to reinterpret.
  if (kind != FrameKind::kCommitGroup && kind != FrameKind::kSnapshot) {
    return make_error(Code::kFieldConflict, "frame kind carries no commit group",
                      std::string(frame_kind_name(kind)));
  }
  Outcome<CommitGroup> decoded = decode_group(body);
  if (!decoded.ok()) {
    return decoded.status();
  }
  if (decoded.value().commit != commit) {
    // The header is the authority for the commit position; a body that claims a
    // different one would let a record be attributed to the wrong commit.
    return make_error(Code::kFieldConflict, "frame header and commit group disagree on commit",
                      "header=" + commit.str() + " group=" + decoded.value().commit.str());
  }
  return decoded;
}

FrameDecode decode_frame(std::span<const std::byte> data) {
  FrameDecode decoded;

  // 1. The header must be present in full before any field of it is believed.
  if (data.size() < kFrameHeaderBytes) {
    decoded.status = FrameStatus::kIncomplete;
    decoded.frame.bytes_consumed = 0;
    decoded.error = make_error(Code::kMalformedInput, "buffer ends before the frame header",
                               "available=" + std::to_string(data.size()));
    return decoded;
  }

  const std::byte* const bytes = data.data();
  const std::uint32_t body_len = get_le32(bytes + kBodyLengthOffset);
  // The declared extent is reported whenever the length field is present, so a
  // caller can tell a frame that declares the rest of the file (a torn tail)
  // from a garbage length in the middle. An implausible length is reported as
  // the header alone so that no arithmetic on it can overflow.
  const std::size_t declared_total =
      body_len > kMaxFrameBodyBytes ? kFrameHeaderBytes
                                    : kFrameHeaderBytes + static_cast<std::size_t>(body_len);
  decoded.frame.bytes_consumed = declared_total;

  // 2. Magic.
  const std::uint32_t magic = get_le32(bytes + kMagicOffset);
  if (magic != kFrameMagic) {
    decoded.status = FrameStatus::kInvalid;
    decoded.error = make_error(Code::kStorageCorrupt, "frame magic does not match the format",
                               "magic=" + std::to_string(magic));
    return decoded;
  }

  // 3. Format version written by a build that may not be this one.
  const std::uint16_t format = get_le16(bytes + kFormatOffset);
  if (format != kStoreFormatVersion) {
    decoded.status = FrameStatus::kInvalid;
    decoded.error =
        make_error(Code::kStorageUnsupportedFormat, "frame format version is not supported",
                   "format=" + std::to_string(format) +
                       " expected=" + std::to_string(kStoreFormatVersion));
    return decoded;
  }

  // 4. Reserved field must be zero: a non-zero value means bytes this build
  // does not understand, which must never be interpreted as zero.
  const std::uint16_t reserved = get_le16(bytes + kReservedOffset);
  if (reserved != 0) {
    decoded.status = FrameStatus::kInvalid;
    decoded.error = make_error(Code::kReservedFieldNonZero, "frame reserved field is not zero",
                               "reserved=" + std::to_string(reserved));
    return decoded;
  }

  // 5. Frame kind must be one of the declared kinds.
  const std::uint16_t kind_value = get_le16(bytes + kKindOffset);
  const auto kind = static_cast<FrameKind>(kind_value);
  if (!enum_value_valid(kind)) {
    decoded.status = FrameStatus::kInvalid;
    decoded.error = make_error(Code::kFieldOutOfRange, "frame kind is outside the declared domain",
                               "kind=" + std::to_string(kind_value));
    return decoded;
  }

  // 6. The declared body must be within the accepted bound. The length was
  // readable, so the header alone is reported as consumed.
  if (body_len > kMaxFrameBodyBytes) {
    decoded.status = FrameStatus::kInvalid;
    decoded.frame.bytes_consumed = kFrameHeaderBytes;
    decoded.error = make_error(Code::kFieldTooLong, "frame body exceeds the accepted bound",
                               "bytes=" + std::to_string(body_len));
    return decoded;
  }

  // 7. The whole declared frame must be present.
  const std::size_t total = kFrameHeaderBytes + static_cast<std::size_t>(body_len);
  decoded.frame.bytes_consumed = total;
  if (data.size() < total) {
    decoded.status = FrameStatus::kIncomplete;
    decoded.error = make_error(Code::kMalformedInput, "buffer ends inside the declared frame",
                               "declared=" + std::to_string(total) +
                                   " available=" + std::to_string(data.size()));
    return decoded;
  }

  const std::span<const std::byte> body = data.subspan(kFrameHeaderBytes, body_len);

  // 8. Checksum.
  const std::uint32_t stored_crc = get_le32(bytes + kBodyCrc32cOffset);
  const std::uint32_t computed_crc = Crc32c::compute(body);
  if (stored_crc != computed_crc) {
    decoded.status = FrameStatus::kInvalid;
    decoded.error = make_error(Code::kStorageCorrupt, "frame body checksum does not match",
                               "stored=" + std::to_string(stored_crc) +
                                   " computed=" + std::to_string(computed_crc));
    return decoded;
  }

  // 9. Digest.
  std::array<std::byte, Digest::kBytes> stored_bytes{};
  std::copy_n(bytes + kBodyDigestOffset, Digest::kBytes, stored_bytes.begin());
  const Digest stored_digest = Digest::from_bytes(stored_bytes);
  const Digest computed_digest = DigestBuilder::of(body);
  if (stored_digest != computed_digest) {
    decoded.status = FrameStatus::kInvalid;
    decoded.error = make_error(Code::kStorageCorrupt, "frame body digest does not match",
                               "stored=" + stored_digest.hex() +
                                   " computed=" + computed_digest.hex());
    return decoded;
  }

  decoded.status = FrameStatus::kOk;
  decoded.error = Status::success();
  decoded.frame.kind = kind;
  decoded.frame.commit = CommitSequence::from_value(get_le64(bytes + kCommitOffset));
  decoded.frame.body.assign(body.begin(), body.end());
  return decoded;
}

std::vector<std::byte> encode_group(const CommitGroup& group) { return encode_body(group); }

Outcome<CommitGroup> decode_group(std::span<const std::byte> body) {
  CommitGroup group;
  const Status status = decode_body(body, group);
  if (status.failed()) {
    return status;
  }
  return group;
}

}  // namespace cxf
