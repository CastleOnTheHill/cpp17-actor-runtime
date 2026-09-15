#!/usr/bin/env python3
"""Paired same-machine P0 screening, not a substitute for the full acceptance matrix.

Build echo-event/echo-fiber in each supplied directory from its own headers and
library using identical flags. Both binaries use the unmodified default runtime
configuration; this runner never disables mailbox accounting to improve scores.

Each repeat runs one baseline and one candidate measurement back to back, so the
per-repeat ratio cancels most machine drift. The gate compares the median of
those paired ratios; the median-of-arms ratio is still reported for continuity
with earlier runs, but it is far noisier on a shared host.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--baseline-source", type=Path)
    parser.add_argument("--candidate-source", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--rounds", type=int, default=1000000)
    parser.add_argument("--fiber-rounds", type=int, default=100000)
    parser.add_argument("--cpus", default="0-7")
    parser.add_argument("--case", action="append", default=[], choices=(
        "event-single", "event-window", "event-parallel", "event-hotspot", "fiber-single", "fiber-parallel"))
    parser.add_argument("--minimum-ratio", type=float,
                        help="Fail if any case paired-ratio median candidate/baseline throughput is below this ratio")
    args = parser.parse_args()
    if min(args.repeats, args.rounds, args.fiber_rounds) < 1:
        parser.error("repeats and round counts must be positive")
    if bool(args.baseline_source) != bool(args.candidate_source):
        parser.error("provide both source directories or neither")
    if args.minimum_ratio is not None and not (0 < args.minimum_ratio <= 10):
        parser.error("minimum ratio must be in (0, 10]")
    cases = [
        ("event-single", "event", 1, 1, 1, False),
        ("event-window", "event", 1, 1, 8, False),
        ("event-parallel", "event", 4, 8, 8, False),
        ("event-hotspot", "event", 4, 8, 8, True),
        ("fiber-single", "fiber", 1, 1, 1, False),
        ("fiber-parallel", "fiber", 4, 8, 1, False),
    ]
    if args.case:
        cases = [case for case in cases if case[0] in args.case]
    binaries = {label: {kind: directory / ("echo-" + kind) for kind in ("event", "fiber")}
                for label, directory in (("baseline", args.baseline), ("candidate", args.candidate))}
    compile_commands = []
    # Argument arrays avoid an extra Windows/WSL shell expansion layer. The
    # libraries must already be built with the same RelWithDebInfo settings.
    if args.baseline_source:
        for label, source, build in (("baseline", args.baseline_source, args.baseline),
                                     ("candidate", args.candidate_source, args.candidate)):
            for kind in ("event", "fiber"):
                command = ["g++", "-O2", "-g", "-DNDEBUG", "-fno-omit-frame-pointer",
                           "-std=c++17", "-pthread"]
                if kind == "fiber":
                    command.append("-DBENCH_FIBER=1")
                command += ["-I", str(source / "include"), str(source / "benchmarks/native_echo.cpp"),
                            str(build / "libactor_runtime.a"), "-o", str(binaries[label][kind])]
                subprocess.run(command, check=True)
                compile_commands.append(command)
    metadata = {
        "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "kernel": platform.release(), "allowed_cpus": sorted(os.sched_getaffinity(0)),
        "arguments": {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()},
        "binary_sha256": {label: {kind: hashlib.sha256(path.read_bytes()).hexdigest()
                                  for kind, path in paths.items()} for label, paths in binaries.items()},
        "scope": "preliminary event/fiber screening; no claim about all P0 scenarios",
        "compile_commands": compile_commands,
    }
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    records = []
    with (args.output / "runs.jsonl").open("x") as output:
        for name, kind, workers, clients, window, hotspot in cases:
            total = args.fiber_rounds if kind == "fiber" else args.rounds
            for repeat in range(args.repeats):
                for label in (("baseline", "candidate") if repeat % 2 == 0 else ("candidate", "baseline")):
                    command = ["taskset", "-c", args.cpus, str(binaries[label][kind]),
                               "--workers", str(workers), "--clients", str(clients), "--window", str(window),
                               "--rounds", str(max(1, total // clients)), "--warmup", "2000", "--payload", "64",
                               "--sample", "64", "--timeout", "60"]
                    if hotspot:
                        command.append("--hotspot")
                    result = subprocess.run(command, capture_output=True, text=True, timeout=130)
                    record = {"case": name, "repeat": repeat, "label": label, "command": command,
                              "exit_code": result.returncode, "stderr": result.stderr, "stdout": result.stdout}
                    if result.returncode == 0:
                        record["metrics"] = json.loads(result.stdout)
                    output.write(json.dumps(record) + "\n")
                    output.flush()
                    if result.returncode:
                        raise RuntimeError(f"{name}/{label} failed; see runs.jsonl")
                    records.append(record)
                    print(f"{name} {repeat + 1}/{args.repeats} {label}: {record['metrics']['rps']:.0f} rps", flush=True)
    summary = []
    for name, *_ in cases:
        # Records for one case are appended per repeat, so filtering by label
        # keeps the per-repeat pairing intact.
        series = {label: [r["metrics"]["rps"] for r in records
                          if r["case"] == name and r["label"] == label]
                  for label in ("baseline", "candidate")}
        medians = {label: statistics.median(values) for label, values in series.items()}
        ratios = sorted(candidate / baseline
                        for baseline, candidate in zip(series["baseline"], series["candidate"]))
        wins = sum(1 for ratio in ratios if ratio > 1.0)
        summary.append({"case": name, "median_rps": medians,
                        "candidate_over_baseline": medians["candidate"] / medians["baseline"],
                        "paired_ratio_median": statistics.median(ratios),
                        "paired_ratio_min": ratios[0], "paired_ratio_max": ratios[-1],
                        "paired_wins": wins, "paired_losses": len(ratios) - wins})
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    if args.minimum_ratio is not None:
        failures = [item["case"] for item in summary
                    if item["paired_ratio_median"] < args.minimum_ratio]
        if failures:
            print("FAIL throughput gate: " + ", ".join(failures))
            raise SystemExit(2)


if __name__ == "__main__":
    main()
