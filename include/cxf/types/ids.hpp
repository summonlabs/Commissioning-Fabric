// Commissioning Fabric - strongly typed identities.
//
// Every identity in the runtime is a distinct type, so an asset id cannot be
// passed where a rack id is expected even though both are 64-bit values. The
// tag parameter is an incomplete type: it exists only to make the types
// distinct and is never instantiated.
#ifndef CXF_TYPES_IDS_HPP
#define CXF_TYPES_IDS_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "cxf/support/status.hpp"
#include "cxf/support/text.hpp"

namespace cxf {

/// Monotonic numeric identity. Zero is never a valid identity: a zero value is
/// how "not yet assigned" is spelled, and the runtime rejects it wherever an
/// identity is required.
template <typename Tag>
class Id {
 public:
  using value_type = std::uint64_t;
  using tag_type = Tag;

  constexpr Id() noexcept = default;

  [[nodiscard]] static constexpr Id from_value(std::uint64_t v) noexcept {
    Id id;
    id.value_ = v;
    return id;
  }

  /// Parse the canonical decimal spelling: no sign, no leading zeros (except
  /// "0" itself, which parses but is not a valid identity).
  [[nodiscard]] static Outcome<Id> parse(std::string_view text) {
    const Outcome<std::uint64_t> value = parse_u64(text);
    if (!value.ok()) {
      return make_error(value.code(), "identity is not a canonical decimal value",
                        std::string(text));
    }
    return Id::from_value(value.value());
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }
  [[nodiscard]] std::string str() const { return to_dec(value_); }

  /// The next identity value. Allocators hand out identities monotonically, and
  /// saturation at the maximum means an exhausted allocator can never wrap and
  /// reissue an identity that was already published.
  [[nodiscard]] constexpr Id next() const noexcept {
    return Id::from_value(value_ == kMax ? kMax : value_ + 1);
  }

  /// True when the allocator has no further identities to hand out.
  [[nodiscard]] constexpr bool exhausted() const noexcept { return value_ == kMax; }

  friend constexpr bool operator==(Id a, Id b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(Id a, Id b) noexcept { return !(a == b); }
  /// Numeric order. Used for deterministic iteration and reporting, never as a
  /// statement about recency or authority.
  friend constexpr bool operator<(Id a, Id b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator>(Id a, Id b) noexcept { return b < a; }
  friend constexpr bool operator<=(Id a, Id b) noexcept { return !(b < a); }
  friend constexpr bool operator>=(Id a, Id b) noexcept { return !(a < b); }

 private:
  static constexpr std::uint64_t kMax = 0xFFFFFFFFFFFFFFFFull;
  std::uint64_t value_{0};
};

#define CXF_DECLARE_ID(name)                 \
  struct name##Tag;                          \
  using name = Id<name##Tag>

CXF_DECLARE_ID(AssetId);
CXF_DECLARE_ID(RackId);
CXF_DECLARE_ID(SiteId);
CXF_DECLARE_ID(CandidateId);
CXF_DECLARE_ID(AttemptId);
CXF_DECLARE_ID(PlanId);
CXF_DECLARE_ID(EvidenceId);
CXF_DECLARE_ID(TokenId);
CXF_DECLARE_ID(RequestId);
CXF_DECLARE_ID(ChangeId);

#undef CXF_DECLARE_ID

}  // namespace cxf

namespace std {

template <typename Tag>
struct hash<cxf::Id<Tag>> {
  [[nodiscard]] std::size_t operator()(const cxf::Id<Tag>& id) const noexcept {
    // Identity values are dense small integers; the value itself is a good hash.
    return static_cast<std::size_t>(id.value());
  }
};

}  // namespace std

#endif  // CXF_TYPES_IDS_HPP
