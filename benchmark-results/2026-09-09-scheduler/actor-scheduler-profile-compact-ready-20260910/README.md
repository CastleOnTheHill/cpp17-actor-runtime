# Final compact-ready profile

Runtime and native event binary match the five-repeat compact-ready gate.
The later sampling diagnostic script does not change the runtime or executable.
Exact runtime/tests/tools source archive, event/fiber/IPC binaries and flags are
preserved locally; SHA256SUMS hashes all three binaries, source archive and perf.data.

Command: perf record -e cpu-clock:u -F 199 --call-graph dwarf,8192 -o perf.data --
taskset -c 0-7 binaries/native_echo --workers 1 --clients 8 --window 16
--payload 64 --rounds 2500000 --warmup 1000 --sample 16 --timeout 110.
Perf: /usr/lib/linux-tools/6.8.0-139-generic/perf.

Conversion: perf script --no-inline -F comm,pid,tid,time,event,ip,sym,dso;
stackcollapse-perf.pl; flamegraph.pl --countname samples. FlameGraph commit:
41fee1f99f9276008b7cd112fca19dc3ea84ac32. Flat report uses --no-children
--no-inline -g none --percent-limit 1; only trailing whitespace was normalized.

Verified: 1445 raw script events = 1445 folded sample weight, zero lost samples,
SVG parses as XML. This is user on-CPU sampling, not kernel or waiting time.
The benchmark phase reports 20M round trips; profiling also includes setup,
warmup, sample sorting and shutdown, and is not the throughput gate measurement.

Large traces, binaries/ and reproduction/*.tar.gz are ignored by Git but retained
locally. A Git push alone will not back up those large artifacts.
