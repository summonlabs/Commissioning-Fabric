#include "cxf/support/serial.hpp"

#include "cxf/support/digest.hpp"
#include "cxf/support/text.hpp"

namespace cxf {
namespace {

[[nodiscard]] Status truncated(std::string_view what, std::size_t need_bytes,
                               std::size_t remaining) {
  return make_error(Code::kMalformedInput, "record ends before a field is complete",
                    std::string(what) + " needs=" + std::to_string(need_bytes) +
                        " remaining=" + std::to_string(remaining));
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

/// A canonical digest spelling is exactly 64 lowercase hex characters.
[[nodiscard]] bool is_canonical_digest_text(std::string_view v) noexcept {
  if (v.size() != Digest::kBytes * 2) {
    return false;
  }
  for (char c : v) {
    if (hex_nibble(c) < 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

void SerialWriter::u8(std::uint8_t v) { buf_.push_back(static_cast<std::byte>(v)); }

void SerialWriter::u16(std::uint16_t v) {
  u8(static_cast<std::uint8_t>(v & 0xFFu));
  u8(static_cast<std::uint8_t>((v >> 8u) & 0xFFu));
}

void SerialWriter::u32(std::uint32_t v) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    u8(static_cast<std::uint8_t>((v >> shift) & 0xFFu));
  }
}

void SerialWriter::u64(std::uint64_t v) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    u8(static_cast<std::uint8_t>((v >> shift) & 0xFFu));
  }
}

void SerialWriter::i64(std::int64_t v) {
  u64(static_cast<std::uint64_t>(v));
}

void SerialWriter::boolean(bool v) { u8(v ? 1u : 0u); }

void SerialWriter::text(std::string_view v) {
  u32(static_cast<std::uint32_t>(v.size()));
  for (char c : v) {
    buf_.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
  }
}

void SerialWriter::blob(std::span<const std::byte> v) {
  u32(static_cast<std::uint32_t>(v.size()));
  buf_.insert(buf_.end(), v.begin(), v.end());
}

void SerialWriter::digest_text(std::string_view v) {
  // Exactly 64 lowercase hex characters are stored as the 32 bytes they denote.
  // A non-canonical spelling is written as an unreadable poisoned field so the
  // record fails closed at read time instead of degrading to a silent default.
  if (!is_canonical_digest_text(v)) {
    u32(0xFFFFFFFFu);
    return;
  }
  u32(static_cast<std::uint32_t>(Digest::kBytes));
  for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
    const auto hi = static_cast<std::uint8_t>(hex_nibble(v[i]));
    const auto lo = static_cast<std::uint8_t>(hex_nibble(v[i + 1]));
    u8(static_cast<std::uint8_t>(static_cast<std::uint8_t>(hi << 4u) | lo));
  }
}

std::string SerialWriter::as_string() const {
  return std::string(reinterpret_cast<const char*>(buf_.data()), buf_.size());
}

Status SerialReader::need(std::size_t n, std::string_view what) const {
  if (remaining() < n) {
    return truncated(what, n, remaining());
  }
  return Status::success();
}

Outcome<std::uint8_t> SerialReader::u8() {
  const Status s = need(1, "u8");
  if (s.failed()) {
    return s;
  }
  const auto v = static_cast<std::uint8_t>(data_[offset_]);
  ++offset_;
  return v;
}

Outcome<std::uint16_t> SerialReader::u16() {
  const Status s = need(2, "u16");
  if (s.failed()) {
    return s;
  }
  std::uint16_t v = 0;
  for (unsigned i = 0; i < 2; ++i) {
    const auto byte = static_cast<std::uint16_t>(static_cast<std::uint8_t>(data_[offset_ + i]));
    v = static_cast<std::uint16_t>(v | static_cast<std::uint16_t>(byte << (8u * i)));
  }
  offset_ += 2;
  return v;
}

Outcome<std::uint32_t> SerialReader::u32() {
  const Status s = need(4, "u32");
  if (s.failed()) {
    return s;
  }
  std::uint32_t v = 0;
  for (unsigned i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data_[offset_ + i])) << (8u * i);
  }
  offset_ += 4;
  return v;
}

Outcome<std::uint64_t> SerialReader::u64() {
  const Status s = need(8, "u64");
  if (s.failed()) {
    return s;
  }
  std::uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data_[offset_ + i])) << (8u * i);
  }
  offset_ += 8;
  return v;
}

Outcome<std::int64_t> SerialReader::i64() {
  const Outcome<std::uint64_t> raw = u64();
  if (!raw.ok()) {
    return raw.status();
  }
  return static_cast<std::int64_t>(raw.value());
}

Outcome<bool> SerialReader::boolean() {
  const Outcome<std::uint8_t> raw = u8();
  if (!raw.ok()) {
    return raw.status();
  }
  if (raw.value() > 1u) {
    return make_error(Code::kFieldOutOfRange, "boolean field must be 0 or 1",
                      "value=" + std::to_string(raw.value()));
  }
  return raw.value() == 1u;
}

Outcome<std::string> SerialReader::text() {
  const Outcome<std::uint32_t> len = u32();
  if (!len.ok()) {
    return len.status();
  }
  const auto n = static_cast<std::size_t>(len.value());
  if (n > kMaxTextFieldBytes) {
    return make_error(Code::kFieldTooLong, "text field exceeds the documented bound",
                      "bytes=" + std::to_string(n));
  }
  const Status s = need(n, "text");
  if (s.failed()) {
    return s;
  }
  std::string out(reinterpret_cast<const char*>(data_.data() + offset_), n);
  offset_ += n;
  if (!is_valid_utf8(out)) {
    return make_error(Code::kFieldInvalidUtf8, "text field is not well-formed UTF-8");
  }
  return out;
}

Outcome<std::vector<std::byte>> SerialReader::blob() {
  const Outcome<std::uint32_t> len = u32();
  if (!len.ok()) {
    return len.status();
  }
  const auto n = static_cast<std::size_t>(len.value());
  if (n > kMaxBlobFieldBytes) {
    return make_error(Code::kFieldTooLong, "blob field exceeds the documented bound",
                      "bytes=" + std::to_string(n));
  }
  const Status s = need(n, "blob");
  if (s.failed()) {
    return s;
  }
  std::vector<std::byte> out(data_.begin() + static_cast<std::ptrdiff_t>(offset_),
                             data_.begin() + static_cast<std::ptrdiff_t>(offset_ + n));
  offset_ += n;
  return out;
}

Outcome<std::string> SerialReader::digest_text() {
  const Outcome<std::uint32_t> len = u32();
  if (!len.ok()) {
    return len.status();
  }
  if (len.value() != Digest::kBytes) {
    return make_error(Code::kMalformedInput, "digest field must carry exactly 32 bytes",
                      "bytes=" + std::to_string(len.value()));
  }
  const Status s = need(Digest::kBytes, "digest");
  if (s.failed()) {
    return s;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(Digest::kBytes * 2);
  for (std::size_t i = 0; i < Digest::kBytes; ++i) {
    const auto v = static_cast<std::uint8_t>(data_[offset_ + i]);
    out.push_back(kHex[(v >> 4u) & 0x0Fu]);
    out.push_back(kHex[v & 0x0Fu]);
  }
  offset_ += Digest::kBytes;
  return out;
}

Status SerialReader::expect_exhausted(std::string_view what) const {
  if (remaining() != 0) {
    return make_error(Code::kTrailingBytes, "bytes remain after the record body",
                      std::string(what) + " trailing=" + std::to_string(remaining()));
  }
  return Status::success();
}

void put_be16(std::byte* out, std::uint16_t v) noexcept {
  out[0] = static_cast<std::byte>((v >> 8u) & 0xFFu);
  out[1] = static_cast<std::byte>(v & 0xFFu);
}

void put_be32(std::byte* out, std::uint32_t v) noexcept {
  for (unsigned i = 0; i < 4; ++i) {
    out[i] = static_cast<std::byte>((v >> (8u * (3u - i))) & 0xFFu);
  }
}

void put_be64(std::byte* out, std::uint64_t v) noexcept {
  for (unsigned i = 0; i < 8; ++i) {
    out[i] = static_cast<std::byte>((v >> (8u * (7u - i))) & 0xFFu);
  }
}

std::uint16_t get_be16(const std::byte* in) noexcept {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[0])) << 8u) |
      static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[1])));
}

std::uint32_t get_be32(const std::byte* in) noexcept {
  std::uint32_t v = 0;
  for (unsigned i = 0; i < 4; ++i) {
    v = (v << 8u) | static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i]));
  }
  return v;
}

std::uint64_t get_be64(const std::byte* in) noexcept {
  std::uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i) {
    v = (v << 8u) | static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[i]));
  }
  return v;
}

void put_le32(std::byte* out, std::uint32_t v) noexcept {
  for (unsigned i = 0; i < 4; ++i) {
    out[i] = static_cast<std::byte>((v >> (8u * i)) & 0xFFu);
  }
}

void put_le64(std::byte* out, std::uint64_t v) noexcept {
  for (unsigned i = 0; i < 8; ++i) {
    out[i] = static_cast<std::byte>((v >> (8u * i)) & 0xFFu);
  }
}

std::uint32_t get_le32(const std::byte* in) noexcept {
  std::uint32_t v = 0;
  for (unsigned i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i])) << (8u * i);
  }
  return v;
}

std::uint64_t get_le64(const std::byte* in) noexcept {
  std::uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[i])) << (8u * i);
  }
  return v;
}

}  // namespace cxf
