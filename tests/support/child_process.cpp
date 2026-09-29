#include "child_process.hpp"

#include <cstring>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace cxf::test {
namespace {

#if defined(_WIN32)

[[nodiscard]] std::wstring utf8_to_wide(std::string_view text) {
  if (text.empty()) {
    return std::wstring{};
  }
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) {
    return std::wstring{};
  }
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  const int written = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), out.data(), needed);
  if (written != needed) {
    return std::wstring{};
  }
  return out;
}

/// Quote one argument for the CreateProcessW command line: the documented
/// backslash-doubling rule, so the child's CRT parses exactly these bytes.
[[nodiscard]] std::wstring quote_argument(std::wstring_view arg) {
  const bool needs_quotes =
      arg.empty() || arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
  if (!needs_quotes) {
    return std::wstring(arg);
  }
  std::wstring out;
  out.push_back(L'"');
  std::size_t backslashes = 0;
  for (const wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
      continue;
    }
    out.append(backslashes, L'\\');
    backslashes = 0;
    out.push_back(c);
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

[[nodiscard]] std::wstring environment_name(std::wstring_view entry) {
  const std::size_t at = entry.find(L'=');
  return at == std::wstring_view::npos ? std::wstring(entry)
                                       : std::wstring(entry.substr(0, at));
}

/// Build a sorted UTF-16 environment block. An empty block is a valid
/// environment with no variables.
[[nodiscard]] std::vector<wchar_t> build_environment_block(const ChildOptions& options) {
  std::vector<std::pair<std::wstring, std::wstring>> entries;
  if (options.inherit_environment) {
    LPWCH block = GetEnvironmentStringsW();
    if (block != nullptr) {
      const wchar_t* cursor = block;
      while (*cursor != L'\0') {
        const std::wstring entry(cursor);
        const std::size_t at = entry.find(L'=');
        if (at != std::wstring::npos && at > 0) {
          entries.emplace_back(entry.substr(0, at), entry.substr(at + 1));
        }
        cursor += entry.size() + 1;
      }
      FreeEnvironmentStringsW(block);
    }
  }
  for (const std::pair<std::string, std::string>& pair : options.environment) {
    const std::wstring name = utf8_to_wide(pair.first);
    const std::wstring value = utf8_to_wide(pair.second);
    bool replaced = false;
    for (std::pair<std::wstring, std::wstring>& entry : entries) {
      if (entry.first.size() == name.size() && _wcsicmp(entry.first.c_str(), name.c_str()) == 0) {
        entry.second = value;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      entries.emplace_back(name, value);
    }
  }
  for (std::size_t i = 1; i < entries.size(); ++i) {
    std::size_t j = i;
    while (j > 0 && _wcsicmp(entries[j - 1].first.c_str(), entries[j].first.c_str()) > 0) {
      std::swap(entries[j - 1], entries[j]);
      --j;
    }
  }
  std::vector<wchar_t> block;
  for (const std::pair<std::wstring, std::wstring>& entry : entries) {
    block.insert(block.end(), entry.first.begin(), entry.first.end());
    block.push_back(L'=');
    block.insert(block.end(), entry.second.begin(), entry.second.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

void drain_handle(std::intptr_t raw, const std::shared_ptr<std::string>& sink) {
  const HANDLE handle = reinterpret_cast<HANDLE>(raw);
  char buffer[8192];
  for (;;) {
    DWORD read = 0;
    if (ReadFile(handle, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr) == 0 ||
        read == 0) {
      break;
    }
    sink->append(buffer, static_cast<std::size_t>(read));
  }
  CloseHandle(handle);
}

#else  // !_WIN32

void drain_fd(int fd, const std::shared_ptr<std::string>& sink) {
  char buffer[8192];
  for (;;) {
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got <= 0) {
      break;
    }
    sink->append(buffer, static_cast<std::size_t>(got));
  }
  ::close(fd);
}

#endif

}  // namespace

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
#if defined(_WIN32)
    : process_(other.process_),
#else
    : pid_(other.pid_),
#endif
      pid_(other.pid_),
      exit_code_(other.exit_code_),
      exited_(other.exited_),
      reaped_(other.reaped_),
      wait_status_(other.wait_status_),
      out_(std::move(other.out_)),
      err_(std::move(other.err_)),
      out_reader_(std::move(other.out_reader_)),
      err_reader_(std::move(other.err_reader_)) {
#if defined(_WIN32)
  other.process_ = -1;
#else
  other.pid_ = -1;
#endif
  other.pid_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    if (running()) {
      static_cast<void>(terminate());
    }
    static_cast<void>(wait());
    close_process_handle();
#if defined(_WIN32)
    process_ = other.process_;
    other.process_ = -1;
#else
    pid_ = other.pid_;
    other.pid_ = -1;
#endif
    pid_ = other.pid_;
    exit_code_ = other.exit_code_;
    exited_ = other.exited_;
    reaped_ = other.reaped_;
    wait_status_ = other.wait_status_;
    out_ = std::move(other.out_);
    err_ = std::move(other.err_);
    out_reader_ = std::move(other.out_reader_);
    err_reader_ = std::move(other.err_reader_);
    other.pid_ = 0;
  }
  return *this;
}

ChildProcess::~ChildProcess() {
  if (running()) {
    static_cast<void>(terminate());
  }
  static_cast<void>(wait());
  close_process_handle();
}

bool ChildProcess::started() const noexcept {
#if defined(_WIN32)
  return process_ != -1;
#else
  return pid_ > 0;
#endif
}

const std::string& ChildProcess::stdout_text() const noexcept {
  static const std::string empty;
  return out_ == nullptr ? empty : *out_;
}

const std::string& ChildProcess::stderr_text() const noexcept {
  static const std::string empty;
  return err_ == nullptr ? empty : *err_;
}

void ChildProcess::join_readers() {
  if (out_reader_.joinable()) {
    out_reader_.join();
  }
  if (err_reader_.joinable()) {
    err_reader_.join();
  }
}

void ChildProcess::close_process_handle() {
#if defined(_WIN32)
  if (process_ != -1) {
    CloseHandle(reinterpret_cast<HANDLE>(process_));
    process_ = -1;
  }
#endif
}

Outcome<ChildProcess> ChildProcess::start(const ChildOptions& options) {
  if (options.program.empty()) {
    return make_error(Code::kPreconditionViolated, "child program path is empty");
  }

  ChildProcess child;
  child.out_ = std::make_shared<std::string>();
  child.err_ = std::make_shared<std::string>();

#if defined(_WIN32)
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  attributes.lpSecurityDescriptor = nullptr;

  HANDLE out_read = nullptr;
  HANDLE out_write = nullptr;
  HANDLE err_read = nullptr;
  HANDLE err_write = nullptr;
  HANDLE input = INVALID_HANDLE_VALUE;

  const auto close_if_valid = [](HANDLE handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  };

  if (options.capture_stdout) {
    if (CreatePipe(&out_read, &out_write, &attributes, 0) == 0) {
      return make_error(Code::kInternalError, "anonymous pipe creation failed",
                        "win32=" + std::to_string(GetLastError()));
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
  }
  if (options.capture_stderr) {
    if (CreatePipe(&err_read, &err_write, &attributes, 0) == 0) {
      close_if_valid(out_read);
      close_if_valid(out_write);
      return make_error(Code::kInternalError, "anonymous pipe creation failed",
                        "win32=" + std::to_string(GetLastError()));
    }
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
  }
  input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

  std::wstring command_line = quote_argument(utf8_to_wide(options.program));
  for (const std::string& argument : options.arguments) {
    command_line.push_back(L' ');
    command_line.append(quote_argument(utf8_to_wide(argument)));
  }

  const std::wstring directory = utf8_to_wide(options.working_directory);
  std::vector<wchar_t> environment = build_environment_block(options);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = input == INVALID_HANDLE_VALUE ? GetStdHandle(STD_INPUT_HANDLE) : input;
  startup.hStdOutput = options.capture_stdout ? out_write : GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = options.capture_stderr ? err_write : GetStdHandle(STD_ERROR_HANDLE);

  PROCESS_INFORMATION process_info{};
  const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
  const BOOL created =
      CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, flags,
                     environment.data(), directory.empty() ? nullptr : directory.c_str(),
                     &startup, &process_info);
  const DWORD error = created != 0 ? 0 : GetLastError();

  close_if_valid(out_write);
  close_if_valid(err_write);
  close_if_valid(input);

  if (created == 0) {
    close_if_valid(out_read);
    close_if_valid(err_read);
    return make_error(Code::kInternalError, "child process could not be started",
                      "program=" + options.program + " win32=" + std::to_string(error));
  }

  CloseHandle(process_info.hThread);
  child.process_ = reinterpret_cast<std::intptr_t>(process_info.hProcess);
  child.pid_ = static_cast<std::uint64_t>(process_info.dwProcessId);
  if (options.capture_stdout) {
    const std::intptr_t handle = reinterpret_cast<std::intptr_t>(out_read);
    const std::shared_ptr<std::string> sink = child.out_;
    child.out_reader_ = std::thread([handle, sink]() { drain_handle(handle, sink); });
  } else {
    close_if_valid(out_read);
  }
  if (options.capture_stderr) {
    const std::intptr_t handle = reinterpret_cast<std::intptr_t>(err_read);
    const std::shared_ptr<std::string> sink = child.err_;
    child.err_reader_ = std::thread([handle, sink]() { drain_handle(handle, sink); });
  } else {
    close_if_valid(err_read);
  }
  return Outcome<ChildProcess>(std::move(child));
#else
  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (options.capture_stdout && ::pipe(out_pipe) != 0) {
    return make_error(Code::kInternalError, "anonymous pipe creation failed");
  }
  if (options.capture_stderr && ::pipe(err_pipe) != 0) {
    if (out_pipe[0] >= 0) {
      ::close(out_pipe[0]);
      ::close(out_pipe[1]);
    }
    return make_error(Code::kInternalError, "anonymous pipe creation failed");
  }

  std::vector<std::string> arguments;
  arguments.push_back(options.program);
  for (const std::string& argument : options.arguments) {
    arguments.push_back(argument);
  }
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);

  std::vector<std::string> environment;
  if (options.inherit_environment) {
    for (char** entry = ::environ; entry != nullptr && *entry != nullptr; ++entry) {
      environment.emplace_back(*entry);
    }
  }
  for (const std::pair<std::string, std::string>& pair : options.environment) {
    const std::string prefix = pair.first + "=";
    bool replaced = false;
    for (std::string& entry : environment) {
      if (entry.size() >= prefix.size() && entry.compare(0, prefix.size(), prefix) == 0) {
        entry = prefix + pair.second;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      environment.push_back(prefix + pair.second);
    }
  }
  std::vector<char*> envp;
  envp.reserve(environment.size() + 1);
  for (std::string& entry : environment) {
    envp.push_back(entry.data());
  }
  envp.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    if (out_pipe[0] >= 0) {
      ::close(out_pipe[0]);
      ::close(out_pipe[1]);
    }
    if (err_pipe[0] >= 0) {
      ::close(err_pipe[0]);
      ::close(err_pipe[1]);
    }
    return make_error(Code::kInternalError, "fork failed");
  }
  if (pid == 0) {
    if (options.capture_stdout) {
      ::dup2(out_pipe[1], STDOUT_FILENO);
      ::close(out_pipe[0]);
      ::close(out_pipe[1]);
    }
    if (options.capture_stderr) {
      ::dup2(err_pipe[1], STDERR_FILENO);
      ::close(err_pipe[0]);
      ::close(err_pipe[1]);
    }
    const int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      ::close(devnull);
    }
    if (!options.working_directory.empty()) {
      if (::chdir(options.working_directory.c_str()) != 0) {
        ::_exit(126);
      }
    }
    ::execve(options.program.c_str(), argv.data(), envp.data());
    ::_exit(127);
  }

  if (out_pipe[0] >= 0) {
    ::close(out_pipe[1]);
  }
  if (err_pipe[0] >= 0) {
    ::close(err_pipe[1]);
  }
  child.pid_ = pid;
  child.pid_ = static_cast<std::uint64_t>(pid);
  if (options.capture_stdout) {
    const int fd = out_pipe[0];
    const std::shared_ptr<std::string> sink = child.out_;
    child.out_reader_ = std::thread([fd, sink]() { drain_fd(fd, sink); });
  }
  if (options.capture_stderr) {
    const int fd = err_pipe[0];
    const std::shared_ptr<std::string> sink = child.err_;
    child.err_reader_ = std::thread([fd, sink]() { drain_fd(fd, sink); });
  }
  return Outcome<ChildProcess>(std::move(child));
#endif
}

bool ChildProcess::running() const {
  if (!started() || exited_) {
    return false;
  }
#if defined(_WIN32)
  return WaitForSingleObject(reinterpret_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
#else
  if (reaped_) {
    return false;
  }
  int status = 0;
  const pid_t result = ::waitpid(pid_, &status, WNOHANG);
  if (result == pid_) {
    const_cast<ChildProcess*>(this)->reaped_ = true;
    const_cast<ChildProcess*>(this)->wait_status_ = status;
    return false;
  }
  return result == 0;
#endif
}

Outcome<int> ChildProcess::wait() {
  if (!started()) {
    return make_error(Code::kPreconditionViolated, "child was never started");
  }
  if (exited_) {
    return exit_code_;
  }
#if defined(_WIN32)
  const DWORD waited = WaitForSingleObject(reinterpret_cast<HANDLE>(process_), INFINITE);
  if (waited != WAIT_OBJECT_0) {
    return make_error(Code::kInternalError, "waiting for the child failed",
                      "win32=" + std::to_string(GetLastError()));
  }
  DWORD code = 0;
  if (GetExitCodeProcess(reinterpret_cast<HANDLE>(process_), &code) == 0) {
    return make_error(Code::kInternalError, "reading the child exit code failed",
                      "win32=" + std::to_string(GetLastError()));
  }
  exit_code_ = static_cast<int>(code);
#else
  int status = wait_status_;
  if (!reaped_) {
    if (::waitpid(pid_, &status, 0) != pid_) {
      return make_error(Code::kInternalError, "waiting for the child failed");
    }
    wait_status_ = status;
    reaped_ = true;
  }
  if (WIFEXITED(status)) {
    exit_code_ = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    exit_code_ = 128 + WTERMSIG(status);
  } else {
    exit_code_ = -1;
  }
#endif
  exited_ = true;
  join_readers();
  return exit_code_;
}

Status ChildProcess::terminate() {
  if (!started()) {
    return make_error(Code::kPreconditionViolated, "child was never started");
  }
  if (exited_) {
    return Status::success();
  }
#if defined(_WIN32)
  if (TerminateProcess(reinterpret_cast<HANDLE>(process_), 137) == 0) {
    const DWORD code = GetLastError();
    if (code != ERROR_ACCESS_DENIED) {
      return make_error(Code::kInternalError, "terminating the child failed",
                        "win32=" + std::to_string(code));
    }
  }
#else
  if (::kill(pid_, SIGKILL) != 0) {
    return make_error(Code::kInternalError, "terminating the child failed");
  }
#endif
  const Outcome<int> code = wait();
  if (!code.ok()) {
    return code.status();
  }
  return Status::success();
}

Outcome<ChildRun> run_child(const ChildOptions& options) {
  Outcome<ChildProcess> child = ChildProcess::start(options);
  if (!child.ok()) {
    return child.status();
  }
  const Outcome<int> code = child->wait();
  if (!code.ok()) {
    return code.status();
  }
  ChildRun run;
  run.exit_code = code.value();
  run.out = child->stdout_text();
  run.err = child->stderr_text();
  return run;
}

}  // namespace cxf::test
