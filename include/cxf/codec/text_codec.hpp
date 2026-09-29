// Commissioning Fabric - the administration CLI's text grammar.
//
// The CLI speaks one line-oriented grammar: one command per line, '#' starts a
// comment, blank lines are ignored, and the fields that follow the command name
// are whitespace-separated key=value pairs. A value may be quoted with double
// quotes to carry spaces; inside a quoted value a backslash escapes a quote or
// a backslash.
//
// The codec is pure. It turns text into typed request values, prints nothing,
// exits nothing and never touches the filesystem. Rejection is precise: an
// unknown or repeated key is kFieldConflict naming the key, a missing required
// key is kFieldMissing, and a malformed value is kMalformedInput or
// kFieldOutOfRange. Every value is checked for shape here; whether a request is
// *allowed* is the runtime's decision, never the codec's.
#ifndef CXF_CODEC_TEXT_CODEC_HPP
#define CXF_CODEC_TEXT_CODEC_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cxf/runtime/fabric.hpp"
#include "cxf/support/status.hpp"
#include "cxf/types/timepoint.hpp"

namespace cxf {

/// Longest accepted command line. The bound is part of the grammar contract:
/// a longer line is rejected rather than read without limit.
inline constexpr std::size_t kMaxCommandLineBytes = 8192;

/// Largest number of fields one command may carry.
inline constexpr std::size_t kMaxCommandFields = 128;

/// Events reported by `events` when no limit is given: every event the runtime
/// still retains.
inline constexpr std::size_t kDefaultEventLimit = kMaxRetainedEvents;

/// One key=value pair, in the order it was written.
struct CommandField {
  std::string key{};
  std::string value{};
};

/// A parsed command: the command name plus its fields in written order.
/// Repeated keys are preserved rather than collapsed, so each command decides
/// whether a repetition is meaningful (dep, fact) or a conflict.
class ParsedCommand {
 public:
  ParsedCommand() = default;
  ParsedCommand(std::string name, std::vector<CommandField> fields)
      : name_(std::move(name)), fields_(std::move(fields)) {}

  /// True exactly for a blank or comment-only line.
  [[nodiscard]] bool empty() const noexcept { return name_.empty(); }
  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] const std::vector<CommandField>& fields() const noexcept { return fields_; }

  [[nodiscard]] std::size_t count(std::string_view key) const noexcept;
  [[nodiscard]] bool has(std::string_view key) const noexcept;
  /// First value of `key`, or nothing when the key is absent.
  [[nodiscard]] std::optional<std::string_view> find(std::string_view key) const noexcept;
  /// Every value of `key`, in written order.
  [[nodiscard]] std::vector<std::string_view> all(std::string_view key) const;
  /// First value of `key`; kFieldMissing naming the key when it is absent.
  [[nodiscard]] Outcome<std::string_view> require(std::string_view key) const;

 private:
  std::string name_{};
  std::vector<CommandField> fields_{};
};

/// One field of the grammar.
struct CommandFieldSpec {
  /// Field key as written on the command line.
  std::string_view key{};
  /// Placeholder text used by the usage line, for example "<token>".
  std::string_view placeholder{};
  bool required{false};
  bool repeatable{false};
};

/// One command of the grammar.
struct CommandSpec {
  std::string_view name{};
  std::vector<CommandFieldSpec> fields{};
};

/// The whole grammar, in the order the usage text lists it.
[[nodiscard]] const std::vector<CommandSpec>& command_specs();

/// Spec of one command, or nullptr when the name is not part of the grammar.
[[nodiscard]] const CommandSpec* find_command_spec(std::string_view name) noexcept;

/// Usage line of one command, for example
/// "admit name=<token> [registry=<token>] [dep=<kind>:<name>:<gen>[:optional]]...".
/// Empty when the name is not part of the grammar.
[[nodiscard]] std::string command_usage_line(std::string_view name);

/// Every usage line, one per command in grammar order, joined by newlines and
/// without a trailing newline.
[[nodiscard]] std::string usage_text();

/// Parse one line of the grammar. A blank or comment-only line yields an empty
/// command. The line is rejected when it is longer than kMaxCommandLineBytes.
[[nodiscard]] Outcome<ParsedCommand> parse_command_line(std::string_view line);

/// Build a command from an already-split argument vector: the command-line
/// form, where the shell has applied quoting, so every argument must itself be
/// a key=value pair with a token-shaped key.
[[nodiscard]] Outcome<ParsedCommand> parse_command_arguments(
    std::string_view command, const std::vector<std::string_view>& arguments);

/// Check the fields of a parsed command against the grammar of its command:
/// an unknown key or a repeated non-repeatable key is kFieldConflict naming the
/// key, a missing required key is kFieldMissing, and a command that is not part
/// of the grammar is kFieldOutOfRange.
[[nodiscard]] Status validate_command_fields(const ParsedCommand& command);

/// Parse "<seconds>" or "never" into a freshness window. Used by age= and
/// validity=; the two are the only spellings, so a missing window can never be
/// read as an unlimited one.
[[nodiscard]] Outcome<FreshnessWindow> parse_freshness_window(std::string_view text);

// ---------------------------------------------------------------------------
// One parser per command. Each returns the request the runtime consumes, or the
// precise reason the fields do not form one.
// ---------------------------------------------------------------------------

/// version
[[nodiscard]] Status parse_version(const ParsedCommand& command);
/// init
[[nodiscard]] Status parse_init(const ParsedCommand& command);
/// store
[[nodiscard]] Status parse_store(const ParsedCommand& command);
/// admit name=<token> [registry=<token>] ...
[[nodiscard]] Outcome<AdmitCandidateRequest> parse_admit(const ParsedCommand& command);
/// identify candidate=<name> asset=<n> registry=<token> evidence=<n> ...
[[nodiscard]] Outcome<BindIdentityRequest> parse_identify(const ParsedCommand& command);
/// place candidate=<name> evidence=<n> site=<n> rack=<n> position=<token> ...
[[nodiscard]] Outcome<BindPlacementRequest> parse_place(const ParsedCommand& command);
/// evidence candidate=<name> dimension=<dimension> subject=<token> verdict=...
/// observed=<rfc3339|now> [age=<seconds>|never].
///
/// The observation instant is required: an observation whose instant is unknown
/// is rejected (kFieldMissing) rather than recorded as the epoch, because the
/// epoch would make a fresh observation look expired. The literal "now" resolves
/// to `observed_now`, which the caller reads from its own clock - the codec
/// never reads a clock, and it refuses a caller-supplied instant that is not a
/// real instant. An absent age= leaves the fabric's configured default freshness
/// in place, which is a policy default and not an invented observation.
[[nodiscard]] Outcome<SubmitEvidenceRequest> parse_evidence(const ParsedCommand& command,
                                                            const FabricOptions& defaults,
                                                            Timestamp observed_now);
/// evaluate candidate=<name> [request=<n>]
[[nodiscard]] Outcome<EvaluateReadinessRequest> parse_evaluate(const ParsedCommand& command);
/// authorize candidate=<name> plan=<n> digest=<hex> [validity=<seconds>|never]
/// An absent validity= leaves the fabric's configured default in place.
[[nodiscard]] Outcome<AuthorizeActivationRequest> parse_authorize(
    const ParsedCommand& command, const FabricOptions& defaults = FabricOptions{});
/// report candidate=<name> token=<n> binding=<hex> result=...
[[nodiscard]] Outcome<ReportActivationRequest> parse_report(const ParsedCommand& command);
/// quarantine candidate=<name> reason=<reason> [detail=<text>] [request=<n>]
[[nodiscard]] Outcome<QuarantineRequest> parse_quarantine(const ParsedCommand& command);
/// release candidate=<name> [detail=<text>] [request=<n>]
[[nodiscard]] Outcome<ReleaseQuarantineRequest> parse_release(const ParsedCommand& command);
/// cancel candidate=<name> [detail=<text>] [request=<n>]
[[nodiscard]] Outcome<CancelRequest> parse_cancel(const ParsedCommand& command);
/// facility [topology=<n>] ... [fact=<kind>:<name>:<gen>]... [detail=<text>]
[[nodiscard]] Outcome<FacilityChangeRequest> parse_facility(const ParsedCommand& command);
/// status candidate=<name>; the result is the candidate name to query.
[[nodiscard]] Outcome<std::string> parse_status(const ParsedCommand& command);
/// explain candidate=<name>; the result is the candidate name to explain.
[[nodiscard]] Outcome<std::string> parse_explain(const ParsedCommand& command);
/// list
[[nodiscard]] Status parse_list(const ParsedCommand& command);
/// events [limit=<n>]; the result is the number of events to report.
[[nodiscard]] Outcome<std::size_t> parse_events(const ParsedCommand& command);
/// checkpoint
[[nodiscard]] Status parse_checkpoint(const ParsedCommand& command);
/// verify
[[nodiscard]] Status parse_verify(const ParsedCommand& command);

}  // namespace cxf

#endif  // CXF_CODEC_TEXT_CODEC_HPP
