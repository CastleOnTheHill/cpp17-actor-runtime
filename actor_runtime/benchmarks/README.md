# Linux actor benchmark

见 [实测报告](../../docs/BENCHMARK_REPORT.md)。独立 CMake 工程；不修改/优化运行库基线。
测试需 Linux，本机在 Ubuntu-24.04 WSL 中执行。教学 worktree 不参与构建。

## 构建

在 WSL 中进入开发 worktree，选择新的临时目录；以下依赖已在本机下载。
若换机器，先从 [CAF 0.19.6](https://github.com/actor-framework/actor-framework/archive/refs/tags/0.19.6.tar.gz)
和 [SObjectizer 5.8.3](https://github.com/Stiffstream/sobjectizer/archive/refs/tags/v.5.8.3.tar.gz) 获取并解压源代码。
需要 g++、CMake、pthread、Python3；对照仅编译 CAF core / SO5 static，不需要网络/TLS模块。

```sh
cd /mnt/c/Users/icema/Documents/ChatGPT/下一代ipc框架-dev
cmake -S actor_runtime/benchmarks -B /tmp/actor-benchmark-repro-build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBENCH_CAF_SOURCE=/tmp/actor-benchmark-deps-20260907/actor-framework-0.19.6 \
  -DBENCH_SO5_SOURCE=/tmp/actor-benchmark-deps-20260907/sobjectizer-v.5.8.3
cmake --build /tmp/actor-benchmark-repro-build --target \
  native_echo native_fiber_echo native_ipc_echo raw_ipc_echo caf_echo so5_echo \
  benchmark_harness_tests wake_probe -j6
```

归档 SHA-256（下载内容应匹配，而不只核对版本字符串）：

```text
48dc4c4abf5ab5a7c6f84b9259cc8be1b02c601d31893647ab44e143cdc4b6d5  caf-0.19.6.tar.gz
c76661771c7a0a272ff2cefc62cf3100e9530ec670f225c0e11c6bfa2cc6974e  so5-5.8.3.tar.gz
```

不传两个第三方源码路径也能构建四个 native/raw 程序，但完整 runner 要求六个程序存在。
不启用 runtime 的 sanitizers/coverage；保留 `-O2 -g` 和 frame pointer 便于独立 profiling。

## 测量

所有 `--output` 必须为**尚不存在**的目录，避免覆盖原始证据。CPU集合需按机器调整；本机固定 0–7。

```sh
python3 actor_runtime/benchmarks/run_suite.py \
  --build /tmp/actor-benchmark-repro-build --output /tmp/actor-benchmark-repro-smoke --smoke-only
python3 actor_runtime/benchmarks/run_suite.py \
  --build /tmp/actor-benchmark-repro-build --output /tmp/actor-benchmark-repro-suite \
  --repeats 3 --target-seconds 1 --cpus 0-7 --revision YOUR_WINDOWS_GIT_COMMIT
python3 actor_runtime/benchmarks/confirm_suite.py \
  --build /tmp/actor-benchmark-repro-build --output /tmp/actor-benchmark-repro-confirm
```

`run_suite.py` 是 53 配置矩阵，另有预热/校准/冒烟；`--only native_echo,caf_echo` 可缩小矩阵。
`confirm_suite.py` 为 12 项 × 5 轮固定预算补测，包括极低频时间戳控制；不能从该控制的一个 RTT 样本推断 p99。
按固定种子串行打乱顺序，严禁与编译/其他 benchmark 并跑。
默认目标时长只是校准估算，不是硬性稳态保证；看每次真实 seconds、样本数和 min/max，波动需保留。

单次运行示例：

```sh
taskset -c 0-7 /tmp/actor-benchmark-repro-build/native_echo \
  --workers 4 --clients 8 --window 16 --payload 64 \
  --rounds 100000 --warmup 1000 --sample 16
```

`--hotspot` 让客户端共享一个 echo Actor；否则每客户端一对。
native batch 固定1；CAF/SO5 `--batch 64` 作敏感性对照。fiber 和 IPC 窗口只支持1。
IPC 两个可执行文件内部先 fork 独立服务进程，再启动客户端线程；不会连接外部服务或占用持久路径。
抽象 Unix socket 使用本进程 PID 名称，结束时系统回收；错误时只终止自己创建的子进程。
每个结果为 JSON；stdout/stderr、完整命令、退出码均记录在 raw.jsonl。失败不会被当成零吞吐混入统计。

## 正确性自检与回归

`benchmark_harness_tests` 检查数据损坏、乱序/重复、阶段重置、采样计数、错误和真实超时路径；不依赖被 NDEBUG 禁用的 assert。
完整回归需要先构建 runtime 的测试目标：

```sh
cmake --build /tmp/actor-benchmark-repro-build --target \
  benchmark_harness_tests actor_runtime_tests wait_semantics_tests api_contract_tests \
  order_fulfillment_example lifecycle_tests blocking_tests ipc_lifecycle_tests \
  c_api_process_test c_api_lifecycle_tests blocking_capacity_tests -j6
ctest --test-dir /tmp/actor-benchmark-repro-build --output-on-failure
```

## 火焰图

安装 Linux perf tools（本机实际可执行文件是 `/usr/lib/linux-tools/6.8.0-139-generic/perf`；WSL 的 `/usr/bin/perf` 包装器可能因内核版本不匹配报错）。
获取 [FlameGraph](https://github.com/brendangregg/FlameGraph) 的 `stackcollapse-perf.pl` 和 `flamegraph.pl`。
当前实测 commit、脚本和归档哈希在结果的 reproduction / SHA256.json 中。

```sh
python3 actor_runtime/benchmarks/profile_suite.py \
  --build /tmp/actor-benchmark-repro-build \
  --summary /tmp/actor-benchmark-repro-suite/summary.json \
  --output /tmp/actor-benchmark-repro-profiles \
  --flamegraph /tmp/actor-benchmark-deps-20260907/FlameGraph \
  --perf /usr/lib/linux-tools/6.8.0-139-generic/perf --seconds 5
```

实际采样时长依照未采样中位 RPS 估算请求数；抖动配置可能显著偏离目标，检查 record.json 中 seconds。
所有图必须有非零样本，且 script 事件数与 folded 权重相等。脚本显式省略 perf period 字段，单位是样本，不是纳秒。
用户态图不包含睡眠/内核 CPU；样本过少和未知符号比例在 analysis.json 中保留，不伪造或补齐调用栈。
本次没有修改系统 perf 安全设置；如未来机器不允许采样，应报告失败而非自动降低全局保护。

## 唤醒诊断（不是修复）

```sh
python3 actor_runtime/benchmarks/run_wake_probe.py \
  --build /tmp/actor-benchmark-repro-build --output /tmp/actor-benchmark-repro-wake
```

比较同一真实运行库的 owner-local 恢复，有无每1ms一条全局无关消息。每组40k请求、全量 RTT、三轮配对且交替顺序。
这会改变调度时序，不能作为生产 workaround，也不计入 CAF/SO5 吞吐排名。

## 保存与复核

`archive_results.py --build ... --deps ... --suite ... --confirm ... --profiles ... --probes ... --output ...`
复制完整证据到新目录，保存 source-snapshot.zip、未 strip 的二进制、CTest 日志、实际 flags、依赖归档、转换脚本和 SHA256.json。
源代码快照包含本轮最终的诊断/归档脚本；初次矩阵运行时的 benchmark 文件哈希另保留在 suite/environment.json。
后加 CMake 测试目标/脚本不改变已经测量的六个二进制；用各结果中的二进制 SHA 对照 profiles/binaries 验证。

大型 perf.data/perf.script、二进制及源码/依赖压缩包被根 .gitignore 排除，但**仍保存在本地**；不会自动上传大型数据。
仅 Git push 不能备份所有原始证据，需要额外备份整个结果目录。最终交付没有自动提交或推送本轮内容。
