"""Capture genuine perf stacks separately from throughput measurements."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import time

from run_suite import execute


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--summary', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--flamegraph', type=Path, required=True)
    p.add_argument('--perf', required=True)
    p.add_argument('--seconds', type=float, default=5)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    rows = json.loads(a.summary.read_text())
    choices = [('native_echo', 1, 1), ('native_echo', 4, 8),
               ('native_fiber_echo', 1, 1), ('native_fiber_echo', 4, 1),
               ('caf_echo', 4, 8), ('so5_echo', 4, 8),
               ('native_ipc_echo', 1, 1), ('raw_ipc_echo', 1, 1)]
    metadata = {'timestamp_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                'perf': execute([a.perf, '--version']),
                'flamegraph_commit': execute(['git', '-C', str(a.flamegraph), 'rev-parse', 'HEAD']),
                'event': 'cpu-clock:u', 'frequency_hz': 199, 'stack_bytes': 8192,
                'scope': 'whole process lifetime including warmup/teardown and inherited IPC child; user CPU only',
                'profiles': []}
    for binary, workers, clients in choices:
        row = next(r for r in rows if r['case']['binary'] == binary and r['case']['workers'] == workers
                   and r['case']['clients'] == clients and r['case']['window'] == 1
                   and r['case']['payload'] == 64 and r['case']['batch'] == 1)
        name = f'{binary}-w{workers}-c{clients}'
        d = a.output / name
        d.mkdir()
        rounds = max(128, min(100000000 // clients, int(row['median_rps'] * a.seconds / clients)))
        cmd = [a.perf, 'record', '-e', 'cpu-clock:u', '-F', '199', '--call-graph', 'dwarf,8192',
               '-o', str(d / 'perf.data'), '--', 'taskset', '-c', '0-7', str(a.build / binary),
               '--workers', str(workers), '--clients', str(clients), '--window', '1', '--payload', '64',
               '--rounds', str(rounds), '--warmup', '128', '--sample', '16', '--timeout', '100']
        result = execute(cmd, 120)
        (d / 'record.json').write_text(json.dumps(result, indent=2) + '\n')
        if result['returncode']:
            raise RuntimeError(f'perf recording failed: {result}')
        def convert(command, filename):
            with (d / filename).open('w') as out, (d / (filename + '.stderr')).open('w') as err:
                r = subprocess.run(command, stdout=out, stderr=err, timeout=90)
            if r.returncode:
                raise RuntimeError(f'conversion failed: {command}')
        # Omit period explicitly: stackcollapse otherwise weights cpu-clock by
        # nanoseconds. Our graph is intentionally one unit per sampled stack.
        convert([a.perf, 'script', '-F', 'comm,pid,tid,time,event,ip,sym,dso', '-i', str(d / 'perf.data')], 'perf.script')
        convert([a.perf, 'report', '--stdio', '--no-children', '--percent-limit', '0.5', '-i', str(d / 'perf.data')], 'perf.report.txt')
        convert([a.perf, 'buildid-list', '-i', str(d / 'perf.data')], 'buildids.txt')
        convert(['perl', str(a.flamegraph / 'stackcollapse-perf.pl'), str(d / 'perf.script')], 'stacks.folded')
        convert(['perl', str(a.flamegraph / 'flamegraph.pl'), '--title', name + ' user CPU samples',
                 '--countname', 'samples', str(d / 'stacks.folded')], 'flamegraph.svg')
        stacks = []
        for line in (d / 'stacks.folded').read_text().splitlines():
            stack, count = line.rsplit(' ', 1)
            stacks.append((stack.split(';'), int(count)))
        total = sum(count for _, count in stacks)
        # perf script emits a blank-line-delimited block per sampled event.
        blocks = [b for b in re.split(r'\n\s*\n', (d / 'perf.script').read_text()) if re.search(r'cpu-clock(?::u)?:', b)]
        if total <= 0 or total != len(blocks):
            raise RuntimeError(f'sample conservation failed {name}: folded={total}, script={len(blocks)}')
        leaf, inclusive = Counter(), Counter()
        unknown = unknown_leaf = 0
        def unresolved(frame):
            return 'unknown' in frame.lower() or bool(re.fullmatch(r'0x[0-9a-f]+|\[.*\]', frame))
        for stack, count in stacks:
            leaf[stack[-1]] += count
            for frame in set(stack[1:]):
                inclusive[frame] += count
            if any(unresolved(s) for s in stack):
                unknown += count
            if unresolved(stack[-1]):
                unknown_leaf += count
        stats = {'name': name, 'samples': total, 'script_samples': len(blocks), 'unknown_stack_samples': unknown,
                 'unknown_stack_percent': 100 * unknown / total,
                 'unknown_leaf_percent': 100 * unknown_leaf / total,
                 'top_leaf': leaf.most_common(25), 'top_inclusive': inclusive.most_common(25),
                 'measurement_instrumented': json.loads(result['stdout'].strip().splitlines()[-1]),
                 'files': {f.name: {'bytes': f.stat().st_size, 'sha256': hashlib.sha256(f.read_bytes()).hexdigest()}
                           for f in d.iterdir() if f.is_file()}}
        (d / 'analysis.json').write_text(json.dumps(stats, indent=2) + '\n')
        metadata['profiles'].append(stats)
        (a.output / 'profiles.json').write_text(json.dumps(metadata, indent=2) + '\n')
        print(f'PASS {name}: {total} real samples, {unknown} stacks contain unknown frames', flush=True)
    # Preserve executables with debug symbols for later analysis (do not strip).
    binaries = a.output / 'binaries'
    binaries.mkdir()
    for binary in sorted({b for b, _, _ in choices}):
        shutil.copy2(a.build / binary, binaries / binary)


if __name__ == '__main__':
    main()
