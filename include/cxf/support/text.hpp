// Commissioning Fabric - text validation and rendering helpers.
#ifndef CXF_SUPPORT_TEXT_HPP
#define CXF_SUPPORT_TEXT_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cxf/support/status.hpp"

namespace cxf {

/// Validate that a byte sequence is well-formed UTF-8 with no over-long forms,
/// no surrogate code points and no bytes above U+10FFFF. Rejects embedded NUL.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

/// Validate a short identifier token: 1..64 bytes of [A-Za-z0-9._:-].
[[nodiscard]] bool is_valid_token(std::string_view text) noexcept;

/// Validate a display name: 1..kMaxTextFieldBytes well-formed UTF-8 bytes,
/// with no control characters other than tab/space.
[[nodiscard]] bool is_valid_display_text(std::string_view text) noexcept;

/// Escape a string for single-line output: non-printable bytes become \xNN.
[[nodiscard]] std::string escape_for_output(std::string_view text);

/// Render an unsigned value as lowercase hexadecimal without a prefix.
[[nodiscard]] std::string to_hex(std::uint64_t value);

/// Render an unsigned value as decimal.
[[nodiscard]] std::string to_dec(std::uint64_t value);

/// Render a signed value as decimal.
[[nodiscard]] std::string to_dec_signed(std::int64_t value);

/// Parse a non-negative decimal integer. Rejects leading '+', leading zeros
/// beyond "0", whitespace, empty input and overflow.
[[nodiscard]] Outcome<std::uint64_t> parse_u64(std::string_view text);

/// Parse a signed decimal integer. Rejects whitespace, empty input, overflow.
[[nodiscard]] Outcome<std::int64_t> parse_i64(std::string_view text);

/// Parse a lowercase hexadecimal integer (no prefix).
[[nodiscard]] Outcome<std::uint64_t> parse_hex_u64(std::string_view text);

/// Case-sensitive ASCII equality, used for the keyword grammar.
[[nodiscard]] bool equals_ascii(std::string_view a, std::string_view b) noexcept;

/// ASCII upper-case copy.
[[nodiscard]] std::string upper_ascii(std::string_view text);

/// Trim ASCII spaces and tabs from both ends.
[[nodiscard]] std::string_view trim_ascii(std::string_view text) noexcept;

/// Split a whole string on a single delimiter, preserving empty fields.
[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char delim);

/// True when a filesystem name is safe to create inside a fabric directory:
/// no separators, no '..', no empty string, no reserved Windows device names,
/// no alternate-data-stream colon, no trailing dot or space.
[[nodiscard]] bool is_safe_path_component(std::string_view name) noexcept;

}  // namespace cxf

#endif  // CXF_SUPPORT_TEXT_HPP
