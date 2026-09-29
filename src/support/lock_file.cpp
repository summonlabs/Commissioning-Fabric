#include "cxf/support/lock_file.hpp"

#include <string>

#include "cxf/support/clock.hpp"
#include "cxf/support/text.hpp"
#include "fs_internal.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace cxf {
namespace {

[[nodiscard]] std::string holder_line(std::string_view holder, Timestamp at) {
  std::string out;
  out.append("holder=");
  out.append(holder.empty() ? std::string_view("unnamed") : holder);
#if defined(_WIN32)
  out.append(" pid=");
  out.append(std::to_string(static_cast<unsigned long>(GetCurrentProcessId())));
#else
  out.append(" pid=");
  out.append(std::to_string(static_cast<long>(::getpid())));
#endif
  out.append(" acquired=");
  out.append(at.to_rfc3339());
  out.push_back('\n');
  return out;
}

}  // namespace

LockFile::LockFile(LockFile&& other) noexcept
    : path_(std::move(other.path_)), handle_(other.handle_), held_(other.held_) {
  other.handle_ = -1;
  other.held_ = false;
  other.path_.clear();
}

LockFile& LockFile::operator=(LockFile&& other) noexcept {
  if (this != &other) {
    static_cast<void>(release());
    path_ = std::move(other.path_);
    handle_ = other.handle_;
    held_ = other.held_;
    other.handle_ = -1;
    other.held_ = false;
    other.path_.clear();
  }
  return *this;
}

LockFile::~LockFile() { static_cast<void>(release()); }

Status LockFile::release() {
  if (!held_) {
    return Status::success();
  }
  held_ = false;
  const std::intptr_t handle = handle_;
  handle_ = -1;
#if defined(_WIN32)
  if (CloseHandle(reinterpret_cast<HANDLE>(handle)) == 0) {
    return make_error(Code::kStorageIo, "closing the lock handle failed",
                      "win32=" + std::to_string(GetLastError()));
  }
#else
  if (::flock(static_cast<int>(handle), LOCK_UN) != 0) {
    static_cast<void>(::close(static_cast<int>(handle)));
    return make_error(Code::kStorageIo, "unlocking the lock file failed",
                      "errno=" + std::to_string(errno));
  }
  if (::close(static_cast<int>(handle)) != 0) {
    return make_error(Code::kStorageIo, "closing the lock file failed",
                      "errno=" + std::to_string(errno));
  }
#endif
  return Status::success();
}

Outcome<std::string> LockFile::holder_info() const {
  if (!held_) {
    return make_error(Code::kPreconditionViolated, "lock is not held by this object");
  }
  return peek_holder(path_);
}

Outcome<LockFile> LockFile::acquire(const fs::Path& path, std::string_view holder) {
  const Status valid = fs::validate_path(path);
  if (valid.failed()) {
    return valid;
  }
  const std::string line = holder_line(holder, SystemClock().now());

#if defined(_WIN32)
  Outcome<std::wstring> native = fs::internal::to_native(path);
  if (!native.ok()) {
    return native.status();
  }
  // Sharing is limited to reads so that a second writer is refused by the
  // operating system itself rather than by an advisory convention.
  HANDLE handle = CreateFileW(native.value().c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const unsigned long code = GetLastError();
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
      return make_error(Code::kStorageLocked, "another process holds the store lock", path);
    }
    if (code == ERROR_ACCESS_DENIED) {
      return make_error(Code::kStorageIo, "access denied opening the store lock", path);
    }
    return make_error(Code::kStorageIo, "opening the store lock failed",
                      path + " win32=" + std::to_string(code));
  }

  // Publish the holder text; failure to write it does not release the lock, so
  // the caller still owns exclusion, but the error is reported because a
  // partially written holder line would be misleading.
  LARGE_INTEGER zero{};
  if (SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN) == 0) {
    const unsigned long code = GetLastError();
    CloseHandle(handle);
    return make_error(Code::kStorageIo, "seeking in the store lock failed",
                      path + " win32=" + std::to_string(code));
  }
  if (SetEndOfFile(handle) == 0) {
    const unsigned long code = GetLastError();
    CloseHandle(handle);
    return make_error(Code::kStorageIo, "truncating the store lock failed",
                      path + " win32=" + std::to_string(code));
  }
  DWORD written = 0;
  if (WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) == 0 ||
      written != line.size()) {
    const unsigned long code = GetLastError();
    CloseHandle(handle);
    return make_error(Code::kStorageIo, "writing the store lock failed",
                      path + " win32=" + std::to_string(code));
  }
  if (FlushFileBuffers(handle) == 0) {
    const unsigned long code = GetLastError();
    CloseHandle(handle);
    return make_error(Code::kStorageIo, "flushing the store lock failed",
                      path + " win32=" + std::to_string(code));
  }
  LockFile lock;
  lock.path_ = path;
  lock.handle_ = reinterpret_cast<std::intptr_t>(handle);
  lock.held_ = true;
  return lock;
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    return make_error(Code::kStorageUnavailable, "opening the store lock failed", path);
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    static_cast<void>(::close(fd));
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      return make_error(Code::kStorageLocked, "another process holds the store lock", path);
    }
    return make_error(Code::kStorageIo, "locking the store lock failed",
                      path + " errno=" + std::to_string(errno));
  }
  if (::ftruncate(fd, 0) != 0 || ::pwrite(fd, line.data(), line.size(), 0) !=
                                    static_cast<ssize_t>(line.size()) ||
      ::fsync(fd) != 0) {
    static_cast<void>(::flock(fd, LOCK_UN));
    static_cast<void>(::close(fd));
    return make_error(Code::kStorageIo, "writing the store lock failed", path);
  }
  LockFile lock;
  lock.path_ = path;
  lock.handle_ = static_cast<std::intptr_t>(fd);
  lock.held_ = true;
  return lock;
#endif
}

Outcome<std::string> LockFile::peek_holder(const fs::Path& path) {
  Outcome<std::vector<std::byte>> bytes = fs::read_file(path, 4096);
  if (!bytes.ok()) {
    return bytes.status();
  }
  return std::string(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
}

}  // namespace cxf
