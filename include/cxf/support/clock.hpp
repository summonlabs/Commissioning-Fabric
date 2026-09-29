// Commissioning Fabric - deterministic time sources.
//
// The runtime never calls the operating system clock directly for anything that
// affects observable semantics: every component receives a Clock. Production
// binaries use SystemClock; tests and benchmarks use ManualClock so that
// freshness, expiry and durability timing are exactly reproducible.
#ifndef CXF_SUPPORT_CLOCK_HPP
#define CXF_SUPPORT_CLOCK_HPP

#include <cstdint>

#include "cxf/support/status.hpp"
#include "cxf/support/text.hpp"

namespace cxf {

/// Nanoseconds since the Unix epoch, UTC. Signed so that pre-epoch values are
/// representable and never silently wrap.
using UnixNanos = std::int64_t;

inline constexpr UnixNanos kNanosPerSecond = 1000000000LL;
inline constexpr UnixNanos kNanosPerMilli = 1000000LL;
inline constexpr UnixNanos kNanosPerMicro = 1000LL;

/// A duration in nanoseconds. Always non-negative in runtime use; the type
/// itself does not clamp so that arithmetic defects remain visible.
class Duration {
 public:
  constexpr Duration() noexcept = default;
  explicit constexpr Duration(UnixNanos nanos) noexcept : nanos_(nanos) {}

  [[nodiscard]] static constexpr Duration from_nanos(UnixNanos v) noexcept {
    return Duration(v);
  }
  [[nodiscard]] static constexpr Duration from_micros(UnixNanos v) noexcept {
    return Duration(v * kNanosPerMicro);
  }
  [[nodiscard]] static constexpr Duration from_millis(UnixNanos v) noexcept {
    return Duration(v * kNanosPerMilli);
  }
  [[nodiscard]] static constexpr Duration from_seconds(UnixNanos v) noexcept {
    return Duration(v * kNanosPerSecond);
  }

  [[nodiscard]] constexpr UnixNanos nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return nanos_ < 0; }

  friend constexpr bool operator==(Duration a, Duration b) noexcept {
    return a.nanos_ == b.nanos_;
  }
  friend constexpr bool operator!=(Duration a, Duration b) noexcept { return !(a == b); }
  friend constexpr bool operator<(Duration a, Duration b) noexcept {
    return a.nanos_ < b.nanos_;
  }
  friend constexpr bool operator<=(Duration a, Duration b) noexcept { return !(b < a); }
  friend constexpr bool operator>(Duration a, Duration b) noexcept { return b < a; }
  friend constexpr bool operator>=(Duration a, Duration b) noexcept { return !(a < b); }

 private:
  UnixNanos nanos_{0};
};

/// An absolute instant in UTC, rendered canonically as RFC 3339 with nanosecond
/// precision and a trailing 'Z'. Formatting is pure arithmetic: no locale, no
/// timezone database, no daylight-saving state.
class Timestamp {
 public:
  constexpr Timestamp() noexcept = default;
  explicit constexpr Timestamp(UnixNanos unix_nanos) noexcept : nanos_(unix_nanos) {}

  [[nodiscard]] static constexpr Timestamp from_unix_nanos(UnixNanos v) noexcept {
    return Timestamp(v);
  }
  [[nodiscard]] static constexpr Timestamp from_unix_seconds(UnixNanos v) noexcept {
    return Timestamp(v * kNanosPerSecond);
  }
  [[nodiscard]] static constexpr Timestamp epoch() noexcept { return Timestamp(0); }

  /// Parse "YYYY-MM-DDTHH:MM:SS[.fraction]Z". Exactly one spelling per instant:
  /// the year is four digits, the separator is 'T', the suffix is 'Z' and the
  /// fraction is 1..9 digits.
  [[nodiscard]] static Outcome<Timestamp> parse_rfc3339(std::string_view text);

  [[nodiscard]] constexpr UnixNanos unix_nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr UnixNanos unix_seconds() const noexcept {
    return nanos_ / kNanosPerSecond;
  }
  [[nodiscard]] std::string to_rfc3339() const;

  friend constexpr bool operator==(Timestamp a, Timestamp b) noexcept {
    return a.nanos_ == b.nanos_;
  }
  friend constexpr bool operator!=(Timestamp a, Timestamp b) noexcept { return !(a == b); }
  friend constexpr bool operator<(Timestamp a, Timestamp b) noexcept {
    return a.nanos_ < b.nanos_;
  }
  friend constexpr bool operator<=(Timestamp a, Timestamp b) noexcept { return !(b < a); }
  friend constexpr bool operator>(Timestamp a, Timestamp b) noexcept { return b < a; }
  friend constexpr bool operator>=(Timestamp a, Timestamp b) noexcept { return !(a < b); }

 private:
  UnixNanos nanos_{0};
};

[[nodiscard]] constexpr Timestamp operator+(Timestamp t, Duration d) noexcept {
  return Timestamp::from_unix_nanos(t.unix_nanos() + d.nanos());
}

[[nodiscard]] constexpr Duration operator-(Timestamp a, Timestamp b) noexcept {
  return Duration::from_nanos(a.unix_nanos() - b.unix_nanos());
}

/// Source of the current instant.
class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock();

  [[nodiscard]] virtual Timestamp now() const = 0;
};

/// Wall-clock time from the operating system. Only used by production entry
/// points and by the durability tests that must observe real elapsed time.
class SystemClock final : public Clock {
 public:
  SystemClock() = default;
  [[nodiscard]] Timestamp now() const override;
};

/// Caller-controlled time. Advancing is explicit so every test that depends on
/// freshness, expiry or ordering is reproducible from its seed.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(Timestamp start = Timestamp::epoch()) noexcept : now_(start) {}

  [[nodiscard]] Timestamp now() const override { return now_; }
  void set(Timestamp t) noexcept { now_ = t; }
  void advance(Duration d) noexcept { now_ = now_ + d; }

 private:
  Timestamp now_{};
};

}  // namespace cxf

#endif  // CXF_SUPPORT_CLOCK_HPP
