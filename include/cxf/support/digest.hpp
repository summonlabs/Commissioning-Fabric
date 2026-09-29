// Commissioning Fabric - 256-bit content digest.
//
// The digest is the stable content address of canonical byte serialisations
// (evidence payloads, authority-token bindings, journal record bodies). It is
// deliberately a pure function of the bytes with no process- or time-dependent
// input, which is what makes replay and stale-authority detection deterministic.
#ifndef CXF_SUPPORT_DIGEST_HPP
#define CXF_SUPPORT_DIGEST_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <string>
#include <string_view>

#include "cxf/support/status.hpp"

namespace cxf {

/// 256-bit digest, rendered as 64 lowercase hex characters.
class Digest {
 public:
  static constexpr std::size_t kBytes = 32;

  Digest() noexcept = default;

  /// Parse exactly 64 lowercase hex characters. Uppercase is rejected so that a
  /// digest has exactly one textual spelling.
  [[nodiscard]] static Outcome<Digest> parse(std::string_view text);

  [[nodiscard]] static Digest from_bytes(std::span<const std::byte> bytes);

  [[nodiscard]] const std::string& hex() const noexcept { return hex_; }
  [[nodiscard]] std::string_view view() const noexcept { return hex_; }
  [[nodiscard]] const char* c_str() const noexcept { return hex_.c_str(); }
  [[nodiscard]] bool is_zero() const noexcept;
  [[nodiscard]] bool empty() const noexcept { return hex_.empty(); }

  friend bool operator==(const Digest& a, const Digest& b) noexcept {
    return a.hex_ == b.hex_;
  }
  friend bool operator!=(const Digest& a, const Digest& b) noexcept {
    return !(a == b);
  }
  friend bool operator<(const Digest& a, const Digest& b) noexcept {
    return a.hex_ < b.hex_;
  }

 private:
  std::string hex_{};
};

/// Streaming digest builder. Feed canonical bytes; call finish() once.
class DigestBuilder {
 public:
  DigestBuilder() noexcept = default;

  void update(std::span<const std::byte> data) noexcept;
  void update(const void* data, std::size_t size) noexcept;

  [[nodiscard]] Digest finish() const noexcept;

  /// Convenience: digest of a byte range.
  [[nodiscard]] static Digest of(std::span<const std::byte> data) noexcept;
  [[nodiscard]] static Digest of(std::string_view text) noexcept;

 private:
  /// Absorb one full 64-bit word into the lanes.
  void absorb(std::uint64_t word) noexcept;

  // Four independent 64-bit lanes, 32 bytes of state. Finalisation mixes the
  // lane length in so that padded inputs with different lengths cannot collide.
  std::uint64_t lane_[4]{0x243F6A8885A308D3ull, 0x13198A2E03707344ull,
                         0xA4093822299F31D0ull, 0x082EFA98EC4E6C89ull};
  std::uint64_t length_{0};
  // Bytes of an incomplete word are carried across update() calls, so feeding
  // the same bytes in any chunking produces the same digest.
  std::uint8_t carry_[8]{};
  std::size_t carry_len_{0};
};

}  // namespace cxf

#endif  // CXF_SUPPORT_DIGEST_HPP
