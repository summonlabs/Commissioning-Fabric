#include "test_harness.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cxf::test {
namespace {

struct RunState {
  Options options{};
  std::string executable{};
  std::vector<ChildModeHandler> child_handlers{};
  Rng* current_rng{nullptr};
  std::uint64_t current_seed{kDefaultSeed};
  std::string current_suite{};
  std::string current_name{};
  int checks{0};
  int check_failures{0};
  int tests_run{0};
  int tests_failed{0};
};

RunState& state() {
  static RunState instance;
  return instance;
}

[[nodiscard]] std::filesystem::path to_path(std::string_view utf8) {
  std::u8string wide;
  wide.reserve(utf8.size());
  for (char c : utf8) {
    wide.push_back(static_cast<char8_t>(static_cast<unsigned char>(c)));
  }
  return std::filesystem::path(wide);
}

[[nodiscard]] std::string from_path(const std::filesystem::path& path) {
  const std::u8string wide = path.u8string();
  std::string out;
  out.reserve(wide.size());
  for (char8_t c : wide) {
    out.push_back(static_cast<char>(c));
  }
  return out;
}

/// 64-bit FNV-1a over the suite and name, used to derive a per-test seed from
/// the single --seed value. A test therefore sees the same stream whether it is
/// selected alone or as part of a suite.
[[nodiscard]] std::uint64_t test_seed(std::uint64_t base, std::string_view suite,
                                      std::string_view name) {
  std::uint64_t hash = 0xCBF29CE484222325ull;
  const auto absorb = [&hash](std::string_view text) {
    for (char c : text) {
      hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
      hash *= 0x100000001B3ull;
    }
  };
  absorb(suite);
  absorb("/");
  absorb(name);
  Rng mixer(base ^ hash);
  return mixer.next_u64();
}

[[nodiscard]] std::string sanitize_label(std::string_view label) {
  std::string out;
  out.reserve(label.size());
  for (char c : label) {
    const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_';
    out.push_back(keep ? c : '-');
  }
  if (out.size() > 48) {
    out.resize(48);
  }
  return out.empty() ? std::string("temp") : out;
}

[[nodiscard]] std::string process_id_text() {
#if defined(_WIN32)
  return std::to_string(static_cast<unsigned long>(GetCurrentProcessId()));
#else
  return std::to_string(static_cast<long>(::getpid()));
#endif
}

[[nodiscard]] std::string current_executable(std::string_view argv0) {
#if defined(_WIN32)
  std::wstring buffer(32768, L'\0');
  const DWORD written =
      GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (written > 0 && written < buffer.size()) {
    buffer.resize(written);
    std::string out;
    out.reserve(buffer.size());
    for (wchar_t c : buffer) {
      if (c < 0x80) {
        out.push_back(static_cast<char>(c));
      } else {
        out.push_back('?');
      }
    }
    return out;
  }
#else
  std::error_code ec;
  const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    return from_path(self);
  }
#endif
  return std::string(argv0);
}

void print_usage(const char* program) {
  std::printf("usage: %s [--suite NAME] [--filter TEXT] [--seed N] [--list]\n"
              "  --suite NAME   run only the tests registered in that suite\n"
              "  --filter TEXT  run only the tests whose name contains TEXT\n"
              "  --seed N       seed for the shared SplitMix64 generator (decimal or 0x)\n"
              "  --list         print every registered test and exit\n",
              program);
}

[[nodiscard]] bool parse_seed(std::string_view text, std::uint64_t& out) {
  if (text.empty()) {
    return false;
  }
  const std::string copy(text);
  char* end = nullptr;
  const bool hex = copy.size() > 2 && copy[0] == '0' && (copy[1] == 'x' || copy[1] == 'X');
  const unsigned long long value = std::strtoull(copy.c_str(), &end, hex ? 16 : 10);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  out = static_cast<std::uint64_t>(value);
  return true;
}

void record_failure(std::string_view file, int line, std::string_view kind,
                    std::string_view expression, std::string_view detail) {
  RunState& s = state();
  ++s.check_failures;
  const std::string at = file.empty() ? std::string("<harness>") : std::string(file);
  const std::string what(kind);
  const std::string text(expression);
  std::printf("%s:%d: %s failed: %s\n", at.c_str(), line, what.c_str(), text.c_str());
  if (!detail.empty()) {
    const std::string note(detail);
    std::printf("  detail: %s\n", note.c_str());
  }
  if (!s.current_name.empty()) {
    std::printf("  reproduce: \"%s\" --suite %s --filter %s --seed %llu\n",
                s.executable.c_str(), s.current_suite.c_str(), s.current_name.c_str(),
                static_cast<unsigned long long>(s.options.seed));
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Rng.
// ---------------------------------------------------------------------------
std::uint64_t Rng::next_u64() noexcept {
  state_ += 0x9E3779B97F4A7C15ull;
  std::uint64_t z = state_;
  z = (z ^ (z >> 30u)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27u)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31u);
}

std::string Rng::token(std::size_t length) {
  static constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789-._:";
  std::string out;
  out.reserve(length);
  for (std::size_t i = 0; i < length; ++i) {
    out.push_back(kAlphabet[below(sizeof(kAlphabet) - 1)]);
  }
  return out;
}

std::string Rng::letters(std::size_t length) {
  static constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz";
  std::string out;
  out.reserve(length);
  for (std::size_t i = 0; i < length; ++i) {
    out.push_back(kAlphabet[below(sizeof(kAlphabet) - 1)]);
  }
  return out;
}

std::vector<std::byte> Rng::bytes(std::size_t length) {
  std::vector<std::byte> out(length);
  for (std::size_t i = 0; i < length; ++i) {
    out[i] = static_cast<std::byte>(below(256));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Registry.
// ---------------------------------------------------------------------------
Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

void Registry::add(std::string_view suite, std::string_view name, TestFn function) {
  tests_.push_back(TestCase{std::string(suite), std::string(name), function});
}

Registrar::Registrar(std::string_view suite, std::string_view name, TestFn function) {
  Registry::instance().add(suite, name, function);
}

// ---------------------------------------------------------------------------
// Descriptions.
// ---------------------------------------------------------------------------
std::string describe_value(std::string_view value) { return "\"" + std::string(value) + "\""; }
std::string describe_value(const std::string& value) { return "\"" + value + "\""; }
std::string describe_value(const char* value) {
  return value == nullptr ? std::string("<null>") : ("\"" + std::string(value) + "\"");
}
std::string describe_value(bool value) { return value ? "true" : "false"; }
std::string describe_value(const Status& status) { return status.message(); }
std::string describe_value(Code code) { return std::string(code_name(code)); }
std::string describe_value(const Digest& digest) {
  return digest.empty() ? std::string("<empty digest>") : ("\"" + digest.hex() + "\"");
}

// ---------------------------------------------------------------------------
// Checks.
// ---------------------------------------------------------------------------
void note_check() noexcept { ++state().checks; }

void fail_check(const char* file, int line, std::string_view kind,
                std::string_view expression, std::string_view detail) {
  record_failure(file == nullptr ? std::string_view{} : std::string_view(file), line, kind,
                 expression, detail);
}

void check_outcome(const char* file, int line, std::string_view expression,
                   const Status& status, bool required) {
  note_check();
  if (status.ok()) {
    return;
  }
  fail_check(file, line, required ? "REQUIRE_OK" : "CHECK_OK", expression, status.message());
  if (required) {
    throw TestAbort{};
  }
}

void check_code(const char* file, int line, std::string_view expression, const Status& status,
                Code expected) {
  note_check();
  const Code actual = status.code();
  const bool good = expected == Code::kOk ? status.ok() : (status.failed() && actual == expected);
  if (good) {
    return;
  }
  std::string detail = "expected ";
  detail.append(code_name(expected));
  detail.append(", got ");
  detail.append(code_name(actual));
  if (status.failed()) {
    detail.append(" [");
    detail.append(status.detail());
    detail.push_back(']');
  }
  fail_check(file, line, "CHECK_CODE", expression, detail);
}

bool is_input_rejection(Code code) noexcept {
  switch (code) {
    case Code::kMalformedInput:
    case Code::kFieldMissing:
    case Code::kFieldTooLong:
    case Code::kFieldInvalidUtf8:
    case Code::kFieldEmpty:
    case Code::kFieldOutOfRange:
    case Code::kFieldConflict:
    case Code::kReservedFieldNonZero:
    case Code::kUnsupportedFormatVersion:
    case Code::kTrailingBytes:
      return true;
    default:
      return false;
  }
}

bool is_storage_failure(Code code) noexcept { return is_storage_code(code); }

// ---------------------------------------------------------------------------
// Options and accessors.
// ---------------------------------------------------------------------------
const Options& options() noexcept { return state().options; }
std::uint64_t seed() noexcept { return state().current_seed; }

Rng& rng() noexcept {
  RunState& s = state();
  if (s.current_rng == nullptr) {
    static Rng fallback(kDefaultSeed);
    return fallback;
  }
  return *s.current_rng;
}

const std::string& executable_path() noexcept { return state().executable; }

void add_child_mode_handler(ChildModeHandler handler) {
  state().child_handlers.push_back(handler);
}

const std::vector<ChildModeHandler>& child_mode_handlers() noexcept {
  return state().child_handlers;
}

// ---------------------------------------------------------------------------
// Runner.
// ---------------------------------------------------------------------------
int run_all(int argc, char** argv) {
  RunState& s = state();
  s.executable = current_executable(argc > 0 && argv[0] != nullptr ? argv[0] : "cxf_tests");

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i] == nullptr ? std::string_view{} : argv[i];
    if (arg == "--suite" && i + 1 < argc) {
      s.options.suite = argv[++i];
    } else if (arg == "--filter" && i + 1 < argc) {
      s.options.filter = argv[++i];
    } else if (arg == "--seed" && i + 1 < argc) {
      if (!parse_seed(argv[++i], s.options.seed)) {
        std::fprintf(stderr, "cxf-tests: --seed expects a decimal or 0x-prefixed integer\n");
        return 2;
      }
    } else if (arg == "--list") {
      s.options.list = true;
    } else if (arg == "--help" || arg == "-h") {
      print_usage(s.executable.c_str());
      return 0;
    } else {
      std::fprintf(stderr, "cxf-tests: unrecognised argument: %s\n", argv[i]);
      print_usage(s.executable.c_str());
      return 2;
    }
  }

  const std::vector<TestCase>& tests = Registry::instance().tests();
  std::vector<const TestCase*> selected;
  for (const TestCase& test : tests) {
    if (!s.options.suite.empty() && test.suite != s.options.suite) {
      continue;
    }
    if (!s.options.filter.empty() && test.name.find(s.options.filter) == std::string::npos) {
      continue;
    }
    selected.push_back(&test);
  }

  if (s.options.list) {
    for (const TestCase* test : selected) {
      std::printf("%s.%s\n", test->suite.c_str(), test->name.c_str());
    }
    std::printf("registered=%zu selected=%zu\n", tests.size(), selected.size());
    return 0;
  }

  std::printf("cxf-tests: registered=%zu selected=%zu seed=%llu (SplitMix64)\n", tests.size(),
              selected.size(), static_cast<unsigned long long>(s.options.seed));
  if (selected.empty()) {
    std::fprintf(stderr, "cxf-tests: no test matched suite=\"%s\" filter=\"%s\"\n",
                 s.options.suite.c_str(), s.options.filter.c_str());
    return 2;
  }

  const int failures_before_run = s.check_failures;
  for (const TestCase* test : selected) {
    s.current_suite = test->suite;
    s.current_name = test->name;
    s.current_seed = test_seed(s.options.seed, test->suite, test->name);
    Rng generator(s.current_seed);
    s.current_rng = &generator;
    const int checks_before = s.checks;
    const int failures_before = s.check_failures;
    ++s.tests_run;
    std::printf("[run ] %s.%s\n", test->suite.c_str(), test->name.c_str());
    std::fflush(stdout);
    try {
      test->function();
    } catch (const TestAbort&) {
      // The failing check has already been recorded.
    } catch (const std::exception& error) {
      record_failure("<harness>", 0, "EXCEPTION", test->name, error.what());
    } catch (...) {
      record_failure("<harness>", 0, "EXCEPTION", test->name, "non-standard exception");
    }
    const int checks_here = s.checks - checks_before;
    const int failures_here = s.check_failures - failures_before;
    if (failures_here > 0) {
      ++s.tests_failed;
      std::printf("[FAIL] %s.%s checks=%d failed=%d\n", test->suite.c_str(), test->name.c_str(),
                  checks_here, failures_here);
    } else {
      std::printf("[ ok ] %s.%s checks=%d\n", test->suite.c_str(), test->name.c_str(), checks_here);
    }
    std::fflush(stdout);
    s.current_rng = nullptr;
  }
  s.current_suite.clear();
  s.current_name.clear();

  std::printf("summary: tests=%d failed=%d checks=%d check_failures=%d\n", s.tests_run,
              s.tests_failed, s.checks, s.check_failures);
  const bool passed = s.tests_failed == 0 && s.check_failures == failures_before_run;
  std::printf("RESULT: %s\n", passed ? "PASS" : "FAIL");
  std::fflush(stdout);
  return passed ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Temporary directories.
// ---------------------------------------------------------------------------
TempDirectory::TempDirectory(std::string_view label) {
  std::error_code ec;
  std::filesystem::path base = std::filesystem::temp_directory_path(ec);
  if (ec || base.empty()) {
    ec.clear();
    base = std::filesystem::current_path(ec);
  }
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t serial = counter.fetch_add(1, std::memory_order_relaxed);
  const std::string name = "cxf-test-" + sanitize_label(label) + "-" + process_id_text() + "-" +
                           std::to_string(serial);
  const std::filesystem::path candidate = base / to_path(name);
  std::filesystem::create_directories(candidate, ec);
  path_ = from_path(candidate);
  ok_ = !ec;
}

TempDirectory::~TempDirectory() { remove_now(); }

std::string TempDirectory::child(std::string_view name) const {
  if (path_.empty()) {
    return std::string(name);
  }
  const char last = path_.back();
  if (last == '/' || last == '\\') {
    return path_ + std::string(name);
  }
  return path_ + "/" + std::string(name);
}

void TempDirectory::remove_now() {
  if (path_.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove_all(to_path(path_), ec);
  // A destructor must never throw: whether the removal succeeded is observable
  // through path_exists(), and every test that cares checks it explicitly.
}

// ---------------------------------------------------------------------------
// File helpers.
// ---------------------------------------------------------------------------
bool path_exists(std::string_view path) {
  std::error_code ec;
  const bool present = std::filesystem::exists(to_path(path), ec);
  return present && !ec;
}

bool is_directory(std::string_view path) {
  std::error_code ec;
  const bool result = std::filesystem::is_directory(to_path(path), ec);
  return result && !ec;
}

bool ensure_directory(std::string_view path) {
  std::error_code ec;
  std::filesystem::create_directories(to_path(path), ec);
  return !ec;
}

bool remove_tree(std::string_view path) {
  std::error_code ec;
  std::filesystem::remove_all(to_path(path), ec);
  return !ec;
}

bool copy_tree(std::string_view from, std::string_view to) {
  std::error_code ec;
  std::filesystem::remove_all(to_path(to), ec);
  ec.clear();
  std::filesystem::copy(to_path(from), to_path(to),
                        std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::overwrite_existing,
                        ec);
  return !ec;
}

FileBytes read_file_bytes(std::string_view path) {
  FileBytes result;
  std::ifstream stream(to_path(path), std::ios::binary);
  if (!stream) {
    return result;
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  if (size < 0) {
    return result;
  }
  stream.seekg(0, std::ios::beg);
  std::vector<char> buffer(static_cast<std::size_t>(size));
  if (!buffer.empty()) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (stream.gcount() != static_cast<std::streamsize>(buffer.size())) {
      return result;
    }
  }
  result.bytes.reserve(buffer.size());
  for (char c : buffer) {
    result.bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
  }
  result.ok = true;
  return result;
}

bool write_file_bytes(std::string_view path, std::span<const std::byte> bytes) {
  const std::filesystem::path target = to_path(path);
  std::error_code ec;
  if (target.has_parent_path()) {
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
      return false;
    }
  }
  std::ofstream stream(target, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  if (!bytes.empty()) {
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  stream.flush();
  return static_cast<bool>(stream);
}

std::string read_file_text(std::string_view path) {
  const FileBytes bytes = read_file_bytes(path);
  if (!bytes.ok) {
    return std::string{};
  }
  return std::string(reinterpret_cast<const char*>(bytes.bytes.data()), bytes.bytes.size());
}

bool write_file_text(std::string_view path, std::string_view text) {
  return write_file_bytes(
      path,
      std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

std::vector<std::string> list_directory_names(std::string_view path) {
  std::vector<std::string> names;
  std::error_code ec;
  std::filesystem::directory_iterator iterator(to_path(path), ec);
  if (ec) {
    return names;
  }
  const std::filesystem::directory_iterator end;
  while (!ec && iterator != end) {
    names.push_back(from_path(iterator->path().filename()));
    iterator.increment(ec);
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::uint64_t file_bytes_size(std::string_view path) {
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(to_path(path), ec);
  if (ec) {
    return 0;
  }
  return static_cast<std::uint64_t>(size);
}

bool truncate_file(std::string_view path, std::uint64_t size) {
  std::error_code ec;
  std::filesystem::resize_file(to_path(path), static_cast<std::uintmax_t>(size), ec);
  return !ec;
}

bool poke_byte(std::string_view path, std::uint64_t offset, std::uint8_t value) {
  FileBytes bytes = read_file_bytes(path);
  if (!bytes.ok || offset >= bytes.bytes.size()) {
    return false;
  }
  bytes.bytes[static_cast<std::size_t>(offset)] = static_cast<std::byte>(value);
  return write_file_bytes(path, std::span<const std::byte>(bytes.bytes));
}

bool flip_bit(std::string_view path, std::uint64_t offset, unsigned bit) {
  FileBytes bytes = read_file_bytes(path);
  if (!bytes.ok || offset >= bytes.bytes.size() || bit > 7) {
    return false;
  }
  const auto index = static_cast<std::size_t>(offset);
  const auto current = static_cast<std::uint8_t>(bytes.bytes[index]);
  bytes.bytes[index] = static_cast<std::byte>(static_cast<std::uint8_t>(current ^ (1u << bit)));
  return write_file_bytes(path, std::span<const std::byte>(bytes.bytes));
}

// ---------------------------------------------------------------------------
// Text helpers.
// ---------------------------------------------------------------------------
bool contains_substring(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    std::string_view line =
        end == std::string_view::npos ? text.substr(start) : text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    lines.emplace_back(line);
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return lines;
}

bool contains_line(std::string_view text, std::string_view line) {
  for (const std::string& candidate : split_lines(text)) {
    if (candidate == line) {
      return true;
    }
  }
  return false;
}

std::string find_line_with_prefix(std::string_view text, std::string_view prefix) {
  for (const std::string& candidate : split_lines(text)) {
    if (candidate.size() >= prefix.size() &&
        std::string_view(candidate).substr(0, prefix.size()) == prefix) {
      return candidate;
    }
  }
  return std::string{};
}

std::string find_key_value(std::string_view text, std::string_view key) {
  const std::string needle = std::string(key) + "=";
  std::size_t at = 0;
  while (at < text.size()) {
    at = text.find(needle, at);
    if (at == std::string_view::npos) {
      return std::string{};
    }
    const bool boundary =
        at == 0 || text[at - 1] == ' ' || text[at - 1] == '\n' || text[at - 1] == '\t';
    if (boundary) {
      const std::size_t start = at + needle.size();
      std::size_t end = start;
      while (end < text.size() && text[end] != ' ' && text[end] != '\n' && text[end] != '\r' &&
             text[end] != '\t') {
        ++end;
      }
      return std::string(text.substr(start, end - start));
    }
    at += needle.size();
  }
  return std::string{};
}

}  // namespace cxf::test

// ---------------------------------------------------------------------------
// main: child-mode handlers first, then the harness. Every test executable is
// driven by this function.
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  for (const cxf::test::ChildModeHandler handler : cxf::test::child_mode_handlers()) {
    const int result = handler(argc, argv);
    if (result >= 0) {
      return result;
    }
  }
  return cxf::test::run_all(argc, argv);
}
