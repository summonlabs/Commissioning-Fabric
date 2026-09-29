// Commissioning Fabric - dependency facts and closure.
//
// The fabric records the generations of the facility entities it depends on as
// typed facts. It never derives a dependency state from the mere existence of a
// name: a fact carries the generation it was reported at, and a candidate's
// expectation is satisfied only by an exact generation match.
#ifndef CXF_MODEL_DEPENDENCY_HPP
#define CXF_MODEL_DEPENDENCY_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cxf/model/candidate.hpp"
#include "cxf/support/status.hpp"
#include "cxf/types/enums.hpp"
#include "cxf/types/generations.hpp"

namespace cxf {

/// One facility fact: an entity the fabric may depend on, and its generation.
struct DependencyFact {
  DependencyKind kind{DependencyKind::kSite};
  std::string name{};
  DependencyGeneration generation{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("kind", kind);
    ar("name", name);
    ar("generation", generation);
  }
};

/// The facility generation record held by the fabric: the generations that
/// fence evidence and authority, plus the dependency facts they cover.
struct FacilityRecord {
  FacilityGenerations generations{};
  std::vector<DependencyFact> dependencies{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("generations", generations);
    ar("dependencies", dependencies);
  }
};

/// Resolution of one dependency reference against the facility record.
struct DependencyOutcome {
  DependencyRef ref{};
  bool known{false};
  bool ambiguous{false};
  DependencyGeneration actual{};
  std::string detail{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("ref", ref);
    ar("known", known);
    ar("ambiguous", ambiguous);
    ar("actual", actual);
    ar("detail", detail);
  }
};

/// Resolution of every declared dependency of a candidate, in declaration order.
struct DependencyResolution {
  std::vector<DependencyOutcome> entries{};

  [[nodiscard]] bool all_required_satisfied() const noexcept;
  [[nodiscard]] std::size_t unsatisfied_required() const noexcept;
  [[nodiscard]] bool any_ambiguous() const noexcept;

  template <typename Ar>
  void visit(Ar& ar) {
    ar("entries", entries);
  }
};

/// Resolve the candidate's declared dependencies against the facility record.
/// A name that resolves to facts of more than one kind is ambiguous and is
/// reported as such rather than resolved by preferring one kind.
[[nodiscard]] DependencyResolution resolve_dependencies(const CommissioningCandidate& candidate,
                                                        const FacilityRecord& facility);

/// A node of the dependency graph used for closure checks.
struct DependencyNode {
  std::string name{};
  std::vector<std::string> depends_on{};

  template <typename Ar>
  void visit(Ar& ar) {
    ar("name", name);
    ar("depends_on", depends_on);
  }
};

/// Detect a cycle in the named dependency graph. On a cycle the returned error
/// detail lists the cycle path starting at its lexicographically smallest node,
/// so the same graph always reports the same cycle.
[[nodiscard]] Status find_dependency_cycle(const std::vector<DependencyNode>& nodes,
                                           std::vector<std::string>& cycle_path);

/// Topologically ordered names (deterministic: ties broken by name). Returns
/// kDependencyCycle when the graph is cyclic.
[[nodiscard]] Outcome<std::vector<std::string>> dependency_order(
    const std::vector<DependencyNode>& nodes);

}  // namespace cxf

#endif  // CXF_MODEL_DEPENDENCY_HPP
