"""Paired single-variable diagnostic; never mixed into framework rankings."""
import argparse
import json
from pathlib import Path
from run_suite import execute


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    rows = []
    for repetition in range(3):
        for pulse in ((False, True) if repetition % 2 == 0 else (True, False)):
            cmd = ['taskset', '-c', '0-7', str(a.build / 'wake_probe'), '--workers', '4', '--clients', '1',
                   '--window', '1', '--payload', '64', '--rounds', '40000', '--warmup', '1000', '--sample', '1']
            if pulse:
                cmd += ['--pulse']
            r = execute(cmd, 120)
            r.update(pulse=pulse, repetition=repetition)
            with (a.output / 'raw.jsonl').open('a') as f:
                f.write(json.dumps(r) + '\n')
            if r['returncode']:
                raise RuntimeError(r)
            m = json.loads(r['stdout'].strip().splitlines()[-1])
            assert m['round_trips'] == 40000 and m['rtt_samples'] == 40000
            r['measurement'] = m
            rows.append(r)
            print(f"wake pulse={pulse} round={repetition+1}: {m['rps']:.0f} rps, p99={m['rtt_p99_us']:.3f}us, seconds={m['seconds']:.3f}", flush=True)
    (a.output / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    main()
