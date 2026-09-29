// Commissioning Fabric - readiness evaluation.
//
// Readiness is a report, never a mutation. Evaluation is pure: it reads the
// candidate, the evidence, the policy of record and the resolved dependencies
// and returns a report. Every dimension it cannot establish is reported as
// unknown with the reason, and never as satisfied: a declared claim is not
// evidence, an expired observation is not a current one, and an observation
// made under an older facility generation describes a world that has moved.
#include "cxf/model/readiness.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cxf/codec/archive.hpp"
#include "cxf/model/candidate.hpp"
#include "cxf/model/dependency.hpp"
#include "cxf/model/evidence.hpp"
#include "cxf/model/lifecycle.hpp"
#include "cxf/support/digest.hpp"
#include "cxf/support/status.hpp"
#include "cxf/types/enums.hpp"
#include "cxf/types/generations.hpp"
#include "cxf/types/ids.hpp"

namespace cxf {
namespace {

using RecordList = std::vector<const EvidenceRecord*>;
using KeyGrouping = std::map<EvidenceKey, RecordList>;

/// True when a source carries enough provenance to establish a fact. A declared
/// source is an assertion by the requester: it is recorded, but discovery is
/// not capability, so it never satisfies a dimension.
[[nodiscard]] constexpr bool may_satisfy(EvidenceSource source) noexcept {
  return source == EvidenceSource::kImported || source == EvidenceSource::kObserved;
}

[[nodiscard]] std::string plural(std::size_t count, std::string_view singular,
                                 std::string_view many) {
  return std::to_string(count) + " " + std::string(count == 1 ? singular : many);
}

/// Evaluation of one (dimension, subject) key.
struct KeyEvaluation {
  EvidenceVerdict verdict{EvidenceVerdict::kUnknown};
  Code code{Code::kOk};
  std::string explanation{};
  EvidenceId witness{};
  EvidenceSource witness_source{EvidenceSource::kDeclared};
  ObservationSequence witness_sequence{};
  std::uint32_t live_records{0};
  std::uint32_t stale_records{0};
  std::uint32_t contradictory_records{0};
};

[[nodiscard]] bool key_satisfied(const DependencyOutcome& outcome) noexcept {
  return outcome.known && !outcome.ambiguous && outcome.actual == outcome.ref.expected;
}

/// Decide one key: contradictory evidence outranks every winner, then the
/// strongest live observation decides, and silence is missing rather than
/// satisfied.
[[nodiscard]] KeyEvaluation evaluate_key(const EvidenceKey& key, const RecordList& records,
                                         const FacilityGenerations& current, Timestamp now) {
  KeyEvaluation out;
  RecordList deciding;
  std::size_t stale = 0;
  std::size_t declared = 0;
  std::size_t undecided = 0;
  for (const EvidenceRecord* record : records) {
    const bool live = liveness_of(*record, current, now) == Liveness::kLive;
    if (!live) {
      ++stale;
    }
    if (!may_satisfy(record->source)) {
      // A declared claim is counted and reported separately, whether or not it
      // is still live: it is never evidence.
      ++declared;
      continue;
    }
    if (!live) {
      continue;
    }
    ++out.live_records;
    if (record->verdict == EvidenceVerdict::kUnknown) {
      ++undecided;
      continue;
    }
    deciding.push_back(record);
  }
  out.stale_records = static_cast<std::uint32_t>(stale);

  if (deciding.empty()) {
    out.verdict = EvidenceVerdict::kUnknown;
    out.code = Code::kEvidenceMissing;
    std::string text = "no live observation establishes subject '" + key.subject + "'";
    if (undecided > 0) {
      text += "; " + plural(undecided, "live observation", "live observations") +
              " recorded an unknown verdict";
    }
    if (stale > 0) {
      text += "; " + plural(stale, "stale record is", "stale records are") +
              " retained for audit (expired or generation-mismatched)";
    }
    if (declared > 0) {
      text += "; " + plural(declared, "declared claim is", "declared claims are") +
              " recorded, but a declared claim is not evidence";
    }
    out.explanation = std::move(text);
    return out;
  }

  bool has_satisfied = false;
  bool has_unsatisfied = false;
  for (const EvidenceRecord* record : deciding) {
    has_satisfied = has_satisfied || record->verdict == EvidenceVerdict::kSatisfied;
    has_unsatisfied = has_unsatisfied || record->verdict == EvidenceVerdict::kUnsatisfied;
  }
  if (has_satisfied && has_unsatisfied) {
    RecordList ordered = deciding;
    std::sort(ordered.begin(), ordered.end(),
              [](const EvidenceRecord* a, const EvidenceRecord* b) { return a->id < b->id; });
    std::string text = "contradictory live observations: ";
    for (std::size_t i = 0; i < ordered.size(); ++i) {
      if (i != 0) {
        text += ", ";
      }
      text += ordered[i]->id.str();
      text += "=";
      text += verdict_name(ordered[i]->verdict);
    }
    out.verdict = EvidenceVerdict::kUnknown;
    out.code = Code::kEvidenceContradictory;
    out.contradictory_records = static_cast<std::uint32_t>(ordered.size());
    out.explanation = std::move(text);
    return out;
  }

  // The winner is unique: outranks() is a strict total order over distinct
  // records, so this does not depend on the order the records were stored in.
  const EvidenceRecord* winner = deciding.front();
  for (std::size_t i = 1; i < deciding.size(); ++i) {
    if (outranks(*deciding[i], *winner)) {
      winner = deciding[i];
    }
  }
  out.witness = winner->id;
  out.witness_source = winner->source;
  out.witness_sequence = winner->observed_sequence;
  if (winner->verdict == EvidenceVerdict::kSatisfied) {
    out.verdict = EvidenceVerdict::kSatisfied;
    out.code = Code::kOk;
    out.explanation = "satisfied by evidence " + winner->id.str() + " from source " +
                      std::string(source_name(winner->source)) + " (sequence " +
                      winner->observed_sequence.str() + ")";
  } else {
    out.verdict = EvidenceVerdict::kUnsatisfied;
    out.code = Code::kEvidenceNegative;
    out.explanation = "evidence " + winner->id.str() + " from source " +
                      std::string(source_name(winner->source)) + " reports unsatisfied";
  }
  return out;
}

/// The dependency dimension is not evidence-driven: it is decided by the typed
/// dependency resolution, so evidence recorded under that dimension is never
/// consulted and is not counted as a live record.
void evaluate_dependency_dimension(DimensionEvaluation& out,
                                   const DependencyResolution& dependencies) {
  if (dependencies.all_required_satisfied() && !dependencies.any_ambiguous()) {
    out.verdict = EvidenceVerdict::kSatisfied;
    out.code = Code::kOk;
    out.explanation = "every required dependency resolved at its expected generation";
    return;
  }
  std::string list;
  for (const DependencyOutcome& entry : dependencies.entries) {
    if (key_satisfied(entry)) {
      continue;
    }
    if (!list.empty()) {
      list += "; ";
    }
    list += dependency_kind_name(entry.ref.kind);
    list += ":";
    list += entry.ref.name;
    list += " expected=";
    list += entry.ref.expected.str();
    list += " actual=";
    list += (entry.known && !entry.ambiguous) ? entry.actual.str() : std::string("unknown");
  }
  out.verdict = EvidenceVerdict::kUnknown;
  out.code = dependencies.any_ambiguous() ? Code::kDependencyAmbiguous
                                          : Code::kDependencyUnsatisified;
  out.explanation = std::string(dependencies.any_ambiguous()
                                    ? "dependency resolution is ambiguous: "
                                    : "unsatisfied dependencies: ") +
                    list;
}

[[nodiscard]] DimensionEvaluation evaluate_dimension(
    EvidenceDimension dimension, bool required, const std::vector<KeyEvaluation>& keys,
    const CommissioningCandidate& candidate, const DependencyResolution& dependencies) {
  DimensionEvaluation out;
  out.dimension = dimension;
  out.required = required;

  if (dimension == EvidenceDimension::kDependency) {
    evaluate_dependency_dimension(out, dependencies);
    return out;
  }

  for (const KeyEvaluation& key : keys) {
    out.live_records += key.live_records;
    out.stale_records += key.stale_records;
    out.contradictory_records += key.contradictory_records;
  }

  // Identity and placement are bindings before they are observations: a
  // satisfied attestation does not bind an asset, and an unbound candidate has
  // no identity to be ready with.
  if (dimension == EvidenceDimension::kIdentity && !candidate.identity.has_value()) {
    out.verdict = EvidenceVerdict::kUnknown;
    out.code = Code::kEvidenceMissing;
    out.explanation = "no identity binding recorded";
    return out;
  }
  if (dimension == EvidenceDimension::kPlacement && !candidate.placement.has_value()) {
    out.verdict = EvidenceVerdict::kUnknown;
    out.code = Code::kEvidenceMissing;
    out.explanation = "no placement binding recorded";
    return out;
  }

  if (keys.empty()) {
    out.verdict = EvidenceVerdict::kUnknown;
    out.code = Code::kEvidenceMissing;
    out.explanation = "no observation recorded for this dimension";
    return out;
  }

  // A dimension with several subjects is satisfied only when every one of them
  // is; the first subject in ascending order that is not satisfied supplies the
  // reason, so the same evidence always reports the same explanation.
  for (const KeyEvaluation& key : keys) {
    if (key.verdict != EvidenceVerdict::kSatisfied) {
      out.verdict = key.verdict;
      out.code = key.code;
      out.explanation = key.explanation;
      out.witness = key.witness;
      out.witness_source = key.witness_source;
      out.witness_sequence = key.witness_sequence;
      return out;
    }
  }

  const KeyEvaluation& first = keys.front();
  out.verdict = EvidenceVerdict::kSatisfied;
  out.code = Code::kOk;
  out.witness = first.witness;
  out.witness_source = first.witness_source;
  out.witness_sequence = first.witness_sequence;
  out.explanation = "satisfied by live observations for " +
                    plural(keys.size(), "subject", "subjects") + " (" +
                    plural(out.live_records, "live record", "live records") + ")";
  return out;
}

}  // namespace

ReadinessPolicy ReadinessPolicy::all_required() noexcept {
  ReadinessPolicy policy;
  policy.required.fill(true);
  return policy;
}

bool ReadinessPolicy::is_required(EvidenceDimension dimension) const noexcept {
  if (!enum_value_valid(dimension)) {
    // The policy says nothing about a dimension outside the domain, and silence
    // is not a requirement.
    return false;
  }
  return required[static_cast<std::size_t>(dimension)];
}

void ReadinessPolicy::set_required(EvidenceDimension dimension, bool value) noexcept {
  if (!enum_value_valid(dimension)) {
    return;
  }
  required[static_cast<std::size_t>(dimension)] = value;
}

std::size_t ReadinessPolicy::required_count() const noexcept {
  std::size_t count = 0;
  for (const bool is_set : required) {
    if (is_set) {
      ++count;
    }
  }
  return count;
}

ReadinessReport evaluate_readiness(const CommissioningCandidate& candidate,
                                   const std::vector<EvidenceRecord>& evidence,
                                   const ReadinessPolicy& policy,
                                   const FacilityGenerations& current,
                                   const DependencyResolution& dependencies, Timestamp now,
                                   ObservationSequence observation) {
  KeyGrouping grouped;
  for (const EvidenceRecord& record : evidence) {
    // Evidence of another candidate is not this candidate's readiness, however
    // similar the dimension and subject look.
    if (record.candidate != candidate.id) {
      continue;
    }
    grouped[key_of(record)].push_back(&record);
  }

  ReadinessReport report;
  report.candidate = candidate.id;
  report.revision = candidate.revision;
  report.incarnation = candidate.incarnation;
  report.lifecycle = candidate.lifecycle;
  report.state = candidate.state;
  report.generations = current;
  report.dependencies = dependencies;
  report.evaluated_at = now;
  report.observation = observation;
  report.dimensions.reserve(kEvidenceDimensionCount);

  // The grouping is ordered by (dimension, subject), so walking it once in the
  // fixed dimension order yields exactly the documented report order.
  auto entry = grouped.begin();
  const std::array<EvidenceDimension, kEvidenceDimensionCount> dimensions =
      all_dimensions();
  for (const EvidenceDimension dimension : dimensions) {
    std::vector<KeyEvaluation> keys;
    while (entry != grouped.end() && entry->first.dimension == dimension) {
      keys.push_back(evaluate_key(entry->first, entry->second, current, now));
      ++entry;
    }
    report.dimensions.push_back(
        evaluate_dimension(dimension, policy.is_required(dimension), keys, candidate,
                           dependencies));
  }

  // A dimension that is not required never blocks activation, but it is also
  // never asserted satisfied: the chain flags carry observed satisfaction only,
  // so a relaxed policy can never talk a candidate past a gate the fabric has
  // no evidence for.
  std::array<bool, kEvidenceDimensionCount> satisfied{};
  bool ready = true;
  for (std::size_t i = 0; i < report.dimensions.size(); ++i) {
    const DimensionEvaluation& dimension = report.dimensions[i];
    satisfied[i] = dimension.verdict == EvidenceVerdict::kSatisfied;
    if (dimension.required && !satisfied[i]) {
      ready = false;
    }
  }
  report.ready = ready;
  report.furthest_state = furthest_reachable_state(satisfied);
  report.digest = compute_report_digest(report);
  return report;
}

Digest compute_report_digest(const ReadinessReport& report) {
  // The digest field is zeroed before encoding: the content address covers
  // every other field of the report, so a decoded report can be re-digested and
  // compared against the value it was stored with.
  ReadinessReport copy = report;
  copy.digest = Digest{};
  const std::vector<std::byte> body = encode_body(copy);
  return DigestBuilder::of(std::span<const std::byte>(body.data(), body.size()));
}

std::vector<const DimensionEvaluation*> blocking_dimensions(const ReadinessReport& report) {
  std::vector<const DimensionEvaluation*> blockers;
  for (const DimensionEvaluation& dimension : report.dimensions) {
    if (dimension.required && dimension.verdict != EvidenceVerdict::kSatisfied) {
      blockers.push_back(&dimension);
    }
  }
  return blockers;
}

std::string first_blocker_summary(const ReadinessReport& report) {
  if (report.ready) {
    return std::string{};
  }
  const std::vector<const DimensionEvaluation*> blockers = blocking_dimensions(report);
  if (blockers.empty()) {
    return std::string{};
  }
  const DimensionEvaluation* first = blockers.front();
  return std::string(dimension_name(first->dimension)) + ": " + first->explanation;
}

}  // namespace cxf
