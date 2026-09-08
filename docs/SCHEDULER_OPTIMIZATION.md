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
