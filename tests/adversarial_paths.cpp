// Adversarial paths and filesystem edges.
//
// The store is a directory of real files, so every hostile path shape has to be
// answered with a clear Status: a store that cannot exist, a parent that does
// not exist, a path that escapes its directory, an alternate-data-stream colon,
// a reserved device name, a path far beyond the classic length limit, a store
// that disappears underneath an open fabric, and a link planted where the
// journal or the snapshot must be.

#include "support/test_harness.hpp"

#include "cxf/runtime/fabric.hpp"
#include "cxf/support/clock.hpp"
#include "cxf/support/fs.hpp"
#include "cxf/support/text.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
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
  declaration.model = "model-path";
  declaration.serial = "SN-" + name;
  return declaration;
}

[[nodiscard]] cxf::Outcome<cxf::RequestOutcome> admit_one(Fabric& fabric,
                                                          const std::string& name) {
  cxf::AdmitCandidateRequest request;
  request.name = name;
  request.declaration = declaration_of(name);
  return fabric.admit_candidate(request);
}

[[nodiscard]] cxf::Outcome<Fabric> open_store(const std::string& directory, bool create,
                                              cxf::ManualClock& clock) {
  FabricOptions options;
  options.directory = directory;
  options.holder = "adversarial-paths";
  options.create_if_missing = create;
  return Fabric::open(options, clock);
}

/// The first non-lock file of a store: the journal, by construction.
[[nodiscard]] std::string find_journal(const std::string& root) {
  std::string lock;
  for (const std::string& name : cxf::test::list_directory_names(root)) {
    const std::string text = cxf::test::read_file_text(root + "/" + name);
    if (cxf::test::contains_substring(text, "holder=")) {
      lock = name;
      break;
    }
  }
  for (const std::string& name : cxf::test::list_directory_names(root)) {
    if (name != lock) {
      return name;
    }
  }
  return std::string{};
}

}  // namespace

CXF_TEST(adversarial, store_path_that_is_a_file) {
  cxf::test::TempDirectory home("path-file");
  REQUIRE(home.ok());
  const std::string store = home.child("store");
  REQUIRE(cxf::test::write_file_text(store, "this is a file, not a store"));
  cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
  cxf::Outcome<Fabric> fabric = open_store(store, true, clock);
  CHECK(!fabric.ok());
  CHECK(cxf::test::is_storage_failure(fabric.code()) || cxf::test::is_input_rejection(fabric.code()));
  // The file is untouched: the runtime did not overwrite it with a store.
  CHECK_EQ(cxf::test::read_file_text(store), std::string("this is a file, not a store"));
}

CXF_TEST(adversarial, missing_parent_without_create_is_unavailable) {
  cxf::test::TempDirectory home("path-missing");
  REQUIRE(home.ok());
  cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
  cxf::Outcome<Fabric> fabric = open_store(home.child("absent/store"), false, clock);
  CHECK_CODE(fabric, Code::kStorageUnavailable);
  CHECK(!cxf::test::path_exists(home.child("absent")));
}

CXF_TEST(adversarial, escaping_and_stream_paths_are_refused) {
  cxf::test::TempDirectory home("path-escape");
  REQUIRE(home.ok());
  cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));

  const std::vector<std::string> hostile = {
      home.child("a/../store"),
      home.child("a\\..\\store"),
      home.child(".."),
      home.child("store:ads"),
      home.child("store:stream:$DATA")};
  for (const std::string& directory : hostile) {
    cxf::Outcome<Fabric> fabric = open_store(directory, true, clock);
    if (fabric.ok()) {
      ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", directory,
                              "a path that escapes or names a stream was accepted");
    } else {
      CHECK(cxf::test::is_input_rejection(fabric.code()));
    }
  }
  // Nothing was created anywhere for the refused paths.
  CHECK_EQ(cxf::test::list_directory_names(home.path()).size(), std::size_t{0});
  // The path validator itself names the reasons.
  CHECK_CODE(cxf::fs::validate_path(home.child("a/../store")), Code::kMalformedInput);
  CHECK_CODE(cxf::fs::validate_path(home.child("store:ads")), Code::kMalformedInput);
  CHECK(!cxf::is_safe_path_component("NUL"));
  CHECK(!cxf::is_safe_path_component("con.txt"));
}

CXF_TEST(adversarial, reserved_device_name_directory) {
  cxf::test::TempDirectory home("path-reserved");
  REQUIRE(home.ok());
  cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
  const std::string store = home.child("NUL");
  cxf::Outcome<Fabric> fabric = open_store(store, true, clock);
  if (fabric.ok()) {
    // The platform let the directory exist under its long-path spelling; the
    // store must then be a working store rather than a silently broken one.
    REQUIRE_OK(admit_one(*fabric, "reserved-1"));
    CHECK_EQ(fabric->stats().candidates, 1u);
    CHECK(fabric->verify_store().ok());
  } else {
    CHECK(cxf::test::is_storage_failure(fabric.code()) ||
          cxf::test::is_input_rejection(fabric.code()));
  }
}

CXF_TEST(adversarial, a_very_long_store_path) {
  cxf::test::TempDirectory home("path-long");
  REQUIRE(home.ok());
  std::string deep = home.path();
  for (int i = 0; i < 5; ++i) {
    deep += "/segment-" + std::to_string(i) + std::string(24, 'x');
  }
  CHECK(deep.size() > 200);
  CHECK(deep.size() < 240);
  std::uint64_t commit = 0;
  {
    cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
    cxf::Outcome<Fabric> fabric = open_store(deep, true, clock);
    REQUIRE_OK(fabric);
    REQUIRE_OK(admit_one(*fabric, "long-1"));
    commit = fabric->meta().commit.value();
    CHECK(fabric->verify_store().ok());
  }
  // The path is long but real: it recovers like any other store once the
  // single writer has closed it.
  cxf::ManualClock second_clock(Timestamp::from_unix_seconds(kBase));
  const cxf::Outcome<Fabric> reopened = open_store(deep, false, second_clock);
  REQUIRE_OK(reopened);
  CHECK_EQ(reopened->meta().commit.value(), commit);
  CHECK_EQ(reopened->stats().candidates, 1u);
  CHECK(reopened->verify_store().ok());
}

CXF_TEST(adversarial, store_directory_removed_while_open) {
  cxf::test::TempDirectory home("path-removed");
  REQUIRE(home.ok());
  const std::string store = home.child("store");
  cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
  cxf::Outcome<Fabric> fabric = open_store(store, true, clock);
  REQUIRE_OK(fabric);
  REQUIRE_OK(admit_one(*fabric, "removed-1"));

  // Deleting the store underneath the open fabric is either refused by the
  // operating system (an open file cannot be unlinked) or accepted; a recursive
  // removal can also delete the journal and fail on the locked file, leaving a
  // half-emptied directory. Whatever happened, the runtime must stay honest.
  const bool removed = cxf::test::remove_tree(store);
  const cxf::Outcome<cxf::RequestOutcome> after = admit_one(*fabric, "removed-2");
  if (!after.ok()) {
    // A refusal is only acceptable as a clear status.
    CHECK(cxf::test::is_storage_failure(after.code()) ||
          cxf::test::is_input_rejection(after.code()));
  } else {
    // If the mutation was accepted, the live fabric still counts it.
    CHECK_EQ(fabric->stats().candidates, 2u);
  }
  fabric = cxf::Outcome<Fabric>(cxf::Status::success());

  cxf::ManualClock second_clock(Timestamp::from_unix_seconds(kBase));
  if (removed && !cxf::test::is_directory(store)) {
    // The directory is gone: a strict open reports it rather than inventing a
    // store in its place, and a creating open may only make a new empty one.
    CHECK_CODE(open_store(store, false, second_clock), Code::kStorageUnavailable);
    return;
  }
  // The directory survived in some shape. Reopening either reports damage or
  // recovers a generation that is a prefix of what was admitted.
  const cxf::Outcome<Fabric> reopened = open_store(store, false, second_clock);
  if (!reopened.ok()) {
    CHECK(cxf::test::is_storage_failure(reopened.code()) ||
          cxf::test::is_input_rejection(reopened.code()));
    return;
  }
  CHECK(reopened->verify_store().ok());
  // A directory whose journal was unlinked reopens as a new empty generation;
  // what matters is that nothing is invented and nothing claims to be the lost
  // state.
  CHECK(reopened->stats().candidates <= 2u);
}

CXF_TEST(adversarial, a_directory_where_the_journal_must_be) {
  cxf::test::TempDirectory home("path-journal-dir");
  REQUIRE(home.ok());
  const std::string store = home.child("store");
  cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
  {
    cxf::Outcome<Fabric> fabric = open_store(store, true, clock);
    REQUIRE_OK(fabric);
    REQUIRE_OK(admit_one(*fabric, "journal-1"));
  }
  const std::string journal = find_journal(store);
  REQUIRE(!journal.empty());
  // Replace the journal with a directory of the same name: a store whose
  // journal cannot be a file must be reported, not guessed at.
  REQUIRE(cxf::fs::remove_file(store + "/" + journal).ok());
  REQUIRE(cxf::test::ensure_directory(store + "/" + journal));
  cxf::ManualClock second_clock(Timestamp::from_unix_seconds(kBase));
  cxf::Outcome<Fabric> fabric = open_store(store, true, second_clock);
  if (fabric.ok()) {
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", "directory at the journal path",
                            "the store opened although its journal path is a directory");
  } else {
    CHECK(cxf::test::is_storage_failure(fabric.code()));
  }
}

CXF_TEST(adversarial, a_link_where_the_journal_must_be) {
  cxf::test::TempDirectory home("path-link");
  REQUIRE(home.ok());
  const std::string store = home.child("store");
  cxf::ManualClock clock(Timestamp::from_unix_seconds(kBase));
  {
    cxf::Outcome<Fabric> fabric = open_store(store, true, clock);
    REQUIRE_OK(fabric);
    REQUIRE_OK(admit_one(*fabric, "link-1"));
  }
  const std::string journal = find_journal(store);
  REQUIRE(!journal.empty());
  const std::string real = store + "/" + journal;
  const std::string moved = home.child("journal-copy");
  REQUIRE(cxf::test::copy_tree(real, moved));
  REQUIRE(cxf::fs::remove_file(real).ok());

  // Creating a link needs a privilege on some platforms; when the platform does
  // not allow it the case is skipped explicitly rather than silently.
  std::error_code ec;
  std::filesystem::create_symlink(moved, real, ec);
  if (ec) {
    std::printf("  note: symbolic links are not permitted here (%s)\n", ec.message().c_str());
    // The store is now missing its journal; the runtime must still answer with a
    // clear status rather than inventing state.
    cxf::ManualClock second_clock(Timestamp::from_unix_seconds(kBase));
    cxf::Outcome<Fabric> fabric = open_store(store, true, second_clock);
    if (fabric.ok()) {
      CHECK(fabric->verify_store().ok());
    } else {
      CHECK(cxf::test::is_storage_failure(fabric.code()));
    }
    return;
  }

  cxf::ManualClock second_clock(Timestamp::from_unix_seconds(kBase));
  cxf::Outcome<Fabric> fabric = open_store(store, true, second_clock);
  if (fabric.ok()) {
    // Opening may succeed only if the link was refused and a store was made
    // again; it must never read the state through the link.
    ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", "link at the journal path",
                            "the store followed a reparse point at the journal path");
  } else {
    CHECK(cxf::test::is_storage_failure(fabric.code()));
  }
  std::error_code ignored;
  std::filesystem::remove(real, ignored);
}
