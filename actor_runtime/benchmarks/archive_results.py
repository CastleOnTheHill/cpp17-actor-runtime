"""Copy completed evidence to durable storage and seal it with SHA-256 hashes."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import shutil
import zipfile
from run_suite import execute


def main():
    p = argparse.ArgumentParser()
    for option in ('build', 'deps', 'suite', 'confirm', 'profiles', 'output', 'probes'):
        p.add_argument('--' + option, type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    for name in ('suite', 'confirm', 'profiles', 'probes'):
        shutil.copytree(getattr(a, name), a.output / name)
    runtime = Path(__file__).resolve().parent.parent
    with zipfile.ZipFile(a.output / 'source-snapshot.zip', 'w', zipfile.ZIP_DEFLATED) as z:
        for directory in ('src', 'include', 'benchmarks', 'tests'):
            for f in (runtime / directory).rglob('*'):
                if f.is_file() and '__pycache__' not in f.parts:
                    z.write(f, f.relative_to(runtime))
        z.write(runtime / 'CMakeLists.txt', 'CMakeLists.txt')
    tools = a.output / 'reproduction'
    tools.mkdir()
    for f in ('caf-0.19.6.tar.gz', 'so5-5.8.3.tar.gz'):
        shutil.copy2(a.deps / f, tools / f)
    for f in ('stackcollapse-perf.pl', 'flamegraph.pl'):
        shutil.copy2(a.deps / 'FlameGraph' / f, tools / f)
    for f in (a.deps / 'FlameGraph').glob('docs/*.txt'):
        if 'license' in f.name.lower():
            shutil.copy2(f, tools / f.name)
    shutil.copy2(a.build / 'CMakeCache.txt', tools / 'CMakeCache.txt')
    shutil.copy2(a.build / 'Testing/Temporary/LastTest.log', tools / 'ctest.log')
    shutil.copy2(a.build / 'wake_probe', a.output / 'profiles/binaries/wake_probe')
    flags = {str(f.relative_to(a.build)): f.read_text() for f in a.build.rglob('flags.make')}
    (tools / 'compile-flags.json').write_text(json.dumps(flags, indent=2) + '\n')
    versions = {name: execute(cmd) for name, cmd in {
        'flamegraph_commit': ['git', '-C', str(a.deps / 'FlameGraph'), 'rev-parse', 'HEAD'],
        'packages': ['dpkg-query', '-W', 'linux-tools-generic', 'linux-tools-common', 'ninja-build',
                     'libgoogle-perftools-dev', 'google-perftools'],
        'perf_event_paranoid': ['cat', '/proc/sys/kernel/perf_event_paranoid'],
        'uname': ['uname', '-a']}.items()}
    (tools / 'versions.json').write_text(json.dumps(versions, indent=2) + '\n')
    rows = json.loads((a.suite / 'summary.json').read_text())
    with (a.output / 'matrix.csv').open('w', newline='') as f:
        flat = [dict(**r['case'], **{k: v for k, v in r.items() if k != 'case'}) for r in rows]
        writer = csv.DictWriter(f, fieldnames=list(flat[0]))
        writer.writeheader(); writer.writerows(flat)
    with (a.output / 'matrix.md').open('w') as f:
        f.write('# Full matrix (three repeats; round trips/s)\n\n')
        f.write('| binary | W/C/window/payload/batch/hotspot | median RPS | min–max RPS | median p99 µs | min seconds |\n|---|---|---:|---:|---:|---:|\n')
        for r in sorted(rows, key=lambda x: (x['case']['binary'], tuple(str(v) for v in x['case'].values()))):
            c = r['case']; config = '/'.join(str(c[k]) for k in ('workers','clients','window','payload','batch','hotspot'))
            f.write(f"| {c['binary']} | {config} | {r['median_rps']:.0f} | {r['min_rps']:.0f}–{r['max_rps']:.0f} | {r['median_rtt_p99_us']:.3f} | {r['min_seconds']:.3f} |\n")
    manifest = {}
    for f in a.output.rglob('*'):
        if f.is_file():
            manifest[str(f.relative_to(a.output))] = dict(bytes=f.stat().st_size, sha256=hashlib.sha256(f.read_bytes()).hexdigest())
    (a.output / 'SHA256.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Sealed {len(manifest)} artifacts, {sum(v["bytes"] for v in manifest.values())/1024**2:.1f} MiB at {a.output}')


if __name__ == '__main__':
    main()
