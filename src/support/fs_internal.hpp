// Shared internal helpers for the platform filesystem layer.
#ifndef CXF_SRC_SUPPORT_FS_INTERNAL_HPP
#define CXF_SRC_SUPPORT_FS_INTERNAL_HPP

#include <string>
#include <string_view>

#include "cxf/support/status.hpp"

namespace cxf::fs::internal {

#if defined(_WIN32)

/// Convert a UTF-8 path to UTF-16. Fails on invalid UTF-8.
[[nodiscard]] Outcome<std::wstring> to_wide(std::string_view utf8);

/// Convert UTF-16 to UTF-8.
[[nodiscard]] std::string to_utf8(std::wstring_view wide);

/// Convert to a native absolute-capable path: forward slashes become
/// backslashes and absolute paths gain the extended-length prefix.
[[nodiscard]] Outcome<std::wstring> to_native(std::string_view utf8);

/// Render a Windows error code for diagnostics.
[[nodiscard]] std::string win_error_message(unsigned long code);

#endif

/// Process-wide unique, deterministic-enough tag for staging file names.
[[nodiscard]] std::string staging_tag();

}  // namespace cxf::fs::internal

#endif  // CXF_SRC_SUPPORT_FS_INTERNAL_HPP
