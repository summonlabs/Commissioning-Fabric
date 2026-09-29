#include "cxf/tools/textlog.hpp"

#include <cstddef>
#include <ostream>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>

#include "cxf/support/status.hpp"
#include "cxf/support/text.hpp"

namespace cxf {
namespace {

/// A stream buffer that accepts every byte and stores none.
class NullBuffer final : public std::streambuf {
 protected:
  int_type overflow(int_type c) override { return traits_type::not_eof(c); }
  std::streamsize xsputn(const char* /*data*/, std::streamsize count) override {
    return count;
  }
  int sync() override { return 0; }
};

/// The process-wide null stream. Constructed once, never mutated, and safe to
/// write to from any thread that the underlying iostreams allow.
[[nodiscard]] std::ostream& null_ostream() {
  static NullBuffer buffer;
  static std::ostream stream(&buffer);
  return stream;
}

}  // namespace

std::string_view log_level_name(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::kTrace:
      return "trace";
    case LogLevel::kInfo:
      return "info";
    case LogLevel::kWarn:
      return "warn";
    case LogLevel::kError:
      return "error";
  }
  return "unknown";
}

Outcome<LogLevel> parse_log_level(std::string_view text) {
  if (text == "trace") {
    return LogLevel::kTrace;
  }
  if (text == "info") {
    return LogLevel::kInfo;
  }
  if (text == "warn") {
    return LogLevel::kWarn;
  }
  if (text == "error") {
    return LogLevel::kError;
  }
  return make_error(Code::kFieldOutOfRange, "unknown log level", std::string(text));
}

std::ostream& null_stream() { return null_ostream(); }

TextLog::TextLog(std::ostream& sink, LogLevel threshold) noexcept
    : sink_(&sink), threshold_(threshold) {}

TextLog TextLog::null(LogLevel threshold) noexcept { return TextLog(null_stream(), threshold); }

bool TextLog::enabled(LogLevel level) const noexcept {
  if (sink_ == nullptr) {
    return false;
  }
  return static_cast<std::uint8_t>(level) >= static_cast<std::uint8_t>(threshold_);
}

std::string TextLog::format_record(LogLevel level, std::string_view component,
                                   std::string_view command, std::string_view detail) {
  std::string out;
  out.reserve(48 + component.size() + command.size() + detail.size());
  out.append("level=");
  out.append(log_level_name(level));
  out.append(" component=");
  out.append(escape_for_output(component));
  out.append(" command=");
  out.append(escape_for_output(command));
  out.append(" detail=");
  out.append(escape_for_output(detail));
  return out;
}

void TextLog::record(LogLevel level, std::string_view component, std::string_view command,
                     std::string_view detail) {
  if (!enabled(level)) {
    return;
  }
  *sink_ << format_record(level, component, command, detail) << '\n';
}

void TextLog::trace(std::string_view component, std::string_view command,
                    std::string_view detail) {
  record(LogLevel::kTrace, component, command, detail);
}

void TextLog::info(std::string_view component, std::string_view command,
                   std::string_view detail) {
  record(LogLevel::kInfo, component, command, detail);
}

void TextLog::warn(std::string_view component, std::string_view command,
                   std::string_view detail) {
  record(LogLevel::kWarn, component, command, detail);
}

void TextLog::error(std::string_view component, std::string_view command,
                    std::string_view detail) {
  record(LogLevel::kError, component, command, detail);
}

}  // namespace cxf
