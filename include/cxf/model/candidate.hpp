// Commissioning Fabric - commissioning candidate and its attempt lineage.
//
// A candidate is the fabric's own record of one physical asset entering
// service. It is not the canonical asset record: identity, placement and
// dependencies arrive as typed evidence and are recorded here as bindings that
// name the exact registry identity and generation they were established under.
#ifndef CXF_MODEL_CANDIDATE_HPP
#define CXF_MODEL_CANDIDATE_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cxf/support/clock.hpp"
#include "cxf/support/digest.hpp"
#include "cxf/support/status.hpp"
#include "cxf/types/enums.hpp"
#include "cxf/types/generations.hpp"
#include "cxf/types/ids.hpp"

namespace cxf {

/// Upper bounds for the text the runtime accepts from requests.
inline constexpr std::size_t kMaxNameBytes = 64;
inline constexpr std::size_t kMaxDetailBytes = 512;

/// What the operator or discovery source declares about an asset at admission.
/// A declaration is a claim: it carries no registry identity and no authority.
struct CandidateDeclaration {
  std::string registry_name{};
  std::string model{};
  std::string serial{};
  HardwareGeneration hardware{};
  FirmwareGeneration firmware{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("registry_name", registry_name);
    ar("model", model);
    ar("serial", serial);
    ar("hardware", hardware);
    ar("firmware", firmware);
  }
};

/// Bound identity of a physical asset. The asset id and the registry name
/// belong to the canonical Asset Registry; this runtime only records them.
struct AssetIdentity {
  AssetId asset{};
  std::string registry_name{};
  std::string model{};
  std::string serial{};
  HardwareGeneration hardware{};
  FirmwareGeneration firmware{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("asset", asset);
    ar("registry_name", registry_name);
    ar("model", model);
    ar("serial", serial);
    ar("hardware", hardware);
    ar("firmware", firmware);
  }
};

/// Placement binding: where the asset physically is, and the topology
/// generation that placement was read from.
struct PlacementBinding {
  SiteId site{};
  RackId rack{};
  std::string position{};
  TopologyGeneration topology{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("site", site);
    ar("rack", rack);
    ar("position", position);
    ar("topology", topology);
  }
};

/// One commissioning attempt. A new attempt is opened after quarantine release
/// or after a failed attempt, so history is never overwritten.
struct Attempt {
  AttemptId id{};
  LifecycleGeneration generation{};
  LifecycleState state{LifecycleState::kDeclared};
  Timestamp opened_at{};
  Timestamp closed_at{};
  std::string note{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("id", id);
    ar("generation", generation);
    ar("state", state);
    ar("opened_at", opened_at);
    ar("closed_at", closed_at);
    ar("note", note);
  }
};

/// A dependency the candidate declares, with the generation the expectation was
/// formed under. A dependency whose live generation differs from the expected
/// one is unsatisfied: it is not silently re-bound.
struct DependencyRef {
  DependencyKind kind{DependencyKind::kSite};
  std::string name{};
  DependencyGeneration expected{};
  bool required{true};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("kind", kind);
    ar("name", name);
    ar("expected", expected);
    ar("required", required);
  }
};

/// The durable commissioning record of one candidate.
struct CommissioningCandidate {
  CandidateId id{};
  std::string name{};
  LifecycleState state{LifecycleState::kDeclared};
  /// Advanced on every accepted lifecycle transition; fences activation
  /// authority derived from an earlier state.
  LifecycleGeneration lifecycle{};
  /// Advanced whenever a new attempt is opened.
  IncarnationId incarnation{};
  /// Advanced on every accepted mutation of this record.
  Revision revision{};
  /// Facility generations as of the last accepted mutation.
  FacilityGenerations generations{};
  CandidateDeclaration declaration{};
  std::optional<AssetIdentity> identity{};
  std::optional<PlacementBinding> placement{};
  std::vector<DependencyRef> dependencies{};
  Attempt attempt{};
  std::vector<Attempt> history{};
  QuarantineReason quarantine_reason{QuarantineReason::kNone};
  std::string quarantine_detail{};
  PlanId last_plan{};
  Digest last_plan_digest{};
  Digest last_readiness_digest{};
  Timestamp admitted_at{};
  Timestamp updated_at{};
  CommitSequence last_commit{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("id", id);
    ar("name", name);
    ar("state", state);
    ar("lifecycle", lifecycle);
    ar("incarnation", incarnation);
    ar("revision", revision);
    ar("generations", generations);
    ar("declaration", declaration);
    ar("identity", identity);
    ar("placement", placement);
    ar("dependencies", dependencies);
    ar("attempt", attempt);
    ar("history", history);
    ar("quarantine_reason", quarantine_reason);
    ar("quarantine_detail", quarantine_detail);
    ar("last_plan", last_plan);
    ar("last_plan_digest", last_plan_digest);
    ar("last_readiness_digest", last_readiness_digest);
    ar("admitted_at", admitted_at);
    ar("updated_at", updated_at);
    ar("last_commit", last_commit);
  }
};

/// Field validation. Every externally supplied value passes through exactly one
/// of these before it is recorded, so the store only ever holds well-formed
/// records and the decoder's checks are defence in depth rather than the first
/// line of validation.
[[nodiscard]] Status validate_candidate_name(std::string_view name);
[[nodiscard]] Status validate_declaration(const CandidateDeclaration& declaration);
[[nodiscard]] Status validate_detail_text(std::string_view text);
[[nodiscard]] Status validate_asset_identity(const AssetIdentity& identity);
[[nodiscard]] Status validate_placement(const PlacementBinding& placement);
[[nodiscard]] Status validate_dependency(const DependencyRef& ref);
[[nodiscard]] Status validate_dependencies(const std::vector<DependencyRef>& refs);

/// True when two dependency references name the same (kind, name) pair.
[[nodiscard]] bool same_dependency_target(const DependencyRef& a, const DependencyRef& b) noexcept;

}  // namespace cxf

#endif  // CXF_MODEL_CANDIDATE_HPP
