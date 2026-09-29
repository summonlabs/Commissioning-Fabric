#include "cxf/support/clock.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <string>

#include "cxf/support/text.hpp"

namespace cxf {
namespace {

/// Days from 1970-01-01 for a proleptic Gregorian civil date (Howard Hinnant's
/// algorithm, valid for the whole range we accept).
[[nodiscard]] constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m,
                                                     unsigned d) noexcept {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);              // [0, 399]
  const unsigned mp = (m > 2u) ? (m - 3u) : (m + 9u);
  const unsigned doy = (153u * mp + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;      // [0, 146096]
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

/// Inverse of days_from_civil.
constexpr void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m,
                               unsigned& d) noexcept {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);           // [0, 146096]
  const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  const unsigned mp = (5u * doy + 2u) / 153u;
  d = doy - (153u * mp + 2u) / 5u + 1u;
  m = (mp < 10u) ? (mp + 3u) : (mp - 9u);
  y += (m <= 2u) ? 1 : 0;
}

[[nodiscard]] bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] unsigned two_digits(std::string_view s, std::size_t at) noexcept {
  return static_cast<unsigned>(s[at] - '0') * 10u + static_cast<unsigned>(s[at + 1] - '0');
}

[[nodiscard]] unsigned four_digits(std::string_view s, std::size_t at) noexcept {
  return (static_cast<unsigned>(s[at] - '0') * 1000u +
          static_cast<unsigned>(s[at + 1] - '0') * 100u +
          static_cast<unsigned>(s[at + 2] - '0') * 10u +
          static_cast<unsigned>(s[at + 3] - '0'));
}

[[nodiscard]] unsigned days_in_month(std::int64_t y, unsigned m) noexcept {
  static constexpr std::array<unsigned, 12> kDays = {31, 28, 31, 30, 31, 30,
                                                     31, 31, 30, 31, 30, 31};
  if (m != 2u) {
    return kDays[m - 1u];
  }
  const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
  return leap ? 29u : 28u;
}

}  // namespace

Clock::~Clock() = default;

Timestamp SystemClock::now() const {
  const auto tp = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(tp).count();
  return Timestamp::from_unix_nanos(static_cast<UnixNanos>(nanos));
}

Outcome<Timestamp> Timestamp::parse_rfc3339(std::string_view text) {
  // YYYY-MM-DDTHH:MM:SS[.frac]Z
  constexpr std::size_t kMinLength = 20;
  // 20 characters plus a fraction of at most ten digits, so that an over-long
  // fraction is reported as an out-of-range fraction rather than as a
  // generic length failure.
  if (text.size() < kMinLength || text.size() > 31) {
    return make_error(Code::kMalformedInput, "timestamp length is not a valid RFC 3339 instant",
                      "length=" + std::to_string(text.size()));
  }
  if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
      text[16] != ':' || text.back() != 'Z') {
    return make_error(Code::kMalformedInput, "timestamp separators are not canonical");
  }
  for (std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u, 17u, 18u}) {
    if (!is_digit(text[i])) {
      return make_error(Code::kMalformedInput, "timestamp contains a non-digit character",
                        "index=" + std::to_string(i));
    }
  }

  const auto year = static_cast<std::int64_t>(four_digits(text, 0));
  const unsigned month = two_digits(text, 5);
  const unsigned day = two_digits(text, 8);
  const unsigned hour = two_digits(text, 11);
  const unsigned minute = two_digits(text, 14);
  const unsigned second = two_digits(text, 17);

  if (year < 1678 || year > 2261) {
    return make_error(Code::kFieldOutOfRange, "timestamp year is outside the supported range",
                      "year=" + std::to_string(year));
  }
  if (month < 1u || month > 12u) {
    return make_error(Code::kFieldOutOfRange, "timestamp month is not 01..12");
  }
  if (day < 1u || day > days_in_month(year, month)) {
    return make_error(Code::kFieldOutOfRange, "timestamp day is not valid for the month");
  }
  if (hour > 23u || minute > 59u || second > 59u) {
    // Leap seconds are deliberately not representable: the runtime never needs
    // a 61st second and accepting one would break monotonic arithmetic.
    return make_error(Code::kFieldOutOfRange, "timestamp time-of-day is out of range");
  }

  UnixNanos fraction = 0;
  if (text.size() > kMinLength) {
    if (text[19] != '.') {
      return make_error(Code::kMalformedInput, "timestamp fraction must be introduced by '.'");
    }
    const std::size_t digits = text.size() - kMinLength - 1;
    if (digits == 0 || digits > 9) {
      return make_error(Code::kFieldOutOfRange, "timestamp fraction must be 1..9 digits",
                        "digits=" + std::to_string(digits));
    }
    UnixNanos scale = 100000000;
    for (std::size_t i = 0; i < digits; ++i) {
      const char c = text[20 + i];
      if (!is_digit(c)) {
        return make_error(Code::kMalformedInput, "timestamp fraction contains a non-digit");
      }
      fraction += static_cast<UnixNanos>(c - '0') * scale;
      scale /= 10;
    }
  }

  const std::int64_t days = days_from_civil(year, month, day);
  const UnixNanos seconds =
      days * 86400 + static_cast<UnixNanos>(hour) * 3600 + static_cast<UnixNanos>(minute) * 60 +
      static_cast<UnixNanos>(second);
  return Timestamp::from_unix_nanos(seconds * kNanosPerSecond + fraction);
}

std::string Timestamp::to_rfc3339() const {
  UnixNanos nanos = nanos_;
  std::int64_t seconds = nanos / kNanosPerSecond;
  UnixNanos fraction = nanos % kNanosPerSecond;
  if (fraction < 0) {
    fraction += kNanosPerSecond;
    seconds -= 1;
  }

  // Floor division, not truncation: a pre-epoch instant such as -1s belongs to
  // 1969-12-31, and truncation towards zero would place it on 1970-01-01.
  std::int64_t days = seconds / 86400;
  std::int64_t rem = seconds % 86400;
  if (rem < 0) {
    rem += 86400;
    days -= 1;
  }
  std::int64_t year = 0;
  unsigned month = 0;
  unsigned day = 0;
  civil_from_days(days, year, month, day);

  const auto hour = static_cast<unsigned>(rem / 3600);
  const auto minute = static_cast<unsigned>((rem % 3600) / 60);
  const auto second = static_cast<unsigned>(rem % 60);

  std::string out;
  out.reserve(30);
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02u:%02u:%02u", static_cast<long long>(year),
                month, day, hour, minute, second);
  out.append(buf);
  if (fraction != 0) {
    std::snprintf(buf, sizeof(buf), ".%09lld", static_cast<long long>(fraction));
    std::string frac(buf);
    while (!frac.empty() && frac.back() == '0') {
      frac.pop_back();
    }
    out.append(frac);
  }
  out.push_back('Z');
  return out;
}

}  // namespace cxf
