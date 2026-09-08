"""Longer fixed-count confirmations and one-variable timestamp sensitivity."""
import argparse
import json
import math
from pathlib import Path
import random
import statistics
from run_suite import execute


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    # Fixed common request budgets within each topology, not best-run selection.
    configs = [(b, w, c, n, 16) for b in ('native_echo', 'caf_echo', 'so5_echo')
               for w, c, n in ((1, 1, 6000000), (4, 8, 1500000))]
    configs += [(b, 1, 1, 60000, 16) for b in ('native_ipc_echo', 'raw_ipc_echo')]
    configs += [('native_fiber_echo', 4, 1, 6000, 1),
                ('native_fiber_echo', 4, 8, 100000, 16), ('native_fiber_echo', 4, 64, 10000, 16)]
    # These low-overhead timestamp controls still sample sequence zero.
    configs += [('native_echo', 1, 1, 6000000, 100000000)]
    rows = []
    for repetition in range(5):
        order = list(configs)
        random.Random(1709 + repetition).shuffle(order)
        for b, w, c, n, sample in order:
            cmd = ['taskset', '-c', '0-7', str(a.build / b), '--workers', str(w), '--clients', str(c),
                   '--window', '1', '--payload', '64', '--rounds', str(n), '--warmup', '1000',
                   '--sample', str(sample), '--timeout', '100']
            r = execute(cmd, 120)
            r.update(repetition=repetition, case=dict(binary=b, workers=w, clients=c, sample=sample, rounds=n))
            try:
                m = json.loads(r['stdout'].strip().splitlines()[-1])
                assert r['returncode'] == 0 and m['round_trips'] == n*c
                assert m['rtt_samples'] == math.ceil(n/sample)*c
                r['measurement'] = m
            except (ValueError, IndexError, AssertionError):
                r['validation_error'] = True
            with (a.output / 'raw.jsonl').open('a') as f:
                f.write(json.dumps(r) + '\n')
            if 'validation_error' in r:
                raise RuntimeError(r)
            rows.append(r)
            print(f"confirm {repetition+1}/5 {b} w{w} c{c} stride{sample}: {m['rps']:.0f} rps, {m['seconds']:.3f}s, p99 {m['rtt_p99_us']:.3f}us", flush=True)
    summary = []
    for b, w, c, n, sample in configs:
        case = dict(binary=b, workers=w, clients=c, sample=sample, rounds=n)
        values = [r['measurement'] for r in rows if r['case'] == case]
        summary.append(dict(case=case, runs=len(values), median_rps=statistics.median(m['rps'] for m in values),
                            min_rps=min(m['rps'] for m in values), max_rps=max(m['rps'] for m in values),
                            median_p99_us=statistics.median(m['rtt_p99_us'] for m in values),
                            min_seconds=min(m['seconds'] for m in values),
                            median_cpu_per_wall=statistics.median(m['cpu_seconds']/m['seconds'] for m in values)))
    (a.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f'PASS {len(configs)} confirmations x 5 repeats', flush=True)


if __name__ == '__main__':
    main()
