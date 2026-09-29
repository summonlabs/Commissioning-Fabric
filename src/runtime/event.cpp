#include "cxf/runtime/event.hpp"

#include <cstddef>
#include <utility>
#include <vector>

namespace cxf {
namespace {

/// Drop the oldest entries until the window honours its documented bound.
void enforce_window(std::vector<EventRecord>& records) {
  if (records.size() <= kMaxRetainedEvents) {
    return;
  }
  const std::size_t excess = records.size() - kMaxRetainedEvents;
  records.erase(records.begin(), records.begin() + static_cast<std::ptrdiff_t>(excess));
}

}  // namespace

void EventLog::append(EventRecord record) {
  if (record.sequence > highest_) {
    highest_ = record.sequence;
  }
  records_.push_back(std::move(record));
  enforce_window(records_);
}

std::vector<EventRecord> EventLog::for_candidate(CandidateId candidate, std::size_t limit) const {
  std::vector<EventRecord> selected;
  if (limit == 0) {
    return selected;
  }
  for (const EventRecord& record : records_) {
    if (record.candidate != candidate) {
      continue;
    }
    selected.push_back(record);
    if (selected.size() == limit) {
      break;
    }
  }
  return selected;
}

void EventLog::reset(std::vector<EventRecord> records) {
  records_ = std::move(records);
  // The window always keeps the newest events, so the highest sequence it holds
  // is also the highest sequence the store ever accepted.
  highest_ = CommitSequence{};
  for (const EventRecord& record : records_) {
    if (record.sequence > highest_) {
      highest_ = record.sequence;
    }
  }
  enforce_window(records_);
}

}  // namespace cxf
