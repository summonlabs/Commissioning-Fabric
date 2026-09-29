#include "cxf/support/crc32c.hpp"

namespace cxf {
namespace {

/// Compile-time generated 8-way slicing table.
struct Table {
  std::uint32_t v[8][256]{};

  constexpr Table() noexcept {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1u) != 0u ? ((crc >> 1u) ^ 0x82F63B78u) : (crc >> 1u);
      }
      v[0][i] = crc;
    }
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t crc = v[0][i];
      for (std::size_t slice = 1; slice < 8; ++slice) {
        crc = v[0][crc & 0xFFu] ^ (crc >> 8u);
        v[slice][i] = crc;
      }
    }
  }
};

constexpr Table kTable{};

}  // namespace

void Crc32c::update(const void* data, std::size_t size) noexcept {
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::uint32_t crc = state_;

  while (size >= 8) {
    std::uint32_t lo = static_cast<std::uint32_t>(p[0]) |
                       (static_cast<std::uint32_t>(p[1]) << 8u) |
                       (static_cast<std::uint32_t>(p[2]) << 16u) |
                       (static_cast<std::uint32_t>(p[3]) << 24u);
    std::uint32_t hi = static_cast<std::uint32_t>(p[4]) |
                       (static_cast<std::uint32_t>(p[5]) << 8u) |
                       (static_cast<std::uint32_t>(p[6]) << 16u) |
                       (static_cast<std::uint32_t>(p[7]) << 24u);
    lo ^= crc;
    crc = kTable.v[7][lo & 0xFFu] ^ kTable.v[6][(lo >> 8u) & 0xFFu] ^
          kTable.v[5][(lo >> 16u) & 0xFFu] ^ kTable.v[4][(lo >> 24u) & 0xFFu] ^
          kTable.v[3][hi & 0xFFu] ^ kTable.v[2][(hi >> 8u) & 0xFFu] ^
          kTable.v[1][(hi >> 16u) & 0xFFu] ^ kTable.v[0][(hi >> 24u) & 0xFFu];
    p += 8;
    size -= 8;
  }

  while (size > 0) {
    crc = kTable.v[0][(crc ^ *p) & 0xFFu] ^ (crc >> 8u);
    ++p;
    --size;
  }

  state_ = crc;
}

void Crc32c::update(std::span<const std::byte> data) noexcept {
  update(data.data(), data.size());
}

std::uint32_t Crc32c::compute(std::span<const std::byte> data) noexcept {
  Crc32c crc;
  crc.update(data);
  return crc.value();
}

}  // namespace cxf
