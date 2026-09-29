// Commissioning Fabric - administration CLI.
//
//   commissioning-fabric [--store <dir>] [--quiet]
//                        [--crash-point <none|before_flush|after_flush|after_publish>]
//                        [--crash-at <commit>] [--snapshot-threshold <bytes>]
//                        <command> [key=value ...]
//   commissioning-fabric batch <file|->
//   commissioning-fabric help [command]
//   commissioning-fabric --version
//
// The store comes from --store, else the CXF_STORE environment variable, else
// ./cxf-store. It is opened through Fabric::open with the real system clock; a
// failure prints "error code=<CodeName> detail=<escaped detail>" to stderr and
// returns the exit code of the reason-code table.
//
// Output is deterministic key=value text: command results are the renderers of
// cxf/runtime/explain.hpp plus the command-specific lines below, so an operator
// can read the plan, the digest, the token and the binding out of one command
// and paste them into the next. Nothing here reads the wall clock except
// through cxf::Clock, and no output carries a timestamp of its own.
//
// The crash-point options arm the durability fault injection points of the
// runtime. They exist to drive the documented crash-consistency tests, they are
// listed in help, and they are never silently ignored.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cxf/codec/text_codec.hpp"
#include "cxf/runtime/explain.hpp"
#include "cxf/runtime/fabric.hpp"
#include "cxf/support/clock.hpp"
#include "cxf/support/fs.hpp"
#include "cxf/support/status.hpp"
#include "cxf/support/text.hpp"
#include "cxf/tools/registry.hpp"
#include "cxf/tools/textlog.hpp"
#include "cxf/version.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using cxf::Code;
using cxf::CommandHandler;
using cxf::CommandRegistry;
using cxf::CrashPoint;
using cxf::ExitCodeBinding;
using cxf::Fabric;
using cxf::LogLevel;
using cxf::Outcome;
using cxf::ParsedCommand;
using cxf::Status;
using cxf::TextLog;

/// Store directory used when neither --store nor CXF_STORE says otherwise.
constexpr std::string_view kDefaultStore = "./cxf-store";
/// Environment variable holding the store directory.
constexpr char kStoreVariable[] = "CXF_STORE";
/// Component name carried by every log record of this program.
constexpr std::string_view kComponent = "cli";
/// Holder text written into the store lock file. Constant on purpose: a lock
/// file must not smuggle a process id or a host name into the output.
constexpr std::string_view kHolder = "commissioning-fabric";

struct OptionSpec {
  std::string_view name;
  bool takes_value;
  std::string_view usage;
};

/// Every global option, in the order help lists them.
constexpr OptionSpec kOptions[] = {
    {"--store", true,
     "--store <dir>  store directory (default: $CXF_STORE, else ./cxf-store)"},
    {"--quiet", false, "--quiet  discard command output; errors still go to stderr"},
    {"--crash-point", true,
     "--crash-point <none|before_flush|after_flush|after_publish>  arm a durability "
     "fault injection point"},
    {"--crash-at", true,
     "--crash-at <commit>  commit index (1-based) at which the armed crash point fires"},
    {"--snapshot-threshold", true,
     "--snapshot-threshold <bytes>  publish a snapshot once the journal exceeds this size"},
    {"--help", false, "--help  print usage and the exit-code table"},
    {"--version", false, "--version  print the library version string"},
};

[[nodiscard]] const OptionSpec* find_option(std::string_view name) noexcept {
  for (const OptionSpec& option : kOptions) {
    if (option.name == name) {
      return &option;
    }
  }
  return nullptr;
}

/// What the command line asked for, before anything is opened.
struct Invocation {
  std::string store{};
  bool quiet{false};
  CrashPoint crash_point{CrashPoint::kNone};
  std::uint64_t crash_at{0};
  std::uint64_t snapshot_threshold{4u * 1024u * 1024u};
  bool help{false};
  bool version{false};
  std::string command{};
  std::vector<std::string_view> arguments{};
};

/// Everything a handler may use. The fabric is null until the store is open,
/// which is why help and version run before it is set.
struct Cli {
  Fabric* fabric{nullptr};
  /// The real clock the store was opened with. It is the only source of "now":
  /// observed=now is stamped from it, and nothing else reads the time.
  const cxf::Clock* clock{nullptr};
  std::ostream* out{nullptr};
  TextLog* log{nullptr};
  const CommandRegistry* registry{nullptr};
};

[[nodiscard]] std::string bool_text(bool value) { return value ? "true" : "false"; }

void write_line(std::ostream& out, std::string_view text) { out << text << '\n'; }

/// Write a rendered block, adding the one newline that terminates its last line.
void write_block(std::ostream& out, const std::string& text) {
  if (text.empty()) {
    return;
  }
  out << text;
  if (text.back() != '\n') {
    out << '\n';
  }
}

void report_error(std::ostream& err, const Status& status) {
  std::string detail = status.detail();
  if (detail.empty()) {
    detail = status.context();
  }
  err << "error code=" << cxf::code_name(status.code())
      << " detail=" << cxf::escape_for_output(detail) << '\n';
}

[[nodiscard]] Status apply_option(const OptionSpec& spec, std::string_view value,
                                  Invocation& invocation) {
  if (spec.name == "--store") {
    if (value.empty()) {
      return make_error(Code::kFieldEmpty, "--store requires a non-empty directory");
    }
    invocation.store.assign(value);
    return Status::success();
  }
  if (spec.name == "--quiet") {
    invocation.quiet = true;
    return Status::success();
  }
  if (spec.name == "--crash-point") {
    const Outcome<CrashPoint> point = cxf::parse_crash_point(value);
    if (!point.ok()) {
      return make_error(point.code(), std::string(point.status().detail()), std::string(value));
    }
    invocation.crash_point = point.value();
    return Status::success();
  }
  if (spec.name == "--crash-at") {
    const Outcome<std::uint64_t> parsed = cxf::parse_u64(value);
    if (!parsed.ok()) {
      return make_error(parsed.code(), std::string(parsed.status().detail()),
                        std::string(value));
    }
    invocation.crash_at = parsed.value();
    return Status::success();
  }
  if (spec.name == "--snapshot-threshold") {
    const Outcome<std::uint64_t> parsed = cxf::parse_u64(value);
    if (!parsed.ok()) {
      return make_error(parsed.code(), std::string(parsed.status().detail()),
                        std::string(value));
    }
    invocation.snapshot_threshold = parsed.value();
    return Status::success();
  }
  if (spec.name == "--help") {
    invocation.help = true;
    return Status::success();
  }
  if (spec.name == "--version") {
    invocation.version = true;
    return Status::success();
  }
  return make_error(Code::kInternalError, "option is not handled by the command line",
                    std::string(spec.name));
}

/// Parse the options that precede the command, then the command and its fields.
/// The first argument that is not an option ends the option list, so a field
/// can never be mistaken for an option and vice versa.
[[nodiscard]] Status parse_invocation(const std::vector<std::string_view>& args,
                                      Invocation& invocation) {
  std::size_t index = 0;
  for (; index < args.size(); ++index) {
    const std::string_view argument = args[index];
    if (argument.size() < 2 || argument[0] != '-') {
      break;
    }
    std::string_view name = argument;
    std::string_view inline_value{};
    bool has_inline_value = false;
    const std::size_t separator = argument.find('=');
    if (separator != std::string_view::npos) {
      name = argument.substr(0, separator);
      inline_value = argument.substr(separator + 1);
      has_inline_value = true;
    }
    const OptionSpec* spec = find_option(name);
    if (spec == nullptr) {
      return make_error(Code::kFieldConflict, "unknown option " + std::string(name),
                        std::string(name));
    }
    std::string_view value = inline_value;
    if (spec->takes_value && !has_inline_value) {
      if (index + 1 >= args.size()) {
        return make_error(Code::kFieldMissing,
                          "option " + std::string(name) + " requires a value",
                          std::string(name));
      }
      ++index;
      value = args[index];
    }
    if (!spec->takes_value && has_inline_value) {
      return make_error(Code::kFieldConflict,
                        "option " + std::string(name) + " does not take a value",
                        std::string(name));
    }
    const Status applied = apply_option(*spec, value, invocation);
    if (applied.failed()) {
      return applied;
    }
  }
  if (invocation.help || invocation.version) {
    return Status::success();
  }
  if (index == args.size()) {
    return make_error(Code::kFieldMissing, "missing command");
  }
  invocation.command.assign(args[index]);
  ++index;
  invocation.arguments.assign(args.begin() + static_cast<std::ptrdiff_t>(index), args.end());
  return Status::success();
}

// ---------------------------------------------------------------------------
// Output helpers shared by the command handlers.
// ---------------------------------------------------------------------------

void print_outcome(std::ostream& out, const cxf::RequestOutcome& outcome) {
  write_line(out, "ok code=Ok");
  write_block(out, cxf::render_outcome(outcome));
  write_line(out, "state=" + std::string(cxf::state_name(outcome.state)));
  write_line(out, "commit=" + outcome.commit.str());
}

/// The resulting state of a candidate, rendered by the runtime's own status
/// renderer. A status query that fails after an accepted mutation is reported
/// as unavailable rather than being papered over.
void print_candidate_state(const Cli& cli, const std::string& candidate) {
  const Outcome<cxf::StatusView> status = cli.fabric->status(candidate);
  if (!status.ok()) {
    const std::string code(cxf::code_name(status.code()));
    write_line(*cli.out, "status=unavailable code=" + code);
    const std::string detail = cxf::escape_for_output(status.status().detail());
    cli.log->warn(kComponent, "status",
                  "the resulting state could not be read for candidate=" + candidate +
                      ": code=" + code + " detail=" + detail);
    return;
  }
  write_block(*cli.out, cxf::render_status(status.value()));
}

[[nodiscard]] Status finish_mutation(const Cli& cli, const cxf::RequestOutcome& outcome,
                                     std::string_view candidate) {
  print_outcome(*cli.out, outcome);
  if (!candidate.empty()) {
    print_candidate_state(cli, std::string(candidate));
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// Command handlers.
// ---------------------------------------------------------------------------

Status run_version(const Cli& cli, const ParsedCommand& command) {
  const Status shape = cxf::parse_version(command);
  if (shape.failed()) {
    return shape;
  }
  write_line(*cli.out, cxf::kVersionString);
  return Status::success();
}

Status run_init(const Cli& cli, const ParsedCommand& command) {
  const Status shape = cxf::parse_init(command);
  if (shape.failed()) {
    return shape;
  }
  write_line(*cli.out, "ok code=Ok");
  write_line(*cli.out, "commit=" + cli.fabric->meta().commit.str());
  write_line(*cli.out, "epoch=" + cli.fabric->meta().epoch.str());
  write_line(*cli.out, "recovered=" + bool_text(cli.fabric->stats().recovered));
  return Status::success();
}

Status run_store(const Cli& cli, const ParsedCommand& command) {
  const Status shape = cxf::parse_store(command);
  if (shape.failed()) {
    return shape;
  }
  // The store directory is the CLI's knowledge, not the runtime's: it is
  // reported as an absolute path so two runs can be compared without knowing
  // the working directory each was started in. A path that cannot be resolved
  // is reported exactly as configured rather than as an empty value.
  const std::string& configured = cli.fabric->options().directory;
  const Outcome<std::string> resolved = cxf::fs::absolute(configured);
  write_line(*cli.out, "store=" + (resolved.ok() ? resolved.value() : configured));
  write_block(*cli.out, cxf::render_store(cli.fabric->meta(), cli.fabric->stats()));
  return Status::success();
}

Status run_admit(const Cli& cli, const ParsedCommand& command) {
  const Outcome<cxf::AdmitCandidateRequest> request = cxf::parse_admit(command);
  if (!request.ok()) {
    return request.status();
  }
  const Outcome<cxf::RequestOutcome> result = cli.fabric->admit_candidate(request.value());
  if (!result.ok()) {
    return result.status();
  }
  return finish_mutation(cli, result.value(), request.value().name);
}

Status run_identify(const Cli& cli, const ParsedCommand& command) {
  const Outcome<cxf::BindIdentityRequest> request = cxf::parse_identify(command);
  if (!request.ok()) {
    return request.status();
  }
  const Outcome<cxf::RequestOutcome> result = cli.fabric->bind_identity(request.value());
  if (!result.ok()) {
    return result.status();
  }
  return finish_mutation(cli, result.value(), request.value().candidate);
}

Status run_place(const Cli& cli, const ParsedCommand& command) {
  const Outcome<cxf::BindPlacementRequest> request = cxf::parse_place(command);
  if (!request.ok()) {
    return request.status();
  }
  const Outcome<cxf::RequestOutcome> result = cli.fabric->bind_placement(request.value());
  if (!result.ok()) {
    return result.status();
  }
  return finish_mutation(cli, result.value(), request.value().candidate);
}

Status run_evidence(const Cli& cli, const ParsedCommand& command) {
  const Outcome<cxf::SubmitEvidenceRequest> request =
      cxf::parse_evidence(command, cli.fabric->options(), cli.clock->now());
  if (!request.ok()) {
    return request.status();
  }
  const Outcome<cxf::RequestOutcome> result = cli.fabric->submit_evidence(request.value());
  if (!result.ok()) {
    return result.status();
  }
  print_outcome(*cli.out, result.value());
  print_candidate_state(cli, request.value().candidate);
  return Status::success();
}

Status run_evaluate(const Cli& cli, const ParsedCommand& command) {
  const Outcome<cxf::EvaluateReadinessRequest> request = cxf::parse_evaluate(command);
  if (!request.ok()) {
    return request.status();
  }
  const Outcome<cxf::ReadinessResult> result =
      cli.fabric->evaluate_readiness(request.value());
  if (!result.ok()) {
    return result.status();
  }
  const cxf::ReadinessResult& value = result.value();
  print_outcome(*cli.out, value.outcome);
  write_line(*cli.out, "plan=" + value.plan.str());
  // The plan digest is a property of the retained plan, so it is read back from
  // the candidate rather than assumed: a plan that was not retained has no
  // digest to report.
  const Outcome<cxf::CandidateView> view = cli.fabric->view(request.value().candidate);
  if (view.ok() && view.value().plan.has_value()) {
    write_line(*cli.out,
               "plan_digest=" + cxf::compute_plan_digest(*view.value().plan).hex());
  } else {
    write_line(*cli.out, "plan_digest=none");
  }
  write_line(*cli.out, "readiness_digest=" + value.report.digest.hex());
  write_block(*cli.out, cxf::render_report(value.report));
  print_candidate_state(cli, request.value().candidate);
  return Status::success();
}

Status run_authorize(const Cli& cli, const ParsedCommand& command) {
  const Outcome<cxf::AuthorizeActivationRequest> request =
      cxf::parse_authorize(command, cli.fabric->options());
  if (!request.ok()) {
    return request.status();
  }
  const Outcome<cxf::ActivationGrant> result =
      cli.fabric->authorize_activation(request.value());
  if (!result.ok()) {
    return result.status();
  }
  const cxf::ActivationGrant& grant = result.value();
  print_outcome(*cli.out, grant.outcome);
  write_line(*cli.out, "plan=" + grant.plan.str());
  write_line(*cli.out, "plan_digest=" + request.value().plan_digest.hex());
  write_line(*cli.out, "token=" + grant.token.str());
  write_line(*cli.out, "binding=" + grant.binding.hex());
  write_line(*cli.out, "issued_at=" + grant.issued_at.to_rfc3339());
  write_line(*cli.out, "validity=" + grant.validity.str());
  print_candidate_state(cli, request.value().candidate);
  return Status::success();
}

Status run_report(const Cli& cli, const ParsedCommand& command) {
  const Outcome<cxf::ReportActivationRequest> request = cxf::parse_report(command);
  if (!request.ok()) {
    return request.status();
  }
  const Outcome<cxf::RequestOutcome> result = cli.fabric->report_activation(request.value());
  if (!result.ok()) {
    return result.status();
  }
  return finish_mutation(cli, result.value(), request.value().candidate);
}

Status run_quarantine(const Cli& cli, const ParsedCommand& command) {
  const Outcome<cxf::QuarantineRequest> request = cxf::parse_quarantine(command);
  if (!request.ok()) {
    return request.status();
  }
  const Outcome<cxf::RequestOutcome> result = cli.fabric->quarantine(request.value());
  if (!result.ok()) {
    return result.status();
  }
  return finish_mutation(cli, result.value(), request.value().candidate);
}

Status run_release(const Cli& cli, const ParsedCommand& command) {
  const Outcome<cxf::ReleaseQuarantineRequest> request = cxf::parse_release(command);
  if (!request.ok()) {
    return request.status();
  }
  const Outcome<cxf::RequestOutcome> result = cli.fabric->release_quarantine(request.value());
  if (!result.ok()) {
    return result.status();
  }
  return finish_mutation(cli, result.value(), request.value().candidate);
}

Status run_cancel(const Cli& cli, const ParsedCommand& command) {
  const Outcome<cxf::CancelRequest> request = cxf::parse_cancel(command);
  if (!request.ok()) {
    return request.status();
  }
  const Outcome<cxf::RequestOutcome> result = cli.fabric->cancel(request.value());
  if (!result.ok()) {
    return result.status();
  }
  return finish_mutation(cli, result.value(), request.value().candidate);
}

Status run_facility(const Cli& cli, const ParsedCommand& command) {
  const Outcome<cxf::FacilityChangeRequest> request = cxf::parse_facility(command);
  if (!request.ok()) {
    return request.status();
  }
  const Outcome<cxf::RequestOutcome> result =
      cli.fabric->record_facility_change(request.value());
  if (!result.ok()) {
    return result.status();
  }
  print_outcome(*cli.out, result.value());
  const cxf::FacilityGenerations& generations = cli.fabric->facility().generations;
  write_line(*cli.out, "topology=" + generations.topology.str());
  write_line(*cli.out, "power=" + generations.power.str());
  write_line(*cli.out, "cooling=" + generations.cooling.str());
  write_line(*cli.out, "network=" + generations.network.str());
  write_line(*cli.out, "policy=" + generations.policy.str());
  write_line(*cli.out, "dependency=" + generations.dependency.str());
  write_line(*cli.out, "firmware=" + generations.firmware.str());
  write_line(*cli.out, "hardware=" + generations.hardware.str());
  return Status::success();
}

Status run_status(const Cli& cli, const ParsedCommand& command) {
  const Outcome<std::string> candidate = cxf::parse_status(command);
  if (!candidate.ok()) {
    return candidate.status();
  }
  const Outcome<cxf::StatusView> view = cli.fabric->status(candidate.value());
  if (!view.ok()) {
    return view.status();
  }
  write_block(*cli.out, cxf::render_status(view.value()));
  return Status::success();
}

Status run_explain(const Cli& cli, const ParsedCommand& command) {
  const Outcome<std::string> candidate = cxf::parse_explain(command);
  if (!candidate.ok()) {
    return candidate.status();
  }
  const Outcome<cxf::ReadinessReport> report =
      cli.fabric->preview_readiness(candidate.value());
  if (!report.ok()) {
    return report.status();
  }
  write_block(*cli.out, cxf::render_report(report.value()));
  return Status::success();
}

Status run_list(const Cli& cli, const ParsedCommand& command) {
  const Status shape = cxf::parse_list(command);
  if (shape.failed()) {
    return shape;
  }
  const Outcome<std::vector<cxf::CandidateSummary>> candidates =
      cli.fabric->list_candidates();
  if (!candidates.ok()) {
    return candidates.status();
  }
  for (const cxf::CandidateSummary& summary : candidates.value()) {
    write_line(*cli.out, cxf::render_candidate_summary(summary));
  }
  return Status::success();
}

Status run_events(const Cli& cli, const ParsedCommand& command) {
  const Outcome<std::size_t> limit = cxf::parse_events(command);
  if (!limit.ok()) {
    return limit.status();
  }
  const Outcome<std::vector<cxf::EventRecord>> events =
      cli.fabric->recent_events(limit.value());
  if (!events.ok()) {
    return events.status();
  }
  for (const cxf::EventRecord& event : events.value()) {
    write_line(*cli.out, cxf::render_event(event));
  }
  return Status::success();
}

Status run_checkpoint(const Cli& cli, const ParsedCommand& command) {
  const Status shape = cxf::parse_checkpoint(command);
  if (shape.failed()) {
    return shape;
  }
  const Status status = cli.fabric->checkpoint();
  if (status.failed()) {
    return status;
  }
  write_line(*cli.out, "ok code=Ok");
  write_line(*cli.out, "commit=" + cli.fabric->meta().commit.str());
  return Status::success();
}

Status run_verify(const Cli& cli, const ParsedCommand& command) {
  const Status shape = cxf::parse_verify(command);
  if (shape.failed()) {
    return shape;
  }
  const Status status = cli.fabric->verify_store();
  if (status.failed()) {
    return status;
  }
  write_line(*cli.out, "ok code=Ok");
  return Status::success();
}

[[nodiscard]] CommandHandler make_handler(std::string name, Cli& cli) {
  return [name = std::move(name), &cli](const ParsedCommand& command) -> Status {
    if (name == "version") {
      return run_version(cli, command);
    }
    if (name == "init") {
      return run_init(cli, command);
    }
    if (name == "store") {
      return run_store(cli, command);
    }
    if (name == "admit") {
      return run_admit(cli, command);
    }
    if (name == "identify") {
      return run_identify(cli, command);
    }
    if (name == "place") {
      return run_place(cli, command);
    }
    if (name == "evidence") {
      return run_evidence(cli, command);
    }
    if (name == "evaluate") {
      return run_evaluate(cli, command);
    }
    if (name == "authorize") {
      return run_authorize(cli, command);
    }
    if (name == "report") {
      return run_report(cli, command);
    }
    if (name == "quarantine") {
      return run_quarantine(cli, command);
    }
    if (name == "release") {
      return run_release(cli, command);
    }
    if (name == "cancel") {
      return run_cancel(cli, command);
    }
    if (name == "facility") {
      return run_facility(cli, command);
    }
    if (name == "status") {
      return run_status(cli, command);
    }
    if (name == "explain") {
      return run_explain(cli, command);
    }
    if (name == "list") {
      return run_list(cli, command);
    }
    if (name == "events") {
      return run_events(cli, command);
    }
    if (name == "checkpoint") {
      return run_checkpoint(cli, command);
    }
    if (name == "verify") {
      return run_verify(cli, command);
    }
    return make_error(Code::kInternalError, "command is not wired to a handler", name);
  };
}

/// Register the options and then the commands, in grammar order. Registration
/// order is the order help prints, so the usage text is deterministic.
[[nodiscard]] Status build_registry(CommandRegistry& registry, Cli& cli) {
  for (const OptionSpec& option : kOptions) {
    const Status status =
        registry.add_option(std::string(option.name), std::string(option.usage));
    if (status.failed()) {
      return status;
    }
  }
  for (const cxf::CommandSpec& spec : cxf::command_specs()) {
    const std::string name(spec.name);
    const Status status =
        registry.add_command(name, cxf::command_usage_line(name), make_handler(name, cli));
    if (status.failed()) {
      return status;
    }
  }
  return Status::success();
}

[[nodiscard]] std::string render_help(const CommandRegistry& registry) {
  std::string out;
  out.append("commissioning-fabric - DCCP governed commissioning of physical "
             "facility assets\n");
  out.append("usage: commissioning-fabric [options] <command> [key=value ...]\n");
  out.append("usage: commissioning-fabric batch <file|->\n");
  out.append("usage: commissioning-fabric help [command]\n");
  out.append("usage: commissioning-fabric --version\n");
  out.push_back('\n');
  out.append("notes:\n");
  out.append("  fields are key=value pairs; a value may be quoted to carry spaces\n");
  out.append("  '#' starts a comment in a batch file; blanks are ignored\n");
  out.append("  batch stops at the first rejected line and names it\n");
  out.append("  evidence requires observed=<rfc3339> or observed=now (never an invented instant)\n");
  out.append("  an absent age= takes the store's default evidence freshness\n");
  out.push_back('\n');
  out.append(registry.usage_text());
  out.append("exit codes:\n");
  for (const ExitCodeBinding& binding : cxf::exit_code_table()) {
    out.append("  exit=");
    out.append(std::to_string(binding.exit_code));
    out.append(" code=");
    out.append(cxf::code_name(binding.code));
    out.push_back('\n');
  }
  return out;
}

/// Prefix a failure with the batch line it came from, so a rejected file names
/// the exact line an operator has to fix.
[[nodiscard]] Status with_line(const Status& status, std::size_t line) {
  return make_error(status.code(), "line " + std::to_string(line) + ": " + status.detail(),
                    status.context());
}

[[nodiscard]] Status run_batch(const Cli& cli, std::istream& input) {
  std::string line;
  std::size_t number = 0;
  while (std::getline(input, line)) {
    ++number;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const Outcome<ParsedCommand> parsed = cxf::parse_command_line(line);
    if (!parsed.ok()) {
      return with_line(parsed.status(), number);
    }
    const ParsedCommand& command = parsed.value();
    if (command.empty()) {
      continue;
    }
    if (command.name() == "help") {
      write_block(*cli.out, render_help(*cli.registry));
      continue;
    }
    const Status status = cli.registry->dispatch(command);
    if (status.failed()) {
      return with_line(status, number);
    }
  }
  if (input.bad()) {
    return make_error(Code::kStorageIo, "reading the batch source failed");
  }
  return Status::success();
}

[[nodiscard]] Status check_batch_source(const std::vector<std::string_view>& arguments) {
  if (arguments.size() != 1) {
    return make_error(Code::kFieldOutOfRange, "batch takes exactly one source: <file> or -",
                      "arguments=" + std::to_string(arguments.size()));
  }
  return Status::success();
}

[[nodiscard]] Status run_batch_source(const Cli& cli,
                                      const std::vector<std::string_view>& arguments) {
  const Status check = check_batch_source(arguments);
  if (check.failed()) {
    return check;
  }
  const std::string_view source = arguments[0];
  if (source == "-") {
    return run_batch(cli, std::cin);
  }
  std::ifstream file(std::string(source), std::ios::binary);
  if (!file.is_open()) {
    return make_error(Code::kStorageIo, "cannot open the batch source",
                      cxf::escape_for_output(source));
  }
  return run_batch(cli, file);
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  // The contract is exact bytes out, so the standard streams are binary and a
  // line ends with a single '\n' on every platform.
  (void)_setmode(_fileno(stdout), _O_BINARY);
  (void)_setmode(_fileno(stderr), _O_BINARY);
#endif

  std::vector<std::string_view> args;
  if (argc > 1) {
    args.reserve(static_cast<std::size_t>(argc - 1));
    for (int index = 1; index < argc; ++index) {
      args.emplace_back(argv[index]);
    }
  }

  Invocation invocation;
  {
    const Status parsed = parse_invocation(args, invocation);
    if (parsed.failed()) {
      report_error(std::cerr, parsed);
      return cxf::exit_code_for(parsed.code());
    }
  }

  if (invocation.store.empty()) {
    const char* const from_environment = std::getenv(kStoreVariable);
    if (from_environment != nullptr && from_environment[0] != '\0') {
      invocation.store.assign(from_environment);
    } else {
      invocation.store.assign(kDefaultStore);
    }
  }

  std::ostream& out = invocation.quiet ? cxf::null_stream() : std::cout;
  TextLog log(std::cerr, invocation.quiet ? LogLevel::kError : LogLevel::kInfo);

  Cli cli;
  cli.out = &out;
  cli.log = &log;

  CommandRegistry registry;
  {
    const Status status = build_registry(registry, cli);
    if (status.failed()) {
      report_error(std::cerr, status);
      return cxf::exit_code_for(status.code());
    }
    cli.registry = &registry;
  }

  if (invocation.help) {
    std::cout << render_help(registry);
    return 0;
  }
  if (invocation.version) {
    std::cout << cxf::kVersionString << '\n';
    return 0;
  }
  if (invocation.command == "help") {
    if (invocation.arguments.empty()) {
      std::cout << render_help(registry);
      return 0;
    }
    for (const std::string_view name : invocation.arguments) {
      if (name == "batch") {
        // batch is a shape of its own rather than a grammar command, so it is
        // described here exactly as the usage header describes it.
        std::cout << "batch <file|->\n";
        continue;
      }
      const std::string_view usage = registry.usage_of(name);
      if (usage.empty()) {
        const Status status = make_error(Code::kFieldOutOfRange,
                                         "unknown command " + std::string(name),
                                         std::string(name));
        report_error(std::cerr, status);
        return cxf::exit_code_for(status.code());
      }
      std::cout << usage << '\n';
    }
    return 0;
  }

  // The request is validated before anything durable is opened: an unknown
  // command or a malformed field must not bring a store into existence, and
  // `version` answers from the build itself without opening anything.
  const bool is_batch = invocation.command == "batch";
  const bool needs_store = invocation.command != "version";

  Status preflight = Status::success();
  ParsedCommand command{};
  if (is_batch) {
    preflight = check_batch_source(invocation.arguments);
  } else if (registry.has_command(invocation.command)) {
    const Outcome<ParsedCommand> parsed =
        cxf::parse_command_arguments(invocation.command, invocation.arguments);
    if (parsed.ok()) {
      command = parsed.value();
    } else {
      preflight = parsed.status();
    }
  } else {
    preflight = make_error(Code::kFieldOutOfRange,
                           "unknown command " + invocation.command, invocation.command);
  }
  if (preflight.failed()) {
    report_error(std::cerr, preflight);
    return cxf::exit_code_for(preflight.code());
  }
  if (!needs_store) {
    const Status status = registry.dispatch(command);
    if (status.failed()) {
      report_error(std::cerr, status);
      return cxf::exit_code_for(status.code());
    }
    return 0;
  }

  cxf::FabricOptions fabric_options;
  fabric_options.directory = invocation.store;
  fabric_options.create_if_missing = true;
  fabric_options.holder.assign(kHolder);
  fabric_options.crash_point = invocation.crash_point;
  fabric_options.crash_at_commit = invocation.crash_at;
  fabric_options.snapshot_threshold_bytes = invocation.snapshot_threshold;

  cxf::SystemClock clock;
  Outcome<Fabric> opened = Fabric::open(fabric_options, clock);
  if (!opened.ok()) {
    report_error(std::cerr, opened.status());
    return cxf::exit_code_for(opened.code());
  }
  Fabric& fabric = opened.value();
  cli.fabric = &fabric;
  cli.clock = &clock;

  if (invocation.crash_point != CrashPoint::kNone) {
    log.warn(kComponent, invocation.command,
             "fault injection armed crash_point=" +
                 std::string(cxf::crash_point_name(invocation.crash_point)) +
                 " crash_at=" + cxf::to_dec(invocation.crash_at));
  }

  const Status status =
      is_batch ? run_batch_source(cli, invocation.arguments) : registry.dispatch(command);
  if (status.failed()) {
    report_error(std::cerr, status);
    return cxf::exit_code_for(status.code());
  }
  return 0;
}
