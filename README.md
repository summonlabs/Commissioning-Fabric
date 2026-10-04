# Commissioning Fabric

Commissioning Fabric is the governed-commissioning authority. It decides whether a physical facility asset may enter
service, records exactly which evidence supported that decision, and issues a
bounded, single-use activation permission that is fenced by every generation it
depends on.

The implementation is C++20 with no third-party runtime
dependencies, builds with CMake, and installs a namespaced imported target
(`summon::commissioning_fabric`) for downstream consumption.

## Systems boundary

Commissioning Fabric owns the governed entry of new physical facility assets
into service:

* commissioning candidates and their attempt lineage;
* the ordered commissioning lifecycle from declaration to commissioned service;
* typed readiness dimensions and their evidence;
* generation-bound evidence snapshots and freshness rules;
* readiness evaluation and the plan that freezes an evaluation;
* activation authorization and its fencing, consumption and replay rules;
* the quarantine, release, cancellation and failure paths;
* its own durable store, single-writer exclusion, snapshots and journal;
* the administration command-line interface.

It explicitly does **not** own, and never creates or mutates:

* the canonical Asset Registry (asset identity and registry records);
* the Facility Topology registry and the Rack Registry;
* placement planning (the fabric records a placement binding, it does not choose
  one);
* power control and cooling control (the fabric records readiness evidence and
  emits a bounded activation permission; it does not switch anything);
* network attachment control;
* firmware update execution;
* ASI accelerator scheduling and DFI network routing;
* policy authoring (the fabric consumes a policy generation and policy
  observations, it does not decide policy).

Adjacent systems interact with this runtime in exactly two ways: they supply
typed, generation-stamped observations, and they accept bounded activation
requests. Commissioning Fabric emits records and requests; it never writes into
another system's records.

## Core question

Under the current facility generation, asset identity, placement, dependencies,
compatibility evidence, power, cooling and network readiness, policy and
authority, may this physical asset enter service now - and what exact evidence
remains unsatisfied if not?

The runtime answers that question with a readiness report that names, per
dimension, the verdict, the reason code, the observation that decided it, and
the number of live, stale and contradictory observations behind it.

## Doctrine

These statements are invariants of the implementation, not aspirations. Each one
is enforced by a specific rule and covered by tests.

* Observation is not authority. Only evidence whose source is `imported` or
  `observed` can satisfy a dimension. A `declared` claim is recorded and
  reported, and never satisfies anything.
* Acknowledgement is not effect. A successful activation report moves a
  candidate to `activating`. `commissioned` additionally requires fresh
  observed health evidence taken after that report.
* Requested state is not observed state. Readiness is computed from observations
  bound to the current facility generations, never from a requested value.
* Discovery is not capability. Admission records a declaration; identity is only
  bound from a live identity observation.
* Identified is not compatible, compatible is not placeable, installed is not
  ready, ready is not commissioned.
* Missing, unknown or unmeasured is never converted to satisfied, healthy, ready
  or permitted. A dimension with no live observation is `unknown` with reason
  `EvidenceMissing`.
* Recovered state is not fresh evidence. Recovery restores exactly one
  authoritative generation; evidence is still subject to its own freshness and
  generation binding afterwards.
* Stale authority is fenced rather than inherited. Every generation movement
  that a plan or grant was bound to invalidates it at the next use.
* Idempotent lost-response replay is answered before ordinary staleness
  rejection. Repeating an accepted request id with identical content returns the
  identical stored result - same ids, same digests, same commit sequence.
* Every externally meaningful mutation binds to the exact identity, generation,
  epoch, revision and evidence it was planned against.

## Lifecycle model

States, in chain order:

| State | Asserts | Does not assert |
| --- | --- | --- |
| `declared` | a candidate record exists with a declaration | anything about the physical asset |
| `identified` | identity is bound to a registry identity observed live | compatibility, placement, readiness |
| `located` | a placement binding is recorded from a live observation | power, cooling or network readiness |
| `dependency_validated` | every required dependency resolves to its expected generation | compatibility or readiness |
| `compatibility_validated` | the hardware and firmware baseline is observed compatible | utilities, network, health |
| `utilities_ready` | electrical and cooling readiness are both observed satisfied | network attachment and health |
| `network_ready` | network attachment readiness is observed satisfied | health and policy |
| `health_validated` | health diagnostics are observed satisfied | policy approval or authority |
| `activation_authorized` | a single-use authority token is outstanding | that activation happened |
| `activating` | an activation was reported successful | that the asset is in service |
| `commissioned` | post-activation observed health evidence confirmed service | - |

Exit states are `failed`, `quarantined` and `cancelled`. The chain is only ever
entered in order and never backwards: the evaluator computes the furthest state
the satisfied gates support and records exactly that transition. Releasing a
quarantine opens a new attempt and restarts at `identified`; the previous
attempt is retained in history and never overwritten.

## Evidence and readiness model

Ten dimensions are always evaluated, in this fixed order: identity, placement,
dependency, compatibility, electrical, cooling, network, health, policy,
service class.

Evidence is an observation with a subject, a tri-state verdict
(`unknown`, `satisfied`, `unsatisfied`), a provenance source
(`declared`, `imported`, `observed`), an observation instant, a content
digest, and the facility generations it was observed under.

* Liveness. A record is live only when its recorded facility generations equal
  the current ones and its freshness window has not elapsed relative to the
  evaluator's clock. Expired and generation-mismatched records are retained for
  audit and counted separately; they never contribute to readiness.
* Selection. When several live records describe the same (dimension, subject),
  the deciding observation is chosen by a total order: stronger provenance
  first, then the later observation instant, then the higher accepted sequence,
  then the lower record id. Evaluation therefore never depends on iteration
  order.
* Contradiction. Live `satisfied` and live `unsatisfied` observations of the
  same subject from satisfying sources make the dimension `unknown` with reason
  `EvidenceContradictory`. Submitting the contradicting observation quarantines
  the candidate with reason `contradictory_evidence`.
* Aggregation. A dimension with several subjects is satisfied only when every
  subject is satisfied. An empty subject set is `unknown` with reason
  `EvidenceMissing`.
* Dependencies are not evidence-driven. The dependency dimension is computed
  from the typed facility facts: a required reference is satisfied only by an
  exact generation match, a reference whose name resolves to facts of more than
  one kind is `DependencyAmbiguous`, and a candidate graph with a cycle is
  `DependencyCycle`.
* Identity and placement need both a live observation and a recorded binding.
* A report is content-addressed. Its digest covers every field except the digest
  itself, so any change to the evidence set, the generations or the verdicts
  changes the digest.

## Authority and fencing

Activation authority is granted against a plan. A plan records the candidate
revision, incarnation, lifecycle generation, facility generations, the resolved
dependencies and the complete readiness report, and it has its own validity
window.

A granted token binds the token id, plan id, plan digest, candidate, revision,
incarnation, lifecycle generation, every facility generation, the attempt and the
issue instant. The *binding* is a digest over exactly those fields; the caller
must return it with the activation result, which proves the caller is acting on
the grant that was issued.

Before a grant is used it is revalidated against live state, in this order:
candidate identity, incarnation, lifecycle generation, facility generations,
revision monotonicity, prior consumption, expiry.

* Any movement of a relevant generation fences the grant
  (`FencingTokenStale`).
* A grant is single use. Consuming it is a recorded mutation.
* A deferred result keeps the grant usable and records the deferral; a failed
  result consumes it and fails the candidate; a successful result consumes it
  and moves the candidate to `activating`.
* Repeating an activation request with the same request id returns the stored
  grant rather than reporting `PlanStale`, which is what makes a lost response
  recoverable without a second activation.

## Persistence, recovery and crash consistency

The store directory contains:

    fabric.lock        single-writer lock (OS lock, not a marker file)
    fabric.snapshot    the atomic full-state image
    fabric.journal     append-only commit groups since the snapshot
    .cxf-stage-*       staging files, renamed into place on publication

**Record format.** Every durable byte is one framed record:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `CXF1` |
| 4 | 2 | format version (currently 1) |
| 6 | 2 | frame kind: `commit_group` or `snapshot` |
| 8 | 2 | reserved, must be zero |
| 10 | 8 | commit sequence |
| 18 | 4 | body length |
| 22 | 4 | CRC-32C of the body |
| 26 | 32 | 256-bit digest of the body |
| 58 | .. | body (canonical little-endian field encoding) |

Invalid magic, an unsupported format version, a non-zero reserved field, an
unknown frame kind, a length above the bound, a CRC mismatch or a digest
mismatch is rejected. Trailing bytes after a complete record are rejected. A
final frame that is shorter than it declares is a torn write and is discarded
with a note; an invalid frame followed by more data is corruption and stops
recovery.

**Commit protocol.** An accepted mutation is staged as one commit group, written
as one frame, flushed to the device, read back and verified, and only then
applied to memory and fenced by advancing the durable commit sequence. Nothing
observable to the caller is produced before that point, so a lost response can
always be replayed from the stored result.

**Recovery.** Opening a store loads the snapshot, applies its records, then
replays journal frames with a commit greater than the applied commit, requiring
the sequences to be exactly contiguous; a gap is reported as corruption. Frames
at or below the applied commit are skipped, which is the normal state after a
crash between publishing a snapshot and resetting the journal.

**Bounds.** The in-memory state is bounded, and the bounds are part of the
deterministic replay contract: 16384 retained events, 8192 retained request
results, 16 retained authorities and 4 retained plans per candidate, 4096
evidence records per candidate. Text fields are bounded at 4096 bytes, blob
fields at 1 MiB, frame bodies at 64 MiB.

**Exclusion.** `fabric.lock` is held through an operating-system lock
(a non-shared handle on Windows, `flock(LOCK_EX | LOCK_NB)` elsewhere). A second
process attempting to open the store gets `StorageLocked`. The lock disappears
when the holding process dies, including an unwinding-free kill, so there is no
stale-lock heuristic anywhere in the implementation.

## Error model

Every operation returns an explicit status. Reason codes are stable, ordered by
family, and a request that violates several rules always resolves to the
lowest-numbered applicable code, so the same invalid request reports the same
primary error regardless of map ordering, thread scheduling or unrelated state.

| Range | Family | Examples |
| --- | --- | --- |
| 10-19 | input shape | `MalformedInput`, `FieldMissing`, `FieldTooLong`, `FieldInvalidUtf8`, `FieldConflict`, `ReservedFieldNonZero`, `UnsupportedFormatVersion`, `TrailingBytes` |
| 20-29 | identity | `CandidateNotFound`, `CandidateAlreadyExists`, `AssetIdentityConflict`, `AssetAlreadyCommissioned` |
| 30-39 | lifecycle | `StateNotAllowed`, `TransitionNotAllowed`, `CandidateTerminal` |
| 40-49 | evidence | `EvidenceMissing`, `EvidenceStale`, `EvidenceContradictory`, `EvidenceNegative`, `EvidenceGenerationMismatch` |
| 50-59 | authority | `PlanNotFound`, `PlanStale`, `AuthorityTokenInvalid`, `AuthorityTokenExpired`, `AuthorityTokenConsumed`, `IdempotencyKeyReuse`, `GenerationRegression`, `FencingTokenStale` |
| 60-69 | dependency and compatibility | `DependencyUnsatisfied`, `DependencyCycle`, `DependencyAmbiguous`, `PlacementUnbound`, `CompatibilityUnsupported`, `PolicyNotSatisfied`, `ServiceClassUnsatisfied` |
| 70-79 | storage | `StorageUnavailable`, `StorageCorrupt`, `StorageLocked`, `StorageIo`, `StorageUnsupportedFormat` |
| 80-89 | runtime | `InternalError`, `PreconditionViolated`, `NotImplemented` |

The command-line interface maps those families onto process exit codes, so a
script can distinguish "you asked wrongly" from "the store is damaged".

## Concurrency model

The runtime is single-writer by construction and by OS-level exclusion. One
process holds the store lock; a second process is refused rather than
serialised. Within a process, a `Fabric` instance is not a thread-safe shared
object: the supported concurrency is one mutating owner per store, with any
number of readers of the immutable values it hands out. Reason codes, selection
orders and validation precedences are all deterministic, so concurrent callers
that are correctly excluded observe the same results in the same order.

## Command-line interface

    commissioning-fabric [--store DIR] [--quiet] [--crash-point X] [--crash-at N]
                         [--snapshot-threshold BYTES] COMMAND [key=value ...]
    commissioning-fabric batch FILE|-      # run commands from a file or stdin
    commissioning-fabric help | --help
    commissioning-fabric --version

The store directory is taken from `--store`, otherwise from the `CXF_STORE`
environment variable, otherwise `./cxf-store`.

The command grammar is line-oriented. `#` starts a comment, blank lines are
ignored, fields are whitespace-separated `key=value` pairs, and a value may be
quoted with double quotes to contain spaces.

    init
    store
    admit name=TOKEN [registry=TOKEN] [model=TEXT] [serial=TOKEN]
          [hardware=N] [firmware=N] [dep=KIND:NAME:GEN[:optional]]... [request=N]
    identify candidate=NAME asset=N registry=TOKEN evidence=N
             [model=TEXT] [serial=TOKEN] [hardware=N] [firmware=N] [request=N]
    place candidate=NAME evidence=N site=N rack=N position=TOKEN
          [topology=N] [request=N]
    evidence candidate=NAME dimension=DIM subject=TOKEN verdict=V source=S
             [source_name=TOKEN] [detail=TEXT] [observed=RFC3339]
             [age=SECONDS|never] [site=N] [rack=N] [position=TOKEN] [request=N]
    evaluate candidate=NAME [request=N]
    authorize candidate=NAME plan=N digest=HEX [validity=SECONDS|never] [request=N]
    report candidate=NAME token=N binding=HEX result=succeeded|failed|deferred
           [detail=TEXT] [observed=RFC3339] [request=N]
    quarantine candidate=NAME reason=REASON [detail=TEXT] [request=N]
    release candidate=NAME [detail=TEXT] [request=N]
    cancel candidate=NAME [detail=TEXT] [request=N]
    facility [topology=N] [power=N] [cooling=N] [network=N] [policy=N]
             [dependency=N] [firmware=N] [hardware=N]
             [fact=KIND:NAME:GEN]... [detail=TEXT] [request=N]
    status candidate=NAME
    explain candidate=NAME
    list
    events [limit=N]
    checkpoint
    verify

Output is deterministic `key=value` text. Successful mutations print the
outcome line, the resulting state and, for `evaluate` and `authorize`, the plan
id, plan digest, token id and binding an operator needs for the next command.
Errors print `error code=NAME detail=...` and return the mapped exit code.

A worked commissioning journey:

    commissioning-fabric --store /srv/cxf init
    commissioning-fabric --store /srv/cxf facility topology=4 power=9 cooling=7 \
        network=11 policy=2 dependency=5 fact=rack:rack7:5 fact=power_feed:pdu-a:9
    commissioning-fabric --store /srv/cxf admit name=dc1-rack7-node03 \
        registry=registry-asset-7781 serial=SN-7781 model=trainium-x8 \
        dep=rack:rack7:5 dep=power_feed:pdu-a:9
    commissioning-fabric --store /srv/cxf evidence candidate=dc1-rack7-node03 \
        dimension=identity subject=registry-asset-7781 verdict=satisfied \
        source=observed source_name=asset-registry age=3600
    commissioning-fabric --store /srv/cxf identify candidate=dc1-rack7-node03 \
        asset=7781 registry=registry-asset-7781 evidence=1 serial=SN-7781
    # ... placement, then one observation per readiness dimension ...
    commissioning-fabric --store /srv/cxf evaluate candidate=dc1-rack7-node03
    commissioning-fabric --store /srv/cxf authorize candidate=dc1-rack7-node03 \
        plan=1 digest=<plan digest from evaluate>
    commissioning-fabric --store /srv/cxf report candidate=dc1-rack7-node03 \
        token=1 binding=<binding from authorize> result=succeeded
    commissioning-fabric --store /srv/cxf status candidate=dc1-rack7-node03
    commissioning-fabric --store /srv/cxf explain candidate=dc1-rack7-node03

`examples/commissioning_example.cpp` drives the same journey through the library
API and prints every intermediate observation.

## Building

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure

Options: `CXF_BUILD_TESTS`, `CXF_BUILD_BENCHMARKS`, `CXF_BUILD_EXAMPLES`,
`CXF_WARNINGS_AS_ERRORS` (default on), `CXF_ENABLE_ASAN`, `CXF_ENABLE_UBSAN`.
First-party code is compiled with `/W4 /WX /permissive-` on MSVC and
`-Wall -Wextra -Wpedantic -Werror` and the configured extension set elsewhere.

## Installing and consuming downstream

    cmake --install build --prefix /opt/commissioning-fabric

The install step publishes the headers under `include/cxf`, the library, and a
CMake package config. An independent project consumes it with:

    find_package(CommissioningFabric 1.0 CONFIG REQUIRED)
    target_link_libraries(my_target PRIVATE summon::commissioning_fabric)

`tests/downstream` is exactly such a project: it is not part of the repository
build and only configures against an installed prefix. `tools/validate-package.ps1`
installs the current build into a staging prefix, configures, builds and runs
that consumer, and is registered as the `cxf.package` test.

## Fault injection for durability testing

The runtime can be told to terminate its own process without unwinding at a
named point in the commit path. This exists so that crash consistency is proven
with real process death rather than a simulated exception, and it is documented
behaviour rather than a hidden switch:

```
--crash-point none|before_flush|after_flush|after_publish
--crash-at N            # the 1-based commit index at which to fire
```

`before_flush` fires before the commit frame reaches the journal, `after_flush`
after the frame is durable but before it is published, and `after_publish` after
the commit has been applied and the durable fence advanced. The same triple is
available programmatically through `cxf::FabricOptions`.

## Repository layout

    include/cxf/support/    status, serialisation, CRC, digest, text, clock,
                            filesystem, single-writer lock
    include/cxf/types/      strong ids, generations, enums, freshness windows
    include/cxf/model/      candidate, evidence, dependencies, readiness,
                            lifecycle, durable record vocabulary
    include/cxf/codec/      field-visit archive used by the durable format
    include/cxf/persist/    frame format, journal, snapshot publication
    include/cxf/runtime/    plans, authority, events, fabric, rendering
    include/cxf/tools/      command registry and structured text log
    src/                    implementations of the above
    apps/commissioning-fabric/  administration CLI
    tests/                  proof obligations and the downstream consumer
    benchmarks/             completed-operation measurements
    examples/               end-to-end library usage
    tools/                  package validation script

## Validation performed

The following was executed on this repository, in this working tree, with MSVC
19.44 (Visual Studio 2022 Build Tools), Ninja, Windows. Nothing in this section
is projected or estimated.

**Builds.** Release and Debug both build the library, the CLI, the example and
the benchmark with zero first-party warnings under `/W4 /WX /permissive-`.
A third configuration (RelWithDebInfo with `CXF_ENABLE_ASAN=ON`) builds the
same targets and runs the example and the benchmark under AddressSanitizer with
no sanitizer diagnostic.

**Library behaviour.** The end-to-end example performs the full journey -
facility publication, admission, identity binding from observed evidence,
placement binding, one observation per readiness dimension, evaluation
(`ready=true`, furthest state `health_validated`), authorization, a successful
activation report (`activating`), post-activation health observation, and
commissioning - and then shows a power-generation change fencing the previous
authority (`fenced=power`) while the commissioned asset is unaffected.

**Command-line behaviour.** The same journey was driven through the built CLI in
both argument and batch modes; `status`, `explain`, `list`, `events`,
`store`, `verify` and `checkpoint` were exercised against the resulting store.

**Durability.** A batch run armed with `--crash-point after_flush --crash-at 3`
terminated its process with exit code 97; reopening the store recovered exactly
three commits and `verify` reported `ok code=Ok`. The same batch armed with
`--crash-point before_flush --crash-at 2` left no trace of the unflushed commit.

**Package.** `tools/validate-package.ps1` installed the build into a staging
prefix, configured `tests/downstream` out of tree with
`find_package(CommissioningFabric 1.0 CONFIG REQUIRED)`, built it with the same
warning policy, and ran it against a store it created:
`downstream-consumer: ok dims=10 ready=false commit=3`.

**Automated proof set.** Nine CTest suites, all passing on this host:

| Test | Scope | Result |
| --- | --- | --- |
| `cxf.unit` | 78 tests: support primitives, types, model, persistence | pass |
| `cxf.property` | 7 property tests, seeded, 22898 checks | pass |
| `cxf.randomized` | 2 seeded state-machine tests against a real store | pass |
| `cxf.adversarial` | 29 tests over hostile input and a deliberately corrupted store | pass |
| `cxf.concurrency` | 3 tests with real racing processes and OS lock release on kill | pass |
| `cxf.crash` | 2 tests with real process death at each commit point | pass |
| `cxf.multiprocess` | 2 tests with sequential independent processes on one store | pass |
| `cxf.e2e_cli` | 7 tests driving the built CLI as a child process | pass |
| `cxf.package` | install, out-of-tree find_package consumer, build and run | pass |

The complete automated proof set is registered with CTest. Run it with:

    ctest --test-dir build --output-on-failure

No test declares a timeout. A test that hangs is a defect to diagnose rather
than a test to kill.

## Benchmarks

`benchmarks/fabric_benchmark.cpp` measures completed operations. Nothing here
times submission or enqueue cost. Every durable measurement wraps a full commit:
the operation returned only after its record was staged, flushed to the device,
read back, verified and published.

Methodology:

* one process, one store on this host's filesystem, Ninja Release build (`/O2`);
* percentiles are nearest-rank over the recorded per-operation samples, so they
  can be reproduced from the printed sample counts;
* the workload is generated (SYNTHETIC candidate and subject names) while the
  storage under measurement is real, which is why durable rows are labelled
  `synthetic_workload_real_durability`;
* the pure evaluation row calls the model function directly on synthetic
  in-memory inputs, so it is labelled `synthetic_inputs_real_computation`;
* no before/after claim is made: there is no earlier revision of this repository
  to compare against.

Measured on this build machine (`--candidates 16 --evaluations 256`):

| Benchmark | Ops | Mean | p50 | p95 | p99 | Completed ops/s |
| --- | --- | --- | --- | --- | --- | --- |
| admit_candidate (durable) | 16 | 884.6 us | 874.9 us | 935.9 us | 1047.1 us | 1130 |
| submit_evidence (durable) | 16 | 952.7 us | 878.8 us | 1277.0 us | 1594.3 us | 1050 |
| evaluate_readiness (durable) | 256 | 990.0 us | 926.4 us | 1502.2 us | 1632.1 us | 1010 |
| checkpoint (durable) | 1 | 13.4 ms | - | - | - | 75 |
| open and recover | 1 | 12.2 ms | - | - | - | 82 |
| evaluate_readiness (pure, in memory) | 256 | 11.8 us | 9.4 us | 14.2 us | 58.3 us | 84455 |

The durable rows are dominated by the device flush in the commit path (roughly
0.9 ms per commit on this host), not by the model computation: the same
evaluation costs about 12 us when it does not have to be made durable. Reading
those two rows together is the honest way to read this table.

## Limits and non-goals

* The runtime is single-writer. It provides no shared-state concurrency inside a
  process: one mutating owner per store, with OS-level exclusion between
  processes.
* Plant behaviour - power switching, cooling, firmware, network attachment - is
  not exercised here. Those systems are represented by typed, generation-stamped
  observations and by a bounded activation permission; every claim about
  physical infrastructure is therefore SYNTHETIC in this repository, while the
  process, filesystem, durability, exclusion and packaging claims are REAL and
  were proven on this host.
* The POSIX branch of the filesystem and lock layer exists and compiles, but the
  released validation matrix covers Windows/MSVC only.
* The CRC-32C in the record format detects accidental corruption and truncation.
  It is not an authenticator and the format does not claim to be tamper-proof
  against an attacker who can rewrite a whole store consistently.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
