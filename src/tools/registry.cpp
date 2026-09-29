#include "cxf/tools/registry.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "cxf/support/status.hpp"
#include "cxf/support/text.hpp"

namespace cxf {
namespace {

/// The documented table, in reason-code order. Every declared code appears
/// exactly once; exit_code_for() reads this table rather than repeating it.
constexpr ExitCodeBinding kExitCodes[] = {
    {Code::kOk, 0},
    // input shape and encoding
    {Code::kMalformedInput, 2},
    {Code::kFieldMissing, 2},
    {Code::kFieldTooLong, 2},
    {Code::kFieldInvalidUtf8, 2},
    {Code::kFieldEmpty, 2},
    {Code::kFieldOutOfRange, 2},
    {Code::kFieldConflict, 2},
    {Code::kReservedFieldNonZero, 2},
    {Code::kUnsupportedFormatVersion, 2},
    {Code::kTrailingBytes, 2},
    // identity and addressing
    {Code::kCandidateNotFound, 3},
    {Code::kCandidateAlreadyExists, 4},
    {Code::kAssetIdentityConflict, 4},
    {Code::kAssetAlreadyCommissioned, 4},
    // lifecycle, ordering and authority
    {Code::kStateNotAllowed, 4},
    {Code::kTransitionNotAllowed, 4},
    {Code::kAttemptNotFound, 3},
    {Code::kCandidateTerminal, 4},
    {Code::kPlanNotFound, 3},
    {Code::kPlanStale, 4},
    {Code::kAuthorityTokenInvalid, 4},
    {Code::kAuthorityTokenExpired, 4},
    {Code::kAuthorityTokenConsumed, 4},
    {Code::kIdempotencyKeyReuse, 4},
    {Code::kGenerationRegression, 4},
    {Code::kFencingTokenStale, 4},
    // evidence
    {Code::kEvidenceUnknownDimension, 5},
    {Code::kEvidenceStale, 5},
    {Code::kEvidenceContradictory, 5},
    {Code::kEvidenceNegative, 5},
    {Code::kEvidenceMissing, 5},
    {Code::kEvidenceGenerationMismatch, 5},
    {Code::kEvidenceDuplicateDigest, 5},
    // dependency, placement, compatibility, policy, service class
    {Code::kDependencyUnsatisified, 6},
    {Code::kDependencyCycle, 6},
    {Code::kDependencyAmbiguous, 6},
    {Code::kPlacementUnbound, 6},
    {Code::kCompatibilityUnsupported, 6},
    {Code::kPolicyNotSatisfied, 6},
    {Code::kServiceClassUnsatisfied, 6},
    // persistence and environment
    {Code::kStorageUnavailable, 7},
    {Code::kStorageCorrupt, 7},
    {Code::kStorageLocked, 7},
    {Code::kStorageIo, 7},
    {Code::kStorageUnsupportedFormat, 7},
    // runtime emergencies
    {Code::kInternalError, 8},
    {Code::kPreconditionViolated, 8},
    {Code::kNotImplemented, 8},
};

}  // namespace

std::span<const ExitCodeBinding> exit_code_table() noexcept { return kExitCodes; }

int exit_code_for(Code code) noexcept {
  for (const ExitCodeBinding& binding : kExitCodes) {
    if (binding.code == code) {
      return binding.exit_code;
    }
  }
  // A code outside the declared domain is an internal failure. It is never
  // reported as success.
  return 8;
}

Status CommandRegistry::add_command(std::string name, std::string usage,
                                    CommandHandler handler) {
  if (name.empty()) {
    return make_error(Code::kFieldConflict, "command name must not be empty");
  }
  if (!is_valid_token(name)) {
    return make_error(Code::kFieldConflict,
                      "command name must be a token of [A-Za-z0-9._:-] characters",
                      escape_for_output(name));
  }
  if (!handler) {
    return make_error(Code::kFieldConflict, "command has no handler",
                      escape_for_output(name));
  }
  if (has_command(name)) {
    return make_error(Code::kFieldConflict, "command is already registered",
                      escape_for_output(name));
  }
  commands_.push_back(Entry{std::move(name), std::move(usage), std::move(handler)});
  return Status::success();
}

Status CommandRegistry::add_option(std::string name, std::string usage) {
  if (name.empty()) {
    return make_error(Code::kFieldConflict, "option name must not be empty");
  }
  if (has_option(name)) {
    return make_error(Code::kFieldConflict, "option is already registered",
                      escape_for_output(name));
  }
  options_.push_back(OptionEntry{std::move(name), std::move(usage)});
  return Status::success();
}

bool CommandRegistry::has_command(std::string_view name) const noexcept {
  return find_command(name) != nullptr;
}

bool CommandRegistry::has_option(std::string_view option) const noexcept {
  for (const OptionEntry& entry : options_) {
    if (entry.name == option) {
      return true;
    }
  }
  return false;
}

const CommandRegistry::Entry* CommandRegistry::find_command(std::string_view name) const
    noexcept {
  for (const Entry& entry : commands_) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

std::string_view CommandRegistry::usage_of(std::string_view name) const noexcept {
  const Entry* entry = find_command(name);
  if (entry == nullptr) {
    return {};
  }
  return entry->usage;
}

Status CommandRegistry::dispatch(const ParsedCommand& command) const {
  if (command.empty()) {
    return make_error(Code::kFieldMissing, "missing command name");
  }
  const Entry* entry = find_command(command.name());
  if (entry == nullptr) {
    return make_error(Code::kFieldOutOfRange,
                      "unknown command " + command.name(), command.name());
  }
  if (!entry->handler) {
    return make_error(Code::kPreconditionViolated, "command has no handler", command.name());
  }
  return entry->handler(command);
}

Status CommandRegistry::check_option(std::string_view option) const {
  if (has_option(option)) {
    return Status::success();
  }
  return make_error(Code::kFieldConflict, "unknown option " + std::string(option),
                    std::string(option));
}

std::string CommandRegistry::usage_text() const {
  std::string out;
  if (!options_.empty()) {
    out.append("options:\n");
    for (const OptionEntry& entry : options_) {
      out.append("  ");
      out.append(entry.usage);
      out.push_back('\n');
    }
  }
  out.append("commands:\n");
  for (const Entry& entry : commands_) {
    out.append("  ");
    out.append(entry.usage);
    out.push_back('\n');
  }
  return out;
}

}  // namespace cxf
