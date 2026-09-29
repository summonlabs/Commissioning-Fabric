// Several real processes mutating one store, then a library read.
//
// Each child process is a separate operating system process that opens the
// store, admits a batch of candidates and exits. The parent then reads the
// store back through the library and proves that every accepted request is
// present exactly once: replaying it returns the recorded commit and effect
// digest without applying anything again, the commit sequence is contiguous,
// and no candidate was lost or duplicated.

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
using cxf::Fabric;
using cxf::FabricOptions;
using cxf::Timestamp;

constexpr std::int64_t kBase = 1700000000;

[[nodiscard]] cxf::CandidateDeclaration declaration_of(const std::string& name) {
  cxf::CandidateDeclaration declaration;
  declaration.registry_name = "registry-" + name;
  declaration.model = "model-multiprocess";
  declaration.serial = "SN-" + name;
  return declaration;
}

[[nodiscard]] std::string batch_name(const std::string& prefix, std::uint64_t index) {
  return prefix + "-" + std::to_string(index);
}

// ---------------------------------------------------------------------------
// Child mode: one batch of admissions in its own process.
// ---------------------------------------------------------------------------
int batch_child_main(int argc, char** argv) {
  if (argc < 2 || std::string_view(argv[1]) != "--child-batch") {
    return -1;
  }
  std::string store;
  std::string prefix;
  std::uint64_t count = 0;
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--store" && i + 1 < argc) {
      store = argv[++i];
    } else if (arg == "--prefix" && i + 1 < argc) {
      prefix = argv[++i];
    } else if (arg == "--count" && i + 1 < argc) {
      count = std::strtoull(argv[++i], nullptr, 10);
    } else {
      std::fprintf(stderr, "child-batch: unexpected argument %s\n", argv[i]);
      return 2;
    }
  }
  if (store.empty() || prefix.empty()) {
    std::fprintf(stderr, "child-batch: --store and --prefix are required\n");
    return 2;
  }

  cxf::SystemClock clock;
  FabricOptions options;
  options.directory = store;
  options.holder = "batch-child";
  cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
  if (!fabric.ok()) {
    std::fprintf(stderr, "child-batch: open failed: %s\n", fabric.status().message().c_str());
    return 3;
  }
  // What this process sees is what its predecessors committed.
  std::printf("child observed=%llu commit=%llu\n",
              static_cast<unsigned long long>(fabric->stats().candidates),
              static_cast<unsigned long long>(fabric->meta().commit.value()));
  std::fflush(stdout);

  for (std::uint64_t i = 0; i < count; ++i) {
    cxf::AdmitCandidateRequest request;
    request.name = batch_name(prefix, i);
    request.declaration = declaration_of(request.name);
    const cxf::Outcome<cxf::RequestOutcome> outcome = fabric->admit_candidate(request);
    if (!outcome.ok()) {
      std::fprintf(stderr, "child-batch: admit failed: %s\n", outcome.status().message().c_str());
      return 4;
    }
    std::printf("accepted request=%llu commit=%llu effect=%s name=%s\n",
                static_cast<unsigned long long>(outcome->request.value()),
                static_cast<unsigned long long>(outcome->commit.value()),
                outcome->effect_digest.hex().c_str(), request.name.c_str());
    std::fflush(stdout);
  }
  std::printf("child done commit=%llu\n",
              static_cast<unsigned long long>(fabric->meta().commit.value()));
  std::fflush(stdout);
  return 0;
}

struct ChildRegistrar {
  ChildRegistrar() { cxf::test::add_child_mode_handler(&batch_child_main); }
};

const ChildRegistrar kChildRegistrar;

struct Accepted {
  std::uint64_t request{0};
  std::uint64_t commit{0};
  std::string effect{};
  std::string name{};
};

struct BatchReport {
  int exit_code{0};
  std::uint64_t observed{0};
  std::uint64_t observed_commit{0};
  std::uint64_t final_commit{0};
  bool done{false};
  std::vector<Accepted> accepted{};
};

[[nodiscard]] BatchReport run_batch(const std::string& store, const std::string& prefix,
                                    std::uint64_t count) {
  cxf::test::ChildOptions options;
  options.program = cxf::test::executable_path();
  options.arguments = {"--child-batch", "--store", store, "--prefix", prefix, "--count",
                       std::to_string(count)};
  const cxf::Outcome<cxf::test::ChildRun> run = cxf::test::run_child(options);
  REQUIRE_OK(run);

  BatchReport report;
  report.exit_code = run->exit_code;
  if (run->exit_code != 0) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", "child-batch " + prefix,
                            "exit " + std::to_string(run->exit_code) + " stderr: " + run->err);
    return report;
  }
  for (const std::string& line : cxf::test::split_lines(run->out)) {
    if (line.rfind("child observed=", 0) == 0) {
      report.observed =
          std::strtoull(cxf::test::find_key_value(line, "observed").c_str(), nullptr, 10);
      report.observed_commit =
          std::strtoull(cxf::test::find_key_value(line, "commit").c_str(), nullptr, 10);
    } else if (line.rfind("accepted ", 0) == 0) {
      Accepted accepted;
      accepted.request =
          std::strtoull(cxf::test::find_key_value(line, "request").c_str(), nullptr, 10);
      accepted.commit =
          std::strtoull(cxf::test::find_key_value(line, "commit").c_str(), nullptr, 10);
      accepted.effect = cxf::test::find_key_value(line, "effect");
      accepted.name = cxf::test::find_key_value(line, "name");
      report.accepted.push_back(accepted);
    } else if (line.rfind("child done ", 0) == 0) {
      report.done = true;
      report.final_commit =
          std::strtoull(cxf::test::find_key_value(line, "commit").c_str(), nullptr, 10);
    }
  }
  return report;
}

}  // namespace

CXF_TEST(multiprocess, sequential_processes_leave_one_exact_generation) {
  constexpr std::uint64_t kBatches = 3;
  constexpr std::uint64_t kPerBatch = 4;
  cxf::test::TempDirectory home("multiprocess");
  REQUIRE(home.ok());
  const std::string store = home.child("store");

  std::uint64_t baseline = 0;
  {
    cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
    FabricOptions options;
    options.directory = store;
    options.holder = "multiprocess-parent";
    cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
    REQUIRE_OK(fabric);
    cxf::FacilityChangeRequest facility;
    facility.topology = cxf::TopologyGeneration::from_value(1);
    facility.detail = "baseline generation";
    REQUIRE_OK(fabric->record_facility_change(facility));
    baseline = fabric->meta().commit.value();
  }

  std::vector<Accepted> accepted;
  std::uint64_t expected_candidates = 0;
  for (std::uint64_t batch = 0; batch < kBatches; ++batch) {
    const std::string prefix = "mp-" + std::to_string(batch);
    const BatchReport report = run_batch(store, prefix, kPerBatch);
    CHECK(report.done);
    // Each process sees the committed state of its predecessors, never a stale
    // or empty one.
    CHECK_EQ(report.observed, expected_candidates);
    CHECK_EQ(report.observed_commit, baseline + expected_candidates);
    CHECK_EQ(report.accepted.size(), static_cast<std::size_t>(kPerBatch));
    expected_candidates += kPerBatch;
    CHECK_EQ(report.final_commit, baseline + expected_candidates);
    for (const Accepted& item : report.accepted) {
      accepted.push_back(item);
    }
  }
  CHECK_EQ(accepted.size(), kBatches * kPerBatch);

  // The commit sequence is contiguous across processes and in order of
  // acceptance.
  for (std::size_t i = 0; i < accepted.size(); ++i) {
    CHECK_EQ(accepted[i].commit, baseline + i + 1);
  }

  cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
  FabricOptions options;
  options.directory = store;
  options.holder = "multiprocess-read";
  cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
  REQUIRE_OK(fabric);
  CHECK_EQ(fabric->meta().commit.value(), baseline + accepted.size());
  CHECK_EQ(fabric->stats().candidates, expected_candidates);
  CHECK(fabric->verify_store().ok());

  // Every accepted request is present exactly once: replaying it with the
  // runtime-reported id returns the recorded commit and effect digest.
  for (const Accepted& item : accepted) {
    cxf::AdmitCandidateRequest request;
    request.request = cxf::RequestId::from_value(item.request);
    request.name = item.name;
    request.declaration = declaration_of(item.name);
    const cxf::Outcome<cxf::RequestOutcome> replay = fabric->admit_candidate(request);
    REQUIRE_OK(replay);
    CHECK(replay->replayed);
    CHECK_EQ(replay->request.value(), item.request);
    CHECK_EQ(replay->commit.value(), item.commit);
    CHECK_EQ(replay->effect_digest.hex(), item.effect);
  }
  // Nothing was applied a second time by the replay pass.
  CHECK_EQ(fabric->stats().candidates, expected_candidates);
  CHECK_EQ(fabric->meta().commit.value(), baseline + accepted.size());

  // The admitted names are exactly the batch names, each present once.
  const cxf::Outcome<std::vector<cxf::CandidateSummary>> list = fabric->list_candidates();
  REQUIRE_OK(list);
  std::vector<std::string> names;
  for (const cxf::CandidateSummary& summary : *list) {
    names.push_back(summary.name);
  }
  std::sort(names.begin(), names.end());
  std::vector<std::string> expected;
  for (std::uint64_t batch = 0; batch < kBatches; ++batch) {
    for (std::uint64_t i = 0; i < kPerBatch; ++i) {
      expected.push_back(batch_name("mp-" + std::to_string(batch), i));
    }
  }
  std::sort(expected.begin(), expected.end());
  CHECK_EQ(names, expected);
}

CXF_TEST(multiprocess, parent_mutation_is_visible_to_the_next_process) {
  cxf::test::TempDirectory home("multiprocess-interleave");
  REQUIRE(home.ok());
  const std::string store = home.child("store");
  {
    cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
    FabricOptions options;
    options.directory = store;
    options.holder = "interleave";
    cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
    REQUIRE_OK(fabric);
  }

  const BatchReport first = run_batch(store, "inter-a", 2);
  CHECK_EQ(first.observed, 0u);
  CHECK_EQ(first.accepted.size(), std::size_t{2});

  // A parent mutation between two child processes.
  {
    cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
    FabricOptions options;
    options.directory = store;
    options.holder = "interleave-parent";
    cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
    REQUIRE_OK(fabric);
    cxf::AdmitCandidateRequest request;
    request.name = "inter-parent-0";
    request.declaration = declaration_of(request.name);
    REQUIRE_OK(fabric->admit_candidate(request));
  }

  const BatchReport second = run_batch(store, "inter-b", 2);
  // The second child sees the first child's two candidates and the parent's.
  CHECK_EQ(second.observed, 3u);
  CHECK_EQ(second.accepted.size(), std::size_t{2});

  cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
  FabricOptions options;
  options.directory = store;
  options.holder = "interleave-read";
  const cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
  REQUIRE_OK(fabric);
  CHECK_EQ(fabric->stats().candidates, 5u);
  CHECK(fabric->verify_store().ok());
}
