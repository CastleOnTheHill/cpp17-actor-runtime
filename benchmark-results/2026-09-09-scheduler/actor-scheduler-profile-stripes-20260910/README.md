# Registry-stripes backlog profile

2026-09-10, uncommitted runtime changes following 883b53b; exact source archive
and native executable are saved locally with hashes in SHA256SUMS.
The executable matches the native_echo hash in ../actor-scheduler-compat-smoke-20260910/environment.json.
Source matches the full SO5 gate except for a two-line scheduling comment clarification.

Command: perf record -e cpu-clock:u -F 199 --call-graph dwarf,8192 -o perf.data --
taskset -c 0-7 binaries/native_echo --workers 1 --clients 8 --window 16
--payload 64 --rounds 2500000 --warmup 1000 --sample 16 --timeout 110.
Perf executable: /usr/lib/linux-tools/6.8.0-139-generic/perf.

Conversion: perf script --no-inline -F comm,pid,tid,time,event,ip,sym,dso;
stackcollapse-perf.pl; flamegraph.pl --countname samples.
FlameGraph scripts: /tmp/actor-benchmark-deps-20260907/FlameGraph,
commit 41fee1f99f9276008b7cd112fca19dc3ea84ac32.
Flat report: perf report --stdio --no-children --no-inline -g none --percent-limit 1.

Verified: 1407 raw script events, folded weight 1407, zero lost samples,
flamegraph.svg parses as XML. This is user on-CPU sampling, not blocked time,
kernel time or a throughput acceptance run. Outer libc frames may be unresolved.
No permanent logging or altered runtime behavior was added for this profile.

Large perf.data/perf.script, binaries/ and reproduction/*.tar.gz are ignored by
Git but retained locally. A Git push alone will not back up these artifacts.
