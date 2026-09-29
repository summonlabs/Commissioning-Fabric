// Commissioning Fabric - error model.
//
// Every externally meaningful operation returns an explicit Status. Reason codes
// are stable, documented and ordered: when a request violates several rules the
// runtime resolves it to the lowest-numbered applicable code so that the same
// invalid request always reports the same primary error regardless of map
// ordering, thread scheduling or unrelated state.
#ifndef CXF_SUPPORT_STATUS_HPP
#define CXF_SUPPORT_STATUS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cxf {

enum class Code : std::uint16_t {
  kOk = 0,

  // --- input shape / encoding (10-19) -------------------------------------
  kMalformedInput = 10,
  kFieldMissing = 11,
  kFieldTooLong = 12,
  kFieldInvalidUtf8 = 13,
  kFieldEmpty = 14,
  kFieldOutOfRange = 15,
  kFieldConflict = 16,
  kReservedFieldNonZero = 17,
  kUnsupportedFormatVersion = 18,
  kTrailingBytes = 19,

  // --- identity / addressing (20-29) --------------------------------------
  kCandidateNotFound = 20,
  kCandidateAlreadyExists = 21,
  kAssetIdentityConflict = 22,
  kAssetAlreadyCommissioned = 23,

  // --- lifecycle / ordering (30-39) ---------------------------------------
  kStateNotAllowed = 30,
  kTransitionNotAllowed = 31,
  kAttemptNotFound = 32,
  kCandidateTerminal = 33,

  // --- evidence (40-49) ---------------------------------------------------
  kEvidenceUnknownDimension = 40,
  kEvidenceStale = 41,
  kEvidenceContradictory = 42,
  kEvidenceNegative = 43,
  kEvidenceMissing = 44,
  kEvidenceGenerationMismatch = 45,
  kEvidenceDuplicateDigest = 46,

  // --- plan binding / authority (50-59) -----------------------------------
  kPlanNotFound = 50,
  kPlanStale = 51,
  kAuthorityTokenInvalid = 52,
  kAuthorityTokenExpired = 53,
  kAuthorityTokenConsumed = 54,
  kIdempotencyKeyReuse = 55,
  kGenerationRegression = 56,
  kFencingTokenStale = 57,

  // --- dependency / placement / compatibility (60-69) ---------------------
  kDependencyUnsatisified = 60,
  kDependencyCycle = 61,
  kDependencyAmbiguous = 62,
  kPlacementUnbound = 63,
  kCompatibilityUnsupported = 64,
  kPolicyNotSatisfied = 65,
  kServiceClassUnsatisfied = 66,

  // --- persistence / environment (70-79) ----------------------------------
  kStorageUnavailable = 70,
  kStorageCorrupt = 71,
  kStorageLocked = 72,
  kStorageIo = 73,
  kStorageUnsupportedFormat = 74,

  // --- runtime emergencies (80-89) ----------------------------------------
  kInternalError = 80,
  kPreconditionViolated = 81,
  kNotImplemented = 82,
};

/// Human-readable, stable name for a reason code.
[[nodiscard]] std::string_view code_name(Code code) noexcept;

/// Default explanation for a reason code.
[[nodiscard]] std::string_view code_message(Code code) noexcept;

/// True when the code belongs to the persistence/environment family.
[[nodiscard]] bool is_storage_code(Code code) noexcept;

/// True when the numeric value is one of the declared reason codes. Used by the
/// durable decoder to reject values outside the closed domain.
[[nodiscard]] bool is_valid_code_value(std::uint16_t value) noexcept;

/// Result of an operation that produces no value.
class Status {
 public:
  Status() noexcept = default;

  /// Success value. Named "success" because a static and a non-static member
  /// cannot share the name "ok" with the same parameter list.
  [[nodiscard]] static Status success() noexcept { return Status{}; }

  static Status error(Code code, std::string detail, std::string context = {}) {
    Status s;
    s.code_ = code;
    s.detail_ = std::move(detail);
    s.context_ = std::move(context);
    return s;
  }

  [[nodiscard]] bool ok() const noexcept { return code_ == Code::kOk; }
  [[nodiscard]] bool failed() const noexcept { return code_ != Code::kOk; }
  [[nodiscard]] Code code() const noexcept { return code_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
  [[nodiscard]] const std::string& context() const noexcept { return context_; }

  [[nodiscard]] std::string message() const;

  explicit operator bool() const noexcept { return ok(); }

 private:
  Code code_{Code::kOk};
  std::string detail_{};
  std::string context_{};
};

/// Result of an operation that produces a value.
template <typename T>
class Outcome {
 public:
  Outcome(T value) : value_(std::move(value)) {}                // NOLINT implicit
  Outcome(Status status) : status_(std::move(status)) {}        // NOLINT implicit

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] Code code() const noexcept { return status_.code(); }

  /// Access the value. Precondition: ok(). Never throws.
  [[nodiscard]] const T& value() const noexcept {
    return *value_;
  }
  [[nodiscard]] T& value() noexcept { return *value_; }
  [[nodiscard]] const T& operator*() const noexcept { return *value_; }
  [[nodiscard]] T& operator*() noexcept { return *value_; }
  [[nodiscard]] const T* operator->() const noexcept { return value_.operator->(); }
  [[nodiscard]] T* operator->() noexcept { return value_.operator->(); }

  /// Value if present, otherwise `fallback`. Explicit about absence.
  template <typename U>
  [[nodiscard]] T value_or(U&& fallback) const {
    return value_.has_value() ? *value_ : static_cast<T>(std::forward<U>(fallback));
  }

  [[nodiscard]] std::string message() const { return status_.message(); }

 private:
  // The value is stored as an optional so that a failed Outcome never requires
  // T to be default-constructible.
  std::optional<T> value_{};
  Status status_{};
};

/// Convenience constructors.
[[nodiscard]] inline Status make_error(Code code, std::string detail,
                                       std::string context = {}) {
  return Status::error(code, std::move(detail), std::move(context));
}

}  // namespace cxf

#endif  // CXF_SUPPORT_STATUS_HPP
