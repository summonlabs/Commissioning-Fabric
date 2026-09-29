// Commissioning Fabric - real child processes for the proof obligations.
//
// The lock, crash-consistency and CLI tests must observe another operating
// system process, so this helper spawns one for real: CreateProcessW on
// Windows, fork/exec on POSIX. There is no shell anywhere in the path, no
// timeout parameter and no watchdog. Waiting for a child to exit is unbounded
// by design: a test that hangs is a defect to diagnose, not one to cut short.
//
// stdout and stderr are captured through anonymous pipes drained by two reader
// threads, so a child that writes a lot to either stream cannot deadlock
// against a parent that is reading the other.
#ifndef CXF_TESTS_SUPPORT_CHILD_PROCESS_HPP
#define CXF_TESTS_SUPPORT_CHILD_PROCESS_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cxf/support/status.hpp"

namespace cxf::test {

/// How to start a child. Program and paths are UTF-8; arguments are passed
/// verbatim (no shell, no globbing, no variable expansion).
struct ChildOptions {
  std::string program{};
  std::vector<std::string> arguments{};
  std::string working_directory{};
  /// Name/value pairs added to (or replacing entries of) the environment.
  std::vector<std::pair<std::string, std::string>> environment{};
  bool inherit_environment{true};
  bool capture_stdout{true};
  bool capture_stderr{true};
};

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;

  /// Start the child. A failure to start is a Status error, never a silent
  /// no-op.
  [[nodiscard]] static Outcome<ChildProcess> start(const ChildOptions& options);

  [[nodiscard]] bool started() const noexcept;
  /// True once wait() has reaped the child.
  [[nodiscard]] bool exited() const noexcept { return exited_; }
  /// Non-blocking: is the child still running?
  [[nodiscard]] bool running() const;
  [[nodiscard]] std::uint64_t pid() const noexcept { return pid_; }

  /// Wait indefinitely for the child and return its exit code. A child killed
  /// by a signal is reported as 128 + signal number.
  [[nodiscard]] Outcome<int> wait();

  /// Terminate the child without letting it unwind (TerminateProcess or
  /// SIGKILL). Used by the lock-release and crash tests.
  [[nodiscard]] Status terminate();

  [[nodiscard]] int exit_code() const noexcept { return exit_code_; }
  /// Captured output. Only meaningful after wait().
  [[nodiscard]] const std::string& stdout_text() const noexcept;
  [[nodiscard]] const std::string& stderr_text() const noexcept;

 private:
  void join_readers();
  void close_process_handle();

#if defined(_WIN32)
  std::intptr_t process_{ -1 };
#else
  int pid_{-1};
#endif
  std::uint64_t pid_{0};
  int exit_code_{-1};
  bool exited_{false};
  /// POSIX only: the status reaped by the non-blocking running() check.
  bool reaped_{false};
  int wait_status_{0};
  std::shared_ptr<std::string> out_{};
  std::shared_ptr<std::string> err_{};
  std::thread out_reader_{};
  std::thread err_reader_{};
};

struct ChildRun {
  int exit_code{0};
  std::string out{};
  std::string err{};
};

/// Start, wait and collect the two streams in one call.
[[nodiscard]] Outcome<ChildRun> run_child(const ChildOptions& options);

}  // namespace cxf::test

#endif  // CXF_TESTS_SUPPORT_CHILD_PROCESS_HPP
