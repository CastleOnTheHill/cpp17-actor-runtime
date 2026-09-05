"""Clean counters, run CTest, measure real project lines using GCC JSON coverage.

No exclusions or source-line suppression. The gate covers all runtime .cpp/.h
files, not examples/tests or STL headers. Branch coverage is separately reported
(including compiler-generated exception edges), never labeled line coverage.
"""
import argparse
import gzip
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--gcov', default='gcov')
    parser.add_argument('--minimum', type=float, default=90)
    args = parser.parse_args()
    build, source = args.build.resolve(), args.source.resolve()
    # Only disposable compiler counters inside the explicitly selected build.
    for counter in build.rglob('*.gcda'):
        counter.unlink()
    subprocess.run(['ctest', '--test-dir', str(build), '--output-on-failure'], check=True)
    output = build / 'coverage'
    output.mkdir(exist_ok=True)
    files = {}
    for object_file in sorted(build.rglob('*.gcno')):
        # Include inline project code exercised by the tests as well as the library.
        result = subprocess.run([args.gcov, '-j', '-b', '-c', str(object_file)],
                                cwd=output, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)
        archive = output / (object_file.stem + '.gcov.json.gz')
        with gzip.open(archive, 'rt') as stream:
            data = json.load(stream)
        for item in data['files']:
            if not item['lines']:  # 声明专用头文件没有可执行行，不存在覆盖率分母。
                continue
            path = Path(item['file']).resolve()
            if not (path.is_relative_to(source / 'src') or path.is_relative_to(source / 'include')):
                continue
            relative = str(path.relative_to(source))
            records = files.setdefault(relative, {})
            for line in item['lines']:
                record = records.setdefault(line['line_number'], {'count': 0, 'branches': {}})
                record['count'] += line['count']
                for index, branch in enumerate(line.get('branches', [])):
                    key = (line.get('function_name', ''), index)
                    record['branches'][key] = record['branches'].get(key, 0) + branch['count']
    expected = {str(p.relative_to(source)) for p in (source / 'src').glob('*.cpp')}
    if not expected.issubset(files):
        raise RuntimeError(f'Missing runtime translation units: {expected - files.keys()}')
    rows = []
    hits = total = branch_hits = branch_total = 0
    for name, records in sorted(files.items()):
        covered = sum(r['count'] > 0 for r in records.values())
        branches = [n for r in records.values() for n in r['branches'].values()]
        row = {'file': name, 'covered': covered, 'lines': len(records),
               'percent': round(100 * covered / len(records), 2),
               'uncovered_lines': [n for n, r in sorted(records.items()) if not r['count']]}
        rows.append(row)
        hits += covered
        total += len(records)
        branch_hits += sum(n > 0 for n in branches)
        branch_total += len(branches)
    percent = 100 * hits / total
    report = {'scope': 'all runtime src/include C++ files; no exclusions',
              'line_covered': hits, 'line_total': total, 'line_percent': percent,
              'branch_covered': branch_hits, 'branch_total': branch_total,
              'branch_percent': 100 * branch_hits / branch_total if branch_total else 100,
              'minimum_line_percent': args.minimum, 'files': rows}
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    lines = ['# WSL GCC coverage', '', report['scope'], '',
             '| File | Covered / executable lines | Line coverage |', '| --- | ---: | ---: |']
    for row in rows:
        lines.append(f"| {row['file']} | {row['covered']} / {row['lines']} | {row['percent']:.2f}% |")
    lines += ['', f'Line coverage: **{hits}/{total} = {percent:.2f}%** (gate >= {args.minimum:.0f}%).',
              f"Raw gcov branch coverage: {branch_hits}/{branch_total} = {report['branch_percent']:.2f}% (includes C++ exception edges).", '']
    (output / 'report.md').write_text('\n'.join(lines), encoding='utf-8')
    print('\n'.join(lines), flush=True)
    if percent < args.minimum:
        for row in rows:
            if row['uncovered_lines']:
                print(f"UNCOVERED {row['file']}: {row['uncovered_lines']}")
        raise SystemExit('Coverage below required threshold')


if __name__ == '__main__':
    main()
