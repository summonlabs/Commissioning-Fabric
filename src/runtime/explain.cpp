#include "cxf/runtime/explain.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cxf/model/readiness.hpp"
#include "cxf/runtime/event.hpp"
#include "cxf/runtime/plan.hpp"
#include "cxf/support/text.hpp"
#include "cxf/types/enums.hpp"

namespace cxf {
namespace {

void append_line(std::string& out, std::string_view line) {
  out.append(line);
  out.push_back('\n');
}

[[nodiscard]] std::string_view bool_name(bool value) noexcept {
  return value ? "true" : "false";
}

[[nodiscard]] std::string_view source_text(const DimensionEvaluation& evaluation) noexcept {
  return evaluation.witness.is_zero() ? std::string_view("none")
                                      : source_name(evaluation.witness_source);
}

/// The evaluation of one dimension, looked up by dimension rather than by
/// position so the rendering is in the fixed domain order even when a report is
/// handed to us out of order.
[[nodiscard]] const DimensionEvaluation* evaluation_of(const ReadinessReport& report,
                                                       EvidenceDimension dimension) {
  for (const DimensionEvaluation& evaluation : report.dimensions) {
    if (evaluation.dimension == dimension) {
      return &evaluation;
    }
  }
  return nullptr;
}

[[nodiscard]] std::string dimension_line(const DimensionEvaluation& evaluation) {
  std::string line = "dimension=";
  line.append(dimension_name(evaluation.dimension));
  line.append(" required=");
  line.append(bool_name(evaluation.required));
  line.append(" verdict=");
  line.append(verdict_name(evaluation.verdict));
  line.append(" code=");
  line.append(code_name(evaluation.code));
  line.append(" witness=");
  line.append(evaluation.witness.is_zero() ? std::string("0") : evaluation.witness.str());
  line.append(" source=");
  line.append(source_text(evaluation));
  line.append(" seq=");
  line.append(evaluation.witness_sequence.str());
  line.append(" live=");
  line.append(to_dec(evaluation.live_records));
  line.append(" stale=");
  line.append(to_dec(evaluation.stale_records));
  line.append(" conflicting=");
  line.append(to_dec(evaluation.contradictory_records));
  line.append(" explanation=");
  line.append(escape_for_output(evaluation.explanation));
  return line;
}

[[nodiscard]] std::string dimension_status_line(const DimensionStatus& status) {
  std::string line = "dimension=";
  line.append(dimension_name(status.dimension));
  line.append(" required=");
  line.append(bool_name(status.required));
  line.append(" verdict=");
  line.append(verdict_name(status.verdict));
  line.append(" explanation=");
  line.append(escape_for_output(status.explanation));
  return line;
}

[[nodiscard]] std::string hex_text(const Digest& digest) { return digest.hex(); }

}  // namespace

std::string render_report(const ReadinessReport& report) {
  std::string out;
  out.append("ready=");
  out.append(bool_name(report.ready));
  out.push_back('\n');
  out.append("state=");
  out.append(state_name(report.state));
  out.push_back('\n');
  out.append("furthest=");
  out.append(state_name(report.furthest_state));
  out.push_back('\n');
  out.append("digest=");
  out.append(hex_text(report.digest));
  out.push_back('\n');

  for (const EvidenceDimension dimension : all_dimensions()) {
    const DimensionEvaluation* evaluation = evaluation_of(report, dimension);
    if (evaluation == nullptr) {
      continue;
    }
    append_line(out, dimension_line(*evaluation));
  }

  for (const DimensionEvaluation* blocker : blocking_dimensions(report)) {
    std::string line = "blocker=";
    line.append(dimension_name(blocker->dimension));
    line.append(": ");
    line.append(escape_for_output(blocker->explanation));
    append_line(out, line);
  }
  return out;
}

std::string render_status(const StatusView& view) {
  std::string out;
  out.append("candidate=");
  out.append(escape_for_output(view.summary.name));
  out.append(" id=");
  out.append(view.summary.id.str());
  out.append(" state=");
  out.append(state_name(view.summary.state));
  out.append(" lifecycle=");
  out.append(view.summary.lifecycle.str());
  out.append(" revision=");
  out.append(view.summary.revision.str());
  out.append(" incarnation=");
  out.append(view.summary.incarnation.str());
  out.append(" attempt=");
  out.append(view.summary.attempt.str());
  out.append(" ready=");
  out.append(bool_name(view.ready));
  out.append(" furthest=");
  out.append(state_name(view.furthest_state));
  out.push_back('\n');

  for (const std::string& blocker : view.blockers) {
    std::string line = "blocker=";
    line.append(escape_for_output(blocker));
    append_line(out, line);
  }
  for (const std::string& fenced : view.fenced_generations) {
    std::string line = "fenced=";
    line.append(escape_for_output(fenced));
    append_line(out, line);
  }
  if (view.summary.state == LifecycleState::kQuarantined) {
    std::string line = "quarantine=reason=";
    line.append(escape_for_output(view.quarantine_reason));
    line.append(" detail=");
    line.append(escape_for_output(view.quarantine_detail));
    append_line(out, line);
  }
  {
    std::string line = "authority=outstanding=";
    line.append(bool_name(view.authority_outstanding));
    line.append(" valid=");
    line.append(bool_name(view.authority_valid));
    line.append(" note=");
    line.append(escape_for_output(view.authority_note));
    append_line(out, line);
  }
  // StatusView carries the dimension subset it can express; the fields that
  // only a full report holds (code, witness, counters) are rendered by
  // render_report.
  for (const DimensionStatus& status : view.dimensions) {
    append_line(out, dimension_status_line(status));
  }
  return out;
}

std::string render_candidate_summary(const CandidateSummary& summary) {
  std::string line = "id=";
  line.append(summary.id.str());
  line.append(" name=");
  line.append(escape_for_output(summary.name));
  line.append(" state=");
  line.append(state_name(summary.state));
  line.append(" lifecycle=");
  line.append(summary.lifecycle.str());
  line.append(" revision=");
  line.append(summary.revision.str());
  line.append(" attempt=");
  line.append(summary.attempt.str());
  line.append(" quarantine=");
  line.append(quarantine_reason_name(summary.quarantine_reason));
  line.append(" plan=");
  line.append(summary.last_plan.str());
  line.append(" updated=");
  line.append(summary.updated_at.to_rfc3339());
  return line;
}

std::string render_event(const EventRecord& event) {
  std::string line = "sequence=";
  line.append(event.sequence.str());
  line.append(" at=");
  line.append(event.at.to_rfc3339());
  line.append(" kind=");
  line.append(event_kind_name(event.kind));
  line.append(" candidate=");
  line.append(event.candidate.str());
  line.append(" attempt=");
  line.append(event.attempt.str());
  line.append(" lifecycle=");
  line.append(event.lifecycle.str());
  line.append(" detail=");
  line.append(escape_for_output(event.detail));
  return line;
}

std::string render_store(const FabricMeta& meta, const StoreStats& stats) {
  std::string out;
  // The store directory is not part of either value this renderer receives, so
  // it is not rendered here: the caller that knows the path prints it.
  out.append("commit=");
  out.append(meta.commit.str());
  out.append(" epoch=");
  out.append(meta.epoch.str());
  out.append(" incarnation=");
  out.append(meta.incarnation.str());
  out.append(" format=");
  out.append(meta.format.str());
  out.append(" created=");
  out.append(meta.created_at.to_rfc3339());
  out.append(" updated=");
  out.append(meta.updated_at.to_rfc3339());
  out.push_back('\n');

  const auto counter = [&out](std::string_view name, std::uint64_t value) {
    out.append(name);
    out.push_back('=');
    out.append(to_dec(value));
    out.push_back('\n');
  };
  counter("candidates", stats.candidates);
  counter("evidence", stats.evidence);
  counter("plans", stats.plans);
  counter("authorities", stats.authorities);
  counter("requests", stats.retained_requests);
  counter("events", stats.events);
  counter("journal_bytes", stats.journal_bytes);
  counter("snapshot_bytes", stats.snapshot_bytes);
  counter("discarded_tail_bytes", stats.discarded_tail_bytes);
  out.append("recovered=");
  out.append(bool_name(stats.recovered));
  out.push_back('\n');
  return out;
}

std::string render_outcome(const RequestOutcome& outcome) {
  std::string line = "request=";
  line.append(outcome.request.str());
  line.append(" kind=");
  line.append(request_kind_name(outcome.kind));
  line.append(" replayed=");
  line.append(bool_name(outcome.replayed));
  line.append(" commit=");
  line.append(outcome.commit.str());
  line.append(" evidence=");
  line.append(outcome.evidence.str());
  line.append(" plan=");
  line.append(outcome.plan.str());
  line.append(" token=");
  line.append(outcome.token.str());
  line.append(" state=");
  line.append(state_name(outcome.state));
  line.append(" effect=");
  line.append(hex_text(outcome.effect_digest));
  return line;
}

}  // namespace cxf
