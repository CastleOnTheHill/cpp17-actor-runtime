"""Matched RTT sampling sensitivity, not a replacement throughput gate.

Changing stride changes clock/sample-collection overhead as well as which
requests are observed. Preserve both outcomes; do not select the nicer p99.
"""
import argparse
import json
from pathlib import Path
import random
import statistics
from compare_so5 import binary_hashes, source_hashes
from run_suite import execute


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--so5', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--total-rounds', type=int, default=4000000)
    args = parser.parse_args()
    if args.repeats < 1 or args.total_rounds < 128 or args.total_rounds % 8:
        parser.error('positive repeats and total rounds >=128 divisible by 8 required')
    binaries = {'native': args.native.resolve(), 'so5': args.so5.resolve()}
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = {'scope': '4 workers, 8 independent clients, window16, 64B, batch1',
                'cpus': '0-7', 'warmup_per_client': 1000,
                'total_rounds': args.total_rounds, 'repeats': args.repeats,
                'strides': [1, 16], 'binaries': binary_hashes(binaries),
                'sources': source_hashes(),
                'caveat': 'stride changes measurement overhead; diagnostic, not ranking'}
    (args.output / 'environment.json').write_text(json.dumps(manifest, indent=2) + '\n')
    rows = []
    rounds = args.total_rounds // 8
    for repetition in range(args.repeats):
        order = [(name, stride) for name in binaries for stride in (1, 16)]
        random.Random(20260910 + repetition).shuffle(order)
        for name, stride in order:
            command = ['taskset', '-c', '0-7', str(binaries[name]),
                       '--workers', '4', '--clients', '8', '--window', '16',
                       '--payload', '64', '--batch', '1', '--rounds', str(rounds),
                       '--warmup', '1000', '--sample', str(stride), '--timeout', '110']
            result = execute(command, 120)
            result.update(framework=name, stride=stride, repetition=repetition)
            try:
                measurement = json.loads(result['stdout'].strip().splitlines()[-1])
                if (result['returncode'] or measurement['round_trips'] != args.total_rounds
                        or measurement['rtt_samples'] != ((rounds + stride - 1) // stride) * 8
                        or measurement['seconds'] <= 0 or measurement['rps'] <= 0):
                    raise ValueError('exit/count/sample/time validation failed')
                result['measurement'] = measurement
            except (ValueError, IndexError, KeyError) as error:
                result['validation_error'] = str(error)
            with (args.output / 'raw.jsonl').open('a') as stream:
                stream.write(json.dumps(result) + '\n')
            if 'validation_error' in result:
                raise RuntimeError(result)
            rows.append(result)
            print(f"{repetition+1}/{args.repeats} {name} stride{stride}: "
                  f"{measurement['rps']:.0f} RPS, p99 {measurement['rtt_p99_us']:.3f} us", flush=True)
    if source_hashes() != manifest['sources'] or binary_hashes(binaries) != manifest['binaries']:
        raise RuntimeError('sources or binaries changed during probe; raw evidence retained')
    summary = []
    for name in binaries:
        for stride in (1, 16):
            values = [r['measurement'] for r in rows if r['framework'] == name and r['stride'] == stride]
            summary.append({'framework': name, 'stride': stride,
                            'median_rps': statistics.median(m['rps'] for m in values),
                            'median_p99_us': statistics.median(m['rtt_p99_us'] for m in values),
                            'min_p99_us': min(m['rtt_p99_us'] for m in values),
                            'max_p99_us': max(m['rtt_p99_us'] for m in values)})
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == '__main__':
    main()
