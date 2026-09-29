#include "cxf/support/text.hpp"

#include "cxf/support/serial.hpp"  // kMaxTextFieldBytes: the documented field bound

#include <array>
#include <cctype>
#include <cstdio>

namespace cxf {
namespace {

constexpr char kHexLower[] = "0123456789abcdef";

[[nodiscard]] bool is_ascii_alpha(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
[[nodiscard]] bool is_ascii_digit(char c) noexcept {
  return c >= '0' && c <= '9';
}
[[nodiscard]] bool is_token_punct(char c) noexcept {
  return c == '.' || c == '_' || c == ':' || c == '-';
}

/// Decode one UTF-8 scalar starting at `i`. Returns the code point and sets
/// `width`, or returns -1 for a malformed sequence.
[[nodiscard]] std::int32_t decode_utf8(std::string_view text, std::size_t i,
                                       std::size_t& width) noexcept {
  const auto byte_at = [&](std::size_t k) {
    return static_cast<std::uint8_t>(text[i + k]);
  };
  const auto b0 = byte_at(0);
  if (b0 < 0x80u) {
    width = 1;
    return static_cast<std::int32_t>(b0);
  }
  if (b0 >= 0xC2u && b0 <= 0xDFu) {
    if (i + 1 >= text.size()) {
      return -1;
    }
    const auto b1 = byte_at(1);
    if ((b1 & 0xC0u) != 0x80u) {
      return -1;
    }
    width = 2;
    return static_cast<std::int32_t>(((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu));
  }
  if (b0 >= 0xE0u && b0 <= 0xEFu) {
    if (i + 2 >= text.size()) {
      return -1;
    }
    const auto b1 = byte_at(1);
    const auto b2 = byte_at(2);
    if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) {
      return -1;
    }
    // Reject over-long forms and surrogates.
    if (b0 == 0xE0u && b1 < 0xA0u) {
      return -1;
    }
    if (b0 == 0xEDu && b1 >= 0xA0u) {
      return -1;
    }
    width = 3;
    return static_cast<std::int32_t>(((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) |
                                     (b2 & 0x3Fu));
  }
  if (b0 >= 0xF0u && b0 <= 0xF4u) {
    if (i + 3 >= text.size()) {
      return -1;
    }
    const auto b1 = byte_at(1);
    const auto b2 = byte_at(2);
    const auto b3 = byte_at(3);
    if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u || (b3 & 0xC0u) != 0x80u) {
      return -1;
    }
    if (b0 == 0xF0u && b1 < 0x90u) {
      return -1;
    }
    if (b0 == 0xF4u && b1 >= 0x90u) {
      return -1;
    }
    width = 4;
    return static_cast<std::int32_t>(((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) |
                                     ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu));
  }
  return -1;
}

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t i = 0;
  while (i < text.size()) {
    std::size_t width = 0;
    const std::int32_t cp = decode_utf8(text, i, width);
    if (cp < 0) {
      return false;
    }
    if (cp == 0) {
      return false;  // Embedded NUL is never accepted in text fields.
    }
    i += width;
  }
  return true;
}

bool is_valid_token(std::string_view text) noexcept {
  if (text.empty() || text.size() > 64) {
    return false;
  }
  for (char c : text) {
    if (!is_ascii_alpha(c) && !is_ascii_digit(c) && !is_token_punct(c)) {
      return false;
    }
  }
  return true;
}

bool is_valid_display_text(std::string_view text) noexcept {
  if (text.empty() || text.size() > kMaxTextFieldBytes) {
    return false;
  }
  std::size_t i = 0;
  while (i < text.size()) {
    std::size_t width = 0;
    const std::int32_t cp = decode_utf8(text, i, width);
    if (cp < 0 || cp == 0) {
      return false;
    }
    if (cp < 0x20 && cp != '\t') {
      return false;
    }
    if (cp == 0x7F) {
      return false;
    }
    i += width;
  }
  return true;
}

std::string escape_for_output(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20 || c == 0x7F) {
      out += "\\x";
      out.push_back(kHexLower[(c >> 4) & 0x0F]);
      out.push_back(kHexLower[c & 0x0F]);
    } else {
      out.push_back(raw);
    }
  }
  return out;
}

std::string to_hex(std::uint64_t value) {
  if (value == 0) {
    return "0";
  }
  std::array<char, 16> buf{};
  std::size_t n = 0;
  while (value != 0) {
    buf[n++] = kHexLower[value & 0x0Fu];
    value >>= 4u;
  }
  std::string out;
  out.reserve(n);
  for (std::size_t i = n; i > 0; --i) {
    out.push_back(buf[i - 1]);
  }
  return out;
}

std::string to_dec(std::uint64_t value) { return std::to_string(value); }

std::string to_dec_signed(std::int64_t value) { return std::to_string(value); }

Outcome<std::uint64_t> parse_u64(std::string_view text) {
  if (text.empty()) {
    return make_error(Code::kFieldEmpty, "expected a decimal integer");
  }
  if (text.size() > 1 && text[0] == '0') {
    return make_error(Code::kMalformedInput,
                      "leading zeros are not accepted in decimal integers",
                      std::string(text));
  }
  std::uint64_t value = 0;
  for (char c : text) {
    if (!is_ascii_digit(c)) {
      return make_error(Code::kMalformedInput, "expected decimal digits only",
                        std::string(text));
    }
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10u) {
      return make_error(Code::kFieldOutOfRange, "decimal integer overflow",
                        std::string(text));
    }
    value = value * 10u + digit;
  }
  return value;
}

Outcome<std::int64_t> parse_i64(std::string_view text) {
  if (text.empty()) {
    return make_error(Code::kFieldEmpty, "expected a signed decimal integer");
  }
  bool negative = false;
  if (text[0] == '-') {
    negative = true;
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return make_error(Code::kMalformedInput, "sign with no digits");
  }
  auto magnitude = parse_u64(text);
  if (!magnitude.ok()) {
    return magnitude.status();
  }
  constexpr auto kMax = static_cast<std::uint64_t>(INT64_MAX);
  if (negative) {
    if (*magnitude > kMax + 1u) {
      return make_error(Code::kFieldOutOfRange, "signed integer underflow");
    }
    if (*magnitude == kMax + 1u) {
      return INT64_MIN;
    }
    return -static_cast<std::int64_t>(*magnitude);
  }
  if (*magnitude > kMax) {
    return make_error(Code::kFieldOutOfRange, "signed integer overflow");
  }
  return static_cast<std::int64_t>(*magnitude);
}

Outcome<std::uint64_t> parse_hex_u64(std::string_view text) {
  if (text.empty()) {
    return make_error(Code::kFieldEmpty, "expected hexadecimal digits");
  }
  if (text.size() > 16) {
    return make_error(Code::kFieldOutOfRange, "hexadecimal integer too long",
                      std::string(text));
  }
  std::uint64_t value = 0;
  for (char c : text) {
    std::uint64_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<std::uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<std::uint64_t>(c - 'a' + 10);
    } else {
      return make_error(Code::kMalformedInput,
                        "expected lowercase hexadecimal digits only",
                        std::string(text));
    }
    value = (value << 4u) | digit;
  }
  return value;
}

bool equals_ascii(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

std::string upper_ascii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if (c >= 'a' && c <= 'z') {
      out.push_back(static_cast<char>(c - 'a' + 'A'));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string_view trim_ascii(std::string_view text) noexcept {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t' ||
                         text[begin] == '\r' || text[begin] == '\n')) {
    ++begin;
  }
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                         text[end - 1] == '\r' || text[end - 1] == '\n')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

std::vector<std::string_view> split(std::string_view text, char delim) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t pos = text.find(delim, start);
    if (pos == std::string_view::npos) {
      parts.push_back(text.substr(start));
      break;
    }
    parts.push_back(text.substr(start, pos - start));
    start = pos + 1;
  }
  return parts;
}

bool is_safe_path_component(std::string_view name) noexcept {
  if (name.empty() || name.size() > 96) {
    return false;
  }
  if (name == "." || name == "..") {
    return false;
  }
  if (name.back() == '.' || name.back() == ' ') {
    return false;  // Windows strips these, which would alias two names.
  }
  for (char c : name) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) {
      return false;
    }
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
        c == '<' || c == '>' || c == '|') {
      return false;
    }
  }
  // Reserved MS-DOS device names remain reserved with any extension.
  std::string_view stem = name;
  const std::size_t dot = name.find('.');
  if (dot != std::string_view::npos) {
    stem = name.substr(0, dot);
  }
  static constexpr std::array<std::string_view, 22> kReserved = {
      "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
      "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
      "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
  const std::string upper = upper_ascii(stem);
  for (const auto& reserved : kReserved) {
    if (upper == reserved) {
      return false;
    }
  }
  return true;
}

}  // namespace cxf
