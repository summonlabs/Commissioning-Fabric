// Commissioning Fabric - canonical little-endian serialisation.
//
// All durable bytes and all content digests are produced by this writer and
// consumed by this reader. The format is canonical: one encoding per value,
// fixed field order, no padding, no trailing bytes, explicit lengths.
#ifndef CXF_SUPPORT_SERIAL_HPP
#define CXF_SUPPORT_SERIAL_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cxf/support/status.hpp"

namespace cxf {

/// Upper bound for any length-prefixed text or blob field that the runtime
/// accepts from an untrusted source. Bounds are explicit so that a corrupt or
/// hostile length cannot cause an unbounded allocation.
inline constexpr std::size_t kMaxTextFieldBytes = 4096;
inline constexpr std::size_t kMaxBlobFieldBytes = 1u << 20;   // 1 MiB
inline constexpr std::size_t kMaxCollectionItems = 1u << 16;  // 65536

/// Appends canonical little-endian encodings to an in-memory byte buffer.
class SerialWriter {
 public:
  SerialWriter() = default;

  void u8(std::uint8_t v);
  void u16(std::uint16_t v);
  void u32(std::uint32_t v);
  void u64(std::uint64_t v);
  void i64(std::int64_t v);
  void boolean(bool v);
  void text(std::string_view v);
  void blob(std::span<const std::byte> v);
  void digest_text(std::string_view v);

  [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return buf_; }
  [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }
  [[nodiscard]] std::span<const std::byte> span() const noexcept {
    return std::span<const std::byte>(buf_.data(), buf_.size());
  }
  [[nodiscard]] std::string as_string() const;

 private:
  std::vector<std::byte> buf_{};
};

/// Reads canonical little-endian encodings from a byte range. Every accessor
/// validates remaining length, field bounds and UTF-8 shape before returning.
class SerialReader {
 public:
  explicit SerialReader(std::span<const std::byte> data) noexcept : data_(data) {}

  [[nodiscard]] bool exhausted() const noexcept { return offset_ == data_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept {
    return data_.size() - offset_;
  }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

  [[nodiscard]] Outcome<std::uint8_t> u8();
  [[nodiscard]] Outcome<std::uint16_t> u16();
  [[nodiscard]] Outcome<std::uint32_t> u32();
  [[nodiscard]] Outcome<std::uint64_t> u64();
  [[nodiscard]] Outcome<std::int64_t> i64();
  [[nodiscard]] Outcome<bool> boolean();
  [[nodiscard]] Outcome<std::string> text();
  [[nodiscard]] Outcome<std::vector<std::byte>> blob();
  [[nodiscard]] Outcome<std::string> digest_text();

  /// Fail when any byte remains unread. Callers use this to reject trailing
  /// bytes, which are always a defect or a corruption.
  [[nodiscard]] Status expect_exhausted(std::string_view what) const;

 private:
  [[nodiscard]] Status need(std::size_t n, std::string_view what) const;

  std::span<const std::byte> data_{};
  std::size_t offset_{0};
};

/// Big-endian helpers for on-disk record framing (network byte order).
void put_be16(std::byte* out, std::uint16_t v) noexcept;
void put_be32(std::byte* out, std::uint32_t v) noexcept;
void put_be64(std::byte* out, std::uint64_t v) noexcept;
[[nodiscard]] std::uint16_t get_be16(const std::byte* in) noexcept;
[[nodiscard]] std::uint32_t get_be32(const std::byte* in) noexcept;
[[nodiscard]] std::uint64_t get_be64(const std::byte* in) noexcept;

/// Little-endian fixed-width helpers.
void put_le32(std::byte* out, std::uint32_t v) noexcept;
void put_le64(std::byte* out, std::uint64_t v) noexcept;
[[nodiscard]] std::uint32_t get_le32(const std::byte* in) noexcept;
[[nodiscard]] std::uint64_t get_le64(const std::byte* in) noexcept;

}  // namespace cxf

#endif  // CXF_SUPPORT_SERIAL_HPP
