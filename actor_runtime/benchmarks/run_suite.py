"""Closed-loop echo comparison. Serial runs; raw evidence retained; no downloaded throughput numbers."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import random
import shlex
import signal
import statistics
import subprocess
import time


def execute(command, timeout=120):
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        out, err = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)  # Only the process group this harness created.
        out, err = process.communicate()
        return {'command': command, 'returncode': -9, 'stdout': out, 'stderr': err + '\nHARNESS_TIMEOUT'}
    return {'command': command, 'returncode': process.returncode, 'stdout': out, 'stderr': err}


def matrix():
    rows = []
    configs = [(1, 1, 1, p, False) for p in (0, 64, 1024)]
    configs += [(w, 1, 1, 64, False) for w in (2, 4)]
    configs += [(w, 8, 1, 64, False) for w in (1, 2, 4)]
    configs += [(w, 8, 16, 64, False) for w in (1, 4)]
    configs += [(4, 8, 16, 64, True), (4, 64, 1, 64, False)]
    for binary in ('native_echo', 'caf_echo', 'so5_echo'):
        for w, c, window, payload, hot in configs:
            rows.append(dict(binary=binary, workers=w, clients=c, window=window, payload=payload, hotspot=hot, batch=1))
    for binary in ('caf_echo', 'so5_echo'):
        for hot in (False, True):
            rows.append(dict(binary=binary, workers=4, clients=8, window=16, payload=64, hotspot=hot, batch=64))
    for w, c in ((1, 1), (2, 1), (4, 1), (4, 8), (4, 64)):
        rows.append(dict(binary='native_fiber_echo', workers=w, clients=c, window=1, payload=64, hotspot=False, batch=1))
    for binary in ('native_ipc_echo', 'raw_ipc_echo'):
        for c, p in ((1, 0), (1, 64), (1, 1024), (8, 64)):
            rows.append(dict(binary=binary, workers=1, clients=c, window=1, payload=p, hotspot=True, batch=1))
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--target-seconds', type=float, default=0.5)
    parser.add_argument('--cpus', default='0-7')
    parser.add_argument('--revision', default='unknown')
    parser.add_argument('--smoke-only', action='store_true')
    parser.add_argument('--only', help='comma-separated binary names; default is all six')
    args = parser.parse_args()
    if args.repeats < 1 or args.target_seconds <= 0:
        parser.error('positive repeats and target seconds required')
    args.build = args.build.resolve()
    args.output.mkdir(parents=True, exist_ok=False)  # Never overwrite earlier evidence.
    def snapshot(cmd):
        r = execute(cmd, 20)
        return {'returncode': r['returncode'], 'stdout': r['stdout'], 'stderr': r['stderr']}
    binaries = sorted({c['binary'] for c in matrix()})
    manifest = {'timestamp_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'revision': args.revision,
                'method': 'closed-loop, per-adapter count calibrated for duration; one round trip = two messages',
                'cpus': args.cpus, 'target_seconds': args.target_seconds, 'repeats': args.repeats,
                'sample_stride': 16, 'latency_percentile': 'sorted sample floor(q*(n-1)); no histogram interpolation',
                'resource_scope': 'RUSAGE_SELF: IPC CPU/RSS exclude server child; wall throughput includes round trip',
                'uname': snapshot(['uname', '-a']), 'cpu': snapshot(['lscpu']), 'compiler': snapshot(['g++', '--version']),
                'cmake': snapshot(['cmake', '--version']),
                'binaries': {b: hashlib.sha256((args.build / b).read_bytes()).hexdigest() for b in binaries},
                'benchmark_sources': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                      for p in Path(__file__).parent.iterdir() if p.is_file()}}
    (args.output / 'environment.json').write_text(json.dumps(manifest, indent=2) + '\n')
    raw = args.output / 'raw.jsonl'
    def command(case, rounds, warmup, sample=16):
        cmd = ['taskset', '-c', args.cpus, str(args.build / case['binary'])]
        for key in ('workers', 'clients', 'window', 'payload', 'batch'):
            cmd += ['--' + key, str(case[key])]
        if case['hotspot']:
            cmd += ['--hotspot']
        return cmd + ['--rounds', str(rounds), '--warmup', str(warmup), '--sample', str(sample), '--timeout', '90']
    def record(case, rounds, warmup, phase, repetition):
        r = execute(command(case, rounds, warmup))
        r.update(case=case, phase=phase, repetition=repetition)
        try:
            value = json.loads(r['stdout'].strip().splitlines()[-1])
            assert r['returncode'] == 0
            assert value['round_trips'] == rounds * case['clients']
            assert value['rtt_samples'] == math.ceil(rounds / 16) * case['clients']
            assert value['rps'] > 0 and value['seconds'] > 0
            r['measurement'] = value
        except (ValueError, IndexError, AssertionError):
            r['validation_error'] = 'exit/count/sample/JSON validation failed'
        with raw.open('a') as f:
            f.write(json.dumps(r) + '\n')
        if 'validation_error' in r:
            raise RuntimeError(f"{shlex.join(r['command'])}\n{r['stderr']}\n{r['stdout']}")
        return r
    # Independently check each adapter's payload/count path and negative CLI behavior.
    smoke = []
    for binary in binaries:
        case = dict(binary=binary, workers=1, clients=1, window=1, payload=64, hotspot=False, batch=1)
        smoke.append(record(case, 128, 16, 'smoke', 0))
        bad = execute([str(args.build / binary), '--workers', '0'])
        if bad['returncode'] == 0:
            raise RuntimeError(binary + ' accepted zero workers')
    (args.output / 'smoke.json').write_text(json.dumps(smoke, indent=2) + '\n')
    if args.smoke_only:
        print('All six adapters passed count/payload/CLI checks.', flush=True)
        return
    cases = matrix()
    if args.only:
        selected = set(args.only.split(','))
        if not selected.issubset(binaries):
            parser.error('unknown binary in --only')
        cases = [c for c in cases if c['binary'] in selected]
    random.Random(20260907).shuffle(cases)
    calibrated = []
    for index, case in enumerate(cases):
        pilot = record(case, 64, 16, 'calibration', 0)['measurement']
        rounds = max(64, min(5000000 // case['clients'], int(64 * args.target_seconds / pilot['seconds'])))
        # Tiny pilot runs are dominated by startup/wakeup. Recalibrate against a
        # substantial run before fixing the count used by every repetition.
        pilot = record(case, rounds, min(1000, max(16, rounds // 20)), 'calibration-refined', 0)['measurement']
        rounds = max(64, min(100000000 // case['clients'], int(rounds * args.target_seconds / pilot['seconds'])))
        warmup = min(1000, max(16, rounds // 20))
        calibrated.append((case, rounds, warmup))
        print(f'calibrate {index+1}/{len(cases)} {case} rounds={rounds}', flush=True)
    measurements = []
    for repetition in range(args.repeats):
        order = list(calibrated)
        random.Random(20260907 + repetition).shuffle(order)
        for index, (case, rounds, warmup) in enumerate(order):
            r = record(case, rounds, warmup, 'measurement', repetition)
            measurements.append(r)
            m = r['measurement']
            print(f"run {repetition+1}.{index+1}/{len(cases)} {case['binary']} w={case['workers']} c={case['clients']} "
                  f"window={case['window']} rps={m['rps']:.0f} seconds={m['seconds']:.3f}", flush=True)
    summaries = []
    for case, rounds, warmup in calibrated:
        values = [r['measurement'] for r in measurements if r['case'] == case]
        summaries.append(dict(case=case, rounds=rounds, warmup=warmup, runs=len(values),
                              median_rps=statistics.median(m['rps'] for m in values),
                              min_rps=min(m['rps'] for m in values), max_rps=max(m['rps'] for m in values),
                              median_rtt_p50_us=statistics.median(m['rtt_p50_us'] for m in values),
                              median_rtt_p99_us=statistics.median(m['rtt_p99_us'] for m in values),
                              min_seconds=min(m['seconds'] for m in values),
                              min_samples=min(m['rtt_samples'] for m in values)))
    (args.output / 'summary.json').write_text(json.dumps(summaries, indent=2) + '\n')
    print(f'PASS: {len(summaries)} cases x {args.repeats} repeats. Raw evidence: {raw}', flush=True)


if __name__ == '__main__':
    main()
