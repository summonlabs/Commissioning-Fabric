// Commissioning Fabric - freshness and validity windows.
//
// Freshness is never implied by the presence of a record: an observation is
// live only while its freshness window has not elapsed relative to the clock
// the evaluator was given, and an unknown observation instant is never treated
// as recent.
#ifndef CXF_TYPES_TIMEPOINT_HPP
#define CXF_TYPES_TIMEPOINT_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "cxf/support/clock.hpp"
#include "cxf/support/status.hpp"
#include "cxf/support/text.hpp"

namespace cxf {

/// Maximum age accepted for an observation. Constructed either as an explicit
/// window or as "never expires"; the two are distinct states so that a missing
/// or zero window can never be read as an unlimited one by accident.
class FreshnessWindow {
 public:
  constexpr FreshnessWindow() noexcept = default;

  [[nodiscard]] static constexpr FreshnessWindow never() noexcept {
    FreshnessWindow w;
    w.never_ = true;
    return w;
  }

  [[nodiscard]] static constexpr FreshnessWindow of(Duration window) noexcept {
    FreshnessWindow w;
    w.window_ = window;
    return w;
  }

  [[nodiscard]] static Outcome<FreshnessWindow> parse(std::string_view text);

  /// Canonical text: "never", or a decimal number of seconds.
  [[nodiscard]] std::string str() const {
    return never_ ? std::string("never") : to_dec(static_cast<std::uint64_t>(window_.nanos() /
                                                                            kNanosPerSecond));
  }

  [[nodiscard]] constexpr bool allows_all_time() const noexcept { return never_; }
  [[nodiscard]] constexpr Duration window() const noexcept { return window_; }

  /// True when an observation taken at observed_at is no longer live at now.
  /// A negative elapsed time (a clock that moved backwards) is reported as
  /// expired rather than silently accepted as fresh.
  [[nodiscard]] bool is_expired(Timestamp observed_at, Timestamp now) const noexcept {
    if (never_) {
      return false;
    }
    const Duration elapsed = now - observed_at;
    if (elapsed.is_negative()) {
      return true;
    }
    return elapsed > window_;
  }

  friend constexpr bool operator==(FreshnessWindow a, FreshnessWindow b) noexcept {
    return a.never_ == b.never_ && (a.never_ || a.window_ == b.window_);
  }
  friend constexpr bool operator!=(FreshnessWindow a, FreshnessWindow b) noexcept {
    return !(a == b);
  }

 private:
  bool never_{false};
  Duration window_{};
};

/// Normalise an elapsed duration for reporting without inventing precision.
[[nodiscard]] std::string render_duration(Duration d);

}  // namespace cxf

#endif  // CXF_TYPES_TIMEPOINT_HPP
