// Crash consistency with real process death.
//
// The test executable re-invokes ITSELF in a child mode that admits candidates
// through the library and dies without unwinding at a configured commit point.
// The parent then reopens the store and proves that exactly one authoritative
// generation survives: the deterministic replay of the commits that were
// published.
//
// The child reports, on stdout, every commit it published. That makes the
// expectation independent of how the runtime numbers commits: the recovered
// generation must be the last commit the child published, and no more.

#include "support/child_process.hpp"
#include "support/test_harness.hpp"

#include "cxf/runtime/fabric.hpp"
#include "cxf/support/clock.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cxf::Code;
using cxf::CommitSequence;
using cxf::CrashPoint;
using cxf::Fabric;
using cxf::FabricOptions;
using cxf::Timestamp;

constexpr std::int64_t kBase = 1700000000;
constexpr int kChildCrashExit = 97;

[[nodiscard]] std::string zero_padded(std::uint64_t value) {
  std::string text = std::to_string(value);
  while (text.size() < 3) {
    text.insert(text.begin(), '0');
  }
  return text;
}

[[nodiscard]] std::string child_candidate_name(const std::string& prefix, std::uint64_t index) {
  return prefix + "-" + zero_padded(index);
}

[[nodiscard]] cxf::CandidateDeclaration declaration_of(const std::string& name) {
  cxf::CandidateDeclaration declaration;
  declaration.registry_name = "registry-" + name;
  declaration.model = "model-crash";
  declaration.serial = "SN-" + name;
  return declaration;
}

// ---------------------------------------------------------------------------
// Child mode: mutate the store and die at a configured commit point.
// ---------------------------------------------------------------------------
int crash_child_main(int argc, char** argv) {
  if (argc < 2 || std::string_view(argv[1]) != "--child-crash") {
    return -1;
  }
  std::string store;
  std::string point = "none";
  std::string prefix = "crash";
  std::uint64_t crash_at = 0;
  std::uint64_t commands = 0;
  std::uint64_t threshold = 0;
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--store" && i + 1 < argc) {
      store = argv[++i];
    } else if (arg == "--crash-point" && i + 1 < argc) {
      point = argv[++i];
    } else if (arg == "--crash-at" && i + 1 < argc) {
      crash_at = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--commands" && i + 1 < argc) {
      commands = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--snapshot-threshold" && i + 1 < argc) {
      threshold = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--prefix" && i + 1 < argc) {
      prefix = argv[++i];
    } else {
      std::fprintf(stderr, "child-crash: unexpected argument %s\n", argv[i]);
      return 2;
    }
  }
  if (store.empty()) {
    std::fprintf(stderr, "child-crash: --store is required\n");
    return 2;
  }
  const cxf::Outcome<CrashPoint> parsed = cxf::parse_crash_point(point);
  if (!parsed.ok()) {
    std::fprintf(stderr, "child-crash: unknown crash point %s\n", point.c_str());
    return 2;
  }

  cxf::SystemClock clock;
  FabricOptions options;
  options.directory = store;
  options.holder = "crash-child";
  options.crash_point = parsed.value();
  options.crash_at_commit = crash_at;
  if (threshold > 0) {
    options.snapshot_threshold_bytes = threshold;
  }
  cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
  if (!fabric.ok()) {
    std::fprintf(stderr, "child-crash: open failed: %s\n", fabric.status().message().c_str());
    return 3;
  }
  std::printf("child base=%llu\n", static_cast<unsigned long long>(fabric->meta().commit.value()));
  std::fflush(stdout);

  for (std::uint64_t i = 0; i < commands; ++i) {
    cxf::AdmitCandidateRequest request;
    request.name = child_candidate_name(prefix, i);
    request.declaration = declaration_of(request.name);
    const cxf::Outcome<cxf::RequestOutcome> outcome = fabric->admit_candidate(request);
    if (!outcome.ok()) {
      std::fprintf(stderr, "child-crash: admit failed: %s\n", outcome.status().message().c_str());
      return 4;
    }
    // Printed and flushed BEFORE the next commit, so the parent can see exactly
    // which commits were published before the process died.
    std::printf("published index=%llu commit=%llu name=%s\n",
                static_cast<unsigned long long>(i + 1),
                static_cast<unsigned long long>(outcome->commit.value()), request.name.c_str());
    std::fflush(stdout);
  }
  std::printf("child completed\n");
  std::fflush(stdout);
  return 0;
}

struct ChildRegistrar {
  ChildRegistrar() { cxf::test::add_child_mode_handler(&crash_child_main); }
};

const ChildRegistrar kChildRegistrar;

struct ChildReport {
  int exit_code{0};
  bool completed{false};
  std::uint64_t base{0};
  std::vector<std::uint64_t> commits{};
  std::vector<std::string> names{};
};

[[nodiscard]] ChildReport run_child_crash(const std::string& store, const char* point,
                                          std::uint64_t crash_at, std::uint64_t commands,
                                          std::uint64_t threshold, const std::string& prefix) {
  cxf::test::ChildOptions options;
  options.program = cxf::test::executable_path();
  options.arguments = {"--child-crash", "--store", store, "--crash-point", point, "--crash-at",
                       std::to_string(crash_at), "--commands", std::to_string(commands),
                       "--prefix", prefix};
  if (threshold > 0) {
    options.arguments.push_back("--snapshot-threshold");
    options.arguments.push_back(std::to_string(threshold));
  }
  const cxf::Outcome<cxf::test::ChildRun> run = cxf::test::run_child(options);
  REQUIRE_OK(run);

  ChildReport report;
  report.exit_code = run->exit_code;
  for (const std::string& line : cxf::test::split_lines(run->out)) {
    if (line == "child completed") {
      report.completed = true;
      continue;
    }
    if (line.rfind("child base=", 0) == 0) {
      report.base = std::strtoull(line.substr(11).c_str(), nullptr, 10);
      continue;
    }
    if (line.rfind("published ", 0) == 0) {
      report.commits.push_back(
          std::strtoull(cxf::test::find_key_value(line, "commit").c_str(), nullptr, 10));
      report.names.push_back(cxf::test::find_key_value(line, "name"));
    }
  }
  if (report.commits.empty() && !report.completed) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", "child output",
                            "the child published nothing and did not complete: " + run->out +
                                " stderr: " + run->err);
  }
  return report;
}

struct Recovered {
  std::uint64_t commit{0};
  std::uint64_t candidates{0};
  std::uint64_t snapshot_bytes{0};
  bool verified{false};
  std::vector<std::string> names{};
};

/// The names the child admits, in order: prefix-000, prefix-001, ...
[[nodiscard]] std::vector<std::string> expected_names(const std::string& prefix,
                                                      std::uint64_t count) {
  std::vector<std::string> names;
  names.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    names.push_back(child_candidate_name(prefix, i));
  }
  return names;
}

[[nodiscard]] bool reopen_and_verify(const std::string& store, Recovered& recovered) {
  cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
  FabricOptions options;
  options.directory = store;
  options.holder = "crash-parent";
  const cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
  if (!fabric.ok()) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", store,
                            "reopening the store failed: " + fabric.status().message());
    return false;
  }
  recovered.commit = fabric->meta().commit.value();
  recovered.candidates = fabric->stats().candidates;
  recovered.snapshot_bytes = fabric->stats().snapshot_bytes;
  recovered.verified = fabric->verify_store().ok();
  // Every recovered candidate must be one the child actually published.
  const cxf::Outcome<std::vector<cxf::CandidateSummary>> list = fabric->list_candidates();
  if (!list.ok()) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", "list_candidates",
                            list.status().message());
    return false;
  }
  recovered.candidates = list->size();
  recovered.names.clear();
  for (const cxf::CandidateSummary& summary : *list) {
    recovered.names.push_back(summary.name);
  }
  std::sort(recovered.names.begin(), recovered.names.end());
  return true;
}

/// Assert that the recovered candidates are exactly the first N names the child
/// would have admitted: the deterministic replay of N commits, in order, with
/// nothing invented and nothing missing.
void expect_prefix(const Recovered& recovered, const std::string& prefix, std::uint64_t count,
                   const std::string& what) {
  const std::vector<std::string> want = expected_names(prefix, count);
  if (recovered.names != want) {
    std::string got;
    for (const std::string& name : recovered.names) {
      got += name + " ";
    }
    std::string expected;
    for (const std::string& name : want) {
      expected += name + " ";
    }
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", what,
                            "recovered [" + got + "] instead of [" + expected + "]");
  }
}

}  // namespace

CXF_TEST(crash, real_process_death_recovers_one_generation) {
  constexpr std::uint64_t kCommands = 4;
  cxf::test::TempDirectory home("crash");
  REQUIRE(home.ok());
  const std::string pristine = home.child("pristine");

  // A store that already has a committed generation before the crash run.
  std::uint64_t baseline = 0;
  {
    cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
    FabricOptions options;
    options.directory = pristine;
    options.holder = "pristine";
    cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
    REQUIRE_OK(fabric);
    cxf::FacilityChangeRequest facility;
    facility.topology = cxf::TopologyGeneration::from_value(1);
    facility.detail = "pristine generation";
    REQUIRE_OK(fabric->record_facility_change(facility));
    baseline = fabric->meta().commit.value();
  }

  // The commit protocol is stage -> flush -> read back and verify -> publish,
  // and every crash point fires inside that protocol, before the call returns
  // to the child. So the child reports one commit fewer than it attempted for
  // each real crash point, while what is DURABLE differs: a commit killed
  // before the flush is lost, one killed after the flush is already on the
  // device, and one killed after publication is durable as well.
  struct Case {
    const char* point;
    bool crashes;
    std::uint64_t reported;
    std::uint64_t durable;
  };
  const std::vector<Case> cases = {{"none", false, kCommands, kCommands},
                                   {"before_flush", true, kCommands - 1, kCommands - 1},
                                   {"after_flush", true, kCommands - 1, kCommands},
                                   {"after_publish", true, kCommands - 1, kCommands}};

  for (const Case& item : cases) {
    const std::string work = home.child(std::string("store-") + item.point);
    REQUIRE(cxf::test::copy_tree(pristine, work));
    const ChildReport report =
        run_child_crash(work, item.point, kCommands, kCommands, 0, "crash");

    // Every commit in this child is one command, so the configured index fires
    // on the last command exactly when the point is real.
    if (item.crashes) {
      CHECK_EQ(report.exit_code, kChildCrashExit);
      CHECK(!report.completed);
    } else {
      CHECK_EQ(report.exit_code, 0);
      CHECK(report.completed);
    }
    CHECK_EQ(report.base, baseline);
    CHECK_EQ(report.commits.size(), static_cast<std::size_t>(item.reported));
    // Commits are strictly increasing and contiguous from the baseline.
    for (std::size_t i = 0; i < report.commits.size(); ++i) {
      CHECK_EQ(report.commits[i], baseline + i + 1);
    }

    // Exactly one authoritative generation: the deterministic replay of every
    // commit that reached the device, whether or not the child published it.
    Recovered recovered;
    REQUIRE(reopen_and_verify(work, recovered));
    CHECK(recovered.verified);
    CHECK_EQ(recovered.commit, baseline + item.durable);
    CHECK_EQ(recovered.candidates, item.durable);
    expect_prefix(recovered, "crash", item.durable, std::string("crash at ") + item.point);
    // Nothing published is ever lost, and a torn tail is never half-applied.
    CHECK(recovered.commit >= (report.commits.empty() ? baseline : report.commits.back()));
    // Reopening again reproduces the same generation rather than a new one.
    Recovered again;
    REQUIRE(reopen_and_verify(work, again));
    CHECK_EQ(again.commit, recovered.commit);
    CHECK_EQ(again.candidates, recovered.candidates);
  }
}

CXF_TEST(crash, crash_between_snapshot_publication_and_journal_reset) {
  constexpr std::uint64_t kCommands = 5;
  cxf::test::TempDirectory home("crash-snapshot");
  REQUIRE(home.ok());
  const std::string pristine = home.child("pristine");
  {
    cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
    FabricOptions options;
    options.directory = pristine;
    options.holder = "pristine";
    cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
    REQUIRE_OK(fabric);
  }

  const std::string work = home.child("store-after-publish");
  REQUIRE(cxf::test::copy_tree(pristine, work));
  // A tiny threshold makes the child checkpoint many times during the run, so
  // the crash can land between a snapshot publication and the journal reset.
  const ChildReport report =
      run_child_crash(work, "after_publish", kCommands, kCommands, 256, "snap");
  CHECK_EQ(report.exit_code, kChildCrashExit);
  CHECK(!report.completed);
  CHECK(report.commits.size() >= 2);

  Recovered recovered;
  REQUIRE(reopen_and_verify(work, recovered));
  CHECK(recovered.verified);
  // Checkpoints consume process commit indices of their own, so the recovered
  // generation is compared against what the child published rather than against
  // a fixed count: nothing published is lost, nothing is half-applied, and the
  // recovered candidates are a contiguous prefix of the deterministic replay.
  CHECK(recovered.commit >= report.commits.back());
  CHECK(recovered.candidates >= report.commits.size());
  expect_prefix(recovered, "snap", recovered.candidates, "crash during snapshotting");
  // A snapshot was really published during the run: otherwise this case would
  // not have exercised the interaction it exists for.
  CHECK(recovered.snapshot_bytes > 0);

  // The same store recovers identically a second time, snapshot and journal
  // together producing one authoritative generation.
  Recovered again;
  REQUIRE(reopen_and_verify(work, again));
  CHECK_EQ(again.commit, recovered.commit);
  CHECK_EQ(again.candidates, recovered.candidates);
}
