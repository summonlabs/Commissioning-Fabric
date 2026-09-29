#include "cxf/support/digest.hpp"

#include <array>

namespace cxf {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

[[nodiscard]] constexpr std::uint64_t rotl(std::uint64_t x, unsigned r) noexcept {
  return (x << r) | (x >> (64u - r));
}

/// SplitMix64 finalizer: deterministic avalanche with no lookup tables.
[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t z) noexcept {
  z += 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30u)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27u)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31u);
}

}  // namespace

void DigestBuilder::absorb(std::uint64_t word) noexcept {
  lane_[0] = mix(lane_[0] ^ word);
  lane_[1] = rotl(lane_[1] + lane_[0], 17) ^ (lane_[0] >> 7u);
  lane_[2] = mix(lane_[2] + word);
  lane_[3] = rotl(lane_[3] ^ lane_[2], 29) + 0x165667B19E3779F9ull;
}

namespace {

[[nodiscard]] std::uint64_t load_le64(const std::uint8_t* p) noexcept {
  std::uint64_t word = 0;
  for (unsigned i = 0; i < 8; ++i) {
    word |= static_cast<std::uint64_t>(p[i]) << (8u * i);
  }
  return word;
}

}  // namespace

void DigestBuilder::update(const void* data, std::size_t size) noexcept {
  const auto* p = static_cast<const std::uint8_t*>(data);
  length_ += size;

  // A partial word left by an earlier call is completed first, so the digest of
  // a byte sequence never depends on how the caller split it into chunks.
  if (carry_len_ > 0) {
    while (size > 0 && carry_len_ < 8) {
      carry_[carry_len_] = *p;
      ++carry_len_;
      ++p;
      --size;
    }
    if (carry_len_ == 8) {
      absorb(load_le64(carry_));
      carry_len_ = 0;
    }
  }

  while (size >= 8) {
    absorb(load_le64(p));
    p += 8;
    size -= 8;
  }

  while (size > 0) {
    carry_[carry_len_] = *p;
    ++carry_len_;
    ++p;
    --size;
  }
}

void DigestBuilder::update(std::span<const std::byte> data) noexcept {
  update(data.data(), data.size());
}

Digest DigestBuilder::finish() const noexcept {
  std::array<std::uint64_t, 4> lanes = {lane_[0], lane_[1], lane_[2], lane_[3]};
  const std::uint64_t len = length_;

  if (carry_len_ > 0) {
    std::uint64_t word = 0;
    for (std::size_t i = 0; i < carry_len_; ++i) {
      word |= static_cast<std::uint64_t>(carry_[i]) << (8u * i);
    }
    // Length tag prevents "AB" + padding from colliding with "AB\0".
    word |= static_cast<std::uint64_t>(carry_len_) << 56u;
    lanes[0] = mix(lanes[0] ^ word);
    lanes[1] = rotl(lanes[1] + lanes[0], 17) ^ (lanes[0] >> 7u);
    lanes[2] = mix(lanes[2] + word);
    lanes[3] = rotl(lanes[3] ^ lanes[2], 29) + 0x165667B19E3779F9ull;
  }

  // Domain separation between lane positions.
  for (std::size_t i = 0; i < lanes.size(); ++i) {
    lanes[i] = mix(lanes[i] ^ (len * (0x9E3779B97F4A7C15ull + i)));
  }
  // Cross-lane avalanche so every output byte depends on every input byte.
  for (int round = 0; round < 4; ++round) {
    lanes[0] = mix(lanes[0] + lanes[3]);
    lanes[1] = mix(lanes[1] + lanes[0]);
    lanes[2] = mix(lanes[2] + lanes[1]);
    lanes[3] = mix(lanes[3] + lanes[2]);
  }

  std::array<std::byte, Digest::kBytes> out{};
  for (std::size_t i = 0; i < lanes.size(); ++i) {
    for (unsigned b = 0; b < 8; ++b) {
      out[i * 8 + b] = static_cast<std::byte>((lanes[i] >> (8u * b)) & 0xFFu);
    }
  }
  return Digest::from_bytes(out);
}

Digest DigestBuilder::of(std::span<const std::byte> data) noexcept {
  DigestBuilder b;
  b.update(data);
  return b.finish();
}

Digest DigestBuilder::of(std::string_view text) noexcept {
  return of(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                       text.size()));
}

Digest Digest::from_bytes(std::span<const std::byte> bytes) {
  // Canonical textual form: exactly 64 lowercase hex characters, two per input
  // byte. A 32-byte input maps one-to-one onto the 32-byte digest space; other
  // lengths are absorbed through the mixer so the mapping stays deterministic.
  std::array<std::byte, kBytes> material{};
  if (bytes.size() == kBytes) {
    for (std::size_t i = 0; i < kBytes; ++i) {
      material[i] = bytes[i];
    }
  } else {
    DigestBuilder b;
    b.update(bytes);
    const Digest folded = b.finish();
    for (std::size_t i = 0; i < kBytes; ++i) {
      // Fold the 64-character text of the mixed digest back into 32 bytes.
      const int hi = hex_value(folded.hex_[i * 2]);
      const int lo = hex_value(folded.hex_[i * 2 + 1]);
      material[i] = static_cast<std::byte>((hi << 4) | lo);
    }
  }

  std::string hex;
  hex.reserve(kBytes * 2);
  for (std::byte b : material) {
    const auto v = static_cast<std::uint8_t>(b);
    hex.push_back(kHexDigits[(v >> 4u) & 0x0Fu]);
    hex.push_back(kHexDigits[v & 0x0Fu]);
  }

  Digest d;
  d.hex_ = std::move(hex);
  return d;
}

Outcome<Digest> Digest::parse(std::string_view text) {
  if (text.size() != kBytes * 2) {
    return make_error(Code::kMalformedInput,
                      "digest must be exactly 64 lowercase hex characters",
                      "length=" + std::to_string(text.size()));
  }
  Digest d;
  d.hex_.reserve(text.size());
  for (char c : text) {
    if (hex_value(c) < 0) {
      return make_error(Code::kMalformedInput,
                        "digest contains a non-lowercase-hex character",
                        std::string("char=") + c);
    }
    d.hex_.push_back(c);
  }
  return d;
}

bool Digest::is_zero() const noexcept {
  for (char c : hex_) {
    if (c != '0') {
      return false;
    }
  }
  return !hex_.empty();
}

}  // namespace cxf
