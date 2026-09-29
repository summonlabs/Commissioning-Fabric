// Commissioning Fabric - OS-level single-writer exclusion.
//
// The durable store claims cross-process exclusion, so the exclusion is a real
// operating-system lock rather than a marker file: on Windows the handle is
// opened without sharing, on POSIX it is held with flock(LOCK_EX | LOCK_NB).
// Either way the lock disappears when the holding process dies, including when
// it is killed without unwinding, which is what makes stale-lock recovery
// unnecessary. The file body carries informational holder text only; it is
// never consulted to decide whether the lock is free.
#ifndef CXF_SUPPORT_LOCK_FILE_HPP
#define CXF_SUPPORT_LOCK_FILE_HPP

#include <string>
#include <string_view>

#include "cxf/support/fs.hpp"
#include "cxf/support/status.hpp"

namespace cxf {

class LockFile {
 public:
  LockFile() noexcept = default;
  LockFile(const LockFile&) = delete;
  LockFile& operator=(const LockFile&) = delete;
  LockFile(LockFile&& other) noexcept;
  LockFile& operator=(LockFile&& other) noexcept;
  ~LockFile();

  /// Take the exclusive lock, creating the file when absent. Contention is
  /// reported as Code::kStorageLocked, never as success.
  [[nodiscard]] static Outcome<LockFile> acquire(const fs::Path& path,
                                                 std::string_view holder);

  [[nodiscard]] bool held() const noexcept { return held_; }
  [[nodiscard]] const fs::Path& path() const noexcept { return path_; }

  /// Release the lock. Returns only after the operating system has been told,
  /// so a subsequent acquire in this or another process can succeed.
  Status release();

  /// Informational holder text of the lock this object owns.
  [[nodiscard]] Outcome<std::string> holder_info() const;

  /// Informational holder text of the lock file, readable while it is locked.
  [[nodiscard]] static Outcome<std::string> peek_holder(const fs::Path& path);

 private:
  fs::Path path_{};
  std::intptr_t handle_{-1};
  bool held_{false};
};

}  // namespace cxf

#endif  // CXF_SUPPORT_LOCK_FILE_HPP
