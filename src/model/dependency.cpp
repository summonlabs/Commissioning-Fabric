// Commissioning Fabric - dependency facts and closure.
//
// A dependency is satisfied only by an exact generation match. A name that the
// facility record carries under more than one kind is ambiguous and is reported
// as such: the fabric never picks the kind that happens to make a candidate
// ready. The graph checks are deterministic, so the same facility always yields
// the same cycle and the same order.
#include "cxf/model/dependency.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cxf/support/status.hpp"
#include "cxf/types/enums.hpp"
#include "cxf/types/generations.hpp"

namespace cxf {
namespace {

/// A resolved dependency is satisfied only by a known, unambiguous fact whose
/// generation equals the expectation exactly.
[[nodiscard]] bool outcome_satisfied(const DependencyOutcome& outcome) noexcept {
  return outcome.known && !outcome.ambiguous && outcome.actual == outcome.ref.expected;
}

/// Kinds under which facts of the facility share the given name, in the fixed
/// declaration order of DependencyKind (not in facility order, so the result
/// does not depend on how the facility record was assembled).
[[nodiscard]] std::vector<DependencyKind> kinds_named(const FacilityRecord& facility,
                                                      std::string_view name) {
  std::vector<DependencyKind> kinds;
  for (const DependencyKind kind : all_dependency_kinds()) {
    for (const DependencyFact& fact : facility.dependencies) {
      if (fact.kind == kind && fact.name == name) {
        kinds.push_back(kind);
        break;
      }
    }
  }
  return kinds;
}

[[nodiscard]] std::string join_kinds(const std::vector<DependencyKind>& kinds) {
  std::string text;
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    if (i != 0) {
      text += ", ";
    }
    text += dependency_kind_name(kinds[i]);
  }
  return text;
}

/// Sorted, de-duplicated adjacency over every name the graph mentions, whether
/// it appears as a node or only as a dependency of one. A name with no entry in
/// the node list is a leaf, never an assumed edge.
[[nodiscard]] std::map<std::string, std::vector<std::string>> build_adjacency(
    const std::vector<DependencyNode>& nodes) {
  std::vector<std::string> names;
  names.reserve(nodes.size() * 2);
  for (const DependencyNode& node : nodes) {
    names.push_back(node.name);
    names.insert(names.end(), node.depends_on.begin(), node.depends_on.end());
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());

  std::map<std::string, std::vector<std::string>> adjacency;
  for (const std::string& name : names) {
    adjacency.emplace(name, std::vector<std::string>{});
  }
  for (const DependencyNode& node : nodes) {
    std::vector<std::string>& edges = adjacency[node.name];
    edges.insert(edges.end(), node.depends_on.begin(), node.depends_on.end());
  }
  for (auto& entry : adjacency) {
    std::vector<std::string>& edges = entry.second;
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
  }
  return adjacency;
}

/// Render a closed cycle walk as the canonical "a -> b -> a" spelling.
[[nodiscard]] std::string join_path(const std::vector<std::string>& path) {
  std::string text;
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (i != 0) {
      text += " -> ";
    }
    text += path[i];
  }
  return text;
}

}  // namespace

bool DependencyResolution::all_required_satisfied() const noexcept {
  for (const DependencyOutcome& entry : entries) {
    if (!entry.ref.required) {
      continue;
    }
    if (!outcome_satisfied(entry)) {
      return false;
    }
  }
  return true;
}

std::size_t DependencyResolution::unsatisfied_required() const noexcept {
  std::size_t count = 0;
  for (const DependencyOutcome& entry : entries) {
    if (entry.ref.required && !outcome_satisfied(entry)) {
      ++count;
    }
  }
  return count;
}

bool DependencyResolution::any_ambiguous() const noexcept {
  for (const DependencyOutcome& entry : entries) {
    if (entry.ambiguous) {
      return true;
    }
  }
  return false;
}

DependencyResolution resolve_dependencies(const CommissioningCandidate& candidate,
                                          const FacilityRecord& facility) {
  DependencyResolution resolution;
  resolution.entries.reserve(candidate.dependencies.size());
  for (const DependencyRef& ref : candidate.dependencies) {
    DependencyOutcome outcome;
    outcome.ref = ref;

    const std::vector<DependencyKind> kinds = kinds_named(facility, ref.name);
    if (kinds.size() > 1) {
      // One name, several kinds: the fabric refuses to choose, because choosing
      // would make the answer depend on which kind it preferred.
      outcome.ambiguous = true;
      outcome.detail = "name '" + ref.name + "' is declared as more than one kind: " +
                       join_kinds(kinds);
      resolution.entries.push_back(std::move(outcome));
      continue;
    }

    const std::string kind_text(dependency_kind_name(ref.kind));
    DependencyGeneration actual{};
    bool found = false;
    bool conflicting = false;
    for (const DependencyFact& fact : facility.dependencies) {
      if (fact.kind != ref.kind || fact.name != ref.name) {
        continue;
      }
      if (!found) {
        actual = fact.generation;
        found = true;
      } else if (fact.generation != actual) {
        conflicting = true;
      }
    }

    if (conflicting) {
      // A facility record that carries two generations for one entity cannot
      // be resolved to either of them without inventing an answer.
      outcome.ambiguous = true;
      outcome.detail = "name '" + ref.name + "' is declared as " + kind_text +
                       " with conflicting generations";
    } else if (found) {
      outcome.known = true;
      outcome.actual = actual;
    } else {
      outcome.detail = "no facility fact names '" + ref.name + "' as " + kind_text;
    }
    resolution.entries.push_back(std::move(outcome));
  }
  return resolution;
}

Status find_dependency_cycle(const std::vector<DependencyNode>& nodes,
                             std::vector<std::string>& cycle_path) {
  cycle_path.clear();
  const std::map<std::string, std::vector<std::string>> adjacency = build_adjacency(nodes);

  enum class Color : std::uint8_t { kWhite = 0, kGray = 1, kBlack = 2 };
  std::map<std::string, Color> color;
  for (const auto& entry : adjacency) {
    color.emplace(entry.first, Color::kWhite);
  }

  struct Frame {
    std::string name{};
    std::size_t next{0};
  };

  // Depth-first search with an explicit stack, roots and edges in sorted name
  // order: the first cycle found is therefore always the same one.
  for (const auto& root_entry : adjacency) {
    if (color[root_entry.first] != Color::kWhite) {
      continue;
    }
    std::vector<Frame> path;
    color[root_entry.first] = Color::kGray;
    path.push_back(Frame{root_entry.first, 0});
    while (!path.empty()) {
      const std::string current = path.back().name;
      const std::vector<std::string>& edges = adjacency.at(current);
      const std::size_t next_index = path.back().next;
      if (next_index >= edges.size()) {
        color[current] = Color::kBlack;
        path.pop_back();
        continue;
      }
      path.back().next = next_index + 1;
      const std::string& next = edges[next_index];
      const Color state = color[next];
      if (state == Color::kGray) {
        // The edge points at a node on the current path: the cycle runs from
        // that occurrence to the node being expanded.
        std::size_t start = 0;
        for (std::size_t i = 0; i < path.size(); ++i) {
          if (path[i].name == next) {
            start = i;
            break;
          }
        }
        std::vector<std::string> cycle;
        for (std::size_t i = start; i < path.size(); ++i) {
          cycle.push_back(path[i].name);
        }
        // Rotate so the walk starts at the lexicographically smallest node on
        // the cycle, then close it back onto that node.
        std::size_t smallest = 0;
        for (std::size_t i = 1; i < cycle.size(); ++i) {
          if (cycle[i] < cycle[smallest]) {
            smallest = i;
          }
        }
        std::vector<std::string> closed;
        closed.reserve(cycle.size() + 1);
        for (std::size_t i = 0; i < cycle.size(); ++i) {
          closed.push_back(cycle[(smallest + i) % cycle.size()]);
        }
        closed.push_back(closed.front());
        cycle_path = closed;
        return make_error(Code::kDependencyCycle, join_path(cycle_path),
                          "cycle length=" + std::to_string(cycle.size()));
      }
      if (state == Color::kWhite) {
        color[next] = Color::kGray;
        path.push_back(Frame{next, 0});
      }
    }
  }
  return Status::success();
}

Outcome<std::vector<std::string>> dependency_order(const std::vector<DependencyNode>& nodes) {
  const std::map<std::string, std::vector<std::string>> adjacency = build_adjacency(nodes);

  std::map<std::string, std::size_t> indegree;
  for (const auto& entry : adjacency) {
    indegree.emplace(entry.first, 0);
  }
  for (const auto& entry : adjacency) {
    for (const std::string& edge : entry.second) {
      ++indegree[edge];
    }
  }

  // Kahn's algorithm with a sorted ready set: the smallest ready name is always
  // emitted next, so ties are broken by name rather than by container order.
  std::set<std::string> ready;
  for (const auto& entry : indegree) {
    if (entry.second == 0) {
      ready.insert(entry.first);
    }
  }

  std::vector<std::string> order;
  order.reserve(indegree.size());
  while (!ready.empty()) {
    const std::string name = *ready.begin();
    ready.erase(ready.begin());
    order.push_back(name);
    for (const std::string& edge : adjacency.at(name)) {
      std::size_t& degree = indegree[edge];
      if (degree > 0) {
        --degree;
      }
      if (degree == 0) {
        ready.insert(edge);
      }
    }
  }

  if (order.size() != indegree.size()) {
    // Report the cycle itself, not merely its existence: the operator needs to
    // know which names to break apart.
    std::vector<std::string> cycle_path;
    const Status cycle = find_dependency_cycle(nodes, cycle_path);
    if (cycle.failed()) {
      return cycle;
    }
    return make_error(Code::kDependencyCycle, "the dependency graph contains a cycle",
                      "names=" + std::to_string(indegree.size()));
  }
  return order;
}

}  // namespace cxf
