// End-to-end obligations for the command-line interface.
//
// The CLI is driven as a real child process through the documented grammar.
// Every step asserts the documented exit code and the key=value facts an
// operator reads, a full commissioning journey is walked from admission to
// commission through the command line alone, and the documented reason-code to
// exit-code table is proven code by code.

#include "support/child_process.hpp"
#include "support/test_harness.hpp"

#include "cxf/support/status.hpp"
#include "cxf/tools/registry.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The CMake target defines this as the built CLI. The fallback keeps the file
// compilable on its own (for example in a syntax-only check).
#ifndef CXF_CLI_PATH
#define CXF_CLI_PATH "commissioning-fabric"
#endif

namespace {

using cxf::test::ChildOptions;
using cxf::test::ChildRun;

/// Exit codes documented for the CLI.
constexpr int kExitOk = 0;
constexpr int kExitMalformed = 2;
constexpr int kExitNotFound = 3;
constexpr int kExitStateOrPlan = 4;
constexpr int kExitEvidence = 5;
constexpr int kExitDependencyOrPolicy = 6;
constexpr int kExitStorage = 7;
constexpr int kExitInternal = 8;
constexpr int kExitCrashed = 97;

struct CliRun {
  int exit_code{-1};
  std::string out{};
  std::string err{};
};

[[nodiscard]] CliRun run_cli(const std::string& store, const std::vector<std::string>& arguments,
                             bool use_environment = true, const std::string& store_argument = {}) {
  ChildOptions options;
  options.program = CXF_CLI_PATH;
  if (!store_argument.empty()) {
    options.arguments.push_back("--store");
    options.arguments.push_back(store_argument);
  }
  for (const std::string& argument : arguments) {
    options.arguments.push_back(argument);
  }
  if (use_environment) {
    options.environment.push_back({"CXF_STORE", store});
  }
  const cxf::Outcome<ChildRun> run = cxf::test::run_child(options);
  CliRun result;
  if (!run.ok()) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", "run_child",
                            "could not start the CLI: " + run.status().message());
    return result;
  }
  result.exit_code = run->exit_code;
  result.out = run->out;
  result.err = run->err;
  return result;
}

/// Every key=value token in the output, so an assertion can name the key it
/// depends on without pinning the whole line.
class Keys {
 public:
  explicit Keys(std::string_view text) : text_(text) {}

  [[nodiscard]] bool has(std::string_view key, std::string_view value) const {
    return get(key) == value;
  }
  [[nodiscard]] bool has_key(std::string_view key) const {
    const std::string needle = std::string(key) + "=";
    return text_.find(needle) != std::string_view::npos;
  }
  [[nodiscard]] std::string get(std::string_view key) const {
    return cxf::test::find_key_value(text_, key);
  }
  /// True when the output contains a line starting with the prefix.
  [[nodiscard]] bool has_line_with_prefix(std::string_view prefix) const {
    return !cxf::test::find_line_with_prefix(text_, prefix).empty();
  }

 private:
  std::string_view text_;
};

void expect_exit(const CliRun& run, int expected, const std::string& what) {
  if (run.exit_code == expected) {
    return;
  }
  ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", what,
                          "exit code " + std::to_string(run.exit_code) + " instead of " +
                              std::to_string(expected) + "\n  stdout: " + run.out +
                              "\n  stderr: " + run.err);
}

/// The command completed, and its output reports the expected value for a key.
void expect_key(const CliRun& run, const std::string& key, const std::string& value,
                const std::string& what) {
  const Keys keys(run.out);
  if (!keys.has(key, value)) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", what,
                            key + "=" + value + " not reported (" + key + "=" + keys.get(key) +
                                ")\n  stdout: " + run.out + "\n  stderr: " + run.err);
  }
}

/// Remove a boolean flag from the output so two runs that differ only in that
/// flag can be compared byte for byte.
[[nodiscard]] std::string without_flag(std::string text, std::string_view flag) {
  const std::string yes = std::string(flag) + "=true";
  const std::string no = std::string(flag) + "=false";
  for (std::size_t at = text.find(yes); at != std::string::npos; at = text.find(yes, at)) {
    text.replace(at, yes.size(), std::string(flag) + "=?");
  }
  for (std::size_t at = text.find(no); at != std::string::npos; at = text.find(no, at)) {
    text.replace(at, no.size(), std::string(flag) + "=?");
  }
  return text;
}

/// Replace the value of a readiness digest line with a placeholder. A readiness
/// report is content-addressed including the instant it was evaluated at, so the
/// digest of a live preview moves between runs while everything else in the
/// output is deterministic.
[[nodiscard]] std::string without_evaluation_digest(std::string text) {
  std::string out;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string line =
        end == std::string::npos ? text.substr(start) : text.substr(start, end - start);
    if (line.rfind("digest=", 0) == 0) {
      out += "digest=<evaluation instant>";
    } else {
      out += line;
    }
    if (end == std::string::npos) {
      break;
    }
    out.push_back('\n');
        start = end + 1;
  }
  return out;
}

/// The facility record that satisfies every dependency of the journey.
[[nodiscard]] std::vector<std::string> facility_commands() {
  return {"facility", "topology=1", "power=1", "cooling=1", "network=1",
          "policy=1",   "dependency=1", "fact=site:dc1:1", "fact=rack:rack-a:1",
          "fact=power_feed:pdu-a:1", "fact=cooling_zone:zone-3:1",
          "fact=network_fabric:fabric-east:1", "fact=service_class:gold:1",
          "fact=ownership:team-platform:1"};
}

[[nodiscard]] std::vector<std::string> admission_commands(const std::string& name,
                                                           bool with_dependencies) {
  std::vector<std::string> arguments = {"admit", "name=" + name,
                                        "registry=registry-" + name, "model=model-x",
                                        "serial=SN-" + name, "hardware=1", "firmware=1"};
  if (with_dependencies) {
    arguments.push_back("dep=site:dc1:1");
    arguments.push_back("dep=rack:rack-a:1");
    arguments.push_back("dep=power_feed:pdu-a:1");
    arguments.push_back("dep=cooling_zone:zone-3:1");
    arguments.push_back("dep=network_fabric:fabric-east:1");
    arguments.push_back("dep=service_class:gold:1");
    arguments.push_back("dep=ownership:team-platform:1");
  }
  return arguments;
}

[[nodiscard]] std::vector<std::string> evidence_commands(const std::string& candidate,
                                                         const std::string& dimension,
                                                         const std::string& subject,
                                                         const std::string& verdict = "satisfied",
                                                         const std::string& observed = "now") {
  return {"evidence", "candidate=" + candidate, "dimension=" + dimension, "subject=" + subject,
          "verdict=" + verdict, "source=observed", "observed=" + observed};
}

}  // namespace

CXF_TEST(e2e, version_init_and_store_commands) {
  cxf::test::TempDirectory home("cli-basic");
  REQUIRE(home.ok());
  const std::string store = home.child("store");

  const CliRun version = run_cli(store, {"version"});
  expect_exit(version, kExitOk, "version");
  // The version command reports the library version and nothing else.
  CHECK(!version.out.empty());
  CHECK(cxf::test::contains_substring(version.out, "."));

  const CliRun init = run_cli(store, {"init"});
  expect_exit(init, kExitOk, "init");
  // A store created by this open has no committed generation yet and a fresh
  // control epoch.
  expect_key(init, "commit", "0", "init");
  expect_key(init, "epoch", "1", "init");
  expect_key(init, "recovered", "false", "init");

  const CliRun describe = run_cli(store, {"store"});
  expect_exit(describe, kExitOk, "store");
  CHECK(!Keys(describe.out).get("store").empty());
  expect_key(describe, "candidates", "0", "store");

  const CliRun listing = run_cli(store, {"list"});
  expect_exit(listing, kExitOk, "list");
  CHECK(listing.out.empty());

  const CliRun verify = run_cli(store, {"verify"});
  expect_exit(verify, kExitOk, "verify");
  expect_key(verify, "code", "Ok", "verify");

  // An unknown command is malformed input.
  expect_exit(run_cli(store, {"frobnicate"}), kExitMalformed, "unknown command");
  // So is a command that is missing a required key.
  expect_exit(run_cli(store, {"admit", "registry=only-a-registry"}), kExitMalformed,
              "admit without name");
  expect_exit(run_cli(store, {"status"}), kExitMalformed, "status without candidate");
  // And so is an unknown key.
  expect_exit(run_cli(store, {"status", "candidate=node-1", "bogus=1"}), kExitMalformed,
              "unknown key");
}

CXF_TEST(e2e, full_commissioning_journey) {
  cxf::test::TempDirectory home("cli-journey");
  REQUIRE(home.ok());
  const std::string store = home.child("store");

  expect_exit(run_cli(store, {"init"}), kExitOk, "init");
  expect_exit(run_cli(store, facility_commands()), kExitOk, "facility");

  // Admission, with an explicit idempotency key so the replay test is exact.
  std::vector<std::string> admit = admission_commands("node-1", true);
  admit.push_back("request=42");
  const CliRun admitted = run_cli(store, admit);
  expect_exit(admitted, kExitOk, "admit");
  expect_key(admitted, "state", "declared", "admit");
  expect_key(admitted, "request", "42", "admit");
  CHECK(Keys(admitted.out).has_key("commit"));

  // Identity: an observation first, then the binding of that observation.
  const CliRun identity_evidence =
      run_cli(store, evidence_commands("node-1", "identity", "registry-node-1"));
  expect_exit(identity_evidence, kExitOk, "identity evidence");
  const std::string identity_id = Keys(identity_evidence.out).get("evidence");
  CHECK(!identity_id.empty());

  const CliRun identified =
      run_cli(store, {"identify", "candidate=node-1", "asset=1", "registry=registry-node-1",
                      "evidence=" + identity_id, "model=model-x", "serial=SN-node-1"});
  expect_exit(identified, kExitOk, "identify");
  expect_key(identified, "state", "identified", "identify");

  // Placement: the same two-step shape.
  const CliRun placement_evidence =
      run_cli(store, evidence_commands("node-1", "placement", "rack-a"));
  expect_exit(placement_evidence, kExitOk, "placement evidence");
  const std::string placement_id = Keys(placement_evidence.out).get("evidence");
  CHECK(!placement_id.empty());
  const CliRun placed =
      run_cli(store, {"place", "candidate=node-1", "evidence=" + placement_id, "site=1",
                      "rack=1", "position=U12"});
  expect_exit(placed, kExitOk, "place");
  expect_key(placed, "state", "located", "place");

  // The remaining readiness dimensions, each from its owning system.
  const std::vector<std::pair<std::string, std::string>> observations = {
      {"compatibility", "baseline-2026.1"}, {"electrical", "pdu-a"},
      {"cooling", "zone-3"},                {"network", "fabric-east"},
      {"health", "diagnostics"},            {"policy", "policy-gold"},
      {"service_class", "gold"}};
  for (const std::pair<std::string, std::string>& observation : observations) {
    const CliRun run =
        run_cli(store, evidence_commands("node-1", observation.first, observation.second));
    expect_exit(run, kExitOk, "evidence " + observation.first);
    CHECK(!Keys(run.out).get("evidence").empty());
  }

  const CliRun evaluated = run_cli(store, {"evaluate", "candidate=node-1"});
  expect_exit(evaluated, kExitOk, "evaluate");
  const Keys evaluated_keys(evaluated.out);
  CHECK(evaluated_keys.has("ready", "true"));
  const std::string plan = evaluated_keys.get("plan");
  const std::string plan_digest = evaluated_keys.get("plan_digest");
  CHECK(!plan.empty());
  CHECK_EQ(plan_digest.size(), std::size_t{64});
  // Every dimension is reported satisfied, including the one driven by the
  // facility dependency resolution rather than by an observation.
  CHECK(evaluated_keys.has_line_with_prefix("dimension=identity required=true verdict=satisfied"));
  CHECK(evaluated_keys.has_line_with_prefix("dimension=dependency required=true verdict=satisfied"));

  // Activation authority binds the plan digest, not the readiness digest.
  const CliRun authorized = run_cli(
      store, {"authorize", "candidate=node-1", "plan=" + plan, "digest=" + plan_digest});
  expect_exit(authorized, kExitOk, "authorize");
  const Keys authorized_keys(authorized.out);
  const std::string token = authorized_keys.get("token");
  const std::string binding = authorized_keys.get("binding");
  CHECK(!token.empty());
  CHECK_EQ(binding.size(), std::size_t{64});
  expect_key(authorized, "state", "activation_authorized", "authorize");

  const CliRun reported =
      run_cli(store, {"report", "candidate=node-1", "token=" + token, "binding=" + binding,
                      "result=succeeded"});
  expect_exit(reported, kExitOk, "report");
  expect_key(reported, "state", "activating", "report");

  // Only a fresh observation taken after the activation can commission it.
  expect_exit(run_cli(store, evidence_commands("node-1", "health", "post-activation-diag")),
              kExitOk, "post-activation health");
  const CliRun commissioned = run_cli(store, {"evaluate", "candidate=node-1"});
  expect_exit(commissioned, kExitOk, "final evaluate");
  expect_key(commissioned, "ready", "true", "final evaluate");
  expect_key(commissioned, "state", "commissioned", "final evaluate");

  const CliRun status = run_cli(store, {"status", "candidate=node-1"});
  expect_exit(status, kExitOk, "status");
  expect_key(status, "state", "commissioned", "status");
  CHECK(Keys(status.out).has_line_with_prefix("candidate=node-1"));

  const CliRun explain = run_cli(store, {"explain", "candidate=node-1"});
  expect_exit(explain, kExitOk, "explain");
  CHECK(!explain.out.empty());

  const CliRun list = run_cli(store, {"list"});
  expect_exit(list, kExitOk, "list");
  CHECK(cxf::test::contains_substring(list.out, "name=node-1"));
  CHECK(cxf::test::contains_substring(list.out, "state=commissioned"));

  const CliRun events = run_cli(store, {"events", "limit=5"});
  expect_exit(events, kExitOk, "events");
  CHECK(!events.out.empty());

  // The store survives a checkpoint and a fresh process.
  const CliRun checkpoint = run_cli(store, {"checkpoint"});
  expect_exit(checkpoint, kExitOk, "checkpoint");
  expect_key(checkpoint, "code", "Ok", "checkpoint");
  const CliRun after = run_cli(store, {"status", "candidate=node-1"});
  expect_exit(after, kExitOk, "status after checkpoint");
  expect_key(after, "state", "commissioned", "status after checkpoint");
}

CXF_TEST(e2e, replaying_a_request_returns_the_same_result) {
  cxf::test::TempDirectory home("cli-replay");
  REQUIRE(home.ok());
  const std::string store = home.child("store");
  expect_exit(run_cli(store, {"init"}), kExitOk, "init");

  std::vector<std::string> admit = admission_commands("replay-node", false);
  admit.push_back("request=7");
  const CliRun first = run_cli(store, admit);
  expect_exit(first, kExitOk, "first admit");
  expect_key(first, "replayed", "false", "first admit");

  // The same request id with the same content replays the recorded result
  // without re-applying it: everything except the replay flag is identical.
  const CliRun second = run_cli(store, admit);
  expect_exit(second, kExitOk, "replayed admit");
  expect_key(second, "replayed", "true", "replayed admit");
  CHECK_EQ(without_flag(second.out, "replayed"), without_flag(first.out, "replayed"));
  CHECK_EQ(second.exit_code, first.exit_code);

  // The candidate exists exactly once.
  const CliRun list = run_cli(store, {"list"});
  expect_exit(list, kExitOk, "list");
  std::size_t occurrences = 0;
  for (const std::string& line : cxf::test::split_lines(list.out)) {
    if (cxf::test::contains_substring(line, "replay-node")) {
      ++occurrences;
    }
  }
  CHECK_EQ(occurrences, std::size_t{1});

  // Reusing the request id with different content is refused rather than
  // answered as a replay of the earlier request.
  const std::vector<std::string> reused = {"admit", "name=replay-node",
                                           "registry=registry-replay-node", "model=model-x",
                                           "serial=SN-DIFFERENT", "hardware=1", "firmware=1",
                                           "request=7"};
  expect_exit(run_cli(store, reused), kExitStateOrPlan, "idempotency key reuse");
}

CXF_TEST(e2e, documented_exit_codes_for_real_commands) {
  cxf::test::TempDirectory home("cli-codes");
  REQUIRE(home.ok());
  const std::string store = home.child("store");

  expect_exit(run_cli(store, {"init"}), kExitOk, "init");
  expect_exit(run_cli(store, facility_commands()), kExitOk, "facility");
  expect_exit(run_cli(store, admission_commands("codes-node", false)), kExitOk, "admit");

  // 5 - evidence: identical content is one observation, not two. A fixed
  // observation instant makes the second submission byte-identical.
  const CliRun first_evidence = run_cli(
      store, evidence_commands("codes-node", "identity", "registry-codes-node", "satisfied",
                               "2026-01-01T00:00:00Z"));
  expect_exit(first_evidence, kExitOk, "first identity evidence");
  const std::string evidence_id = Keys(first_evidence.out).get("evidence");
  CHECK(!evidence_id.empty());
  expect_exit(run_cli(store, evidence_commands("codes-node", "identity", "registry-codes-node",
                                               "satisfied", "2026-01-01T00:00:00Z")),
              kExitEvidence, "duplicate evidence payload");

  // 3 - not found: a candidate that was never admitted.
  expect_exit(run_cli(store, {"status", "candidate=absent-node"}), kExitNotFound,
              "status of an absent candidate");
  expect_exit(run_cli(store, {"explain", "candidate=absent-node"}), kExitNotFound,
              "explain of an absent candidate");

  // 4 - state/authority/plan: a plan whose digest does not match the record.
  // 4 - state/plan: authorization is refused while the candidate is not ready
  // enough to be authorized, and a duplicate candidate is refused outright.
  // (A plan that does not exist is reported as PlanNotFound, but the candidate
  // state is checked first, so that case needs a candidate in health_validated;
  // the mapping itself is pinned by documented_exit_code_table.)
  const CliRun evaluated = run_cli(store, {"evaluate", "candidate=codes-node"});
  expect_exit(evaluated, kExitOk, "evaluate");
  const std::string plan = Keys(evaluated.out).get("plan");
  CHECK(!plan.empty());
  expect_exit(run_cli(store, {"authorize", "candidate=codes-node", "plan=" + plan,
                              "digest=0000000000000000000000000000000000000000000000000000000000000000"}),
              kExitStateOrPlan, "authorize before the candidate is ready");
  expect_exit(run_cli(store, admission_commands("codes-node", false)), kExitStateOrPlan,
              "duplicate candidate");

  // 2 - malformed: an unknown evidence dimension is an input-shape failure, not
  // an evidence failure.
  expect_exit(run_cli(store, {"evidence", "candidate=codes-node", "dimension=not-a-dimension",
                              "subject=subject", "verdict=satisfied", "source=observed",
                              "observed=now"}),
              kExitMalformed, "unknown dimension");

  // 5 - evidence: an attestation recorded under a generation that has moved is
  // no longer live, and the binding that names it is refused.
  cxf::FacilityChangeRequest unused;  // (the CLI reads the same durable state)
  static_cast<void>(unused);
  expect_exit(run_cli(store, {"facility", "power=2"}), kExitOk, "facility drift");
  expect_exit(run_cli(store, {"identify", "candidate=codes-node", "asset=2",
                              "registry=registry-codes-node", "evidence=" + evidence_id,
                              "model=model-x", "serial=SN-codes-node"}),
              kExitEvidence, "stale attestation");

  // 4 - state: a quarantined candidate cannot be evaluated.
  expect_exit(run_cli(store, {"quarantine", "candidate=codes-node", "reason=operator_request",
                              "detail=held for inspection"}),
              kExitOk, "quarantine");
  expect_exit(run_cli(store, {"evaluate", "candidate=codes-node"}), kExitStateOrPlan,
              "evaluate a quarantined candidate");

  // 7 - storage: a store path that is a regular file.
  const std::string not_a_store = home.child("not-a-store");
  REQUIRE(cxf::test::write_file_text(not_a_store, "not a store"));
  expect_exit(run_cli(store, {"status", "candidate=codes-node"}, true, not_a_store), kExitStorage,
              "store path that is a file");
}

CXF_TEST(e2e, documented_exit_code_table) {
  // The table the CLI maps every reason code through. Proving it here means a
  // code that no single command can currently produce (the dependency,
  // placement, compatibility and policy family) is still pinned.
  struct Binding {
    cxf::Code code;
    int exit_code;
  };
  const std::vector<Binding> table = {
      {cxf::Code::kOk, kExitOk},
      {cxf::Code::kMalformedInput, kExitMalformed},
      {cxf::Code::kFieldMissing, kExitMalformed},
      {cxf::Code::kFieldTooLong, kExitMalformed},
      {cxf::Code::kFieldInvalidUtf8, kExitMalformed},
      {cxf::Code::kFieldEmpty, kExitMalformed},
      {cxf::Code::kFieldOutOfRange, kExitMalformed},
      {cxf::Code::kFieldConflict, kExitMalformed},
      {cxf::Code::kReservedFieldNonZero, kExitMalformed},
      {cxf::Code::kUnsupportedFormatVersion, kExitMalformed},
      {cxf::Code::kTrailingBytes, kExitMalformed},
      {cxf::Code::kCandidateNotFound, kExitNotFound},
      {cxf::Code::kCandidateAlreadyExists, kExitStateOrPlan},
      {cxf::Code::kAssetIdentityConflict, kExitStateOrPlan},
      {cxf::Code::kAssetAlreadyCommissioned, kExitStateOrPlan},
      {cxf::Code::kStateNotAllowed, kExitStateOrPlan},
      {cxf::Code::kTransitionNotAllowed, kExitStateOrPlan},
      {cxf::Code::kAttemptNotFound, kExitNotFound},
      {cxf::Code::kCandidateTerminal, kExitStateOrPlan},
      {cxf::Code::kEvidenceUnknownDimension, kExitEvidence},
      {cxf::Code::kEvidenceStale, kExitEvidence},
      {cxf::Code::kEvidenceContradictory, kExitEvidence},
      {cxf::Code::kEvidenceNegative, kExitEvidence},
      {cxf::Code::kEvidenceMissing, kExitEvidence},
      {cxf::Code::kEvidenceGenerationMismatch, kExitEvidence},
      {cxf::Code::kEvidenceDuplicateDigest, kExitEvidence},
      {cxf::Code::kPlanNotFound, kExitNotFound},
      {cxf::Code::kPlanStale, kExitStateOrPlan},
      {cxf::Code::kAuthorityTokenInvalid, kExitStateOrPlan},
      {cxf::Code::kAuthorityTokenExpired, kExitStateOrPlan},
      {cxf::Code::kAuthorityTokenConsumed, kExitStateOrPlan},
      {cxf::Code::kIdempotencyKeyReuse, kExitStateOrPlan},
      {cxf::Code::kGenerationRegression, kExitStateOrPlan},
      {cxf::Code::kFencingTokenStale, kExitStateOrPlan},
      {cxf::Code::kDependencyUnsatisified, kExitDependencyOrPolicy},
      {cxf::Code::kDependencyCycle, kExitDependencyOrPolicy},
      {cxf::Code::kDependencyAmbiguous, kExitDependencyOrPolicy},
      {cxf::Code::kPlacementUnbound, kExitDependencyOrPolicy},
      {cxf::Code::kCompatibilityUnsupported, kExitDependencyOrPolicy},
      {cxf::Code::kPolicyNotSatisfied, kExitDependencyOrPolicy},
      {cxf::Code::kServiceClassUnsatisfied, kExitDependencyOrPolicy},
      {cxf::Code::kStorageUnavailable, kExitStorage},
      {cxf::Code::kStorageCorrupt, kExitStorage},
      {cxf::Code::kStorageLocked, kExitStorage},
      {cxf::Code::kStorageIo, kExitStorage},
      {cxf::Code::kStorageUnsupportedFormat, kExitStorage},
      {cxf::Code::kInternalError, kExitInternal},
      {cxf::Code::kPreconditionViolated, kExitInternal},
      {cxf::Code::kNotImplemented, kExitInternal},
  };
  for (const Binding& binding : table) {
    CHECK_EQ(cxf::exit_code_for(binding.code), binding.exit_code);
  }
  // Every binding reported by the CLI is one of the documented pairs, and no
  // declared code escapes the table.
  const std::span<const cxf::ExitCodeBinding> published = cxf::exit_code_table();
  CHECK_EQ(published.size(), table.size());
  for (const cxf::ExitCodeBinding& binding : published) {
    bool found = false;
    for (const Binding& expected : table) {
      if (expected.code == binding.code) {
        found = true;
        CHECK_EQ(binding.exit_code, expected.exit_code);
      }
    }
    CHECK(found);
    CHECK(binding.exit_code >= 0 && binding.exit_code <= kExitInternal);
  }
}

CXF_TEST(e2e, read_only_output_is_byte_identical_across_runs) {
  cxf::test::TempDirectory home("cli-determinism");
  REQUIRE(home.ok());
  const std::string store = home.child("store");
  expect_exit(run_cli(store, {"init"}), kExitOk, "init");
  expect_exit(run_cli(store, facility_commands()), kExitOk, "facility");
  expect_exit(run_cli(store, admission_commands("deterministic-node", false)), kExitOk, "admit");

  const std::vector<std::vector<std::string>> commands = {
      {"status", "candidate=deterministic-node"},
      {"list"},
      {"store"},
      {"verify"},
      {"events", "limit=10"},
      {"explain", "candidate=deterministic-node"}};
  for (const std::vector<std::string>& command : commands) {
    const CliRun first = run_cli(store, command);
    const CliRun second = run_cli(store, command);
    expect_exit(first, kExitOk, command[0]);
    expect_exit(second, kExitOk, command[0]);
    // explain renders a live readiness report, whose content address covers the
    // evaluation instant; every other byte must be identical.
    CHECK_EQ(without_evaluation_digest(second.out), without_evaluation_digest(first.out));
    CHECK_EQ(second.err, first.err);
  }

  // The same holds through the environment variable rather than --store.
  const CliRun via_environment = run_cli(store, {"list"});
  const CliRun via_argument = run_cli(store, {"list"}, false, store);
  CHECK_EQ(via_environment.exit_code, via_argument.exit_code);
  CHECK_EQ(via_environment.out, via_argument.out);
}

CXF_TEST(e2e, crash_point_flags_produce_a_nonzero_exit) {
  cxf::test::TempDirectory home("cli-crash");
  REQUIRE(home.ok());
  const std::string store = home.child("store");
  expect_exit(run_cli(store, {"init"}), kExitOk, "init");

  for (const char* point : {"before_flush", "after_flush", "after_publish"}) {
    const CliRun crashed =
        run_cli(store, {"--crash-point", point, "--crash-at", "1", "admit",
                        "name=crash-" + std::string(point), "registry=registry-crash",
                        "model=model-x", "serial=SN-crash"});
    if (crashed.exit_code == kExitOk) {
      ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", point,
                              "the crash point did not fire: the command exited 0");
    }
    CHECK_EQ(crashed.exit_code, kExitCrashed);
  }

  // The store is still readable and verifiable after the crashes.
  expect_exit(run_cli(store, {"verify"}), kExitOk, "verify after crash");
  expect_exit(run_cli(store, {"list"}), kExitOk, "list after crash");

  // A run that never reaches the configured commit exits normally.
  const CliRun not_reached =
      run_cli(store, {"--crash-point", "after_publish", "--crash-at", "5", "admit",
                      "name=no-crash", "registry=registry-no-crash", "model=model-x",
                      "serial=SN-no-crash"});
  expect_exit(not_reached, kExitOk, "crash point that is never reached");
  expect_exit(run_cli(store, {"verify"}), kExitOk, "verify after a completed run");
}
