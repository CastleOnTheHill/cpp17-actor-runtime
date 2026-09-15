#!/usr/bin/env python3
"""Serial, fresh-process memory diagnostics; no third-party Python dependencies."""
import argparse
import hashlib
import json
import platform
import statistics
import subprocess
from pathlib import Path


def run(args, **kw):
    return subprocess.run(args, text=True, capture_output=True, check=True, **kw).stdout.strip()


def hashes(paths):
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--revision", help="Windows-managed worktrees: revision verified by host git")
    opts = parser.parse_args()
    if not 1 <= opts.repeats <= 5:
        parser.error("repeats must be 1..5")
    root = Path(__file__).resolve().parents[2]
    opts.output.mkdir(parents=True, exist_ok=False)
    sources = sorted((root / "actor_runtime/src").rglob("*pp")) + sorted((root / "actor_runtime/src").rglob("*.h")) + sorted((root / "actor_runtime/include").rglob("*.h"))
    source_before = hashes(sources)
    manifest = {
        "revision": opts.revision or run(["git", "-C", str(root), "rev-parse", "HEAD"]),
        "status": "Windows-managed worktree; host git verified separately" if opts.revision else run(["git", "-C", str(root), "status", "--short"]),
        "uname": platform.uname()._asdict(), "cpu": run(["lscpu"]),
        "compiler": run(["g++", "--version"]), "libc": run(["ldd", "--version"]),
        "stack_limit_kib": run(["bash", "-c", "ulimit -s"]),
        "max_map_count": Path("/proc/sys/vm/max_map_count").read_text().strip(),
        "memory": run(["free", "-k"]), "repeats": opts.repeats,
        "method": "fresh process per case/repeat, serial; quiescent smaps_rollup snapshots; no sanitizers",
        "source_hashes": source_before,
        "artifact_hashes": hashes([opts.binary, opts.build / "libactor_runtime.a", Path(__file__), Path(__file__).with_name("memory_probe.cpp")]),
        "library_flags": (opts.build / "CMakeFiles/actor_runtime.dir/flags.make").read_text(),
        "probe_flags": "-std=c++17 -O2 -g -fno-omit-frame-pointer -pthread",
    }
    cases = []
    def add(name, mode, n=0, payload=0, stack=256, touch=0, workers=1, rounds=1):
        cases.append((name, [mode, n, payload, stack, touch, workers, rounds]))
    for w in [1, 4, 0]:
        add(f"idle_w{w}", "idle", workers=w)
    for n in [1000, 10000]:
        add(f"actors_{n}", "actors", n)
    add("actor_cycles", "actors", 10000, rounds=5)
    for n in [10000, 100000]:
        for p in [0, 64, 1024]:
            add(f"mailbox_{n}_{p}", "mailbox", n, p)
    for n, stack, touch in [(1000, 256, 0), (2000, 256, 0), (1000, 256, 64), (1000, 64, 0)]:
        add(f"fibers_{n}_s{stack}_t{touch}", "fibers", n, stack=stack, touch=touch)
    for n, p in [(1000, 0), (1000, 4096), (3000, 4096)]:
        add(f"completed_{n}_{p}", "completed", n, p, rounds=3)
    for n in [1000, 3000]:
        add(f"tasks_{n}", "tasks", n, 4096)
    for mode in ["timers", "waiters"]:
        for n in [10000, 100000]:
            add(f"{mode}_{n}", mode, n)
    manifest["cases"] = dict(cases)
    (opts.output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    groups = {}
    with (opts.output / "raw.jsonl").open("w", encoding="utf-8") as raw:
        for name, args in cases:
            samples = []
            for repeat in range(opts.repeats):
                command = [str(opts.binary)] + list(map(str, args))
                proc = subprocess.run(command, text=True, capture_output=True, timeout=120)
                record = {"case": name, "repeat": repeat + 1, "command": command,
                          "returncode": proc.returncode, "stdout": proc.stdout, "stderr": proc.stderr}
                raw.write(json.dumps(record) + "\n")
                raw.flush()
                if proc.returncode != 0:
                    raise RuntimeError(f"{name}: {proc.stderr}")
                stages = [json.loads(line) for line in proc.stdout.splitlines()]
                if stages[-1] != {"stage": "pass"}:
                    raise RuntimeError(f"missing pass: {name}")
                samples.append({s["stage"]: s for s in stages[:-1]})
            groups[name] = {}
            for stage, first in samples[0].items():
                groups[name][stage] = {}
                for field in first:
                    if field == "stage":
                        continue
                    vals = [s[stage][field] for s in samples]
                    groups[name][stage][field] = {"median": statistics.median(vals), "min": min(vals), "max": max(vals)}
                if stage not in ["sizes", "process_baseline"]:
                    base = "system_idle" if args[0] in ["idle", "actors"] else "actor_ready"
                    if base in samples[0]:
                        for field in ["rss_kib", "pss_kib", "vms_kib", "malloc_used_bytes", "vmas"]:
                            vals = [s[stage][field] - s[base][field] for s in samples]
                            groups[name][stage]["delta_" + field] = {"median": statistics.median(vals), "min": min(vals), "max": max(vals)}
            print(f"PASS {name} x{opts.repeats}", flush=True)
            (opts.output / "summary.json").write_text(json.dumps(groups, indent=2), encoding="utf-8")
    if source_before != hashes(sources):
        raise RuntimeError("runtime sources changed during measurement")
    (opts.output / "validation.json").write_text(json.dumps({"all_pass": True, "cases": len(cases), "runs": len(cases) * opts.repeats, "runtime_sources_unchanged": True}, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
