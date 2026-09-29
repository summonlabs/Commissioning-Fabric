#include "cxf/support/status.hpp"

namespace cxf {

std::string_view code_name(Code code) noexcept {
  switch (code) {
    case Code::kOk: return "Ok";
    case Code::kMalformedInput: return "MalformedInput";
    case Code::kFieldMissing: return "FieldMissing";
    case Code::kFieldTooLong: return "FieldTooLong";
    case Code::kFieldInvalidUtf8: return "FieldInvalidUtf8";
    case Code::kFieldEmpty: return "FieldEmpty";
    case Code::kFieldOutOfRange: return "FieldOutOfRange";
    case Code::kFieldConflict: return "FieldConflict";
    case Code::kReservedFieldNonZero: return "ReservedFieldNonZero";
    case Code::kUnsupportedFormatVersion: return "UnsupportedFormatVersion";
    case Code::kTrailingBytes: return "TrailingBytes";
    case Code::kCandidateNotFound: return "CandidateNotFound";
    case Code::kCandidateAlreadyExists: return "CandidateAlreadyExists";
    case Code::kAssetIdentityConflict: return "AssetIdentityConflict";
    case Code::kAssetAlreadyCommissioned: return "AssetAlreadyCommissioned";
    case Code::kStateNotAllowed: return "StateNotAllowed";
    case Code::kTransitionNotAllowed: return "TransitionNotAllowed";
    case Code::kAttemptNotFound: return "AttemptNotFound";
    case Code::kCandidateTerminal: return "CandidateTerminal";
    case Code::kEvidenceUnknownDimension: return "EvidenceUnknownDimension";
    case Code::kEvidenceStale: return "EvidenceStale";
    case Code::kEvidenceContradictory: return "EvidenceContradictory";
    case Code::kEvidenceNegative: return "EvidenceNegative";
    case Code::kEvidenceMissing: return "EvidenceMissing";
    case Code::kEvidenceGenerationMismatch: return "EvidenceGenerationMismatch";
    case Code::kEvidenceDuplicateDigest: return "EvidenceDuplicateDigest";
    case Code::kPlanNotFound: return "PlanNotFound";
    case Code::kPlanStale: return "PlanStale";
    case Code::kAuthorityTokenInvalid: return "AuthorityTokenInvalid";
    case Code::kAuthorityTokenExpired: return "AuthorityTokenExpired";
    case Code::kAuthorityTokenConsumed: return "AuthorityTokenConsumed";
    case Code::kIdempotencyKeyReuse: return "IdempotencyKeyReuse";
    case Code::kGenerationRegression: return "GenerationRegression";
    case Code::kFencingTokenStale: return "FencingTokenStale";
    case Code::kDependencyUnsatisified: return "DependencyUnsatisfied";
    case Code::kDependencyCycle: return "DependencyCycle";
    case Code::kDependencyAmbiguous: return "DependencyAmbiguous";
    case Code::kPlacementUnbound: return "PlacementUnbound";
    case Code::kCompatibilityUnsupported: return "CompatibilityUnsupported";
    case Code::kPolicyNotSatisfied: return "PolicyNotSatisfied";
    case Code::kServiceClassUnsatisfied: return "ServiceClassUnsatisfied";
    case Code::kStorageUnavailable: return "StorageUnavailable";
    case Code::kStorageCorrupt: return "StorageCorrupt";
    case Code::kStorageLocked: return "StorageLocked";
    case Code::kStorageIo: return "StorageIo";
    case Code::kStorageUnsupportedFormat: return "StorageUnsupportedFormat";
    case Code::kInternalError: return "InternalError";
    case Code::kPreconditionViolated: return "PreconditionViolated";
    case Code::kNotImplemented: return "NotImplemented";
  }
  return "Unknown";
}

std::string_view code_message(Code code) noexcept {
  switch (code) {
    case Code::kOk: return "operation completed";
    case Code::kMalformedInput: return "request or record is malformed";
    case Code::kFieldMissing: return "a required field is absent";
    case Code::kFieldTooLong: return "a field exceeds its documented bound";
    case Code::kFieldInvalidUtf8: return "a text field is not well-formed UTF-8";
    case Code::kFieldEmpty: return "a field that must carry a value is empty";
    case Code::kFieldOutOfRange: return "a field value is outside its permitted range";
    case Code::kFieldConflict: return "two supplied fields contradict each other";
    case Code::kReservedFieldNonZero: return "a reserved field must be zero";
    case Code::kUnsupportedFormatVersion: return "the durable format version is not supported";
    case Code::kTrailingBytes: return "unexpected bytes follow a complete record";
    case Code::kCandidateNotFound: return "no such commissioning candidate";
    case Code::kCandidateAlreadyExists: return "a candidate with that identity already exists";
    case Code::kAssetIdentityConflict: return "the observed asset identity contradicts the declared identity";
    case Code::kAssetAlreadyCommissioned: return "the asset is already commissioned";
    case Code::kStateNotAllowed: return "the operation is not allowed in the current lifecycle state";
    case Code::kTransitionNotAllowed: return "the lifecycle transition is not permitted";
    case Code::kAttemptNotFound: return "no such commissioning attempt";
    case Code::kCandidateTerminal: return "the candidate is in a terminal state";
    case Code::kEvidenceUnknownDimension: return "the evidence dimension is not part of the readiness model";
    case Code::kEvidenceStale: return "the evidence is stale under its freshness rule or generation binding";
    case Code::kEvidenceContradictory: return "live evidence contradicts other live evidence for the same subject";
    case Code::kEvidenceNegative: return "the evidence reports an unsatisfied condition";
    case Code::kEvidenceMissing: return "required evidence has not been submitted";
    case Code::kEvidenceGenerationMismatch: return "the evidence was observed under different facility generations";
    case Code::kEvidenceDuplicateDigest: return "identical evidence content was already recorded";
    case Code::kPlanNotFound: return "no such readiness plan";
    case Code::kPlanStale: return "the plan no longer matches current facility state";
    case Code::kAuthorityTokenInvalid: return "the activation authority token does not bind to this request";
    case Code::kAuthorityTokenExpired: return "the activation authority token has expired";
    case Code::kAuthorityTokenConsumed: return "the activation authority token was already consumed";
    case Code::kIdempotencyKeyReuse: return "the idempotency key was reused with different request content";
    case Code::kGenerationRegression: return "the supplied generation is older than the currently held generation";
    case Code::kFencingTokenStale: return "the request carries a fenced-out authority generation";
    case Code::kDependencyUnsatisified: return "a required dependency is not satisfied";
    case Code::kDependencyCycle: return "the dependency graph contains a cycle";
    case Code::kDependencyAmbiguous: return "the dependency reference resolves to more than one entity";
    case Code::kPlacementUnbound: return "no placement binding is recorded";
    case Code::kCompatibilityUnsupported: return "the hardware or firmware baseline is not supported";
    case Code::kPolicyNotSatisfied: return "policy approval is absent or negative";
    case Code::kServiceClassUnsatisfied: return "service-class or ownership requirements are not satisfied";
    case Code::kStorageUnavailable: return "the durable store is not available";
    case Code::kStorageCorrupt: return "the durable store failed an integrity check";
    case Code::kStorageLocked: return "another process holds the single-writer lock on the store";
    case Code::kStorageIo: return "a filesystem operation failed";
    case Code::kStorageUnsupportedFormat: return "the durable store uses an unsupported format version";
    case Code::kInternalError: return "an internal invariant was violated";
    case Code::kPreconditionViolated: return "a documented precondition was not met";
    case Code::kNotImplemented: return "the operation is not implemented";
  }
  return "unknown code";
}

bool is_valid_code_value(std::uint16_t value) noexcept {
  switch (value) {
    case 0:
    case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 17: case 18: case 19:
    case 20: case 21: case 22: case 23:
    case 30: case 31: case 32: case 33:
    case 40: case 41: case 42: case 43: case 44: case 45: case 46:
    case 50: case 51: case 52: case 53: case 54: case 55: case 56: case 57:
    case 60: case 61: case 62: case 63: case 64: case 65: case 66:
    case 70: case 71: case 72: case 73: case 74:
    case 80: case 81: case 82:
      return true;
    default:
      return false;
  }
}

bool is_storage_code(Code code) noexcept {
  return code >= Code::kStorageUnavailable && code <= Code::kStorageUnsupportedFormat;
}

std::string Status::message() const {
  std::string out(code_name(code_));
  out.append(": ");
  out.append(code_message(code_));
  if (!detail_.empty()) {
    out.append(" [");
    out.append(detail_);
    out.push_back(']');
  }
  if (!context_.empty()) {
    out.append(" (");
    out.append(context_);
    out.push_back(')');
  }
  return out;
}

}  // namespace cxf
