// Commissioning Fabric - proof-obligation test harness.
//
// The harness is deliberately small and self-contained: registration by suite
// and name, CHECK/CHECK_EQ/REQUIRE macros that report file:line and the failing
// expression, deterministic --suite/--filter/--seed selection, reproduction
// data on failure, and a main() so every test executable is driven by it.
//
// Randomised tests draw from a shared deterministic SplitMix64 generator. The
// stream a test sees depends only on the seed and on the test suite and name,
// so selecting a single test with --filter reproduces the same values as a full
// suite run.
#ifndef CXF_TESTS_SUPPORT_TEST_HARNESS_HPP
#define CXF_TESTS_SUPPORT_TEST_HARNESS_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "cxf/support/digest.hpp"
#include "cxf/support/status.hpp"

namespace cxf::test {

/// Fixed default seed. Never derived from the clock: a failing run must be
/// reproducible tomorrow.
inline constexpr std::uint64_t kDefaultSeed = 20260215ULL;

// ---------------------------------------------------------------------------
// Deterministic random numbers (SplitMix64).
//
//   state += 0x9E3779B97F4A7C15
//   z = state
//   z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
//   z = (z ^ (z >> 27)) * 0x94D049BB133111EB
//   result = z ^ (z >> 31)
//
// The algorithm is fixed by this comment: changing it changes every recorded
// seed and is therefore a deliberate, visible act.
// ---------------------------------------------------------------------------
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t next_u64() noexcept;
  [[nodiscard]] std::uint32_t next_u32() noexcept {
    return static_cast<std::uint32_t>(next_u64() >> 32u);
  }
  /// Value in [0, bound); a zero bound yields zero.
  [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept {
    return bound == 0 ? 0 : next_u64() % bound;
  }
  /// Value in [low, high]; low must not exceed high.
  [[nodiscard]] std::uint64_t inclusive(std::uint64_t low, std::uint64_t high) noexcept {
    return low + below(high - low + 1);
  }
  /// True with probability numerator/denominator (denominator must be nonzero).
  [[nodiscard]] bool chance(std::uint32_t numerator, std::uint32_t denominator) noexcept {
    return denominator != 0 && static_cast<std::uint32_t>(below(denominator)) < numerator;
  }
  /// Lowercase token text drawn from [a-z0-9-._:].
  [[nodiscard]] std::string token(std::size_t length);
  /// Lowercase ASCII letters.
  [[nodiscard]] std::string letters(std::size_t length);
  [[nodiscard]] std::vector<std::byte> bytes(std::size_t length);
  [[nodiscard]] std::uint64_t state() const noexcept { return state_; }

 private:
  std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// Registration.
// ---------------------------------------------------------------------------
using TestFn = void (*)();

struct TestCase {
  std::string suite{};
  std::string name{};
  TestFn function{nullptr};
};

class Registry {
 public:
  [[nodiscard]] static Registry& instance();
  void add(std::string_view suite, std::string_view name, TestFn function);
  [[nodiscard]] const std::vector<TestCase>& tests() const noexcept { return tests_; }

 private:
  std::vector<TestCase> tests_{};
};

struct Registrar {
  Registrar(std::string_view suite, std::string_view name, TestFn function);
};

/// Thrown by REQUIRE so the rest of the test body is abandoned. The process is
/// never torn down: the runner catches this and continues with the next test.
class TestAbort {};

// ---------------------------------------------------------------------------
// Failure reporting.
// ---------------------------------------------------------------------------

/// Record one failed check. Prints the failure with the reproduction data.
void fail_check(const char* file, int line, std::string_view kind,
                std::string_view expression, std::string_view detail);

/// Record one attempted check, so the summary counts what actually ran.
void note_check() noexcept;

/// Render a value for a failure message.
[[nodiscard]] std::string describe_value(std::string_view value);
[[nodiscard]] std::string describe_value(const std::string& value);
[[nodiscard]] std::string describe_value(const char* value);
[[nodiscard]] std::string describe_value(bool value);
[[nodiscard]] std::string describe_value(const Status& status);
[[nodiscard]] std::string describe_value(Code code);
[[nodiscard]] std::string describe_value(const Digest& digest);

template <typename T>
  requires std::is_arithmetic_v<T>
[[nodiscard]] std::string describe_value(T value) {
  if constexpr (std::is_same_v<T, bool>) {
    return value ? std::string("true") : std::string("false");
  } else if constexpr (std::is_signed_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else {
    return std::to_string(static_cast<unsigned long long>(value));
  }
}

template <typename T>
  requires std::is_enum_v<T>
[[nodiscard]] std::string describe_value(T value) {
  return std::to_string(static_cast<long long>(static_cast<std::underlying_type_t<T>>(value)));
}

/// Fallback for values with no textual rendering: the check still reports the
/// expression and the file:line, which is what a reader needs to reproduce it.
template <typename T>
  requires(!std::is_arithmetic_v<T> && !std::is_enum_v<T> &&
           !std::is_convertible_v<T, std::string_view> && !std::is_same_v<T, Status> &&
           !std::is_same_v<T, Digest>)
[[nodiscard]] std::string describe_value(const T& /*value*/) {
  return "<value>";
}

template <typename A, typename B>
[[nodiscard]] std::string describe_pair(const A& a, const B& b) {
  return describe_value(a) + "  |  " + describe_value(b);
}

void check_outcome(const char* file, int line, std::string_view expression,
                   const Status& status, bool required);
template <typename T>
void check_outcome(const char* file, int line, std::string_view expression,
                   const Outcome<T>& outcome, bool required) {
  note_check();
  if (outcome.ok()) {
    return;
  }
  fail_check(file, line, required ? "REQUIRE_OK" : "CHECK_OK", expression,
             outcome.status().message());
  if (required) {
    throw TestAbort{};
  }
}

void check_code(const char* file, int line, std::string_view expression, const Status& status,
                Code expected);
template <typename T>
void check_code(const char* file, int line, std::string_view expression,
                const Outcome<T>& outcome, Code expected) {
  note_check();
  const Code actual = outcome.code();
  const bool failed = !outcome.ok();
  const bool good = expected == Code::kOk ? !failed : (failed && actual == expected);
  if (good) {
    return;
  }
  std::string detail = "expected ";
  detail.append(code_name(expected));
  detail.append(", got ");
  detail.append(code_name(actual));
  if (failed) {
    detail.append(" [");
    detail.append(outcome.status().detail());
    detail.push_back(']');
  }
  fail_check(file, line, "CHECK_CODE", expression, detail);
}

/// True when the code is one of the input-shape rejections. Used where the
/// documentation fixes the rejection but not the exact code.
[[nodiscard]] bool is_input_rejection(Code code) noexcept;
/// True when the code belongs to the persistence/environment family.
[[nodiscard]] bool is_storage_failure(Code code) noexcept;

// ---------------------------------------------------------------------------
// Options and the running test.
// ---------------------------------------------------------------------------
struct Options {
  std::string suite{};
  std::string filter{};
  std::uint64_t seed{kDefaultSeed};
  bool list{false};
};

[[nodiscard]] const Options& options() noexcept;
[[nodiscard]] std::uint64_t seed() noexcept;
/// The generator of the test that is currently running. Valid inside a test.
[[nodiscard]] Rng& rng() noexcept;
/// Path of the running executable, used to re-invoke this process as a child.
[[nodiscard]] const std::string& executable_path() noexcept;

/// A child-mode handler is consulted by main() before the harness parses
/// arguments. It returns a non-negative process exit code when it recognised
/// the invocation, or -1 to let the harness handle it.
using ChildModeHandler = int (*)(int argc, char** argv);
void add_child_mode_handler(ChildModeHandler handler);
/// The registered handlers, consulted by main() before the harness runs.
[[nodiscard]] const std::vector<ChildModeHandler>& child_mode_handlers() noexcept;

/// Parse arguments, run the selected tests, print the summary and return the
/// process exit code (0 only when every selected test passed).
[[nodiscard]] int run_all(int argc, char** argv);

// ---------------------------------------------------------------------------
// Temporary directories and small file helpers.
//
// Every store used by a test lives under the process temporary directory and is
// removed by the object that created it. Nothing is ever written into the
// repository tree.
// ---------------------------------------------------------------------------
class TempDirectory {
 public:
  explicit TempDirectory(std::string_view label);
  ~TempDirectory();
  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::string child(std::string_view name) const;
  /// Remove the tree now; the destructor remains safe to run afterwards.
  void remove_now();

 private:
  std::string path_{};
  bool ok_{false};
};

struct FileBytes {
  bool ok{false};
  std::vector<std::byte> bytes{};
};

[[nodiscard]] bool path_exists(std::string_view path);
[[nodiscard]] bool is_directory(std::string_view path);
[[nodiscard]] bool ensure_directory(std::string_view path);
[[nodiscard]] bool remove_tree(std::string_view path);
[[nodiscard]] bool copy_tree(std::string_view from, std::string_view to);
[[nodiscard]] FileBytes read_file_bytes(std::string_view path);
[[nodiscard]] bool write_file_bytes(std::string_view path, std::span<const std::byte> bytes);
[[nodiscard]] std::string read_file_text(std::string_view path);
[[nodiscard]] bool write_file_text(std::string_view path, std::string_view text);
[[nodiscard]] std::vector<std::string> list_directory_names(std::string_view path);
[[nodiscard]] std::uint64_t file_bytes_size(std::string_view path);
/// Truncate a file to the given size (leaving the file in place).
[[nodiscard]] bool truncate_file(std::string_view path, std::uint64_t size);
/// Overwrite one byte at the given offset.
[[nodiscard]] bool poke_byte(std::string_view path, std::uint64_t offset, std::uint8_t value);
/// Flip one bit of one byte at the given offset.
[[nodiscard]] bool flip_bit(std::string_view path, std::uint64_t offset, unsigned bit);

/// Text helpers used by the CLI-driven tests.
[[nodiscard]] bool contains_line(std::string_view text, std::string_view line);
[[nodiscard]] bool contains_substring(std::string_view text, std::string_view needle);
[[nodiscard]] std::vector<std::string> split_lines(std::string_view text);
/// First line that starts with the prefix, or an empty string.
[[nodiscard]] std::string find_line_with_prefix(std::string_view text, std::string_view prefix);
/// Value of the given key= on the first line that carries it (up to the next space).
[[nodiscard]] std::string find_key_value(std::string_view text, std::string_view key);

}  // namespace cxf::test

// ---------------------------------------------------------------------------
// Macros.
// ---------------------------------------------------------------------------
#define CXF_TEST(suite, name)                                                             \
  static void cxf_test_##suite##_##name();                                                \
  namespace {                                                                             \
  const ::cxf::test::Registrar cxf_registrar_##suite##_##name(#suite, #name,              \
                                                             &cxf_test_##suite##_##name); \
  }                                                                                       \
  static void cxf_test_##suite##_##name()

#define CHECK(expression)                                                     \
  do {                                                                        \
    ::cxf::test::note_check();                                                \
    if (!(expression)) {                                                      \
      ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK", #expression, "");  \
    }                                                                         \
  } while (false)

#define CHECK_EQ(left, right)                                                       \
  do {                                                                              \
    ::cxf::test::note_check();                                                      \
    const auto& cxf_left_value = (left);                                            \
    const auto& cxf_right_value = (right);                                          \
    if (!(cxf_left_value == cxf_right_value)) {                                     \
      ::cxf::test::fail_check(__FILE__, __LINE__, "CHECK_EQ", #left " == " #right,  \
                              ::cxf::test::describe_pair(cxf_left_value,            \
                                                         cxf_right_value));         \
    }                                                                               \
  } while (false)

#define REQUIRE(expression)                                                      \
  do {                                                                           \
    ::cxf::test::note_check();                                                   \
    if (!(expression)) {                                                         \
      ::cxf::test::fail_check(__FILE__, __LINE__, "REQUIRE", #expression, "");   \
      throw ::cxf::test::TestAbort{};                                            \
    }                                                                            \
  } while (false)

#define CHECK_OK(value) ::cxf::test::check_outcome(__FILE__, __LINE__, #value, (value), false)

#define REQUIRE_OK(value) ::cxf::test::check_outcome(__FILE__, __LINE__, #value, (value), true)

#define CHECK_CODE(value, expected) \
  ::cxf::test::check_code(__FILE__, __LINE__, #value, (value), (expected))

#endif  // CXF_TESTS_SUPPORT_TEST_HARNESS_HPP
