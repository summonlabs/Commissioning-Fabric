// Commissioning Fabric - bounded audit event window.
//
// Events explain what the runtime did and when. They carry no authority: no
// decision is ever made from the event log. The in-memory window is bounded so
// that a long-running process cannot grow without limit, and the bound is part
// of the deterministic replay contract.
#ifndef CXF_RUNTIME_EVENT_HPP
#define CXF_RUNTIME_EVENT_HPP

#include <cstddef>
#include <vector>

#include "cxf/model/record.hpp"
#include "cxf/types/ids.hpp"

namespace cxf {

/// How many events are retained in memory and in a snapshot.
inline constexpr std::size_t kMaxRetainedEvents = 16384;

class EventLog {
 public:
  /// Append an event; the oldest events are dropped once the window is full.
  void append(EventRecord record);

  [[nodiscard]] const std::vector<EventRecord>& records() const noexcept { return records_; }
  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
  [[nodiscard]] bool empty() const noexcept { return records_.empty(); }

  /// Highest sequence ever accepted, including events already dropped.
  [[nodiscard]] CommitSequence highest_sequence() const noexcept { return highest_; }

  /// Events for one candidate, oldest first, at most limit entries.
  [[nodiscard]] std::vector<EventRecord> for_candidate(CandidateId candidate,
                                                       std::size_t limit) const;

  /// Replace the whole window (used by recovery and by snapshot load).
  void reset(std::vector<EventRecord> records);

 private:
  std::vector<EventRecord> records_{};
  CommitSequence highest_{};
};

}  // namespace cxf

#endif  // CXF_RUNTIME_EVENT_HPP
