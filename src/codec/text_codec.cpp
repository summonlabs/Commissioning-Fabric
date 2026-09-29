#include "cxf/codec/text_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cxf/model/candidate.hpp"
#include "cxf/model/dependency.hpp"
#include "cxf/runtime/fabric.hpp"
#include "cxf/support/clock.hpp"
#include "cxf/support/digest.hpp"
#include "cxf/support/status.hpp"
#include "cxf/support/text.hpp"
#include "cxf/types/enums.hpp"
#include "cxf/types/generations.hpp"
#include "cxf/types/ids.hpp"
#include "cxf/types/timepoint.hpp"

namespace cxf {
namespace {

[[nodiscard]] bool is_space(char c) noexcept { return c == ' ' || c == '\t'; }

/// Longest value echoed back in an error detail, so that a hostile line cannot
/// turn a rejection into an unbounded output line.
constexpr std::size_t kMaxEchoBytes = 160;

/// "key=value", with an over-long value truncated rather than repeated in full.
[[nodiscard]] std::string field_echo(std::string_view key, std::string_view value) {
  std::string out(key);
  out.push_back('=');
  if (value.size() > kMaxEchoBytes) {
    out.append(value.substr(0, kMaxEchoBytes));
    out.append("...");
  } else {
    out.append(value);
  }
  return out;
}

/// A value-shaped rejection: the detail names the key and echoes the value, and
/// the context carries the key so a programmatic caller can attribute it.
[[nodiscard]] Status value_error(std::string_view key, std::string_view value, Code code,
                                 std::string_view reason) {
  std::string detail = field_echo(key, value);
  detail.append(": ");
  detail.append(reason);
  return make_error(code, std::move(detail), std::string(key));
}

[[nodiscard]] Status check_command_name(std::string_view name) {
  if (name.empty()) {
    return make_error(Code::kFieldMissing, "missing command name");
  }
  if (name.size() > kMaxNameBytes) {
    return make_error(Code::kFieldTooLong, "command name is longer than allowed",
                      "bytes=" + std::to_string(name.size()));
  }
  if (!is_valid_token(name)) {
    return make_error(Code::kMalformedInput,
                      "command name must be a token of [A-Za-z0-9._:-] characters",
                      escape_for_output(name));
  }
  return Status::success();
}

[[nodiscard]] Status check_token_value(std::string_view field, std::string_view value) {
  if (value.empty()) {
    return make_error(Code::kFieldEmpty,
                      std::string(field) + " is required and must not be empty",
                      std::string(field));
  }
  if (value.size() > kMaxNameBytes) {
    return make_error(Code::kFieldTooLong, std::string(field) + " is longer than allowed",
                      std::string(field) + " bytes=" + std::to_string(value.size()));
  }
  if (!is_valid_token(value)) {
    return make_error(Code::kFieldOutOfRange,
                      std::string(field) + " must be a token of [A-Za-z0-9._:-] characters",
                      field_echo(field, value));
  }
  return Status::success();
}

[[nodiscard]] Status check_text_value(std::string_view field, std::string_view value,
                                      bool required) {
  if (value.empty()) {
    if (!required) {
      return Status::success();
    }
    return make_error(Code::kFieldEmpty,
                      std::string(field) + " is required and must not be empty",
                      std::string(field));
  }
  if (value.size() > kMaxDetailBytes) {
    return make_error(Code::kFieldTooLong, std::string(field) + " is longer than allowed",
                      std::string(field) + " bytes=" + std::to_string(value.size()));
  }
  if (!is_valid_utf8(value)) {
    return make_error(Code::kFieldInvalidUtf8,
                      std::string(field) + " is not well-formed UTF-8", std::string(field));
  }
  if (!is_valid_display_text(value)) {
    return make_error(Code::kMalformedInput,
                      std::string(field) + " contains a control character",
                      std::string(field));
  }
  return Status::success();
}

// --- typed field readers ---------------------------------------------------
//
// Every reader leaves its destination untouched when the key is absent: whether
// a key must be present is decided by validate_command_fields, which reports a
// missing required key as kFieldMissing before any reader runs.

[[nodiscard]] Status read_token(const ParsedCommand& command, std::string_view key,
                                std::string& out) {
  const std::optional<std::string_view> value = command.find(key);
  if (!value.has_value()) {
    return Status::success();
  }
  const Status check = check_token_value(key, *value);
  if (check.failed()) {
    return check;
  }
  out.assign(*value);
  return Status::success();
}

[[nodiscard]] Status read_text(const ParsedCommand& command, std::string_view key,
                               std::string& out) {
  const std::optional<std::string_view> value = command.find(key);
  if (!value.has_value()) {
    return Status::success();
  }
  const Status check = check_text_value(key, *value, /*required=*/false);
  if (check.failed()) {
    return check;
  }
  out.assign(*value);
  return Status::success();
}

template <typename IdT>
[[nodiscard]] Status read_id(const ParsedCommand& command, std::string_view key, IdT& out) {
  const std::optional<std::string_view> value = command.find(key);
  if (!value.has_value()) {
    return Status::success();
  }
  const Outcome<IdT> parsed = IdT::parse(*value);
  if (!parsed.ok()) {
    return value_error(key, *value, parsed.code(), parsed.status().detail());
  }
  out = parsed.value();
  return Status::success();
}

template <typename GenT>
[[nodiscard]] Status read_generation(const ParsedCommand& command, std::string_view key,
                                     GenT& out) {
  const std::optional<std::string_view> value = command.find(key);
  if (!value.has_value()) {
    return Status::success();
  }
  const Outcome<GenT> parsed = GenT::parse(*value);
  if (!parsed.ok()) {
    return value_error(key, *value, parsed.code(), parsed.status().detail());
  }
  out = parsed.value();
  return Status::success();
}

/// Read a generation into an optional slot: a key that is present is recorded
/// even when its value is zero, so "not supplied" and "zero" stay distinct.
template <typename GenT>
[[nodiscard]] Status read_optional_generation(const ParsedCommand& command,
                                              std::string_view key,
                                              std::optional<GenT>& out) {
  if (!command.has(key)) {
    return Status::success();
  }
  GenT value{};
  const Status status = read_generation(command, key, value);
  if (status.failed()) {
    return status;
  }
  out = value;
  return Status::success();
}

[[nodiscard]] Status read_digest(const ParsedCommand& command, std::string_view key,
                                 Digest& out) {
  const std::optional<std::string_view> value = command.find(key);
  if (!value.has_value()) {
    return Status::success();
  }
  const Outcome<Digest> parsed = Digest::parse(*value);
  if (!parsed.ok()) {
    return value_error(key, *value, parsed.code(), parsed.status().detail());
  }
  out = parsed.value();
  return Status::success();
}

[[nodiscard]] Status read_timestamp(const ParsedCommand& command, std::string_view key,
                                    Timestamp& out) {
  const std::optional<std::string_view> value = command.find(key);
  if (!value.has_value()) {
    return Status::success();
  }
  const Outcome<Timestamp> parsed = Timestamp::parse_rfc3339(*value);
  if (!parsed.ok()) {
    return value_error(key, *value, parsed.code(), parsed.status().detail());
  }
  out = parsed.value();
  return Status::success();
}

[[nodiscard]] Status read_freshness(const ParsedCommand& command, std::string_view key,
                                    FreshnessWindow& out);

template <typename E>
[[nodiscard]] Status read_enum(const ParsedCommand& command, std::string_view key, E& out,
                               Outcome<E> (*parse)(std::string_view)) {
  const std::optional<std::string_view> value = command.find(key);
  if (!value.has_value()) {
    return Status::success();
  }
  const Outcome<E> parsed = parse(*value);
  if (!parsed.ok()) {
    return value_error(key, *value, parsed.code(), parsed.status().detail());
  }
  out = parsed.value();
  return Status::success();
}

[[nodiscard]] Status read_number(const ParsedCommand& command, std::string_view key,
                                 std::uint64_t& out) {
  const std::optional<std::string_view> value = command.find(key);
  if (!value.has_value()) {
    return Status::success();
  }
  const Outcome<std::uint64_t> parsed = parse_u64(*value);
  if (!parsed.ok()) {
    return value_error(key, *value, parsed.code(), parsed.status().detail());
  }
  out = parsed.value();
  return Status::success();
}

// --- repeated structured fields --------------------------------------------

[[nodiscard]] Outcome<DependencyRef> parse_dependency_ref(std::string_view value) {
  const std::vector<std::string_view> parts = split(value, ':');
  if (parts.size() != 3 && parts.size() != 4) {
    return make_error(Code::kMalformedInput, "dep= must be <kind>:<name>:<gen>[:optional]",
                      field_echo("dep", value));
  }
  DependencyRef ref{};
  const Outcome<DependencyKind> kind = parse_dependency_kind(parts[0]);
  if (!kind.ok()) {
    return value_error("dep", value, kind.code(), kind.status().detail());
  }
  ref.kind = kind.value();
  const Status name_check = check_token_value("dep name", parts[1]);
  if (name_check.failed()) {
    return name_check;
  }
  ref.name.assign(parts[1]);
  const Outcome<DependencyGeneration> generation = DependencyGeneration::parse(parts[2]);
  if (!generation.ok()) {
    return value_error("dep", value, generation.code(), generation.status().detail());
  }
  ref.expected = generation.value();
  if (parts.size() == 4) {
    if (parts[3] != "optional") {
      return make_error(Code::kFieldOutOfRange, "dep= trailing field must be 'optional'",
                        field_echo("dep", value));
    }
    ref.required = false;
  }
  return ref;
}

[[nodiscard]] Outcome<DependencyFact> parse_dependency_fact(std::string_view value) {
  const std::vector<std::string_view> parts = split(value, ':');
  if (parts.size() != 3) {
    return make_error(Code::kMalformedInput, "fact= must be <kind>:<name>:<gen>",
                      field_echo("fact", value));
  }
  DependencyFact fact{};
  const Outcome<DependencyKind> kind = parse_dependency_kind(parts[0]);
  if (!kind.ok()) {
    return value_error("fact", value, kind.code(), kind.status().detail());
  }
  fact.kind = kind.value();
  const Status name_check = check_token_value("fact name", parts[1]);
  if (name_check.failed()) {
    return name_check;
  }
  fact.name.assign(parts[1]);
  const Outcome<DependencyGeneration> generation = DependencyGeneration::parse(parts[2]);
  if (!generation.ok()) {
    return value_error("fact", value, generation.code(), generation.status().detail());
  }
  fact.generation = generation.value();
  return fact;
}

[[nodiscard]] Status read_dependencies(const ParsedCommand& command,
                                       std::vector<DependencyRef>& out) {
  for (std::string_view value : command.all("dep")) {
    const Outcome<DependencyRef> parsed = parse_dependency_ref(value);
    if (!parsed.ok()) {
      return parsed.status();
    }
    out.push_back(parsed.value());
  }
  return Status::success();
}

[[nodiscard]] Status read_facts(const ParsedCommand& command,
                                std::vector<DependencyFact>& out) {
  for (std::string_view value : command.all("fact")) {
    const Outcome<DependencyFact> parsed = parse_dependency_fact(value);
    if (!parsed.ok()) {
      return parsed.status();
    }
    out.push_back(parsed.value());
  }
  return Status::success();
}

/// The command must be the one the parser implements, and its fields must match
/// the grammar of that command.
[[nodiscard]] Status check_shape(const ParsedCommand& command, std::string_view expected) {
  if (command.name() != expected) {
    return make_error(Code::kFieldConflict,
                      "command " + command.name() + " does not match " + std::string(expected),
                      "command=" + command.name());
  }
  return validate_command_fields(command);
}

}  // namespace

// ---------------------------------------------------------------------------
// ParsedCommand.
// ---------------------------------------------------------------------------

std::size_t ParsedCommand::count(std::string_view key) const noexcept {
  std::size_t total = 0;
  for (const CommandField& field : fields_) {
    if (field.key == key) {
      ++total;
    }
  }
  return total;
}

bool ParsedCommand::has(std::string_view key) const noexcept {
  for (const CommandField& field : fields_) {
    if (field.key == key) {
      return true;
    }
  }
  return false;
}

std::optional<std::string_view> ParsedCommand::find(std::string_view key) const noexcept {
  for (const CommandField& field : fields_) {
    if (field.key == key) {
      return std::string_view(field.value);
    }
  }
  return std::nullopt;
}

std::vector<std::string_view> ParsedCommand::all(std::string_view key) const {
  std::vector<std::string_view> values;
  for (const CommandField& field : fields_) {
    if (field.key == key) {
      values.push_back(field.value);
    }
  }
  return values;
}

Outcome<std::string_view> ParsedCommand::require(std::string_view key) const {
  const std::optional<std::string_view> value = find(key);
  if (!value.has_value()) {
    return make_error(Code::kFieldMissing, "missing key " + std::string(key), std::string(key));
  }
  return *value;
}

// ---------------------------------------------------------------------------
// The grammar.
// ---------------------------------------------------------------------------

const std::vector<CommandSpec>& command_specs() {
  static const std::vector<CommandSpec> specs = {
      CommandSpec{"version", {}},
      CommandSpec{"init", {}},
      CommandSpec{"store", {}},
      CommandSpec{"admit",
                  {{"name", "<token>", true, false},
                   {"registry", "<token>", false, false},
                   {"model", "<text>", false, false},
                   {"serial", "<token>", false, false},
                   {"hardware", "<n>", false, false},
                   {"firmware", "<n>", false, false},
                   {"dep", "<kind>:<name>:<gen>[:optional]", false, true},
                   {"request", "<n>", false, false}}},
      CommandSpec{"identify",
                  {{"candidate", "<name>", true, false},
                   {"asset", "<n>", true, false},
                   {"registry", "<token>", true, false},
                   {"evidence", "<n>", true, false},
                   {"model", "<text>", false, false},
                   {"serial", "<token>", false, false},
                   {"hardware", "<n>", false, false},
                   {"firmware", "<n>", false, false},
                   {"request", "<n>", false, false}}},
      CommandSpec{"place",
                  {{"candidate", "<name>", true, false},
                   {"evidence", "<n>", true, false},
                   {"site", "<n>", true, false},
                   {"rack", "<n>", true, false},
                   {"position", "<token>", true, false},
                   {"topology", "<n>", false, false},
                   {"request", "<n>", false, false}}},
      CommandSpec{"evidence",
                  {{"candidate", "<name>", true, false},
                   {"dimension", "<dimension>", true, false},
                   {"subject", "<token>", true, false},
                   {"verdict", "<unknown|satisfied|unsatisfied>", true, false},
                   {"source", "<declared|imported|observed>", true, false},
                   {"source_name", "<token>", false, false},
                   {"detail", "<text>", false, false},
                   {"observed", "<rfc3339|now>", true, false},
                   {"age", "<seconds>|never", false, false},
                   {"site", "<n>", false, false},
                   {"rack", "<n>", false, false},
                   {"position", "<token>", false, false},
                   {"request", "<n>", false, false}}},
      CommandSpec{"evaluate",
                  {{"candidate", "<name>", true, false}, {"request", "<n>", false, false}}},
      CommandSpec{"authorize",
                  {{"candidate", "<name>", true, false},
                   {"plan", "<n>", true, false},
                   {"digest", "<hex>", true, false},
                   {"validity", "<seconds>|never", false, false},
                   {"request", "<n>", false, false}}},
      CommandSpec{"report",
                  {{"candidate", "<name>", true, false},
                   {"token", "<n>", true, false},
                   {"binding", "<hex>", true, false},
                   {"result", "<succeeded|failed|deferred>", true, false},
                   {"detail", "<text>", false, false},
                   {"observed", "<rfc3339>", false, false},
                   {"request", "<n>", false, false}}},
      CommandSpec{"quarantine",
                  {{"candidate", "<name>", true, false},
                   {"reason",
                    "<none|identity_conflict|unsupported_hardware|failed_diagnostics|"
                    "stale_readiness|dependency_ambiguity|contradictory_evidence|policy_denied|"
                    "operator_request>",
                    true, false},
                   {"detail", "<text>", false, false},
                   {"request", "<n>", false, false}}},
      CommandSpec{"release",
                  {{"candidate", "<name>", true, false},
                   {"detail", "<text>", false, false},
                   {"request", "<n>", false, false}}},
      CommandSpec{"cancel",
                  {{"candidate", "<name>", true, false},
                   {"detail", "<text>", false, false},
                   {"request", "<n>", false, false}}},
      CommandSpec{"facility",
                  {{"topology", "<n>", false, false},
                   {"power", "<n>", false, false},
                   {"cooling", "<n>", false, false},
                   {"network", "<n>", false, false},
                   {"policy", "<n>", false, false},
                   {"dependency", "<n>", false, false},
                   {"firmware", "<n>", false, false},
                   {"hardware", "<n>", false, false},
                   {"fact", "<kind>:<name>:<gen>", false, true},
                   {"detail", "<text>", false, false},
                   {"request", "<n>", false, false}}},
      CommandSpec{"status", {{"candidate", "<name>", true, false}}},
      CommandSpec{"explain", {{"candidate", "<name>", true, false}}},
      CommandSpec{"list", {}},
      CommandSpec{"events", {{"limit", "<n>", false, false}}},
      CommandSpec{"checkpoint", {}},
      CommandSpec{"verify", {}},
  };
  return specs;
}

const CommandSpec* find_command_spec(std::string_view name) noexcept {
  const std::vector<CommandSpec>& specs = command_specs();
  for (const CommandSpec& spec : specs) {
    if (spec.name == name) {
      return &spec;
    }
  }
  return nullptr;
}

std::string command_usage_line(std::string_view name) {
  const CommandSpec* spec = find_command_spec(name);
  if (spec == nullptr) {
    return {};
  }
  std::string out(spec->name);
  for (const CommandFieldSpec& field : spec->fields) {
    out.push_back(' ');
    if (!field.required) {
      out.push_back('[');
    }
    out.append(field.key);
    out.push_back('=');
    out.append(field.placeholder);
    if (!field.required) {
      out.push_back(']');
    }
    if (field.repeatable) {
      out.append("...");
    }
  }
  return out;
}

std::string usage_text() {
  std::string out;
  bool first = true;
  for (const CommandSpec& spec : command_specs()) {
    if (!first) {
      out.push_back('\n');
    }
    first = false;
    out.append(command_usage_line(spec.name));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Parsing.
// ---------------------------------------------------------------------------

Outcome<ParsedCommand> parse_command_line(std::string_view line) {
  if (line.size() > kMaxCommandLineBytes) {
    return make_error(Code::kFieldTooLong, "command line is longer than the accepted bound",
                      "bytes=" + std::to_string(line.size()));
  }

  std::size_t index = 0;
  while (index < line.size() && is_space(line[index])) {
    ++index;
  }
  if (index == line.size() || line[index] == '#') {
    return ParsedCommand{};
  }

  const std::size_t name_begin = index;
  while (index < line.size() && !is_space(line[index]) && line[index] != '#') {
    ++index;
  }
  std::string name(line.substr(name_begin, index - name_begin));
  const Status name_status = check_command_name(name);
  if (name_status.failed()) {
    return name_status;
  }

  std::vector<CommandField> fields;
  while (true) {
    while (index < line.size() && is_space(line[index])) {
      ++index;
    }
    if (index == line.size() || line[index] == '#') {
      break;
    }

    const std::size_t key_begin = index;
    while (index < line.size() && line[index] != '=' && !is_space(line[index]) &&
           line[index] != '#') {
      ++index;
    }
    if (index == line.size() || line[index] != '=') {
      return make_error(Code::kMalformedInput, "expected key=value",
                        escape_for_output(line.substr(key_begin)));
    }
    std::string key(line.substr(key_begin, index - key_begin));
    ++index;
    if (!is_valid_token(key)) {
      return make_error(Code::kMalformedInput,
                        "field key must be a token of [A-Za-z0-9._:-] characters",
                        escape_for_output(key));
    }

    std::string value;
    if (index < line.size() && line[index] == '"') {
      ++index;
      bool closed = false;
      while (index < line.size()) {
        const char c = line[index];
        if (c == '\\') {
          if (index + 1 >= line.size()) {
            return make_error(Code::kMalformedInput,
                              "quoted value ends with an incomplete escape",
                              field_echo(key, value));
          }
          const char escaped = line[index + 1];
          if (escaped != '"' && escaped != '\\') {
            return make_error(Code::kMalformedInput,
                              "only a quote and a backslash may be escaped inside a value",
                              field_echo(key, value));
          }
          value.push_back(escaped);
          index += 2;
          continue;
        }
        if (c == '"') {
          ++index;
          closed = true;
          break;
        }
        value.push_back(c);
        ++index;
      }
      if (!closed) {
        return make_error(Code::kMalformedInput, "quoted value is not terminated",
                          field_echo(key, value));
      }
      if (index < line.size() && !is_space(line[index]) && line[index] != '#') {
        return make_error(Code::kMalformedInput, "unexpected text after a quoted value",
                          escape_for_output(line.substr(index)));
      }
    } else {
      const std::size_t value_begin = index;
      while (index < line.size() && !is_space(line[index]) && line[index] != '#') {
        ++index;
      }
      value.assign(line.substr(value_begin, index - value_begin));
    }

    fields.push_back(CommandField{std::move(key), std::move(value)});
    if (fields.size() > kMaxCommandFields) {
      return make_error(Code::kFieldTooLong, "command carries more fields than the accepted bound",
                        "fields=" + std::to_string(fields.size()));
    }
  }

  return ParsedCommand(std::move(name), std::move(fields));
}

Outcome<ParsedCommand> parse_command_arguments(
    std::string_view command, const std::vector<std::string_view>& arguments) {
  const Status name_status = check_command_name(command);
  if (name_status.failed()) {
    return name_status;
  }
  if (arguments.size() > kMaxCommandFields) {
    return make_error(Code::kFieldTooLong, "command carries more fields than the accepted bound",
                      "fields=" + std::to_string(arguments.size()));
  }
  std::vector<CommandField> fields;
  fields.reserve(arguments.size());
  for (std::string_view argument : arguments) {
    const std::size_t separator = argument.find('=');
    if (separator == std::string_view::npos || separator == 0) {
      return make_error(Code::kMalformedInput, "argument is not a key=value pair",
                        escape_for_output(argument));
    }
    const std::string_view key = argument.substr(0, separator);
    if (!is_valid_token(key)) {
      return make_error(Code::kMalformedInput,
                        "field key must be a token of [A-Za-z0-9._:-] characters",
                        escape_for_output(key));
    }
    fields.push_back(
        CommandField{std::string(key), std::string(argument.substr(separator + 1))});
  }
  return ParsedCommand(std::string(command), std::move(fields));
}

Status validate_command_fields(const ParsedCommand& command) {
  if (command.empty()) {
    return make_error(Code::kFieldMissing, "missing command name");
  }
  const CommandSpec* spec = find_command_spec(command.name());
  if (spec == nullptr) {
    return make_error(Code::kFieldOutOfRange, "unknown command", command.name());
  }
  std::vector<std::size_t> counts(spec->fields.size(), 0);
  for (const CommandField& field : command.fields()) {
    std::size_t index = spec->fields.size();
    for (std::size_t candidate = 0; candidate < spec->fields.size(); ++candidate) {
      if (spec->fields[candidate].key == field.key) {
        index = candidate;
        break;
      }
    }
    if (index == spec->fields.size()) {
      return make_error(Code::kFieldConflict,
                        "unknown key " + field.key + " for command " + std::string(spec->name),
                        "key=" + field.key);
    }
    if (!spec->fields[index].repeatable && counts[index] != 0) {
      return make_error(Code::kFieldConflict,
                        "key " + field.key + " is given more than once for command " +
                            std::string(spec->name),
                        "key=" + field.key);
    }
    ++counts[index];
  }
  for (std::size_t index = 0; index < spec->fields.size(); ++index) {
    if (spec->fields[index].required && counts[index] == 0) {
      return make_error(Code::kFieldMissing,
                        "missing required key " + std::string(spec->fields[index].key) +
                            " for command " + std::string(spec->name),
                        "key=" + std::string(spec->fields[index].key));
    }
  }
  return Status::success();
}

Outcome<FreshnessWindow> parse_freshness_window(std::string_view text) {
  return FreshnessWindow::parse(text);
}

namespace {

Status read_freshness(const ParsedCommand& command, std::string_view key,
                      FreshnessWindow& out) {
  const std::optional<std::string_view> value = command.find(key);
  if (!value.has_value()) {
    return Status::success();
  }
  const Outcome<FreshnessWindow> parsed = parse_freshness_window(*value);
  if (!parsed.ok()) {
    return value_error(key, *value, parsed.code(), parsed.status().detail());
  }
  out = parsed.value();
  return Status::success();
}

}  // namespace

// ---------------------------------------------------------------------------
// Command parsers.
// ---------------------------------------------------------------------------

Status parse_version(const ParsedCommand& command) { return check_shape(command, "version"); }

Status parse_init(const ParsedCommand& command) { return check_shape(command, "init"); }

Status parse_store(const ParsedCommand& command) { return check_shape(command, "store"); }

Outcome<AdmitCandidateRequest> parse_admit(const ParsedCommand& command) {
  const Status shape = check_shape(command, "admit");
  if (shape.failed()) {
    return shape;
  }
  AdmitCandidateRequest request{};
  Status status = read_token(command, "name", request.name);
  if (status.failed()) {
    return status;
  }
  status = read_token(command, "registry", request.declaration.registry_name);
  if (status.failed()) {
    return status;
  }
  status = read_text(command, "model", request.declaration.model);
  if (status.failed()) {
    return status;
  }
  status = read_token(command, "serial", request.declaration.serial);
  if (status.failed()) {
    return status;
  }
  status = read_generation(command, "hardware", request.declaration.hardware);
  if (status.failed()) {
    return status;
  }
  status = read_generation(command, "firmware", request.declaration.firmware);
  if (status.failed()) {
    return status;
  }
  status = read_dependencies(command, request.dependencies);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "request", request.request);
  if (status.failed()) {
    return status;
  }
  return request;
}

Outcome<BindIdentityRequest> parse_identify(const ParsedCommand& command) {
  const Status shape = check_shape(command, "identify");
  if (shape.failed()) {
    return shape;
  }
  BindIdentityRequest request{};
  Status status = read_token(command, "candidate", request.candidate);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "asset", request.identity.asset);
  if (status.failed()) {
    return status;
  }
  status = read_token(command, "registry", request.identity.registry_name);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "evidence", request.evidence);
  if (status.failed()) {
    return status;
  }
  status = read_text(command, "model", request.identity.model);
  if (status.failed()) {
    return status;
  }
  status = read_token(command, "serial", request.identity.serial);
  if (status.failed()) {
    return status;
  }
  status = read_generation(command, "hardware", request.identity.hardware);
  if (status.failed()) {
    return status;
  }
  status = read_generation(command, "firmware", request.identity.firmware);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "request", request.request);
  if (status.failed()) {
    return status;
  }
  return request;
}

Outcome<BindPlacementRequest> parse_place(const ParsedCommand& command) {
  const Status shape = check_shape(command, "place");
  if (shape.failed()) {
    return shape;
  }
  BindPlacementRequest request{};
  Status status = read_token(command, "candidate", request.candidate);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "evidence", request.evidence);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "site", request.placement.site);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "rack", request.placement.rack);
  if (status.failed()) {
    return status;
  }
  status = read_token(command, "position", request.placement.position);
  if (status.failed()) {
    return status;
  }
  status = read_generation(command, "topology", request.placement.topology);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "request", request.request);
  if (status.failed()) {
    return status;
  }
  return request;
}

Outcome<SubmitEvidenceRequest> parse_evidence(const ParsedCommand& command,
                                              const FabricOptions& defaults,
                                              Timestamp observed_now) {
  const Status shape = check_shape(command, "evidence");
  if (shape.failed()) {
    return shape;
  }
  SubmitEvidenceRequest request{};
  request.freshness = defaults.default_evidence_freshness;
  // The instant is required, and it is either stated by the operator or
  // explicitly delegated to the caller's clock. An unstated instant is never
  // filled in: a fabricated epoch would make a fresh observation look expired.
  {
    const std::optional<std::string_view> observed = command.find("observed");
    if (!observed.has_value()) {
      return make_error(Code::kFieldMissing,
                        "missing required key observed for command evidence", "observed");
    }
    if (*observed == "now") {
      if (observed_now.unix_nanos() <= 0) {
        return make_error(Code::kPreconditionViolated,
                          "observed=now needs the instant the caller submitted the request at",
                          "observed=now");
      }
      request.observed_at = observed_now;
    } else {
      const Outcome<Timestamp> parsed = Timestamp::parse_rfc3339(*observed);
      if (!parsed.ok()) {
        return value_error("observed", *observed, parsed.code(), parsed.status().detail());
      }
      request.observed_at = parsed.value();
    }
  }
  Status status = read_token(command, "candidate", request.candidate);
  if (status.failed()) {
    return status;
  }
  status = read_enum(command, "dimension", request.dimension, &parse_dimension);
  if (status.failed()) {
    return status;
  }
  status = read_token(command, "subject", request.subject);
  if (status.failed()) {
    return status;
  }
  status = read_enum(command, "verdict", request.verdict, &parse_verdict);
  if (status.failed()) {
    return status;
  }
  status = read_enum(command, "source", request.source, &parse_source);
  if (status.failed()) {
    return status;
  }
  status = read_token(command, "source_name", request.source_name);
  if (status.failed()) {
    return status;
  }
  status = read_text(command, "detail", request.detail);
  if (status.failed()) {
    return status;
  }
  status = read_freshness(command, "age", request.freshness);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "site", request.placement.site);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "rack", request.placement.rack);
  if (status.failed()) {
    return status;
  }
  status = read_token(command, "position", request.placement.position);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "request", request.request);
  if (status.failed()) {
    return status;
  }
  return request;
}

Outcome<EvaluateReadinessRequest> parse_evaluate(const ParsedCommand& command) {
  const Status shape = check_shape(command, "evaluate");
  if (shape.failed()) {
    return shape;
  }
  EvaluateReadinessRequest request{};
  Status status = read_token(command, "candidate", request.candidate);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "request", request.request);
  if (status.failed()) {
    return status;
  }
  return request;
}

Outcome<AuthorizeActivationRequest> parse_authorize(const ParsedCommand& command,
                                                    const FabricOptions& defaults) {
  const Status shape = check_shape(command, "authorize");
  if (shape.failed()) {
    return shape;
  }
  AuthorizeActivationRequest request{};
  request.validity = defaults.default_authority_validity;
  Status status = read_token(command, "candidate", request.candidate);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "plan", request.plan);
  if (status.failed()) {
    return status;
  }
  status = read_digest(command, "digest", request.plan_digest);
  if (status.failed()) {
    return status;
  }
  status = read_freshness(command, "validity", request.validity);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "request", request.request);
  if (status.failed()) {
    return status;
  }
  return request;
}

Outcome<ReportActivationRequest> parse_report(const ParsedCommand& command) {
  const Status shape = check_shape(command, "report");
  if (shape.failed()) {
    return shape;
  }
  ReportActivationRequest request{};
  Status status = read_token(command, "candidate", request.candidate);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "token", request.token);
  if (status.failed()) {
    return status;
  }
  status = read_digest(command, "binding", request.binding);
  if (status.failed()) {
    return status;
  }
  status = read_enum(command, "result", request.result, &parse_activation_result);
  if (status.failed()) {
    return status;
  }
  status = read_text(command, "detail", request.detail);
  if (status.failed()) {
    return status;
  }
  status = read_timestamp(command, "observed", request.observed_at);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "request", request.request);
  if (status.failed()) {
    return status;
  }
  return request;
}

Outcome<QuarantineRequest> parse_quarantine(const ParsedCommand& command) {
  const Status shape = check_shape(command, "quarantine");
  if (shape.failed()) {
    return shape;
  }
  QuarantineRequest request{};
  Status status = read_token(command, "candidate", request.candidate);
  if (status.failed()) {
    return status;
  }
  status = read_enum(command, "reason", request.reason, &parse_quarantine_reason);
  if (status.failed()) {
    return status;
  }
  status = read_text(command, "detail", request.detail);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "request", request.request);
  if (status.failed()) {
    return status;
  }
  return request;
}

Outcome<ReleaseQuarantineRequest> parse_release(const ParsedCommand& command) {
  const Status shape = check_shape(command, "release");
  if (shape.failed()) {
    return shape;
  }
  ReleaseQuarantineRequest request{};
  Status status = read_token(command, "candidate", request.candidate);
  if (status.failed()) {
    return status;
  }
  status = read_text(command, "detail", request.detail);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "request", request.request);
  if (status.failed()) {
    return status;
  }
  return request;
}

Outcome<CancelRequest> parse_cancel(const ParsedCommand& command) {
  const Status shape = check_shape(command, "cancel");
  if (shape.failed()) {
    return shape;
  }
  CancelRequest request{};
  Status status = read_token(command, "candidate", request.candidate);
  if (status.failed()) {
    return status;
  }
  status = read_text(command, "detail", request.detail);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "request", request.request);
  if (status.failed()) {
    return status;
  }
  return request;
}

Outcome<FacilityChangeRequest> parse_facility(const ParsedCommand& command) {
  const Status shape = check_shape(command, "facility");
  if (shape.failed()) {
    return shape;
  }
  FacilityChangeRequest request{};
  Status status = read_optional_generation(command, "topology", request.topology);
  if (status.failed()) {
    return status;
  }
  status = read_optional_generation(command, "power", request.power);
  if (status.failed()) {
    return status;
  }
  status = read_optional_generation(command, "cooling", request.cooling);
  if (status.failed()) {
    return status;
  }
  status = read_optional_generation(command, "network", request.network);
  if (status.failed()) {
    return status;
  }
  status = read_optional_generation(command, "policy", request.policy);
  if (status.failed()) {
    return status;
  }
  status = read_optional_generation(command, "dependency", request.dependency);
  if (status.failed()) {
    return status;
  }
  status = read_optional_generation(command, "firmware", request.firmware);
  if (status.failed()) {
    return status;
  }
  status = read_optional_generation(command, "hardware", request.hardware);
  if (status.failed()) {
    return status;
  }
  status = read_facts(command, request.dependency_facts);
  if (status.failed()) {
    return status;
  }
  status = read_text(command, "detail", request.detail);
  if (status.failed()) {
    return status;
  }
  status = read_id(command, "request", request.request);
  if (status.failed()) {
    return status;
  }
  return request;
}

Outcome<std::string> parse_status(const ParsedCommand& command) {
  const Status shape = check_shape(command, "status");
  if (shape.failed()) {
    return shape;
  }
  std::string candidate;
  const Status status = read_token(command, "candidate", candidate);
  if (status.failed()) {
    return status;
  }
  if (candidate.empty()) {
    return make_error(Code::kFieldMissing, "candidate is required", "candidate");
  }
  return candidate;
}

Outcome<std::string> parse_explain(const ParsedCommand& command) {
  const Status shape = check_shape(command, "explain");
  if (shape.failed()) {
    return shape;
  }
  std::string candidate;
  const Status status = read_token(command, "candidate", candidate);
  if (status.failed()) {
    return status;
  }
  if (candidate.empty()) {
    return make_error(Code::kFieldMissing, "candidate is required", "candidate");
  }
  return candidate;
}

Status parse_list(const ParsedCommand& command) { return check_shape(command, "list"); }

Outcome<std::size_t> parse_events(const ParsedCommand& command) {
  const Status shape = check_shape(command, "events");
  if (shape.failed()) {
    return shape;
  }
  std::uint64_t limit = static_cast<std::uint64_t>(kDefaultEventLimit);
  const Status status = read_number(command, "limit", limit);
  if (status.failed()) {
    return status;
  }
  if (limit > static_cast<std::uint64_t>(kMaxRetainedEvents)) {
    return make_error(Code::kFieldOutOfRange, "limit exceeds the retained event window",
                      "limit=" + std::to_string(limit));
  }
  return static_cast<std::size_t>(limit);
}

Status parse_checkpoint(const ParsedCommand& command) {
  return check_shape(command, "checkpoint");
}

Status parse_verify(const ParsedCommand& command) { return check_shape(command, "verify"); }

}  // namespace cxf
