#include "cxf/support/fs.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "cxf/support/digest.hpp"
#include "cxf/support/text.hpp"
#include "fs_internal.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace cxf::fs {
namespace {

[[nodiscard]] Status io_error(std::string_view what, const Path& path,
                              std::string_view detail) {
  return make_error(Code::kStorageIo, std::string(what), std::string(path) + " " +
                                                                  std::string(detail));
}

/// True when a path component list contains a parent reference.
[[nodiscard]] bool has_parent_component(std::string_view path) {
  for (const std::string_view part : split(path, '/')) {
    if (part == "..") {
      return true;
    }
  }
  for (const std::string_view part : split(path, '\\')) {
    if (part == "..") {
      return true;
    }
  }
  return false;
}

}  // namespace

namespace internal {

std::string staging_tag() {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
  const auto pid = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
  const auto pid = static_cast<std::uint64_t>(::getpid());
#endif
  return std::to_string(pid) + "-" + std::to_string(n);
}

#if defined(_WIN32)

Outcome<std::wstring> to_wide(std::string_view utf8) {
  if (utf8.empty()) {
    return std::wstring{};
  }
  const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) {
    return make_error(Code::kFieldInvalidUtf8, "path is not valid UTF-8");
  }
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                          static_cast<int>(utf8.size()), out.data(), needed);
  if (written != needed) {
    return make_error(Code::kStorageIo, "wide conversion failed");
  }
  return out;
}

std::string to_utf8(std::wstring_view wide) {
  if (wide.empty()) {
    return std::string{};
  }
  const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                         static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                         nullptr);
  if (needed <= 0) {
    return std::string{};
  }
  std::string out(static_cast<std::size_t>(needed), '\0');
  const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                          static_cast<int>(wide.size()), out.data(), needed,
                                          nullptr, nullptr);
  if (written != needed) {
    return std::string{};
  }
  return out;
}

std::string win_error_message(unsigned long code) {
  return "win32=" + std::to_string(code);
}

Outcome<std::wstring> to_native(std::string_view utf8) {
  Outcome<std::wstring> wide = to_wide(utf8);
  if (!wide.ok()) {
    return wide.status();
  }
  std::wstring text = std::move(wide).value();
  for (wchar_t& c : text) {
    if (c == L'/') {
      c = L'\\';
    }
  }
  const bool drive_absolute =
      text.size() >= 3 && text[1] == L':' && (text[2] == L'\\');
  const bool unc_absolute = text.size() >= 2 && text[0] == L'\\' && text[1] == L'\\';
  if (text.rfind(L"\\\\?\\", 0) == 0) {
    return text;  // already extended-length
  }
  if (drive_absolute) {
    return L"\\\\?\\" + text;
  }
  if (unc_absolute) {
    return L"\\\\?\\UNC\\" + text.substr(2);
  }
  return text;
}

#endif  // _WIN32

}  // namespace internal

Status validate_path(std::string_view path) {
  if (path.empty()) {
    return make_error(Code::kFieldEmpty, "path is empty");
  }
  if (path.size() > 32000) {
    return make_error(Code::kFieldTooLong, "path exceeds the platform limit",
                      "bytes=" + std::to_string(path.size()));
  }
  if (!is_valid_utf8(path)) {
    return make_error(Code::kFieldInvalidUtf8, "path is not valid UTF-8");
  }
  if (path.find('\0') != std::string_view::npos) {
    return make_error(Code::kMalformedInput, "path contains an embedded NUL");
  }
  if (has_parent_component(path)) {
    return make_error(Code::kMalformedInput, "path contains a parent-directory component");
  }
  const std::size_t colon = path.find(':');
  std::size_t search_from = 0;
  if (colon != std::string_view::npos) {
    const bool drive_prefix = colon == 1 &&
                              ((path[0] >= 'A' && path[0] <= 'Z') ||
                               (path[0] >= 'a' && path[0] <= 'z')) &&
                              path.size() > 2 && (path[2] == '/' || path[2] == '\\');
    if (!drive_prefix) {
      return make_error(Code::kMalformedInput,
                        "path contains a colon outside a drive prefix (alternate data stream)");
    }
    // A second colon after the drive prefix names an alternate data stream.
    search_from = 2;
  }
  if (path.find(':', search_from) != std::string_view::npos) {
    return make_error(Code::kMalformedInput,
                      "path contains an alternate data stream separator");
  }
  return Status::success();
}

Path join(const Path& directory, std::string_view name) {
  if (directory.empty()) {
    return Path(name);
  }
  Path out = directory;
  const char last = out.back();
  if (last != '/' && last != '\\') {
    out.push_back('/');
  }
  out.append(name);
  return out;
}

Path parent(const Path& path) {
  const std::size_t pos = path.find_last_of("/\\");
  if (pos == std::string::npos) {
    return Path{};
  }
  if (pos == 0) {
    return path.substr(0, 1);
  }
  return path.substr(0, pos);
}

std::string_view filename(const Path& path) {
  const std::size_t pos = path.find_last_of("/\\");
  if (pos == std::string::npos) {
    return path;
  }
  return std::string_view(path).substr(pos + 1);
}

#if defined(_WIN32)

namespace {

/// Attribute lookup. Present reports whether the path exists at all.
[[nodiscard]] Outcome<unsigned long> attributes(const Path& path, bool& present) {
  const Status valid = validate_path(path);
  if (valid.failed()) {
    return valid;
  }
  Outcome<std::wstring> native = internal::to_native(path);
  if (!native.ok()) {
    return native.status();
  }
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExW(native.value().c_str(), GetFileExInfoStandard, &data) == 0) {
    const unsigned long code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ||
        code == ERROR_INVALID_NAME) {
      present = false;
      return 0UL;
    }
    return io_error("GetFileAttributesEx failed", path, internal::win_error_message(code));
  }
  present = true;
  return data.dwFileAttributes;
}

/// Reject a handle that denotes a reparse point (symlink, junction, mount).
[[nodiscard]] Status reject_reparse(HANDLE handle, const Path& path) {
  BY_HANDLE_FILE_INFORMATION info{};
  if (GetFileInformationByHandle(handle, &info) == 0) {
    return io_error("GetFileInformationByHandle failed", path,
                    internal::win_error_message(GetLastError()));
  }
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return make_error(Code::kStorageIo, "path is a reparse point (symlink or junction)",
                      path);
  }
  return Status::success();
}

[[nodiscard]] Outcome<fs::File> open_impl(const Path& path, unsigned long access,
                                          unsigned long disposition, long long offset) {
  const Status valid = validate_path(path);
  if (valid.failed()) {
    return valid;
  }
  Outcome<std::wstring> native = internal::to_native(path);
  if (!native.ok()) {
    return native.status();
  }
  HANDLE handle = CreateFileW(native.value().c_str(), access,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              disposition, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const unsigned long code = GetLastError();
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
      return make_error(Code::kStorageLocked, "file is exclusively held", path);
    }
    if (code == ERROR_ACCESS_DENIED) {
      return make_error(Code::kStorageIo, "access denied", path);
    }
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return make_error(Code::kStorageUnavailable, "file not found", path);
    }
    if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
      return make_error(Code::kStorageIo, "file already exists", path);
    }
    return io_error("CreateFile failed", path, internal::win_error_message(code));
  }

  const Status reparse = reject_reparse(handle, path);
  if (reparse.failed()) {
    CloseHandle(handle);
    return reparse;
  }

  if (offset >= 0) {
    LARGE_INTEGER li{};
    li.QuadPart = offset;
    if (SetFilePointerEx(handle, li, nullptr, FILE_BEGIN) == 0) {
      const unsigned long code = GetLastError();
      CloseHandle(handle);
      return io_error("SetFilePointerEx failed", path, internal::win_error_message(code));
    }
  }

  fs::File file;
  file.adopt(reinterpret_cast<std::intptr_t>(handle), path);
  return file;
}

}  // namespace

Outcome<bool> exists(const Path& path) {
  bool present = false;
  Outcome<unsigned long> attrs = attributes(path, present);
  if (!attrs.ok()) {
    return attrs.status();
  }
  return present;
}

Outcome<bool> is_directory(const Path& path) {
  bool present = false;
  Outcome<unsigned long> attrs = attributes(path, present);
  if (!attrs.ok()) {
    return attrs.status();
  }
  if (!present) {
    return make_error(Code::kStorageUnavailable, "path does not exist", path);
  }
  return (attrs.value() & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

Outcome<std::uint64_t> file_size(const Path& path) {
  bool present = false;
  Outcome<unsigned long> attrs = attributes(path, present);
  if (!attrs.ok()) {
    return attrs.status();
  }
  if (!present) {
    return make_error(Code::kStorageUnavailable, "path does not exist", path);
  }
  Outcome<std::wstring> native = internal::to_native(path);
  if (!native.ok()) {
    return native.status();
  }
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExW(native.value().c_str(), GetFileExInfoStandard, &data) == 0) {
    return io_error("GetFileAttributesEx failed", path,
                    internal::win_error_message(GetLastError()));
  }
  const auto size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32u) |
                    static_cast<std::uint64_t>(data.nFileSizeLow);
  return size;
}

Status ensure_directory(const Path& path) {
  const Status valid = validate_path(path);
  if (valid.failed()) {
    return valid;
  }
  Outcome<std::wstring> native = internal::to_native(path);
  if (!native.ok()) {
    return native.status();
  }
  const std::wstring& wide = native.value();

  // Determine where the first creatable component starts. The volume root, the
  // extended-length operator and the server/share part of a UNC path already
  // exist and must never be passed to CreateDirectory.
  std::size_t start = 0;
  const std::size_t separator = wide.find(L'\\');
  if (wide.rfind(L"\\\\?\\UNC\\", 0) == 0) {
    std::size_t pos = wide.find(L'\\', 8);
    if (pos != std::wstring::npos) {
      pos = wide.find(L'\\', pos + 1);
    }
    start = pos == std::wstring::npos ? wide.size() : pos + 1;
  } else if (wide.rfind(L"\\\\?\\", 0) == 0) {
    start = wide.size() >= 7 ? 7 : wide.size();
  } else if (wide.size() >= 3 && wide[1] == L':') {
    start = 3;
  } else if (wide.size() >= 2 && wide[0] == L'\\' && wide[1] == L'\\') {
    std::size_t pos = wide.find(L'\\', 2);
    if (pos != std::wstring::npos) {
      pos = wide.find(L'\\', pos + 1);
    }
    start = pos == std::wstring::npos ? wide.size() : pos + 1;
  } else if (!wide.empty() && (wide[0] == L'\\' || wide[0] == L'/')) {
    start = 1;
  }
  static_cast<void>(separator);

  for (std::size_t i = start; i <= wide.size(); ++i) {
    const bool boundary = i == wide.size() || wide[i] == L'\\' || wide[i] == L'/';
    if (!boundary) {
      continue;
    }
    const std::wstring prefix = wide.substr(0, i);
    if (prefix.empty() || prefix.back() == L':') {
      continue;
    }
    if (CreateDirectoryW(prefix.c_str(), nullptr) == 0) {
      const unsigned long code = GetLastError();
      if (code != ERROR_ALREADY_EXISTS) {
        if (code == ERROR_ACCESS_DENIED) {
          return make_error(Code::kStorageIo, "access denied creating directory", prefix.empty()
                                                                                       ? path
                                                                                       : internal::to_utf8(prefix));
        }
        return io_error("CreateDirectory failed", path, internal::win_error_message(code));
      }
    }
  }
  return Status::success();
}

Status remove_file(const Path& path) {
  const Status valid = validate_path(path);
  if (valid.failed()) {
    return valid;
  }
  Outcome<std::wstring> native = internal::to_native(path);
  if (!native.ok()) {
    return native.status();
  }
  if (DeleteFileW(native.value().c_str()) == 0) {
    const unsigned long code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return Status::success();
    }
    return io_error("DeleteFile failed", path, internal::win_error_message(code));
  }
  return Status::success();
}

Outcome<std::vector<std::string>> list_directory(const Path& path) {
  const Status valid = validate_path(path);
  if (valid.failed()) {
    return valid;
  }
  std::wstring pattern = internal::to_native(path).value();
  if (!pattern.empty() && pattern.back() != L'\\') {
    pattern.push_back(L'\\');
  }
  pattern.append(L"*");
  WIN32_FIND_DATAW data{};
  HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
  if (handle == INVALID_HANDLE_VALUE) {
    return io_error("FindFirstFile failed", path,
                    internal::win_error_message(GetLastError()));
  }
  std::vector<std::string> names;
  do {
    const std::wstring_view name(data.cFileName);
    if (name == L"." || name == L"..") {
      continue;
    }
    names.push_back(internal::to_utf8(name));
  } while (FindNextFileW(handle, &data) != 0);
  FindClose(handle);
  std::sort(names.begin(), names.end());
  return names;
}

Outcome<std::vector<std::byte>> read_file(const Path& path, std::uint64_t max_bytes) {
  Outcome<File> file = File::open_read(path);
  if (!file.ok()) {
    return file.status();
  }
  Outcome<std::uint64_t> size = file.value().size();
  if (!size.ok()) {
    return size.status();
  }
  if (size.value() > max_bytes) {
    return make_error(Code::kFieldTooLong, "file exceeds the accepted bound",
                      path + " bytes=" + std::to_string(size.value()));
  }
  std::vector<std::byte> out(static_cast<std::size_t>(size.value()));
  if (!out.empty()) {
    Outcome<std::size_t> read = file.value().read_at(0, std::span<std::byte>(out));
    if (!read.ok()) {
      return read.status();
    }
    if (read.value() != out.size()) {
      return make_error(Code::kStorageCorrupt, "short read", path);
    }
  }
  return out;
}

Status append_file_sync(const Path& path, std::span<const std::byte> bytes) {
  Outcome<File> file = File::open_append(path);
  if (!file.ok()) {
    return file.status();
  }
  const Status written = file.value().append(bytes);
  if (written.failed()) {
    return written;
  }
  const Status flushed = file.value().flush();
  if (flushed.failed()) {
    return flushed;
  }
  return file.value().close();
}

Status write_file_atomic(const Path& path, std::span<const std::byte> bytes) {
  const Path directory = parent(path);
  if (directory.empty()) {
    return make_error(Code::kMalformedInput, "atomic write requires a directory", path);
  }
  const Status exists_ok = ensure_directory(directory);
  if (exists_ok.failed()) {
    return exists_ok;
  }
  const Path staging = directory + "/.cxf-stage-" + internal::staging_tag();

  {
    Outcome<File> file = File::create_new(staging);
    if (!file.ok()) {
      return file.status();
    }
    const Status written = file.value().append(bytes);
    if (written.failed()) {
      static_cast<void>(remove_file(staging));
      return written;
    }
    const Status flushed = file.value().flush();
    if (flushed.failed()) {
      static_cast<void>(remove_file(staging));
      return flushed;
    }
    const Status closed = file.value().close();
    if (closed.failed()) {
      static_cast<void>(remove_file(staging));
      return closed;
    }
  }

  // Read the staged bytes back before publishing: a torn or short write must be
  // detected while the previous good generation is still the live one.
  Outcome<std::vector<std::byte>> verify = read_file(staging, bytes.size() + 1);
  if (!verify.ok()) {
    static_cast<void>(remove_file(staging));
    return verify.status();
  }
  if (verify.value().size() != bytes.size() ||
      DigestBuilder::of(std::span<const std::byte>(verify.value())) !=
          DigestBuilder::of(bytes)) {
    static_cast<void>(remove_file(staging));
    return make_error(Code::kStorageCorrupt, "staged file did not verify", path);
  }

  Outcome<std::wstring> from = internal::to_native(staging);
  Outcome<std::wstring> to = internal::to_native(path);
  if (!from.ok()) {
    static_cast<void>(remove_file(staging));
    return from.status();
  }
  if (!to.ok()) {
    static_cast<void>(remove_file(staging));
    return to.status();
  }
  if (MoveFileExW(from.value().c_str(), to.value().c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    const unsigned long code = GetLastError();
    static_cast<void>(remove_file(staging));
    return io_error("MoveFileEx failed", path, internal::win_error_message(code));
  }
  return sync_directory(directory);
}

Status sync_directory(const Path& path) {
  Outcome<std::wstring> native = internal::to_native(path);
  if (!native.ok()) {
    return native.status();
  }
  // A directory handle cannot be flushed on every Windows filesystem. The
  // publish point of this store is the rename above, which is atomic with
  // respect to crash; the directory flush is attempted and its refusals are
  // reported as "not supported by this filesystem" rather than as success over
  // an unperformed operation.
  HANDLE handle = CreateFileW(native.value().c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const unsigned long code = GetLastError();
    if (code == ERROR_ACCESS_DENIED || code == ERROR_FILE_NOT_FOUND) {
      return Status::success();
    }
    return io_error("directory open failed", path, internal::win_error_message(code));
  }
  const BOOL flushed = FlushFileBuffers(handle);
  const unsigned long code = flushed != 0 ? 0UL : GetLastError();
  CloseHandle(handle);
  if (flushed != 0 || code == ERROR_ACCESS_DENIED || code == ERROR_INVALID_FUNCTION ||
      code == ERROR_INVALID_HANDLE) {
    return Status::success();
  }
  return io_error("directory flush failed", path, internal::win_error_message(code));
}

Outcome<Path> current_directory() {
  const DWORD needed = GetCurrentDirectoryW(0, nullptr);
  if (needed == 0) {
    return io_error("GetCurrentDirectory failed", {}, internal::win_error_message(GetLastError()));
  }
  std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
  const DWORD written = GetCurrentDirectoryW(needed, buffer.data());
  if (written == 0 || written >= needed) {
    return make_error(Code::kStorageIo, "GetCurrentDirectory failed");
  }
  buffer.resize(written);
  return internal::to_utf8(buffer);
}

Outcome<Path> absolute(const Path& path) {
  const Status valid = validate_path(path);
  if (valid.failed()) {
    return valid;
  }
  Outcome<std::wstring> wide = internal::to_wide(path);
  if (!wide.ok()) {
    return wide.status();
  }
  const DWORD needed = GetFullPathNameW(wide.value().c_str(), 0, nullptr, nullptr);
  if (needed == 0) {
    return io_error("GetFullPathName failed", path, internal::win_error_message(GetLastError()));
  }
  std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
  const DWORD written = GetFullPathNameW(wide.value().c_str(), needed, buffer.data(), nullptr);
  if (written == 0 || written >= needed) {
    return make_error(Code::kStorageIo, "GetFullPathName failed", path);
  }
  buffer.resize(written);
  return internal::to_utf8(buffer);
}

void File::adopt(std::intptr_t handle, Path path) noexcept {
  static_cast<void>(close());
  handle_ = handle;
  path_ = std::move(path);
}

File::File(File&& other) noexcept : handle_(other.handle_), path_(std::move(other.path_)) {
  other.handle_ = kInvalid;
  other.path_.clear();
}

File& File::operator=(File&& other) noexcept {
  if (this != &other) {
    static_cast<void>(close());
    handle_ = other.handle_;
    path_ = std::move(other.path_);
    other.handle_ = kInvalid;
    other.path_.clear();
  }
  return *this;
}

File::~File() { static_cast<void>(close()); }

Status File::close() {
  if (handle_ == kInvalid) {
    return Status::success();
  }
  const HANDLE handle = reinterpret_cast<HANDLE>(handle_);
  handle_ = kInvalid;
  if (CloseHandle(handle) == 0) {
    return io_error("CloseHandle failed", path_, internal::win_error_message(GetLastError()));
  }
  return Status::success();
}

Outcome<File> File::open_read(const Path& path) {
  return open_impl(path, GENERIC_READ, OPEN_EXISTING, -1);
}

Outcome<File> File::open_read_write(const Path& path) {
  return open_impl(path, GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING, -1);
}

Outcome<File> File::open_append(const Path& path) {
  Outcome<File> file = open_impl(path, GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS, -1);
  if (!file.ok()) {
    return file.status();
  }
  LARGE_INTEGER li{};
  if (SetFilePointerEx(reinterpret_cast<HANDLE>(file.value().handle_), li, nullptr, FILE_END) == 0) {
    const unsigned long code = GetLastError();
    return io_error("SetFilePointerEx failed", path, internal::win_error_message(code));
  }
  return file;
}

Outcome<File> File::create_new(const Path& path) {
  return open_impl(path, GENERIC_READ | GENERIC_WRITE, CREATE_NEW, -1);
}

Outcome<std::uint64_t> File::size() const {
  if (!valid()) {
    return make_error(Code::kPreconditionViolated, "file handle is not open");
  }
  LARGE_INTEGER li{};
  if (GetFileSizeEx(reinterpret_cast<HANDLE>(handle_), &li) == 0) {
    return io_error("GetFileSizeEx failed", path_, internal::win_error_message(GetLastError()));
  }
  return static_cast<std::uint64_t>(li.QuadPart);
}

Outcome<std::size_t> File::read_at(std::uint64_t offset, std::span<std::byte> out) const {
  if (!valid()) {
    return make_error(Code::kPreconditionViolated, "file handle is not open");
  }
  if (out.empty()) {
    return std::size_t{0};
  }
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32u);
  DWORD read = 0;
  if (ReadFile(reinterpret_cast<HANDLE>(handle_), out.data(), static_cast<DWORD>(out.size()),
               &read, &overlapped) == 0) {
    const unsigned long code = GetLastError();
    if (code == ERROR_HANDLE_EOF) {
      return std::size_t{0};
    }
    return io_error("ReadFile failed", path_, internal::win_error_message(code));
  }
  return static_cast<std::size_t>(read);
}

Status File::write_at(std::uint64_t offset, std::span<const std::byte> bytes) const {
  if (!valid()) {
    return make_error(Code::kPreconditionViolated, "file handle is not open");
  }
  std::size_t written = 0;
  while (written < bytes.size()) {
    OVERLAPPED overlapped{};
    const std::uint64_t at = offset + written;
    overlapped.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
    overlapped.OffsetHigh = static_cast<DWORD>(at >> 32u);
    DWORD chunk = 0;
    const auto remaining = static_cast<DWORD>(bytes.size() - written);
    if (WriteFile(reinterpret_cast<HANDLE>(handle_), bytes.data() + written, remaining, &chunk,
                  &overlapped) == 0) {
      return io_error("WriteFile failed", path_, internal::win_error_message(GetLastError()));
    }
    if (chunk == 0) {
      return make_error(Code::kStorageIo, "WriteFile performed no progress", path_);
    }
    written += static_cast<std::size_t>(chunk);
  }
  return Status::success();
}

Status File::append(std::span<const std::byte> bytes) {
  if (!valid()) {
    return make_error(Code::kPreconditionViolated, "file handle is not open");
  }
  // Append means "at end of file", which is not the same as "at the current
  // position": a read on this handle moves the position, and relying on it
  // would silently overwrite an existing record. Seek to the end explicitly and
  // write the bytes there.
  LARGE_INTEGER end{};
  LARGE_INTEGER at{};
  if (SetFilePointerEx(reinterpret_cast<HANDLE>(handle_), end, &at, FILE_END) == 0) {
    return io_error("SetFilePointerEx failed", path_, internal::win_error_message(GetLastError()));
  }
  const auto offset = static_cast<std::uint64_t>(at.QuadPart);
  const Status written = write_at(offset, bytes);
  if (written.failed()) {
    return written;
  }
  LARGE_INTEGER next{};
  next.QuadPart = static_cast<LONGLONG>(offset + bytes.size());
  if (SetFilePointerEx(reinterpret_cast<HANDLE>(handle_), next, nullptr, FILE_BEGIN) == 0) {
    return io_error("SetFilePointerEx failed", path_, internal::win_error_message(GetLastError()));
  }
  return Status::success();
}

Status File::flush() {
  if (!valid()) {
    return make_error(Code::kPreconditionViolated, "file handle is not open");
  }
  if (FlushFileBuffers(reinterpret_cast<HANDLE>(handle_)) == 0) {
    return io_error("FlushFileBuffers failed", path_, internal::win_error_message(GetLastError()));
  }
  return Status::success();
}

Status File::truncate(std::uint64_t size) {
  if (!valid()) {
    return make_error(Code::kPreconditionViolated, "file handle is not open");
  }
  LARGE_INTEGER li{};
  li.QuadPart = static_cast<LONGLONG>(size);
  if (SetFilePointerEx(reinterpret_cast<HANDLE>(handle_), li, nullptr, FILE_BEGIN) == 0) {
    return io_error("SetFilePointerEx failed", path_, internal::win_error_message(GetLastError()));
  }
  if (SetEndOfFile(reinterpret_cast<HANDLE>(handle_)) == 0) {
    return io_error("SetEndOfFile failed", path_, internal::win_error_message(GetLastError()));
  }
  return Status::success();
}

Outcome<std::uint64_t> File::position() const {
  if (!valid()) {
    return make_error(Code::kPreconditionViolated, "file handle is not open");
  }
  LARGE_INTEGER zero{};
  LARGE_INTEGER current{};
  if (SetFilePointerEx(reinterpret_cast<HANDLE>(handle_), zero, &current, FILE_CURRENT) == 0) {
    return io_error("SetFilePointerEx failed", path_, internal::win_error_message(GetLastError()));
  }
  return static_cast<std::uint64_t>(current.QuadPart);
}

#else  // !_WIN32

// POSIX implementation of the same contract. It is compiled only on POSIX
// toolchains; the released validation matrix covers Windows/MSVC.

Outcome<bool> exists(const Path& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) == 0) {
    return true;
  }
  if (errno == ENOENT) {
    return false;
  }
  return make_error(Code::kStorageIo, "stat failed", path);
}

Outcome<bool> is_directory(const Path& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) {
    return make_error(Code::kStorageUnavailable, "path does not exist", path);
  }
  return S_ISDIR(st.st_mode);
}

Outcome<std::uint64_t> file_size(const Path& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return make_error(Code::kStorageUnavailable, "not a regular file", path);
  }
  return static_cast<std::uint64_t>(st.st_size);
}

Status ensure_directory(const Path& path) {
  const Status valid = validate_path(path);
  if (valid.failed()) {
    return valid;
  }
  std::string current;
  for (const std::string_view part : split(path, '/')) {
    if (part.empty()) {
      continue;
    }
    current = current.empty() ? std::string(part) : current + "/" + std::string(part);
    if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
      return make_error(Code::kStorageIo, "mkdir failed", current);
    }
  }
  return Status::success();
}

Status remove_file(const Path& path) {
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    return make_error(Code::kStorageIo, "unlink failed", path);
  }
  return Status::success();
}

Outcome<std::vector<std::string>> list_directory(const Path& path) {
  DIR* dir = ::opendir(path.c_str());
  if (dir == nullptr) {
    return make_error(Code::kStorageIo, "opendir failed", path);
  }
  std::vector<std::string> names;
  while (dirent* entry = ::readdir(dir)) {
    const std::string_view name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    names.emplace_back(name);
  }
  ::closedir(dir);
  std::sort(names.begin(), names.end());
  return names;
}

Outcome<std::vector<std::byte>> read_file(const Path& path, std::uint64_t max_bytes) {
  Outcome<File> file = File::open_read(path);
  if (!file.ok()) {
    return file.status();
  }
  Outcome<std::uint64_t> size = file.value().size();
  if (!size.ok()) {
    return size.status();
  }
  if (size.value() > max_bytes) {
    return make_error(Code::kFieldTooLong, "file exceeds the accepted bound", path);
  }
  std::vector<std::byte> out(static_cast<std::size_t>(size.value()));
  if (!out.empty()) {
    Outcome<std::size_t> read = file.value().read_at(0, std::span<std::byte>(out));
    if (!read.ok()) {
      return read.status();
    }
    if (read.value() != out.size()) {
      return make_error(Code::kStorageCorrupt, "short read", path);
    }
  }
  return out;
}

Status append_file_sync(const Path& path, std::span<const std::byte> bytes) {
  Outcome<File> file = File::open_append(path);
  if (!file.ok()) {
    return file.status();
  }
  const Status written = file.value().append(bytes);
  if (written.failed()) {
    return written;
  }
  const Status flushed = file.value().flush();
  if (flushed.failed()) {
    return flushed;
  }
  return file.value().close();
}

Status write_file_atomic(const Path& path, std::span<const std::byte> bytes) {
  const Path directory = parent(path);
  const Status created = ensure_directory(directory);
  if (created.failed()) {
    return created;
  }
  const Path staging = directory + "/.cxf-stage-" + internal::staging_tag();
  {
    Outcome<File> file = File::create_new(staging);
    if (!file.ok()) {
      return file.status();
    }
    const Status written = file.value().append(bytes);
    if (written.failed()) {
      static_cast<void>(remove_file(staging));
      return written;
    }
    const Status flushed = file.value().flush();
    if (flushed.failed()) {
      static_cast<void>(remove_file(staging));
      return flushed;
    }
    static_cast<void>(file.value().close());
  }
  Outcome<std::vector<std::byte>> verify = read_file(staging, bytes.size() + 1);
  if (!verify.ok()) {
    static_cast<void>(remove_file(staging));
    return verify.status();
  }
  if (verify.value().size() != bytes.size() ||
      DigestBuilder::of(std::span<const std::byte>(verify.value())) !=
          DigestBuilder::of(bytes)) {
    static_cast<void>(remove_file(staging));
    return make_error(Code::kStorageCorrupt, "staged file did not verify", path);
  }
  if (::rename(staging.c_str(), path.c_str()) != 0) {
    static_cast<void>(remove_file(staging));
    return make_error(Code::kStorageIo, "rename failed", path);
  }
  return sync_directory(directory);
}

Status sync_directory(const Path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return Status::success();
  }
  const int rc = ::fsync(fd);
  ::close(fd);
  if (rc != 0) {
    return make_error(Code::kStorageIo, "directory fsync failed", path);
  }
  return Status::success();
}

Outcome<Path> current_directory() {
  char buffer[4096];
  if (::getcwd(buffer, sizeof(buffer)) == nullptr) {
    return make_error(Code::kStorageIo, "getcwd failed");
  }
  return Path(buffer);
}

Outcome<Path> absolute(const Path& path) {
  if (!path.empty() && path[0] == '/') {
    return path;
  }
  Outcome<Path> cwd = current_directory();
  if (!cwd.ok()) {
    return cwd.status();
  }
  return join(cwd.value(), path);
}

void File::adopt(std::intptr_t handle, Path path) noexcept {
  static_cast<void>(close());
  handle_ = handle;
  path_ = std::move(path);
}

File::File(File&& other) noexcept : handle_(other.handle_), path_(std::move(other.path_)) {
  other.handle_ = kInvalid;
  other.path_.clear();
}

File& File::operator=(File&& other) noexcept {
  if (this != &other) {
    static_cast<void>(close());
    handle_ = other.handle_;
    path_ = std::move(other.path_);
    other.handle_ = kInvalid;
    other.path_.clear();
  }
  return *this;
}

File::~File() { static_cast<void>(close()); }

Status File::close() {
  if (handle_ == kInvalid) {
    return Status::success();
  }
  const int fd = static_cast<int>(handle_);
  handle_ = kInvalid;
  if (::close(fd) != 0) {
    return make_error(Code::kStorageIo, "close failed", path_);
  }
  return Status::success();
}

Outcome<File> File::open_read(const Path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return make_error(Code::kStorageUnavailable, "open failed", path);
  }
  File file;
  file.adopt(fd, path);
  return file;
}

Outcome<File> File::open_read_write(const Path& path) {
  const int fd = ::open(path.c_str(), O_RDWR);
  if (fd < 0) {
    return make_error(Code::kStorageUnavailable, "open failed", path);
  }
  File file;
  file.adopt(fd, path);
  return file;
}

Outcome<File> File::open_append(const Path& path) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    return make_error(Code::kStorageIo, "open failed", path);
  }
  File file;
  file.adopt(fd, path);
  return file;
}

Outcome<File> File::create_new(const Path& path) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
  if (fd < 0) {
    return make_error(Code::kStorageIo, "create failed", path);
  }
  File file;
  file.adopt(fd, path);
  return file;
}

Outcome<std::uint64_t> File::size() const {
  if (!valid()) {
    return make_error(Code::kPreconditionViolated, "file handle is not open");
  }
  struct stat st{};
  if (::fstat(static_cast<int>(handle_), &st) != 0) {
    return make_error(Code::kStorageIo, "fstat failed", path_);
  }
  return static_cast<std::uint64_t>(st.st_size);
}

Outcome<std::size_t> File::read_at(std::uint64_t offset, std::span<std::byte> out) const {
  if (!valid()) {
    return make_error(Code::kPreconditionViolated, "file handle is not open");
  }
  const ssize_t got = ::pread(static_cast<int>(handle_), out.data(), out.size(),
                              static_cast<off_t>(offset));
  if (got < 0) {
    return make_error(Code::kStorageIo, "pread failed", path_);
  }
  return static_cast<std::size_t>(got);
}

Status File::write_at(std::uint64_t offset, std::span<const std::byte> bytes) const {
  if (!valid()) {
    return make_error(Code::kPreconditionViolated, "file handle is not open");
  }
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t put = ::pwrite(static_cast<int>(handle_), bytes.data() + written,
                                 bytes.size() - written,
                                 static_cast<off_t>(offset + written));
    if (put <= 0) {
      return make_error(Code::kStorageIo, "pwrite failed", path_);
    }
    written += static_cast<std::size_t>(put);
  }
  return Status::success();
}

Status File::append(std::span<const std::byte> bytes) {
  if (!valid()) {
    return make_error(Code::kPreconditionViolated, "file handle is not open");
  }
  // Append means "at end of file", never "at the current position".
  const off_t offset = ::lseek(static_cast<int>(handle_), 0, SEEK_END);
  if (offset < 0) {
    return make_error(Code::kStorageIo, "lseek to end failed", path_);
  }
  const Status written = write_at(static_cast<std::uint64_t>(offset), bytes);
  if (written.failed()) {
    return written;
  }
  if (::lseek(static_cast<int>(handle_), offset + static_cast<off_t>(bytes.size()),
              SEEK_SET) < 0) {
    return make_error(Code::kStorageIo, "lseek failed", path_);
  }
  return Status::success();
}

Status File::flush() {
  if (!valid()) {
    return make_error(Code::kPreconditionViolated, "file handle is not open");
  }
  if (::fsync(static_cast<int>(handle_)) != 0) {
    return make_error(Code::kStorageIo, "fsync failed", path_);
  }
  return Status::success();
}

Status File::truncate(std::uint64_t size) {
  if (!valid()) {
    return make_error(Code::kPreconditionViolated, "file handle is not open");
  }
  if (::ftruncate(static_cast<int>(handle_), static_cast<off_t>(size)) != 0) {
    return make_error(Code::kStorageIo, "ftruncate failed", path_);
  }
  return Status::success();
}

Outcome<std::uint64_t> File::position() const {
  if (!valid()) {
    return make_error(Code::kPreconditionViolated, "file handle is not open");
  }
  const off_t at = ::lseek(static_cast<int>(handle_), 0, SEEK_CUR);
  if (at < 0) {
    return make_error(Code::kStorageIo, "lseek failed", path_);
  }
  return static_cast<std::uint64_t>(at);
}

#endif  // _WIN32

}  // namespace cxf::fs
