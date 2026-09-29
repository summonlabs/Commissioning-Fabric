// Commissioning Fabric - deterministic human-readable rendering.
//
// Every rendering is a pure function of the value it describes: same input,
// same bytes out. Nothing is summarised away that a reader would need to decide
// what to do next, and nothing is asserted that the runtime does not know.
#ifndef CXF_RUNTIME_EXPLAIN_HPP
#define CXF_RUNTIME_EXPLAIN_HPP

#include <string>

#include "cxf/model/readiness.hpp"
#include "cxf/model/record.hpp"
#include "cxf/runtime/fabric.hpp"

namespace cxf {

/// Multi-line rendering of a readiness report: one line per dimension in fixed
/// order, then the blockers and the generation binding.
[[nodiscard]] std::string render_report(const ReadinessReport& report);

/// Multi-line rendering of the current status of one candidate.
[[nodiscard]] std::string render_status(const StatusView& view);

/// One-line summary of a candidate, as used by list output.
[[nodiscard]] std::string render_candidate_summary(const CandidateSummary& summary);

/// One-line rendering of an event.
[[nodiscard]] std::string render_event(const EventRecord& event);

/// Rendering of the immutable fabric metadata and store counters.
[[nodiscard]] std::string render_store(const FabricMeta& meta, const StoreStats& stats);

/// Rendering of a request outcome.
[[nodiscard]] std::string render_outcome(const RequestOutcome& outcome);

}  // namespace cxf

#endif  // CXF_RUNTIME_EXPLAIN_HPP
