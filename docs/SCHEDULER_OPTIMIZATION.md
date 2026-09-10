# Scheduler optimization work log

Goal: commit the existing implementation/benchmark, fix local wakeups, then reduce
global locking and duplicate scheduling; reach at least 80% of SObjectizer's
throughput in matched benchmark configurations without sacrificing correctness.
This threshold is the working interpretation of “数据接近，相差不大”.

## Baseline

- Existing runtime: `5acad12`; benchmark baseline committed as `1c8681c`.
- Historical measurements remain immutable in benchmark-results/2026-09-08.
- Fresh pre-fix event echo: 4 workers / 8 clients / window1 / 64B,
  800,000 round trips in 1.291608354 s = 619,382.8 RPS; p99 59.754 us.
- No benchmark payload, compiler optimization or comparison semantics changed.
- Large raw perf files and debug binaries remain local as documented; commit
  includes compact evidence and SVGs, not these ignored large files.

## P0: local-only wakeup

Regression uses only public `submitBlocking -> await(task)` APIs, one scheduler
worker, no timeout timer/global Actor traffic. Thirty gated external completions
are released after 2 ms idle; median completion-to-resume must be below 4 ms.
This wide allowance catches periodic ~8 ms recovery without failing on a single
host preemption outlier. It is a latency regression, not a production SLA.

Reproduction command (after building target):

```sh
/tmp/actor-scheduler-fix-20260909/scheduler_wakeup_tests
```

- RED before runtime edit: median **7951.64 us**, exit1:
  `local completion relies on periodic polling instead of notification`.
- Fix: localQueue state now uses queueMutex, also held by the condition-variable
  predicate; predicate checks localQueue in both timer/no-timer paths. Shutdown
  stopping transition is serialized with the same sleeping predicate.
- GREEN: median **75.369 us**, exit0. Full runtime CTest: **11/11 passed**, 1.64 s.
- This first commit intentionally does not optimize shared-queue contention.

## Remaining acceptance work

- [x] Commit existing implementation/benchmark baseline.
- [x] Red/green local-only completion regression and full runtime regression.
- [ ] Reduce global critical sections and duplicate Actor scheduling safely.
- [ ] Exercise concurrent senders, FIFO, no concurrent handler execution,
      self-send, stop/re-register, timer/cancellation and shutdown races.
- [ ] Repeat unchanged matched native/SObjectizer matrix and five-run key cases;
      record all failures/ranges and verify >=80% throughput where applicable.
- [ ] Recheck multiworker fiber wakeup stability, IPC, sanitizers and coverage.
- [ ] Profile final code, retain new raw data, document locks/invariants and results.
- [ ] Commit final fixes and verify clean development worktree.

Diagnosis order: missed local wakeup (reproduced), redundant scheduling/lock
contention (measured), allocation overhead (secondary hypothesis). No claimed
SObjectizer parity or overall goal completion until the matched runs prove it.

## P1 intermediate measurements (not acceptance)

Unchanged quick gate, three repeats, 1,000,000 total round trips per framework,
64B/window1/batch1, fixed affinity 0-7. Short exploratory runs only; final gate
requires the full matched matrix and longer budgets.

| Stage | 1w/1c native RPS / SO5 ratio | 4w/8c native RPS / SO5 ratio |
|---|---:|---:|
| Queue lock/notification overhead reduction | 2,388,965 / 67.3% | 646,565 / 47.2% |
| Plus single dispatch token | 2,134,310 / 63.9% | 574,052 / 41.4% |
| Plus narrow registry/admission lock | 2,216,446 / 61.9% | 865,051 / 64.8% |

All three gates fail the 80% target. The isolated token change did NOT improve
these short runs; do not hide that result. Admission narrowing improves 4w/8c
relative to the original ~619k repro, but is not yet sufficient. Last CTest
run: 12/12 passed (2.13 s), including concurrent producer/self-send regression.

Implementation invariants at commit 1bcfdf0 (superseded below):

- One queued/running DispatchMessage token per Actor, protected by mailboxMutex.
- Token handoff releases execution ownership while holding mailboxMutex, then
  reschedules or clears the token. No producer can strand a racing message.
- Sending resolves a shared ActorControl under registryMutex, then allocates and
  posts outside that global lock. Admission under mailboxMutex checks receiving
  and system accepting state; stop/shutdown close admission before cancellation
  is queued. This orders accepted sends before shutdown's outstanding-job drain.
- Queue selection acquires queueMutex once; sleeping uses actual readiness and
  timer deadlines, not periodic polling. Ordinary completions do not lock/notify
  shutdown state on every job; only final drain needs its wait mutex.

Raw quick gates currently in WSL /tmp/actor-scheduler-gate-{red,token,admission}-20260909;
copy them into a new durable result directory before final handoff.

Important original-spec constraints rechecked: Actor must NOT be permanently
pinned to a worker; suspended fibers remain owner-bound; ActorRef stores only
ActorSystem* and ActorId and sends dynamically through the registry. Preserve
these semantics rather than replacing the scheduler with fixed Actor affinity.

## Worker-local ready queue: idle-peer regression (2026-09-09)

Uncommitted experiments carry the next immutable message in its dispatch job,
replace the execution try-mutex with one acquire CAS/release store (no spin),
and put worker-originated ordinary dispatches in stealable worker-local queues.
Pinned fiber work remains in the owner-only queue. A local scheduling budget of
eight checks other categories regularly; every dispatch still invokes one handler.
These experiments are not yet accepted or a claim of SObjectizer parity.

The first local-queue implementation notified idle peers only for queue size > 1.
That assumes an awake owner is available to run its queued continuation, which
does not hold while that owner is still executing a long handler.

Added a public-interface regression to `scheduler_dispatch_tests`: with two workers,
a publisher allows its peer to park, sends exactly one message to an independent
Actor, then waits at most 500 ms for that Actor to execute before returning.
This bounded handler wait is a scheduling stress fixture, not recommended business
code. No timer/global pulse is added to conceal a missing ready notification.

Actual RED command:

```sh
cmake --build /tmp/actor-scheduler-fix-20260909 --target scheduler_dispatch_tests -j6
ctest --test-dir /tmp/actor-scheduler-fix-20260909 -R '^scheduler_dispatch_tests$' --output-on-failure
```

Result: exit 1, 0.54 s, `one local dispatch did not wake an idle peer while its owner was busy`.

Candidate correction publishes a sequentially consistent ready epoch for every
stealable dispatch before checking the sleeper count. A sleeper registers before
checking that epoch. Thus either it observes publication or the publisher observes
a sleeper and takes the condition-variable mutex before notifying. The latter
closes the predicate-to-wait gap; neither side relies on periodic polling.
Single-worker ready queues remain owner-only and do not require this handshake.

GREEN: all 12 CTest targets passed in 1.67 s, including the idle-peer regression.
After private in-place Message initialization, all 12 passed again in 1.74 s.

The subsequent full 12-configuration matched matrix (3 repeats, 1M round trips)
FAILED 8/12 throughput gates. In particular, 4w/8c/window1 reached 1,747,475 RPS
(129.3% of SO5), but 4w/1c was only 39,395 RPS (5.9%) and the shared-server
4w/8c/window16 case regressed to 98,592 RPS (17.4%). Single-worker 64B was
2,273,344 RPS (63.5%). This is NOT an accepted final scheduler.

Full evidence: `/tmp/actor-scheduler-gate-full-ready-20260909`. The manifest now
captures runtime/benchmark hashes, host/compiler details and available build flags;
the runner rejects source or binary changes during a measurement series.

Hotspot profile: 499 user CPU samples, zero lost, 1,002,607 voluntary context
switches for 1M round trips in 11.862 s. This supports investigating worker
handoff/notification costs, not assigning all elapsed time to mutex samples.
Profile and exact executable: `/tmp/actor-scheduler-profile-hotspot-20260909`.
Next experiment: owner takes newer work, thieves older work, with a periodic
oldest-entry selection to preserve fairness. Its results are not yet available.

### Follow-up diagnosis (still not acceptance)

The locality experiment also failed 8/12 cases: 4w/1c 42,570 RPS, hotspot
108,248 RPS. Bounded 32-pass idle yielding improved those to 76,543 and 258,414
RPS, but adding lock-free readiness hints did not establish an additional benefit
(77,855 and 250,747 RPS respectively). These are three-repeat matched runs; retain
all raw ranges instead of selecting the fastest observation.

Added public-interface tests for an older ready Actor behind a continuing newer
self-send chain, and for workers parking after bounded idle backoff. Both now
pass along with the existing tests: 12/12 CTest targets, 1.90 s.

A diagnostic 1M-request 4w/1c run recorded zero execution-ownership retries,
873,106 steals, 457,913 wait calls and 1,724,541 local-queue notifications across
the four workers. Throughput was 85,769 RPS; this instrumented run is diagnostic,
not acceptance. Thus removing the 50 us ownership-contention fallback is not
supported as a cure for this event-echo case. The next experiment suppresses a
local wake when already-awake workers suffice for the outstanding job count.
It retains the publication/sleep handshake and the busy-publisher regression.

Temporary scheduler counters are enabled only by diagnosticLogging; the native
benchmark opts in via ACTOR_BENCH_DIAGNOSTICS. Remove this temporary instrumentation
before final performance acceptance, or explicitly separate and document its cost.

## Follow-up state and evidence (2026-09-09; not final acceptance)

- Generic wake credits improved sparse throughput, but opportunistic migration
  failed the existing actual-migration test. That assertion was not weakened.
- Each worker now has an acknowledged wake request and its own condition variable.
  Fiber jobs notify only their owner. Ordinary work claims sleeping workers based
  on outstanding work versus awake/already-requested workers; epoch ordering stays.
- Every 64 worker-originated posts, one ordinary job is directed to the next
  worker. This is neither permanent Actor affinity nor a mailbox batch: each job
  still handles one message, and fairness checks occur every 8 local dispatches.
- The dispatch token is atomic Idle/Scheduled/Pending. Producers use mailboxMutex
  for admission/FIFO; the consumer releases execution before Scheduled->Idle CAS.
  A racing producer either marks Pending first or inherits Idle and schedules.
  Pending clears only under mailboxMutex. Empty completion avoids that mutex.
- Pending handoff moves ActorControl ownership after unlocking the mailbox.
  ExecutionGuard clears its pointer before transfer. ActorRef still resolves
  every send through the registry; its public representation remains unchanged.
- A private sendOwned helper avoids an intermediate any move. Public sends keep
  their by-value owning semantics. A new public test retains small/heap messages
  after caller mutation, Actor destruction and system shutdown, through both
  send entry points on 1/4 workers, checking IDs, payload, FIFO and correlation.

Three-repeat, 1M-request full matched matrices, never dropping unfavorable cases:

| Stage | Below 80% SO5 | 1w/8c/window16 RPS | 4w/8c/window16 RPS | Hotspot RPS |
|---|---:|---:|---:|---:|
| pending-transfer | 5/12 | 2,365,439 | 1,592,037 | 572,856 |
| owned-send | 5/12 | 2,494,889 | 1,619,911 | 571,060 |
| credit-transfer (all workers; rejected) | 5/12 | 2,631,501 | 1,909,469 | 392,434 |

Transferring the existing outstanding-job count to a pending successor removes
an atomic increment/decrement pair without a shutdown zero gap. However, applying
it to multiple workers also changes wake-demand accounting; the hotspot regressed
in all three runs (379,950-421,823 RPS). The next revision retains credit transfer
only for a single worker, where no peer wake demand exists. Multiworker sends
retain the old/new counted-job overlap. This revised version requires retesting.

Credit-transfer passed all 12 normal CTest targets (1.58s), then 20 repeats each
of wakeup, dispatch, runtime and API contract tests. The earlier pending-transfer
stage passed ASan/UBSan CTest at 11:35 CST in
`/tmp/actor-scheduler-asan-token-20260909`; that does not validate later edits.
Tests also cover idle-peer wakeup during a busy publisher, bounded idle parking,
oldest-ready fairness, and 20 rounds of concurrent send/stop/name reuse.

Short runs are noisy, not final acceptance. Pending-transfer 4w/64c p99 was
560.385 us versus SO5 63.978 us despite higher throughput. Tail latency remains
an explicit concern. Do not round 79.9% upward to pass or claim parity from one
favorable configuration. No complete-goal claim: longer matrices, remaining
throughput gaps, final sanitizer/coverage/fiber/IPC checks and commit remain.

Each stage has summary.json, raw.jsonl and environment.json under
`/tmp/actor-scheduler-gate-<stage>-20260909`; durable copies belong under
`benchmark-results/2026-09-09-scheduler`. Failed experiments are retained.

### Single-worker credit revision: checkpoint, not completion

The `single-credit` revision passed normal and ASan/UBSan CTest, 12/12 each.
Exact logs are archived in `validation-single-credit`. Its complete three-repeat
matrix failed 3/12 throughput gates: 1w/1c/0B 79.4%, 1w/1c/1024B 71.6%, and
1w/8c/window16/64B 70.9%. Other marginal passes are not considered stable proof.
Hotspot throughput recovered to 573,305 RPS (103.2% SO5); 4w/8c/window1 is
1,769,454 RPS (131.3%). 4w/64c p99 remains 536.947 us versus SO5 72.447 us.
No tail-latency parity or overall completion is claimed.

This checkpoint retains all 21 completed experimental suites, including rejected
versions. The source still contains temporary opt-in diagnostic counters. Before
final acceptance remove those probes, rerun the longer gate and finish the
remaining sanitizer/coverage/fiber/IPC validation against that exact final code.

## Post-checkpoint profiling and cleanup

Checkpoint `ea249a9` committed the implementation, tests, 21 experimental suites
(1,032 raw runs; no nonzero exit/validation-error/nonpositive-throughput entries),
and both 12/12 test logs. Teaching was not modified and no remote push was made.

Temporary scheduler counters and ACTOR_BENCH_DIAGNOSTICS were then removed; the
existing public diagnosticLogging option still controls lifecycle/error logs.
The cleanup stage passed 12/12 CTest (1.65s) but failed 6/12 short throughput gates.
This confirms that marginal short-run passes are not stable acceptance evidence.

A new 1w/8c/window16/64B profile of cleanup used 20M round trips, cpu-clock:u at
199 Hz and DWARF 8192: 1,586 samples, zero lost. sendOwned self samples were 16.14%,
mutex lock/unlock 20.49%, processJob 6.18%, deque emplace 3.22%, and Job destruction
1.64%. These are on-CPU samples, not wall-time or predicted optimization gains.
Some third-party debug-info inline records produced addr2line warnings; the flat
table was also inspected with --no-inline. Exact binary, source archive and raw
profile are in `/tmp/actor-scheduler-profile-cleanup-20260909`.

The next isolated change passes the worker's selected Job by reference into
processJob instead of moving it again. Selection owns it until processing returns;
rescheduling still transfers its shared owners to a separately queued job. Public
APIs, handler count, queue order and execution position are unchanged. This passed
12/12 CTest (1.51s); the job-reference matrix failed 3/12: single-worker 64B 77.5%,
1024B 76.4%, and 8 clients/window16 75.7%. Raw results are archived, not cherry-picked.

The current job-credit experiment generalizes single-worker count transfer to
the first locally posted dispatch successor, not just pending-mailbox successors.
The publishing job retains its one drain obligation through return; only then
can this single worker execute the successor. A per-job guard skips exactly one
decrement if transfer occurred. Further successors count normally. Multiworker
enqueue counting is unchanged. A new burst test posts 128 successors then throws,
repeats 100 times on 1/4 workers, checks FIFO/completion and shuts down to catch
lost or duplicate drain obligations. This experiment still requires measured
results and sanitizer validation; no overall completion is asserted.

### Experiment decision and current validation

The job-credit experiment passed 12/12 normal tests (1.55s) and 20 repetitions
each of dispatch, wakeup, runtime and API tests. It failed 4/12 short throughput
gates and showed no demonstrated benefit: single-worker 64B 2,680,295 RPS,
1024B 2,546,347, 8 clients/window16 2,483,313. It was reverted; its raw evidence
and the new burst/exception/drain regression were retained. Current runtime is
the job-reference version, with temporary instrumentation removed.

Fresh checks after that revert, including the added burst test:

- ASan/UBSan: 12/12 CTest, 2.49s, leak detection enabled.
- Coverage-instrumented CTest: 12/12, 2.11s.
- All runtime src/include executable lines, no exclusions: **1154/1184 = 97.47%**;
  project gate remains 90%. Raw gcov branch coverage is 1169/1880 = 62.18%,
  including C++ exception edges; it is not mislabeled as line coverage.
- Exact reports/logs: `validation-job-reference` in the scheduler evidence root.
- Cleanup profile: `profile-cleanup`; 1,586 script events = 1,586 folded samples;
  generated SVG parses successfully. Snapshot and executable remain locally in
  ignored reproduction/binaries subdirectories, with hashes in SHA256SUMS.

A five-repeat / 4M-request full matched confirmation was requested, but both
the initial launch and the allowed retry timed out in permission approval before
process creation. No running benchmark handle was returned. Do not claim those
longer measurements ran or use the short three-repeat results as their substitute.
Next action is to obtain execution approval and run that unchanged confirmation
matrix, then address its remaining gaps and recheck fiber/IPC performance.

## Long confirmation and latency-oriented experiments

The launch issue recovered on continuation. The committed `883b53b` runtime was
rebuilt, then `confirm-job-reference` completed all 12 configurations with 5
repeats and 4M round trips each. It failed 3/12 throughput gates: single worker
64B 76.6%, 1024B 77.2%, and 8 clients/window16 74.1%. The other nine passed.
4w/64c p99 was 584.026 us versus SO5 63.787 us. All raw results and the manifest
are retained; these long measurements supersede the earlier unexecuted plan.

Subsequent isolated experiments use the complete three-repeat / 1M matrix:

- FIFO: owners and thieves take the oldest ready dispatch; the eight-job check
  for pinned/global/timer work remains. Normal CTest passed 12/12 (1.70s).
  4w/64c p99 decreased to 191.107 us, with throughput 1,756,530 RPS (129.8% SO5).
  Hotspot throughput fell to 492,906 RPS (85.1% SO5); no claim of a free gain.
- FIFO plus 256 bounded idle yields: normal CTest 12/12 (1.59s), including idle
  CPU parking. Sparse 4w/1c p99 was 4.599 us (SO5 3.067) and throughput 724,337.
  CPU seconds/wall second rose from about 1.4 to 3.1-3.3 (SO5 about 2.5).
  Idle backoff is bounded and never retries Actor ownership; ownership failures
  still requeue and sleep. This latency/CPU tradeoff is explicitly retained.
- Empty waiters: avoid calling/hash-building the notification path when the
  Actor-owned wait index is empty. Existing broadcast/snapshot semantics remain
  on the nonempty path. Normal CTest passed 12/12 (1.54s); 5/12 throughput gates
  still failed, so this change alone was insufficient.
- Present metadata: freshly allocated address strings are already empty. Only
  copy present process/name metadata, avoiding a conditional-expression string
  temporary. New API tests first passed against old code and then with the change;
  they retain sender IDs, address strings and payload after sender destruction
  and shutdown for anonymous/named Actors on 1/4 workers. Full CTest 12/12 (2.03s).
  This matrix failed only 1/12: 4w/8c/window16 was 1,606,407 RPS (78.7% SO5).
  Single-worker 64B reached 3,103,210 (84.5%), 1024B 2,687,711 (83.0%), and
  8c/window16 2,788,673 (80.2%). These remain short, exploratory results.

Current experiment separates the global private-publication epoch into per-worker
epochs. Each private queue has one publisher. Before scanning queues, a worker
snapshots all epochs; its final sleeping predicate checks all for changes. SC
publication and SC sleeper registration retain the original notification handshake.
Pinned/global queues still use queueMutex and targeted condition variables. This
removes a global RMW on every private enqueue while adding epoch reads to the
infrequent slow/sleep path. Normal CTest passed 12/12, followed by 20 repetitions
each of scheduler/wakeup/runtime tests. The short matrix still failed 1/12:
4w/8c/window16 reached 1,589,864 RPS (79.70% SO5).

All of FIFO/idle/empty-waiter/metadata/per-worker-epoch work is newer than the
97.47% coverage and sanitizer checkpoint. Revalidate those gates against the
chosen final code; do not treat earlier green reports as proof of later changes.

## Ready hints and striped registry reads

The next isolated change checks synchronized global/local readiness counts and
the owner-only timer heap after the eight-job local budget. If those categories
are empty, it can take another local dispatch without queueMutex. Actual parking
still performs the locked scan and epoch handshake. A regression posts older
work from an external thread into globalQueue before starting a continuous local
chain; the existing <=32-newer-dispatch fairness assertion remains unchanged.
Normal CTest passed 12/12 plus 20 repeated scheduler/wakeup/runtime suites.
The full short matrix failed 1/12: 4w/8c/window16 was 79.9765%, not rounded up
to a pass. Its 1,609,655 RPS was insufficient evidence of improvement alone.

Registry lookup profiling motivated the following independent change. There is
still one authoritative unordered_map and ActorRef still resolves its ID for
every send. Normal C++ sends lock one of 64 cache-line-separated reader mutexes,
copy the ActorControl shared pointer, then release the lock before allocation
and mailbox admission. Registration, erasure and final clear take registryMutex
then every reader mutex in ascending order. Other registry readers still use
registryMutex. This allows concurrent reads while excluding all readers during
rehash/mutation; it does not cache stale Actor references or duplicate the map.
Membership changes now pay for 64 locks (about 4 KiB of reader-lock storage).

The new registry-growth regression sends 8,000 messages to 64 stable Actors from
four external threads while registering and stopping 256 transient Actors.
Normal CTest passed 12/12 (1.73s), then 20 repetitions each of dispatch, wakeup,
runtime and API tests passed (22.72s). The three-repeat / 1M matched matrix:

| Configuration (64B, batch1) | Native RPS | Native / SO5 | p99 native / SO5 (us) |
| --- | ---: | ---: | ---: |
| 1w/1c/window1 | 3,061,957 | 91.5% | 0.717 / 0.618 |
| 4w/8c/window1 | 2,836,676 | 210.6% | 7.716 / 8.339 |
| 1w/8c/window16 | 2,824,212 | 77.3% | 125.262 / 93.962 |
| 4w/8c/window16 | 2,722,533 | 136.2% | 388.862 / 100.034 |
| 4w/8c/window16, shared hotspot | 579,302 | 102.2% | 437.529 / 1110.828 |
| 4w/64c/window1 | 3,786,727 | 284.1% | 79.513 / 67.524 |

Eleven of twelve throughput configurations passed, but the one failing case
and elevated tails remain real gaps. These are exploratory medians, not a claim
of overall superiority to SO5 or a current CAF comparison. All eight new matrices,
including unfavorable experiments and the earlier long confirmation, are kept
under benchmark-results/2026-09-09-scheduler.

### 2026-09-10 long confirmation and refreshed safety gates

`actor-scheduler-gate-confirm-stripes-20260910` completed all 120 matched runs
(12 configurations x 2 frameworks x 5 repeats, 4M round trips per run). Source
and executable hashes were stable throughout. It still fails 1/12: 1w/8c/window16
reaches 2,813,253 RPS versus 3,620,627 (77.70%). This repeats the short-test gap;
do not rerun unchanged code merely to obtain a favorable median.

| Configuration (64B, batch1) | Native RPS | Native / SO5 | p99 native / SO5 (us) |
| --- | ---: | ---: | ---: |
| 1w/1c/window1 | 3,092,471 | 85.94% | 0.671 / 0.559 |
| 1w/8c/window1 | 2,973,493 | 86.86% | 5.516 / 5.489 |
| 4w/8c/window1 | 3,493,064 | 261.01% | 7.393 / 8.400 |
| 1w/8c/window16 | 2,813,253 | 77.70% | 111.194 / 91.441 |
| 4w/8c/window16 | 3,273,295 | 166.93% | 177.014 / 101.978 |
| 4w/8c/window16, shared hotspot | 569,760 | 95.06% | 455.599 / 827.535 |
| 4w/64c/window1 | 3,739,517 | 287.13% | 100.359 / 69.615 |

Latency columns remain medians of per-run p99, not a pooled percentile. The
4w/window16 tail is lower than in the earlier short run, but remains 1.74x SO5;
this is not a blanket latency pass. CPU affinity, batch1, fixed dependency
versions and unlocked WSL host-frequency/load limitations remain unchanged.

Fresh validation includes all changes above and the comment-only clarification
that category round-robin can defer service beyond one local-budget check:

- ASan/UBSan with leak detection: 12/12 CTest, 3.17s.
- Coverage-instrumented CTest: 12/12, 2.39s.
- Runtime src/include lines, no exclusions: **1178/1209 = 97.44%**, gate 90%.
- Raw gcov branches: **1197/1918 = 62.41%**, including C++ exception edges.
- Benchmark harness self-tests: 1/1, 1.01s; all six adapter smoke tests passed.
- Exact logs/reports: `validation-registry-stripes` in the scheduler evidence root.

The complete throughput acceptance criterion is still unachieved. Remaining
work includes the repeatable single-worker backlog gap and tail-latency diagnosis.
Existing green safety gates do not substitute for performance measurements or
prove race freedom.

### Current-code compatibility and comparison measurements

The six-adapter smoke run, 12 fixed-budget configurations x 5 repeats, and the
full 13-configuration fiber/IPC matrix x 3 repeats all completed. Outputs are
`actor-scheduler-compat-smoke-20260910`, `actor-scheduler-compat-confirm-20260910`
and `actor-scheduler-fiber-ipc-matrix-20260910`. The smoke manifest hashes current
native event/fiber/IPC binaries and the preserved CAF/SO5/raw-socket comparators.
The runtime source snapshot and matching native event executable are preserved
in `actor-scheduler-profile-stripes-20260910`; binaries/large traces/archives
remain local and ignored, with SHA256SUMS committed. No runtime changes occurred
during the compatibility, profile or fiber/IPC runs.

Five-repeat matched 64B/window1/batch1 results: 1w/1c native 3,071,855 RPS,
CAF 2,488,518, SO5 3,611,485; 4w/8c native 3,615,531, CAF 5,919,930,
SO5 1,334,100. The native 4w p99 is 7.169 us versus CAF 1.280 and SO5 8.588.
These are current-code comparisons, not reused CAF historical measurements.

The 13-case matrix is separately calibrated per adapter for about one second,
not an equal-request-budget ranking. Fiber results (median RPS / p99 us):
1w/1c 891,950 / 2.192; 2w/1c 249,666 / 103.327;
4w/1c 164,837 / 122.730; 4w/8c 1,125,675 / 115.253;
4w/64c 1,059,999 / 209.372. The latter spans 788,865-1,339,418 RPS,
so substantial variance remains. IPC 0/64/1024B and 8-client count/payload checks
passed. The five-repeat fixed-budget 64B single-client IPC median is 10,508 RPS
(p99 176.249 us), raw socket 29,959 (75.105 us). Do not claim an IPC speedup
comparable to the event scheduler gains.

The six-run wake diagnostic uses 40k round trips each with all RTTs sampled:
without pulse 153,627-179,656 RPS, p99 113.494-122.726 us; with pulse
135,864-173,542 RPS, p99 115.397-140.756 us. No old ~10ms p99 step was
observed in these runs; this finite evidence does not prove an absolute SLA.

Fresh backlog profile: 20M round trips, user cpu-clock at 199Hz with DWARF8192,
1,407 script events = 1,407 folded samples, zero lost samples, valid SVG XML.
Flat no-inline self samples: mutex lock/unlock 18.76%, sendOwned 11.73%,
processJob 7.68%, deque emplace 4.19%. The profile is not a timing-gate run;
it shows on-CPU cost, not kernel/wait time or guaranteed optimization gains.
See BENCHMARK_PROGRESS_2026-09-10.md for the user-facing comparison.
