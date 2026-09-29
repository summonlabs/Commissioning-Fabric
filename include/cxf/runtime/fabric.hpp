// Commissioning Fabric - the governed commissioning runtime.
//
// The fabric owns the commissioning lifecycle of physical facility assets. It
// holds its own durable store, exposes one method per externally meaningful
// mutation, and answers the core question: under the current facility
// generation, identity, placement, dependencies, compatibility evidence,
// power/cooling/network readiness, policy and authority, may this asset enter
// service now - and if not, what exactly remains unsatisfied.
//
// Doctrine enforced by the implementation:
//   - observation is not authority; acknowledgement is not effect;
//   - requested state is not observed state; installed is not active;
//   - drained is not decommissioned; a process exit is not completion;
//   - missing or unknown is never converted to satisfied;
//   - recovered state is not automatically fresh evidence;
//   - stale authority is fenced, not inherited.
#ifndef CXF_RUNTIME_FABRIC_HPP
#define CXF_RUNTIME_FABRIC_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cxf/model/candidate.hpp"
#include "cxf/model/dependency.hpp"
#include "cxf/model/evidence.hpp"
#include "cxf/model/readiness.hpp"
#include "cxf/model/record.hpp"
#include "cxf/runtime/authority.hpp"
#include "cxf/runtime/event.hpp"
#include "cxf/runtime/plan.hpp"
#include "cxf/support/clock.hpp"
#include "cxf/support/digest.hpp"
#include "cxf/support/fs.hpp"
#include "cxf/support/lock_file.hpp"
#include "cxf/support/status.hpp"
#include "cxf/types/enums.hpp"
#include "cxf/types/generations.hpp"
#include "cxf/types/ids.hpp"
#include "cxf/types/timepoint.hpp"

namespace cxf {

/// Durable format version of the store written by this build.
inline constexpr std::uint16_t kStoreFormatVersion = 1;

/// Bounds of the in-memory state. They are part of the deterministic replay
/// contract: replaying the same records always produces the same bounded state.
inline constexpr std::size_t kMaxRetainedRequests = 8192;
inline constexpr std::size_t kMaxRetainedAuthoritiesPerCandidate = 16;
inline constexpr std::size_t kMaxRetainedPlansPerCandidate = 4;

/// Fault injection points used by the durability tests. A run configured with a
/// crash point terminates its own process without unwinding at the named point,
/// which is how crash consistency is proven with real process death rather than
/// with a simulated exception.
enum class CrashPoint : std::uint8_t {
  kNone = 0,
  /// After the commit record has been staged and before it is flushed.
  kBeforeFlush = 1,
  /// After the record has been flushed to the device and before it is published.
  kAfterFlush = 2,
  /// After the commit has been published and the durable fence advanced.
  kAfterPublish = 3,
};

[[nodiscard]] std::string_view crash_point_name(CrashPoint point) noexcept;
[[nodiscard]] Outcome<CrashPoint> parse_crash_point(std::string_view text);

struct FabricOptions {
  /// Store directory. Created when missing if create_if_missing is set.
  fs::Path directory{};
  bool create_if_missing{true};
  /// Informational text written into the store lock file.
  std::string holder{};
  CrashPoint crash_point{CrashPoint::kNone};
  /// Commit index (1-based) at which the configured crash point fires.
  std::uint64_t crash_at_commit{0};
  /// Publish a snapshot once the journal exceeds this many bytes.
  std::uint64_t snapshot_threshold_bytes{4u * 1024u * 1024u};
  ReadinessPolicy policy{};
  FreshnessWindow default_evidence_freshness{
      FreshnessWindow::of(Duration::from_seconds(3600))};
  FreshnessWindow default_plan_validity{FreshnessWindow::of(Duration::from_seconds(900))};
  FreshnessWindow default_authority_validity{
      FreshnessWindow::of(Duration::from_seconds(300))};
};

// ---------------------------------------------------------------------------
// Requests. A request id of zero asks the runtime to allocate one, which is
// reported back in the result; a non-zero id is the caller's idempotency key.
// ---------------------------------------------------------------------------

struct AdmitCandidateRequest {
  RequestId request{};
  std::string name{};
  CandidateDeclaration declaration{};
  std::vector<DependencyRef> dependencies{};
};

struct BindIdentityRequest {
  RequestId request{};
  std::string candidate{};
  /// Live identity attestation this binding is derived from.
  EvidenceId evidence{};
  AssetIdentity identity{};
};

struct BindPlacementRequest {
  RequestId request{};
  std::string candidate{};
  /// Live placement attestation this binding is derived from.
  EvidenceId evidence{};
  PlacementBinding placement{};
};

struct SubmitEvidenceRequest {
  RequestId request{};
  std::string candidate{};
  EvidenceDimension dimension{EvidenceDimension::kIdentity};
  std::string subject{};
  EvidenceVerdict verdict{EvidenceVerdict::kUnknown};
  EvidenceSource source{EvidenceSource::kDeclared};
  std::string source_name{};
  std::string detail{};
  Timestamp observed_at{};
  FreshnessWindow freshness{};
  PlacementBinding placement{};
};

struct EvaluateReadinessRequest {
  RequestId request{};
  std::string candidate{};
};

struct AuthorizeActivationRequest {
  RequestId request{};
  std::string candidate{};
  PlanId plan{};
  Digest plan_digest{};
  FreshnessWindow validity{};
};

struct ReportActivationRequest {
  RequestId request{};
  std::string candidate{};
  TokenId token{};
  Digest binding{};
  ActivationResultKind result{ActivationResultKind::kDeferred};
  std::string detail{};
  /// When the activation attempt was observed to have taken effect.
  Timestamp observed_at{};
};

struct QuarantineRequest {
  RequestId request{};
  std::string candidate{};
  QuarantineReason reason{QuarantineReason::kOperatorRequest};
  std::string detail{};
};

struct ReleaseQuarantineRequest {
  RequestId request{};
  std::string candidate{};
  std::string detail{};
};

struct CancelRequest {
  RequestId request{};
  std::string candidate{};
  std::string detail{};
};

/// A facility change publishes new generations and/or dependency facts. A
/// generation may only move forward; a regression is rejected rather than
/// absorbed.
struct FacilityChangeRequest {
  RequestId request{};
  std::optional<TopologyGeneration> topology{};
  std::optional<PowerGeneration> power{};
  std::optional<CoolingGeneration> cooling{};
  std::optional<NetworkGeneration> network{};
  std::optional<PolicyGeneration> policy{};
  std::optional<DependencyGeneration> dependency{};
  std::optional<FirmwareGeneration> firmware{};
  std::optional<HardwareGeneration> hardware{};
  std::vector<DependencyFact> dependency_facts{};
  std::string detail{};
};

// ---------------------------------------------------------------------------
// Results.
// ---------------------------------------------------------------------------

/// Outcome of an accepted mutation. A rejected mutation is reported as a
/// Status error whose code and detail are the ones stored durably, so a replay
/// reproduces the original answer exactly.
struct RequestOutcome {
  RequestId request{};
  RequestKind kind{RequestKind::kAdmitCandidate};
  Digest request_digest{};
  Digest effect_digest{};
  bool replayed{false};
  bool facility_changed{false};
  CommitSequence commit{};
  EvidenceId evidence{};
  PlanId plan{};
  TokenId token{};
  LifecycleState state{LifecycleState::kDeclared};
  Timestamp recorded_at{};
};

struct ReadinessResult {
  RequestOutcome outcome{};
  PlanId plan{};
  /// Content address of the recorded plan. This is the value an authorization
  /// request must carry: authority binds to the plan, not to the report alone.
  Digest plan_digest{};
  ReadinessReport report{};
};

struct ActivationGrant {
  RequestOutcome outcome{};
  TokenId token{};
  PlanId plan{};
  CandidateId candidate{};
  /// Fencing value the caller must return with the activation result.
  Digest binding{};
  Timestamp issued_at{};
  FreshnessWindow validity{};
};

// ---------------------------------------------------------------------------
// Read-only views.
// ---------------------------------------------------------------------------

struct CandidateSummary {
  CandidateId id{};
  std::string name{};
  LifecycleState state{LifecycleState::kDeclared};
  LifecycleGeneration lifecycle{};
  IncarnationId incarnation{};
  Revision revision{};
  AttemptId attempt{};
  QuarantineReason quarantine_reason{QuarantineReason::kNone};
  PlanId last_plan{};
  Timestamp updated_at{};
};

struct CandidateView {
  CommissioningCandidate candidate{};
  std::vector<EvidenceRecord> evidence{};
  std::optional<ReadinessPlan> plan{};
  std::optional<ActivationAuthority> authority{};
  std::vector<EventRecord> events{};
};

struct DimensionStatus {
  EvidenceDimension dimension{EvidenceDimension::kIdentity};
  bool required{true};
  EvidenceVerdict verdict{EvidenceVerdict::kUnknown};
  std::string explanation{};
};

struct StatusView {
  CandidateSummary summary{};
  bool ready{false};
  LifecycleState furthest_state{LifecycleState::kDeclared};
  std::vector<DimensionStatus> dimensions{};
  std::vector<std::string> blockers{};
  /// Facility generations that moved after the last recorded plan.
  std::vector<std::string> fenced_generations{};
  std::string quarantine_reason{};
  std::string quarantine_detail{};
  bool authority_outstanding{false};
  bool authority_valid{false};
  std::string authority_note{};
  PlanId plan{};
  Digest readiness_digest{};
};

struct StoreStats {
  std::uint64_t candidates{0};
  std::uint64_t evidence{0};
  std::uint64_t plans{0};
  std::uint64_t authorities{0};
  std::uint64_t retained_requests{0};
  std::uint64_t events{0};
  std::uint64_t journal_bytes{0};
  std::uint64_t snapshot_bytes{0};
  std::uint64_t discarded_tail_bytes{0};
  CommitSequence commit{};
  ControlEpoch epoch{};
  bool recovered{false};
};

// ---------------------------------------------------------------------------
// The runtime.
// ---------------------------------------------------------------------------

class Fabric {
 public:
  Fabric(const Fabric&) = delete;
  Fabric& operator=(const Fabric&) = delete;
  Fabric(Fabric&& other) noexcept;
  Fabric& operator=(Fabric&& other) noexcept;
  ~Fabric();

  /// Open the store in the configured directory: create it when absent, or
  /// recover exactly one authoritative generation when it exists. The clock is
  /// retained by reference and must outlive the fabric.
  [[nodiscard]] static Outcome<Fabric> open(const FabricOptions& options, Clock& clock);

  [[nodiscard]] const FabricMeta& meta() const noexcept;
  [[nodiscard]] const FacilityRecord& facility() const noexcept;
  [[nodiscard]] const StoreStats& stats() const noexcept;
  [[nodiscard]] const FabricOptions& options() const noexcept;

  // --- mutations ---------------------------------------------------------
  [[nodiscard]] Outcome<RequestOutcome> admit_candidate(const AdmitCandidateRequest& request);
  [[nodiscard]] Outcome<RequestOutcome> bind_identity(const BindIdentityRequest& request);
  [[nodiscard]] Outcome<RequestOutcome> bind_placement(const BindPlacementRequest& request);
  [[nodiscard]] Outcome<RequestOutcome> submit_evidence(const SubmitEvidenceRequest& request);
  [[nodiscard]] Outcome<ReadinessResult> evaluate_readiness(
      const EvaluateReadinessRequest& request);
  [[nodiscard]] Outcome<ActivationGrant> authorize_activation(
      const AuthorizeActivationRequest& request);
  [[nodiscard]] Outcome<RequestOutcome> report_activation(
      const ReportActivationRequest& request);
  [[nodiscard]] Outcome<RequestOutcome> quarantine(const QuarantineRequest& request);
  [[nodiscard]] Outcome<RequestOutcome> release_quarantine(
      const ReleaseQuarantineRequest& request);
  [[nodiscard]] Outcome<RequestOutcome> cancel(const CancelRequest& request);
  [[nodiscard]] Outcome<RequestOutcome> record_facility_change(
      const FacilityChangeRequest& request);

  // --- queries -----------------------------------------------------------
  [[nodiscard]] Outcome<CandidateView> view(std::string_view candidate_name) const;
  [[nodiscard]] Outcome<std::vector<CandidateSummary>> list_candidates() const;
  /// Readiness as it stands now, without recording a plan or moving a state.
  [[nodiscard]] Outcome<ReadinessReport> preview_readiness(std::string_view candidate_name) const;
  [[nodiscard]] Outcome<StatusView> status(std::string_view candidate_name) const;
  [[nodiscard]] Outcome<std::string> explain(std::string_view candidate_name) const;
  [[nodiscard]] Outcome<std::vector<EventRecord>> recent_events(std::size_t limit) const;

  // --- durability --------------------------------------------------------
  /// Read the durable store back from disk and verify every record.
  [[nodiscard]] Status verify_store() const;
  /// Publish a snapshot of current state and reset the journal.
  [[nodiscard]] Status checkpoint();

 private:
  Fabric();

  struct Impl;
  std::unique_ptr<Impl> impl_{};
};

}  // namespace cxf

#endif  // CXF_RUNTIME_FABRIC_HPP
