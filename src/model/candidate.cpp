// Commissioning Fabric - candidate field validation.
//
// Every externally supplied value passes through exactly one of these checks
// before it is recorded, so the store only ever holds well-formed records and a
// missing or malformed field is rejected rather than absorbed. Nothing here
// invents a value: an absent serial stays absent, and a zero generation stays
// unknown.
#include "cxf/model/candidate.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cxf/support/status.hpp"
#include "cxf/support/text.hpp"
#include "cxf/types/enums.hpp"

namespace cxf {
namespace {

/// A required identifier token: 1..max_bytes bytes of [A-Za-z0-9._:-].
[[nodiscard]] Status check_token(std::string_view text, std::string_view field,
                                 std::size_t max_bytes) {
  if (text.empty()) {
    return make_error(Code::kFieldEmpty,
                      std::string(field) + " is required and must not be empty",
                      std::string(field));
  }
  if (text.size() > max_bytes) {
    return make_error(Code::kFieldTooLong, std::string(field) + " is longer than allowed",
                      std::string(field) + " bytes=" + std::to_string(text.size()));
  }
  if (!is_valid_token(text)) {
    return make_error(Code::kFieldOutOfRange,
                      std::string(field) + " must be a token of [A-Za-z0-9._:-] characters",
                      std::string(field) + "=" + escape_for_output(text));
  }
  return Status::success();
}

/// Bounded display text. An empty value is accepted when the field is optional;
/// when it is required, emptiness is reported as a missing field rather than as
/// invalid text, so the caller learns which of the two it is.
[[nodiscard]] Status check_display_text(std::string_view text, std::string_view field,
                                        std::size_t max_bytes, bool optional) {
  if (text.empty()) {
    if (optional) {
      return Status::success();
    }
    return make_error(Code::kFieldEmpty,
                      std::string(field) + " is required and must not be empty",
                      std::string(field));
  }
  if (text.size() > max_bytes) {
    return make_error(Code::kFieldTooLong, std::string(field) + " is longer than allowed",
                      std::string(field) + " bytes=" + std::to_string(text.size()));
  }
  if (!is_valid_utf8(text)) {
    return make_error(Code::kFieldInvalidUtf8,
                      std::string(field) + " is not well-formed UTF-8", std::string(field));
  }
  if (!is_valid_display_text(text)) {
    return make_error(Code::kMalformedInput,
                      std::string(field) + " contains a control character",
                      std::string(field));
  }
  return Status::success();
}

}  // namespace

Status validate_candidate_name(std::string_view name) {
  return check_token(name, "candidate name", kMaxNameBytes);
}

Status validate_detail_text(std::string_view text) {
  // Detail is free text and may be absent: an operator note is not required for
  // a record to be well-formed.
  return check_display_text(text, "detail", kMaxDetailBytes, /*optional=*/true);
}

Status validate_declaration(const CandidateDeclaration& declaration) {
  // A declaration is a claim and may be incomplete: an empty registry name is
  // admitted, because an asset can be declared before its registry identity is
  // known. A name that is present must be a well-formed token.
  if (!declaration.registry_name.empty()) {
    const Status status =
        check_token(declaration.registry_name, "declaration registry_name", kMaxNameBytes);
    if (status.failed()) {
      return status;
    }
  }
  Status status = check_display_text(declaration.model, "declaration model", kMaxDetailBytes,
                                     /*optional=*/true);
  if (status.failed()) {
    return status;
  }
  if (!declaration.serial.empty()) {
    status = check_token(declaration.serial, "declaration serial", kMaxNameBytes);
    if (status.failed()) {
      return status;
    }
  }
  // Hardware and firmware may be zero: an unknown generation is recorded as
  // unknown and is never invented by validation.
  return Status::success();
}

Status validate_asset_identity(const AssetIdentity& identity) {
  if (identity.asset.is_zero()) {
    return make_error(Code::kFieldOutOfRange,
                      "asset identity requires a non-zero asset id", "asset=0");
  }
  Status status = check_token(identity.registry_name, "registry_name", kMaxNameBytes);
  if (status.failed()) {
    return status;
  }
  status = check_display_text(identity.model, "model", kMaxDetailBytes, /*optional=*/true);
  if (status.failed()) {
    return status;
  }
  // A bound identity names the exact unit: the serial is required and must be
  // token-shaped rather than free text.
  if (identity.serial.empty()) {
    return make_error(Code::kFieldMissing,
                      "serial is required for a bound asset identity", "serial");
  }
  status = check_token(identity.serial, "serial", kMaxNameBytes);
  if (status.failed()) {
    return status;
  }
  // hardware and firmware may be zero: still unknown, never assumed.
  return Status::success();
}

Status validate_placement(const PlacementBinding& placement) {
  if (placement.site.is_zero()) {
    return make_error(Code::kFieldOutOfRange,
                      "placement requires a non-zero site id", "site=0");
  }
  if (placement.rack.is_zero()) {
    return make_error(Code::kFieldOutOfRange,
                      "placement requires a non-zero rack id", "rack=0");
  }
  const Status status = check_token(placement.position, "position", kMaxNameBytes);
  if (status.failed()) {
    return status;
  }
  // topology may be zero: an unread topology generation is not an assumption.
  return Status::success();
}

Status validate_dependency(const DependencyRef& ref) {
  if (!enum_value_valid(ref.kind)) {
    return make_error(Code::kFieldOutOfRange,
                      "dependency kind is outside the declared domain",
                      "kind=" + std::to_string(static_cast<unsigned>(ref.kind)));
  }
  const Status status = check_token(ref.name, "dependency name", kMaxNameBytes);
  if (status.failed()) {
    return status;
  }
  // expected may be zero: the expectation is then "no generation recorded",
  // which only an exactly zero generation fact can satisfy.
  return Status::success();
}

Status validate_dependencies(const std::vector<DependencyRef>& refs) {
  // Pass one: every element is shape-checked in declaration order, so the
  // reported failure is the first malformed reference.
  for (const DependencyRef& ref : refs) {
    const Status status = validate_dependency(ref);
    if (status.failed()) {
      return status;
    }
  }
  // Pass two: the first declaration that repeats an earlier (kind, name) pair
  // is reported. A std::map keeps the search deterministic and independent of
  // how the vector was built.
  std::map<std::pair<DependencyKind, std::string>, std::size_t> first_index;
  for (std::size_t i = 0; i < refs.size(); ++i) {
    const DependencyRef& ref = refs[i];
    const auto key = std::make_pair(ref.kind, ref.name);
    const auto found = first_index.find(key);
    if (found != first_index.end()) {
      return make_error(Code::kFieldConflict,
                        "dependency " + std::string(dependency_kind_name(ref.kind)) + ":" +
                            ref.name + " is declared more than once",
                        "first index=" + std::to_string(found->second) +
                            " duplicate index=" + std::to_string(i));
    }
    first_index.emplace(key, i);
  }
  return Status::success();
}

bool same_dependency_target(const DependencyRef& a, const DependencyRef& b) noexcept {
  return a.kind == b.kind && a.name == b.name;
}

}  // namespace cxf
