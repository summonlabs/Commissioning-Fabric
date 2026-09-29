// Commissioning Fabric - deterministic structured logging for the tools.
//
// One record is one line of key=value pairs: the level, the component, the
// command the record belongs to, and the detail. Records carry no timestamp and
// no process identity, so the same input always renders the same bytes; nothing
// is transmitted anywhere and the logger holds no global state. The sink is
// supplied by the caller, which is what lets quiet mode route every record to
// the null stream.
#ifndef CXF_TOOLS_TEXTLOG_HPP
#define CXF_TOOLS_TEXTLOG_HPP

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

#include "cxf/support/status.hpp"

namespace cxf {

/// Severity of one record, ordered from least to most severe: a threshold
/// admits itself and every higher level.
enum class LogLevel : std::uint8_t {
  kTrace = 0,
  kInfo = 1,
  kWarn = 2,
  kError = 3,
};

/// Canonical lowercase name of a level.
[[nodiscard]] std::string_view log_level_name(LogLevel level) noexcept;

/// Parse a level name. An unknown name is kFieldOutOfRange.
[[nodiscard]] Outcome<LogLevel> parse_log_level(std::string_view text);

/// A stream that discards every byte written to it. This is the sink of
/// TextLog::null() and of quiet mode; it is shared, stateless and safe to use
/// from several loggers.
[[nodiscard]] std::ostream& null_stream();

/// A single-line key=value logger writing to a caller-supplied stream.
///
/// The rendered record is exactly
///   level=<level> component=<component> command=<command> detail=<detail>
/// with each value escaped for single-line output and a trailing newline. An
/// empty component, command or detail renders as an empty value rather than
/// being omitted, so a reader can always tell "absent" from "empty".
class TextLog {
 public:
  /// Log to `sink`, discarding records below `threshold`.
  explicit TextLog(std::ostream& sink, LogLevel threshold = LogLevel::kInfo) noexcept;

  /// A logger that discards every record.
  [[nodiscard]] static TextLog null(LogLevel threshold = LogLevel::kError) noexcept;

  /// The sink this logger writes to.
  [[nodiscard]] std::ostream& sink() const noexcept { return *sink_; }

  [[nodiscard]] LogLevel threshold() const noexcept { return threshold_; }
  void set_threshold(LogLevel threshold) noexcept { threshold_ = threshold; }

  /// True when a record at `level` would be written.
  [[nodiscard]] bool enabled(LogLevel level) const noexcept;

  /// Write one record when the level is admitted by the threshold.
  void record(LogLevel level, std::string_view component, std::string_view command,
              std::string_view detail);
  void trace(std::string_view component, std::string_view command, std::string_view detail);
  void info(std::string_view component, std::string_view command, std::string_view detail);
  void warn(std::string_view component, std::string_view command, std::string_view detail);
  void error(std::string_view component, std::string_view command, std::string_view detail);

  /// The exact line a record renders to, without writing it. This is the only
  /// definition of the record format; record() writes precisely this line.
  [[nodiscard]] static std::string format_record(LogLevel level, std::string_view component,
                                                 std::string_view command,
                                                 std::string_view detail);

 private:
  std::ostream* sink_{nullptr};
  LogLevel threshold_{LogLevel::kInfo};
};

}  // namespace cxf

#endif  // CXF_TOOLS_TEXTLOG_HPP
