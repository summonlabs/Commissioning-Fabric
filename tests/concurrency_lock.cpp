// Real exclusion between real processes.
//
// The store claims cross-process single-writer exclusion, so these obligations
// use actual operating system processes: several of them race to open one
// store, a holder is killed without unwinding and the lock must disappear with
// it, and separate processes open the same store one after another and must see
// each other's committed state.
//
// The last test runs several threads against several independent stores: this
// runtime is single-writer per store, so the claim under test is exclusion and
// non-interference, not shared-state concurrency.

#include "support/child_process.hpp"
#include "support/test_harness.hpp"

#include "cxf/runtime/fabric.hpp"
#include "cxf/support/clock.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using cxf::Code;
using cxf::Fabric;
using cxf::FabricOptions;
using cxf::Timestamp;

constexpr std::int64_t kBase = 1700000000;

[[nodiscard]] std::string candidate_name(const std::string& prefix, std::uint64_t index) {
  return prefix + "-" + std::to_string(index);
}

[[nodiscard]] cxf::CandidateDeclaration declaration_of(const std::string& name) {
  cxf::CandidateDeclaration declaration;
  declaration.registry_name = "registry-" + name;
  declaration.model = "model-c";
  declaration.serial = "SN-" + name;
  return declaration;
}

/// Poll until every result file carries text, or until a child that has not
/// reported is found dead. No timeout: a hang is a defect to diagnose.
[[nodiscard]] bool await_results(const std::vector<cxf::test::ChildProcess*>& children,
                                 const std::vector<std::string>& results) {
  for (;;) {
    bool complete = true;
    for (std::size_t i = 0; i < children.size(); ++i) {
      if (!cxf::test::read_file_text(results[i]).empty()) {
        continue;
      }
      if (!children[i]->running()) {
        ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", results[i],
                                "child exited without reporting a result");
        return false;
      }
      complete = false;
    }
    if (complete) {
      return true;
    }
    std::this_thread::yield();
  }
}

// ---------------------------------------------------------------------------
// Child modes.
// ---------------------------------------------------------------------------

/// Hold the store lock until the parent kills this process. The result file
/// says whether this process won the race.
int child_hold_main(int argc, char** argv) {
  if (argc < 2 || std::string_view(argv[1]) != "--child-hold") {
    return -1;
  }
  std::string store;
  std::string result;
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--store" && i + 1 < argc) {
      store = argv[++i];
    } else if (arg == "--result" && i + 1 < argc) {
      result = argv[++i];
    } else {
      std::fprintf(stderr, "child-hold: unexpected argument %s\n", argv[i]);
      return 2;
    }
  }
  if (store.empty() || result.empty()) {
    std::fprintf(stderr, "child-hold: --store and --result are required\n");
    return 2;
  }

  cxf::SystemClock clock;
  FabricOptions options;
  options.directory = store;
  options.holder = "hold-child";
  cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
  if (fabric.ok()) {
    static_cast<void>(cxf::test::write_file_text(result, "held"));
  } else if (fabric.code() == Code::kStorageLocked) {
    static_cast<void>(cxf::test::write_file_text(result, "locked"));
    return 0;
  } else {
    static_cast<void>(cxf::test::write_file_text(
        result, std::string("error:") + std::string(cxf::code_name(fabric.code()))));
    return 3;
  }

  // Hold the lock until this process is terminated without unwinding.
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

/// Open the store, report what it sees, add a few candidates and report again.
int child_admit_main(int argc, char** argv) {
  if (argc < 2 || std::string_view(argv[1]) != "--child-admit") {
    return -1;
  }
  std::string store;
  std::string prefix;
  std::string result;
  std::uint64_t count = 0;
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--store" && i + 1 < argc) {
      store = argv[++i];
    } else if (arg == "--prefix" && i + 1 < argc) {
      prefix = argv[++i];
    } else if (arg == "--count" && i + 1 < argc) {
      count = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--result" && i + 1 < argc) {
      result = argv[++i];
    } else {
      std::fprintf(stderr, "child-admit: unexpected argument %s\n", argv[i]);
      return 2;
    }
  }
  if (store.empty() || prefix.empty() || result.empty()) {
    std::fprintf(stderr, "child-admit: --store, --prefix and --result are required\n");
    return 2;
  }

  cxf::SystemClock clock;
  FabricOptions options;
  options.directory = store;
  options.holder = "admit-child";
  cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
  if (!fabric.ok()) {
    std::fprintf(stderr, "child-admit: open failed: %s\n", fabric.status().message().c_str());
    return 3;
  }
  const std::uint64_t observed = fabric->stats().candidates;
  for (std::uint64_t i = 0; i < count; ++i) {
    cxf::AdmitCandidateRequest request;
    request.name = candidate_name(prefix, i);
    request.declaration = declaration_of(request.name);
    const cxf::Outcome<cxf::RequestOutcome> outcome = fabric->admit_candidate(request);
    if (!outcome.ok()) {
      std::fprintf(stderr, "child-admit: admit failed: %s\n", outcome.status().message().c_str());
      return 4;
    }
  }
  const std::uint64_t final_commit = fabric->meta().commit.value();
  std::string text = "observed=" + std::to_string(observed);
  text += " candidates=" + std::to_string(fabric->stats().candidates);
  text += " commit=" + std::to_string(final_commit);
  static_cast<void>(cxf::test::write_file_text(result, text));
  return 0;
}

struct ChildRegistrar {
  ChildRegistrar() {
    cxf::test::add_child_mode_handler(&child_hold_main);
    cxf::test::add_child_mode_handler(&child_admit_main);
  }
};

const ChildRegistrar kChildRegistrar;

}  // namespace

CXF_TEST(concurrency, exactly_one_process_wins_the_store) {
  constexpr std::size_t kRacers = 6;
  cxf::test::TempDirectory home("lock-race");
  REQUIRE(home.ok());
  const std::string store = home.child("store");

  // Seed the store so the race is about the lock and not about creating the
  // directory.
  {
    cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
    FabricOptions options;
    options.directory = store;
    options.holder = "seed";
    cxf::Outcome<Fabric> seed = Fabric::open(options, clock);
    REQUIRE_OK(seed);
    cxf::AdmitCandidateRequest request;
    request.name = "seed-0";
    request.declaration = declaration_of(request.name);
    REQUIRE_OK(seed->admit_candidate(request));
  }

  std::vector<std::string> results;
  std::vector<cxf::test::ChildProcess> children;
  children.reserve(kRacers);
  results.reserve(kRacers);
  for (std::size_t i = 0; i < kRacers; ++i) {
    results.push_back(home.child("result-" + std::to_string(i) + ".txt"));
    cxf::test::ChildOptions options;
    options.program = cxf::test::executable_path();
    options.arguments = {"--child-hold", "--store", store, "--result", results.back()};
    cxf::Outcome<cxf::test::ChildProcess> child = cxf::test::ChildProcess::start(options);
    REQUIRE_OK(child);
    children.push_back(std::move(child.value()));
  }

  std::vector<cxf::test::ChildProcess*> handles;
  for (cxf::test::ChildProcess& child : children) {
    handles.push_back(&child);
  }
  REQUIRE(await_results(handles, results));

  std::size_t holders = 0;
  std::size_t locked = 0;
  for (const std::string& path : results) {
    const std::string content = cxf::test::read_file_text(path);
    if (content == "held") {
      ++holders;
    } else if (content == "locked") {
      ++locked;
    } else {
      ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", path, "unexpected child result: " + content);
    }
  }
  CHECK_EQ(holders, std::size_t{1});
  CHECK_EQ(locked, kRacers - 1);

  // The losers exited on their own; the winner is terminated without unwinding,
  // which is exactly how a crashed commissioning host disappears.
  for (std::size_t i = 0; i < children.size(); ++i) {
    if (cxf::test::read_file_text(results[i]) == "held") {
      const cxf::Status killed = children[i].terminate();
      CHECK_CODE(killed, Code::kOk);
      CHECK(children[i].exited());
    } else {
      const cxf::Outcome<int> code = children[i].wait();
      REQUIRE_OK(code);
      CHECK_EQ(code.value(), 0);
    }
  }

  // The lock disappeared with the process: the parent can open the store.
  cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
  FabricOptions options;
  options.directory = store;
  options.holder = "after-death";
  const cxf::Outcome<Fabric> after = Fabric::open(options, clock);
  REQUIRE_OK(after);
  CHECK_EQ(after->stats().candidates, 1u);
  CHECK(after->verify_store().ok());
}

CXF_TEST(concurrency, sequential_processes_see_each_others_state) {
  cxf::test::TempDirectory home("sequential");
  REQUIRE(home.ok());
  const std::string store = home.child("store");
  {
    cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
    FabricOptions options;
    options.directory = store;
    options.holder = "seed";
    cxf::Outcome<Fabric> seed = Fabric::open(options, clock);
    REQUIRE_OK(seed);
  }

  struct Round {
    const char* prefix;
    std::uint64_t count;
    std::uint64_t expected_observed;
  };
  const std::vector<Round> rounds = {{"a", 3, 0}, {"b", 2, 3}, {"c", 4, 5}};
  std::uint64_t expected_candidates = 0;
  for (const Round& round : rounds) {
    const std::string result = home.child(std::string("round-") + round.prefix + ".txt");
    cxf::test::ChildOptions options;
    options.program = cxf::test::executable_path();
    options.arguments = {"--child-admit", "--store", store, "--prefix", round.prefix,
                         "--count", std::to_string(round.count), "--result", result};
    const cxf::Outcome<cxf::test::ChildRun> run = cxf::test::run_child(options);
    REQUIRE_OK(run);
    CHECK_EQ(run->exit_code, 0);
    const std::string text = cxf::test::read_file_text(result);
    // Every process sees the state its predecessors committed.
    CHECK_EQ(cxf::test::find_key_value(text, "observed"), std::to_string(round.expected_observed));
    expected_candidates += round.count;
    CHECK_EQ(cxf::test::find_key_value(text, "candidates"), std::to_string(expected_candidates));
  }

  cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
  FabricOptions options;
  options.directory = store;
  options.holder = "final-read";
  const cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
  REQUIRE_OK(fabric);
  CHECK_EQ(fabric->stats().candidates, expected_candidates);
  const cxf::Outcome<std::vector<cxf::CandidateSummary>> list = fabric->list_candidates();
  REQUIRE_OK(list);
  CHECK_EQ(list->size(), static_cast<std::size_t>(expected_candidates));
  CHECK(fabric->verify_store().ok());
}

CXF_TEST(concurrency, threads_on_separate_stores_do_not_interfere) {
  constexpr int kThreads = 4;
  constexpr std::uint64_t kAdmissions = 6;
  cxf::test::TempDirectory home("threads");
  REQUIRE(home.ok());

  std::vector<std::string> failures(kThreads);
  std::vector<std::uint64_t> commits(kThreads, 0);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&home, &failures, &commits, t]() {
      const std::string store = home.child("store-" + std::to_string(t));
      cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase + t));
      FabricOptions options;
      options.directory = store;
      options.holder = "thread-" + std::to_string(t);
      cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
      if (!fabric.ok()) {
        failures[static_cast<std::size_t>(t)] = "open: " + fabric.status().message();
        return;
      }
      for (std::uint64_t i = 0; i < kAdmissions; ++i) {
        cxf::AdmitCandidateRequest request;
        request.name = candidate_name("t" + std::to_string(t), i);
        request.declaration = declaration_of(request.name);
        const cxf::Outcome<cxf::RequestOutcome> outcome = fabric->admit_candidate(request);
        if (!outcome.ok()) {
          failures[static_cast<std::size_t>(t)] = "admit: " + outcome.status().message();
          return;
        }
      }
      if (!fabric->verify_store().ok()) {
        failures[static_cast<std::size_t>(t)] = "verify_store failed";
        return;
      }
      commits[static_cast<std::size_t>(t)] = fabric->meta().commit.value();
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  for (int t = 0; t < kThreads; ++t) {
    const std::size_t index = static_cast<std::size_t>(t);
    CHECK_EQ(failures[index], std::string{});
    CHECK(commits[index] > 0);
    cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase + t));
    FabricOptions options;
    options.directory = home.child("store-" + std::to_string(t));
    options.holder = "verify-" + std::to_string(t);
    const cxf::Outcome<Fabric> fabric = Fabric::open(options, clock);
    REQUIRE_OK(fabric);
    CHECK_EQ(fabric->meta().commit.value(), commits[index]);
    CHECK_EQ(fabric->stats().candidates, kAdmissions);
  }
}
