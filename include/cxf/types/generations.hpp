// Commissioning Fabric - generations, epochs and fencing counters.
//
// A generation names one revision of an external fact (topology, power, cooling,
// network, policy, firmware, hardware, dependencies, lifecycle). Evidence and
// authority are bound to the generations they were established under, so any
// advance of a relevant generation fences the authority derived from it.
#ifndef CXF_TYPES_GENERATIONS_HPP
#define CXF_TYPES_GENERATIONS_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cxf/support/status.hpp"
#include "cxf/support/text.hpp"

namespace cxf {

/// Monotone counter. Distinct tags keep unrelated counters from being mixed up.
template <typename Tag>
class Generation {
 public:
  using value_type = std::uint64_t;
  using tag_type = Tag;

  constexpr Generation() noexcept = default;

  [[nodiscard]] static constexpr Generation from_value(std::uint64_t v) noexcept {
    Generation g;
    g.value_ = v;
    return g;
  }

  [[nodiscard]] static Outcome<Generation> parse(std::string_view text) {
    const Outcome<std::uint64_t> value = parse_u64(text);
    if (!value.ok()) {
      return make_error(value.code(), "generation is not a canonical decimal value",
                        std::string(text));
    }
    return Generation::from_value(value.value());
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }
  [[nodiscard]] std::string str() const { return to_dec(value_); }

  /// Successor. Saturating at the maximum value: a counter never wraps, because
  /// a wrapped generation would make stale authority look current.
  [[nodiscard]] constexpr Generation next() const noexcept {
    return Generation::from_value(value_ == kMax ? kMax : value_ + 1);
  }

  friend constexpr bool operator==(Generation a, Generation b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(Generation a, Generation b) noexcept { return !(a == b); }
  friend constexpr bool operator<(Generation a, Generation b) noexcept {
    return a.value_ < b.value_;
  }
  friend constexpr bool operator>(Generation a, Generation b) noexcept { return b < a; }
  friend constexpr bool operator<=(Generation a, Generation b) noexcept { return !(b < a); }
  friend constexpr bool operator>=(Generation a, Generation b) noexcept { return !(a < b); }

 private:
  static constexpr std::uint64_t kMax = 0xFFFFFFFFFFFFFFFFull;
  std::uint64_t value_{0};
};

#define CXF_DECLARE_GENERATION(name)         \
  struct name##Tag;                          \
  using name = Generation<name##Tag>

CXF_DECLARE_GENERATION(LifecycleGeneration);
CXF_DECLARE_GENERATION(HardwareGeneration);
CXF_DECLARE_GENERATION(FirmwareGeneration);
CXF_DECLARE_GENERATION(TopologyGeneration);
CXF_DECLARE_GENERATION(PowerGeneration);
CXF_DECLARE_GENERATION(CoolingGeneration);
CXF_DECLARE_GENERATION(NetworkGeneration);
CXF_DECLARE_GENERATION(PolicyGeneration);
CXF_DECLARE_GENERATION(DependencyGeneration);
CXF_DECLARE_GENERATION(ControlEpoch);
CXF_DECLARE_GENERATION(IncarnationId);
CXF_DECLARE_GENERATION(Revision);
CXF_DECLARE_GENERATION(CommitSequence);
CXF_DECLARE_GENERATION(ObservationSequence);
CXF_DECLARE_GENERATION(FormatVersion);

#undef CXF_DECLARE_GENERATION

/// The complete generation binding under which a fact was established.
///
/// Two FacilityGenerations values are equal exactly when every generation they
/// carry is equal; that equality is the freshness test for generation-bound
/// evidence and the invalidation test for activation authority.
struct FacilityGenerations {
  TopologyGeneration topology{};
  PowerGeneration power{};
  CoolingGeneration cooling{};
  NetworkGeneration network{};
  PolicyGeneration policy{};
  DependencyGeneration dependency{};
  FirmwareGeneration firmware{};
  HardwareGeneration hardware{};
  LifecycleGeneration lifecycle{};

  friend constexpr bool operator==(const FacilityGenerations& a,
                                   const FacilityGenerations& b) noexcept {
    return a.topology == b.topology && a.power == b.power && a.cooling == b.cooling &&
           a.network == b.network && a.policy == b.policy && a.dependency == b.dependency &&
           a.firmware == b.firmware && a.hardware == b.hardware &&
           a.lifecycle == b.lifecycle;
  }
  friend constexpr bool operator!=(const FacilityGenerations& a,
                                   const FacilityGenerations& b) noexcept {
    return !(a == b);
  }

  /// Names of the generations that differ, in fixed field order. Used by the
  /// explanation output so a stale binding names exactly what moved.
  [[nodiscard]] std::vector<std::string> differences(const FacilityGenerations& other) const;

  /// Canonical field description shared by the encoder and the decoder.
  template <typename Ar>
  void visit(Ar& ar) {
    ar("topology", topology);
    ar("power", power);
    ar("cooling", cooling);
    ar("network", network);
    ar("policy", policy);
    ar("dependency", dependency);
    ar("firmware", firmware);
    ar("hardware", hardware);
    ar("lifecycle", lifecycle);
  }
};

}  // namespace cxf

#endif  // CXF_TYPES_GENERATIONS_HPP
