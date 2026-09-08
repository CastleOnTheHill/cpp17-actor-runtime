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

Current implementation invariants:

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
