// Commissioning Fabric - field-visit archive over the canonical byte format.
//
// Domain records describe themselves once, as a visit() member that names each
// field. The same description drives encoding and decoding, so a field can never
// be written without being readable, and a decoder cannot drift from its encoder.
// Decoding validates every field: bounds, enum domains, boolean shape and digest
// spelling are all checked before a value is accepted.
#ifndef CXF_CODEC_ARCHIVE_HPP
#define CXF_CODEC_ARCHIVE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "cxf/support/digest.hpp"
#include "cxf/support/serial.hpp"
#include "cxf/support/status.hpp"
#include "cxf/types/enums.hpp"
#include "cxf/types/generations.hpp"
#include "cxf/types/ids.hpp"
#include "cxf/types/timepoint.hpp"

namespace cxf {

class EncodeArchive;
class DecodeArchive;

// ---------------------------------------------------------------------------
// Field encoders.
// ---------------------------------------------------------------------------

inline void encode_field(EncodeArchive& archive, std::string_view /*name*/, std::uint8_t v);
inline void encode_field(EncodeArchive& archive, std::string_view /*name*/, std::uint16_t v);
inline void encode_field(EncodeArchive& archive, std::string_view /*name*/, std::uint32_t v);
inline void encode_field(EncodeArchive& archive, std::string_view /*name*/, std::uint64_t v);
inline void encode_field(EncodeArchive& archive, std::string_view /*name*/, std::int64_t v);
inline void encode_field(EncodeArchive& archive, std::string_view /*name*/, bool v);
inline void encode_field(EncodeArchive& archive, std::string_view /*name*/, const std::string& v);
inline void encode_field(EncodeArchive& archive, std::string_view /*name*/,
                         const std::vector<std::byte>& v);
inline void encode_field(EncodeArchive& archive, std::string_view /*name*/, const Digest& v);
inline void encode_field(EncodeArchive& archive, std::string_view /*name*/, Timestamp v);
inline void encode_field(EncodeArchive& archive, std::string_view /*name*/, Duration v);
inline void encode_field(EncodeArchive& archive, std::string_view /*name*/, FreshnessWindow v);
inline void encode_field(EncodeArchive& archive, std::string_view /*name*/, Code v);

template <typename Tag>
inline void encode_field(EncodeArchive& archive, std::string_view /*name*/, const Id<Tag>& v);
template <typename Tag>
inline void encode_field(EncodeArchive& archive, std::string_view /*name*/,
                         const Generation<Tag>& v);

template <typename T>
inline void encode_field(EncodeArchive& archive, std::string_view name,
                         const std::optional<T>& v);

template <typename T>
inline void encode_field(EncodeArchive& archive, std::string_view name,
                         const std::vector<T>& v);

/// Fixed-size arrays carry no length: their size is part of the type, so a
/// truncated array cannot be mistaken for a shorter one.
template <typename T, std::size_t N>
inline void encode_field(EncodeArchive& archive, std::string_view name,
                         const std::array<T, N>& v);

/// Enumerations are encoded as their explicit numeric value; decoding rejects a
/// value outside the declared domain rather than inventing a nearest match.
template <typename E>
  requires std::is_enum_v<E>
inline void encode_field(EncodeArchive& archive, std::string_view /*name*/, E v);

// ---------------------------------------------------------------------------
// Field decoders.
// ---------------------------------------------------------------------------

[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view /*name*/,
                                         std::uint8_t& v);
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view /*name*/,
                                         std::uint16_t& v);
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view /*name*/,
                                         std::uint32_t& v);
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view /*name*/,
                                         std::uint64_t& v);
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view /*name*/,
                                         std::int64_t& v);
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view /*name*/,
                                         bool& v);
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view /*name*/,
                                         std::string& v);
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view /*name*/,
                                         std::vector<std::byte>& v);
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view /*name*/,
                                         Digest& v);
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view /*name*/,
                                         Timestamp& v);
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view /*name*/,
                                         Duration& v);
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view /*name*/,
                                         FreshnessWindow& v);
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view /*name*/,
                                         Code& v);

template <typename Tag>
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view /*name*/,
                                         Id<Tag>& v);
template <typename Tag>
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view /*name*/,
                                         Generation<Tag>& v);

template <typename T>
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view name,
                                         std::optional<T>& v);
template <typename T>
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view name,
                                         std::vector<T>& v);
template <typename T, std::size_t N>
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view name,
                                         std::array<T, N>& v);
template <typename E>
  requires std::is_enum_v<E>
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view name, E& v);

// ---------------------------------------------------------------------------
// Archives.
// ---------------------------------------------------------------------------

class EncodeArchive {
 public:
  explicit EncodeArchive(SerialWriter& writer) noexcept : writer_(&writer) {}

  template <typename T>
  void operator()(std::string_view name, const T& value) {
    encode_field(*this, name, value);
  }

  [[nodiscard]] SerialWriter& writer() noexcept { return *writer_; }

 private:
  SerialWriter* writer_;
};

class DecodeArchive {
 public:
  explicit DecodeArchive(SerialReader& reader) noexcept : reader_(&reader) {}

  template <typename T>
  void operator()(std::string_view name, T& value) {
    if (failed_) {
      return;
    }
    const Status status = decode_field(*this, name, value);
    if (status.failed()) {
      failed_ = true;
      status_ = status;
    }
  }

  [[nodiscard]] SerialReader& reader() noexcept { return *reader_; }
  [[nodiscard]] bool ok() const noexcept { return !failed_; }
  [[nodiscard]] Status status() const { return status_; }
  void fail(Status status) {
    if (!failed_) {
      failed_ = true;
      status_ = std::move(status);
    }
  }

 private:
  SerialReader* reader_;
  bool failed_{false};
  Status status_{};
};

// ---------------------------------------------------------------------------
// Nested-record fallback: any type with a visit() member describes itself.
// ---------------------------------------------------------------------------

template <typename T>
  requires requires(T& value, EncodeArchive& archive) { value.visit(archive); }
inline void encode_field(EncodeArchive& archive, std::string_view /*name*/, const T& value) {
  // visit() only reads through the archive; the cast removes const so that one
  // visit() description can serve both directions.
  const_cast<T&>(value).visit(archive);
}

template <typename T>
  requires requires(T& value, DecodeArchive& archive) { value.visit(archive); }
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view /*name*/,
                                         T& value) {
  value.visit(archive);
  return archive.status();
}

// ---------------------------------------------------------------------------
// Whole-body helpers.
// ---------------------------------------------------------------------------

/// Encode a self-describing record body.
template <typename T>
[[nodiscard]] inline std::vector<std::byte> encode_body(const T& value) {
  SerialWriter writer;
  EncodeArchive archive(writer);
  const_cast<T&>(value).visit(archive);
  return writer.bytes();
}

/// Decode a self-describing record body, rejecting trailing bytes.
template <typename T>
[[nodiscard]] inline Status decode_body(std::span<const std::byte> body, T& out) {
  SerialReader reader(body);
  DecodeArchive archive(reader);
  out.visit(archive);
  if (!archive.ok()) {
    return archive.status();
  }
  return reader.expect_exhausted("record body");
}

// ---------------------------------------------------------------------------
// Definitions of the primitive field codecs.
// ---------------------------------------------------------------------------

inline void encode_field(EncodeArchive& archive, std::string_view, std::uint8_t v) {
  archive.writer().u8(v);
}
inline void encode_field(EncodeArchive& archive, std::string_view, std::uint16_t v) {
  archive.writer().u16(v);
}
inline void encode_field(EncodeArchive& archive, std::string_view, std::uint32_t v) {
  archive.writer().u32(v);
}
inline void encode_field(EncodeArchive& archive, std::string_view, std::uint64_t v) {
  archive.writer().u64(v);
}
inline void encode_field(EncodeArchive& archive, std::string_view, std::int64_t v) {
  archive.writer().i64(v);
}
inline void encode_field(EncodeArchive& archive, std::string_view, bool v) {
  archive.writer().boolean(v);
}
inline void encode_field(EncodeArchive& archive, std::string_view, const std::string& v) {
  archive.writer().text(v);
}
inline void encode_field(EncodeArchive& archive, std::string_view,
                         const std::vector<std::byte>& v) {
  archive.writer().blob(v);
}
inline void encode_field(EncodeArchive& archive, std::string_view, const Digest& v) {
  archive.writer().digest_text(v.view());
}
inline void encode_field(EncodeArchive& archive, std::string_view, Timestamp v) {
  archive.writer().i64(v.unix_nanos());
}
inline void encode_field(EncodeArchive& archive, std::string_view, Duration v) {
  archive.writer().i64(v.nanos());
}
inline void encode_field(EncodeArchive& archive, std::string_view, FreshnessWindow v) {
  archive.writer().boolean(v.allows_all_time());
  archive.writer().i64(v.window().nanos());
}
inline void encode_field(EncodeArchive& archive, std::string_view, Code v) {
  archive.writer().u16(static_cast<std::uint16_t>(v));
}

template <typename Tag>
inline void encode_field(EncodeArchive& archive, std::string_view, const Id<Tag>& v) {
  archive.writer().u64(v.value());
}

template <typename Tag>
inline void encode_field(EncodeArchive& archive, std::string_view, const Generation<Tag>& v) {
  archive.writer().u64(v.value());
}

template <typename T>
inline void encode_field(EncodeArchive& archive, std::string_view name,
                         const std::optional<T>& v) {
  archive.writer().boolean(v.has_value());
  if (v.has_value()) {
    encode_field(archive, name, *v);
  }
}

template <typename T>
inline void encode_field(EncodeArchive& archive, std::string_view name,
                         const std::vector<T>& v) {
  archive.writer().u32(static_cast<std::uint32_t>(v.size()));
  for (const T& item : v) {
    encode_field(archive, name, item);
  }
}

template <typename T, std::size_t N>
inline void encode_field(EncodeArchive& archive, std::string_view name,
                         const std::array<T, N>& v) {
  for (const T& item : v) {
    encode_field(archive, name, item);
  }
}

template <typename E>
  requires std::is_enum_v<E>
inline void encode_field(EncodeArchive& archive, std::string_view, E v) {
  archive.writer().u8(static_cast<std::uint8_t>(v));
}

[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view,
                                         std::uint8_t& v) {
  Outcome<std::uint8_t> raw = archive.reader().u8();
  if (!raw.ok()) {
    return raw.status();
  }
  v = raw.value();
  return Status::success();
}

[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view,
                                         std::uint16_t& v) {
  Outcome<std::uint16_t> raw = archive.reader().u16();
  if (!raw.ok()) {
    return raw.status();
  }
  v = raw.value();
  return Status::success();
}

[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view,
                                         std::uint32_t& v) {
  Outcome<std::uint32_t> raw = archive.reader().u32();
  if (!raw.ok()) {
    return raw.status();
  }
  v = raw.value();
  return Status::success();
}

[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view,
                                         std::uint64_t& v) {
  Outcome<std::uint64_t> raw = archive.reader().u64();
  if (!raw.ok()) {
    return raw.status();
  }
  v = raw.value();
  return Status::success();
}

[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view,
                                         std::int64_t& v) {
  Outcome<std::int64_t> raw = archive.reader().i64();
  if (!raw.ok()) {
    return raw.status();
  }
  v = raw.value();
  return Status::success();
}

[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view,
                                         bool& v) {
  Outcome<bool> raw = archive.reader().boolean();
  if (!raw.ok()) {
    return raw.status();
  }
  v = raw.value();
  return Status::success();
}

[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view,
                                         std::string& v) {
  Outcome<std::string> raw = archive.reader().text();
  if (!raw.ok()) {
    return raw.status();
  }
  v = std::move(raw).value();
  return Status::success();
}

[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view,
                                         std::vector<std::byte>& v) {
  Outcome<std::vector<std::byte>> raw = archive.reader().blob();
  if (!raw.ok()) {
    return raw.status();
  }
  v = std::move(raw).value();
  return Status::success();
}

[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view,
                                         Digest& v) {
  Outcome<std::string> raw = archive.reader().digest_text();
  if (!raw.ok()) {
    return raw.status();
  }
  Outcome<Digest> parsed = Digest::parse(raw.value());
  if (!parsed.ok()) {
    return parsed.status();
  }
  v = parsed.value();
  return Status::success();
}

[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view,
                                         Timestamp& v) {
  Outcome<std::int64_t> raw = archive.reader().i64();
  if (!raw.ok()) {
    return raw.status();
  }
  v = Timestamp::from_unix_nanos(raw.value());
  return Status::success();
}

[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view,
                                         Duration& v) {
  Outcome<std::int64_t> raw = archive.reader().i64();
  if (!raw.ok()) {
    return raw.status();
  }
  v = Duration::from_nanos(raw.value());
  return Status::success();
}

[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view,
                                         FreshnessWindow& v) {
  Outcome<bool> never = archive.reader().boolean();
  if (!never.ok()) {
    return never.status();
  }
  Outcome<std::int64_t> nanos = archive.reader().i64();
  if (!nanos.ok()) {
    return nanos.status();
  }
  v = never.value() ? FreshnessWindow::never()
                    : FreshnessWindow::of(Duration::from_nanos(nanos.value()));
  return Status::success();
}

[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view,
                                         Code& v) {
  Outcome<std::uint16_t> raw = archive.reader().u16();
  if (!raw.ok()) {
    return raw.status();
  }
  // The reason-code domain is closed: a value that is not one of the declared
  // codes is a corrupt record rather than a new code.
  if (!is_valid_code_value(raw.value())) {
    return make_error(Code::kFieldOutOfRange, "reason code is outside the declared domain",
                      "code=" + std::to_string(raw.value()));
  }
  v = static_cast<Code>(raw.value());
  return Status::success();
}

template <typename Tag>
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view,
                                         Id<Tag>& v) {
  Outcome<std::uint64_t> raw = archive.reader().u64();
  if (!raw.ok()) {
    return raw.status();
  }
  v = Id<Tag>::from_value(raw.value());
  return Status::success();
}

template <typename Tag>
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view,
                                         Generation<Tag>& v) {
  Outcome<std::uint64_t> raw = archive.reader().u64();
  if (!raw.ok()) {
    return raw.status();
  }
  v = Generation<Tag>::from_value(raw.value());
  return Status::success();
}

template <typename T>
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view name,
                                         std::optional<T>& v) {
  Outcome<bool> present = archive.reader().boolean();
  if (!present.ok()) {
    return present.status();
  }
  if (!present.value()) {
    v.reset();
    return Status::success();
  }
  T value{};
  const Status status = decode_field(archive, name, value);
  if (status.failed()) {
    return status;
  }
  v = std::move(value);
  return Status::success();
}

template <typename T>
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view name,
                                         std::vector<T>& v) {
  Outcome<std::uint32_t> count = archive.reader().u32();
  if (!count.ok()) {
    return count.status();
  }
  if (count.value() > kMaxCollectionItems) {
    return make_error(Code::kFieldTooLong, "collection exceeds the documented bound",
                      "items=" + std::to_string(count.value()));
  }
  v.clear();
  v.reserve(count.value());
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    T item{};
    const Status status = decode_field(archive, name, item);
    if (status.failed()) {
      return status;
    }
    v.push_back(std::move(item));
  }
  return Status::success();
}

template <typename T, std::size_t N>
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view name,
                                         std::array<T, N>& v) {
  for (std::size_t i = 0; i < N; ++i) {
    const Status status = decode_field(archive, name, v[i]);
    if (status.failed()) {
      return status;
    }
  }
  return Status::success();
}

template <typename E>
  requires std::is_enum_v<E>
[[nodiscard]] inline Status decode_field(DecodeArchive& archive, std::string_view name, E& v) {
  Outcome<std::uint8_t> raw = archive.reader().u8();
  if (!raw.ok()) {
    return raw.status();
  }
  const E candidate = static_cast<E>(raw.value());
  if (!enum_value_valid(candidate)) {
    return make_error(Code::kFieldOutOfRange, "enumeration value is outside the declared domain",
                      std::string(name) + " value=" + std::to_string(raw.value()));
  }
  v = candidate;
  return Status::success();
}

}  // namespace cxf

#endif  // CXF_CODEC_ARCHIVE_HPP
