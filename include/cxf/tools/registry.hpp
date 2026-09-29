// Commissioning Fabric - the tools' command registry.
//
// The registry is the one place that knows which commands and options exist,
// what each of them looks like, and which reason code maps to which process
// exit code. Unknown commands and unknown options are rejected here with a
// deterministic reason rather than being silently ignored, and the usage text
// is held in registration order so help output never depends on a container's
// iteration order.
#ifndef CXF_TOOLS_REGISTRY_HPP
#define CXF_TOOLS_REGISTRY_HPP

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cxf/codec/text_codec.hpp"
#include "cxf/support/status.hpp"

namespace cxf {

/// Runs one parsed command and reports success or the reason it was refused.
/// A handler owns nothing global: whatever state it needs is captured when it
/// is registered.
using CommandHandler = std::function<Status(const ParsedCommand& command)>;

/// One row of the documented reason-code to exit-code table.
struct ExitCodeBinding {
  Code code{Code::kOk};
  int exit_code{0};
};

/// The exit-code table, in reason-code order. It is exposed as data so the
/// mapping is documented in exactly one place: exit_code_for() is defined in
/// terms of this table, and help output prints it.
///
///   0  kOk
///   2  input shape: malformed, missing, too long, invalid UTF-8, empty, out of
///      range, conflicting, reserved, unsupported format version, trailing bytes
///   3  addressing: kCandidateNotFound, kAttemptNotFound, kPlanNotFound
///   4  ordering and authority: state, transition, terminal, identity conflict,
///      already commissioned, already exists, stale plan, invalid/expired/
///      consumed token, idempotency reuse, generation regression, stale fence
///   5  evidence: every kEvidence* code
///   6  dependency, placement, compatibility, policy and service class
///   7  storage: every kStorage* code
///   8  kInternalError, kPreconditionViolated, kNotImplemented
[[nodiscard]] std::span<const ExitCodeBinding> exit_code_table() noexcept;

/// Process exit code for a reason code. A numeric value outside the declared
/// domain is an internal failure and maps to 8 rather than to success.
[[nodiscard]] int exit_code_for(Code code) noexcept;

/// Maps command and option names to their handlers and their usage text.
class CommandRegistry {
 public:
  /// One registered command, in registration order.
  struct Entry {
    std::string name{};
    std::string usage{};
    CommandHandler handler{};
  };

  /// One registered global option, in registration order.
  struct OptionEntry {
    std::string name{};
    std::string usage{};
  };

  /// Register a command. An empty name, a name that is not a token, a null
  /// handler or a name already registered is rejected (kFieldConflict).
  [[nodiscard]] Status add_command(std::string name, std::string usage,
                                   CommandHandler handler);

  /// Register a global option. An empty name or a duplicate is rejected
  /// (kFieldConflict).
  [[nodiscard]] Status add_option(std::string name, std::string usage);

  [[nodiscard]] bool has_command(std::string_view name) const noexcept;
  [[nodiscard]] bool has_option(std::string_view name) const noexcept;

  [[nodiscard]] const std::vector<Entry>& commands() const noexcept { return commands_; }
  [[nodiscard]] const std::vector<OptionEntry>& options() const noexcept { return options_; }
  [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }

  /// Registered command, or nullptr when the name is unknown.
  [[nodiscard]] const Entry* find_command(std::string_view name) const noexcept;
  /// Usage line of a registered command, or nothing when it is unknown.
  [[nodiscard]] std::string_view usage_of(std::string_view name) const noexcept;

  /// Run the handler of command.name(). An empty command is kFieldMissing and
  /// an unregistered name is kFieldOutOfRange naming the command.
  [[nodiscard]] Status dispatch(const ParsedCommand& command) const;

  /// kOk for a registered option; kFieldConflict naming an unknown option.
  [[nodiscard]] Status check_option(std::string_view option) const;

  /// The ordered usage text: the options, then the commands, each line indented
  /// by two spaces under its section header, terminated by a newline.
  [[nodiscard]] std::string usage_text() const;

 private:
  std::vector<Entry> commands_{};
  std::vector<OptionEntry> options_{};
};

}  // namespace cxf

#endif  // CXF_TOOLS_REGISTRY_HPP
