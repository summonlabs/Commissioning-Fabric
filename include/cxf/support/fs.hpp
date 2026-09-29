// Commissioning Fabric - filesystem primitives used by the durable store.
//
// All paths are UTF-8 byte strings. On Windows they are converted to UTF-16 and
// absolute paths are opened through the extended-length prefix so that long
// paths work without depending on machine-wide policy. Every operation is
// explicit about what it guarantees: write_file_atomic publishes by rename, and
// only then is the new content observable under the final name.
#ifndef CXF_SUPPORT_FS_HPP
#define CXF_SUPPORT_FS_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cxf/support/status.hpp"

namespace cxf::fs {

/// A UTF-8 filesystem path.
using Path = std::string;

/// Reject paths that cannot denote a single regular file in the fabric store:
/// empty, containing NUL or an alternate-data-stream colon outside the drive
/// prefix, containing a '..' component, or not valid UTF-8.
[[nodiscard]] Status validate_path(std::string_view path);

/// Join a directory and a single (already validated) name component.
[[nodiscard]] Path join(const Path& directory, std::string_view name);

/// Directory containing the path (no trailing separator), or empty when none.
[[nodiscard]] Path parent(const Path& path);

/// Final component of the path.
[[nodiscard]] std::string_view filename(const Path& path);

/// The process working directory as UTF-8.
[[nodiscard]] Outcome<Path> current_directory();

/// Absolute form of the path (no symlink resolution: the store must not depend
/// on where a link points).
[[nodiscard]] Outcome<Path> absolute(const Path& path);

[[nodiscard]] Outcome<bool> exists(const Path& path);
[[nodiscard]] Outcome<bool> is_directory(const Path& path);

/// Create the directory and any missing parents. Succeeds when it exists.
[[nodiscard]] Status ensure_directory(const Path& path);

/// File size, or an error when the path is absent or is a directory.
[[nodiscard]] Outcome<std::uint64_t> file_size(const Path& path);

/// Remove a file. Succeeds when it does not exist.
[[nodiscard]] Status remove_file(const Path& path);

/// Directory entries (names only), sorted by byte order for determinism.
/// The entries "." and ".." are never returned.
[[nodiscard]] Outcome<std::vector<std::string>> list_directory(const Path& path);

/// Read a whole file. Fails when the file exceeds max_bytes rather than
/// allocating without bound.
[[nodiscard]] Outcome<std::vector<std::byte>> read_file(const Path& path,
                                                        std::uint64_t max_bytes);

/// Append bytes and flush to the device before returning.
[[nodiscard]] Status append_file_sync(const Path& path, std::span<const std::byte> bytes);

/// Write bytes to a temporary file in the same directory, flush it, read it
/// back and verify its digest, publish it by atomic rename over the final path,
/// then flush the directory. Returns only after the new content is durable
/// under the final name.
[[nodiscard]] Status write_file_atomic(const Path& path, std::span<const std::byte> bytes);

/// Flush a directory entry (metadata) to the device.
[[nodiscard]] Status sync_directory(const Path& path);

/// Open regular-file handle with explicit access. Reparse points in the final
/// path component are rejected: the store is never redirected through a link.
class File {
 public:
  File() noexcept = default;
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  File(File&& other) noexcept;
  File& operator=(File&& other) noexcept;
  ~File();

  /// Open an existing file for reading (sharing reads).
  [[nodiscard]] static Outcome<File> open_read(const Path& path);
  /// Open an existing file for reading and writing.
  [[nodiscard]] static Outcome<File> open_read_write(const Path& path);
  /// Create or open a file for appending; the write position is always the end.
  [[nodiscard]] static Outcome<File> open_append(const Path& path);
  /// Create a new file, failing when it already exists.
  [[nodiscard]] static Outcome<File> create_new(const Path& path);

  [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalid; }
  [[nodiscard]] Outcome<std::uint64_t> size() const;

  /// Read up to out.size() bytes at offset. Short reads at end of file are
  /// reported by the returned length.
  [[nodiscard]] Outcome<std::size_t> read_at(std::uint64_t offset,
                                             std::span<std::byte> out) const;
  /// Write all bytes at offset, extending the file when needed.
  [[nodiscard]] Status write_at(std::uint64_t offset, std::span<const std::byte> bytes) const;
  /// Write all bytes at the current append position.
  [[nodiscard]] Status append(std::span<const std::byte> bytes);
  /// Flush file data and metadata to the device.
  [[nodiscard]] Status flush();
  /// Set the end of file to size.
  [[nodiscard]] Status truncate(std::uint64_t size);
  /// Close now; further use fails. The destructor also closes.
  [[nodiscard]] Status close();
  /// Current append offset (honest about the handle's real position).
  [[nodiscard]] Outcome<std::uint64_t> position() const;

  /// Internal: adopt an already-open platform handle. Used by the platform
  /// implementation and by the store lock; not part of the supported surface.
  void adopt(std::intptr_t handle, Path path) noexcept;

 private:
  static constexpr std::intptr_t kInvalid = -1;
  std::intptr_t handle_{kInvalid};
  Path path_{};
};

}  // namespace cxf::fs

#endif  // CXF_SUPPORT_FS_HPP
