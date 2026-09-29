#include "cxf/types/timepoint.hpp"

#include <string>

namespace cxf {

Outcome<FreshnessWindow> FreshnessWindow::parse(std::string_view text) {
  if (text == "never") {
    return FreshnessWindow::never();
  }
  const Outcome<std::uint64_t> seconds = parse_u64(text);
  if (!seconds.ok()) {
    return make_error(Code::kMalformedInput,
                      "freshness window must be 'never' or a decimal number of seconds",
                      std::string(text));
  }
  return FreshnessWindow::of(Duration::from_seconds(static_cast<UnixNanos>(seconds.value())));
}

std::string render_duration(Duration d) {
  if (d.is_negative()) {
    return "-" + to_dec(static_cast<std::uint64_t>(-d.nanos())) + "ns";
  }
  const UnixNanos nanos = d.nanos();
  if (nanos % kNanosPerSecond == 0) {
    return to_dec(static_cast<std::uint64_t>(nanos / kNanosPerSecond)) + "s";
  }
  if (nanos % kNanosPerMilli == 0) {
    return to_dec(static_cast<std::uint64_t>(nanos / kNanosPerMilli)) + "ms";
  }
  return to_dec(static_cast<std::uint64_t>(nanos)) + "ns";
}

}  // namespace cxf
