// Commissioning Fabric - CRC-32C (Castagnoli) integrity check.
#ifndef CXF_SUPPORT_CRC32C_HPP
#define CXF_SUPPORT_CRC32C_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace cxf {

/// CRC-32C (Castagnoli, reflected polynomial 0x82F63B78).
///
/// Used as the per-record integrity check of the durable log and snapshot
/// formats. It detects accidental corruption and truncation; it is not a
/// cryptographic authenticator and the format documentation says so.
class Crc32c {
 public:
  Crc32c() noexcept = default;

  void update(std::span<const std::byte> data) noexcept;
  void update(const void* data, std::size_t size) noexcept;

  [[nodiscard]] std::uint32_t value() const noexcept { return ~state_; }

  /// One-shot convenience.
  [[nodiscard]] static std::uint32_t compute(std::span<const std::byte> data) noexcept;

 private:
  std::uint32_t state_{0xFFFFFFFFu};
};

}  // namespace cxf

#endif  // CXF_SUPPORT_CRC32C_HPP
