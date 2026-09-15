# Runtime memory evidence — 2026-09-11

评估报告：[MEMORY_ASSESSMENT_2026-09-11.md](../../docs/MEMORY_ASSESSMENT_2026-09-11.md)。

测量对象：dev `ad2389b10794b7b84ae0b103f58182a9fe7e7007`，未修改运行库实现。最终正式样本 25 配置 × 3 次 = 75 次，全部通过。此前调试/试跑不混入最终汇总。

## 文件

- `raw.jsonl`：逐次命令、退出码、stderr、所有阶段 JSON stdout。
- `summary.json`：逐配置/阶段指标的 median/min/max。`delta_*` 为进程内配对差值后再汇总，基线见报告。
- `manifest.json`：WSL/CPU/compiler/libc 环境、构建 flags、场景参数、源码及探针/静态库/二进制 SHA-256。revision 由 Windows git 验证传入，因为 Windows worktree 的 `.git` 指针不能直接供 Linux git 使用。
- `validation.json`：完成的运行数及测量前后 runtime 源码哈希一致性。
- `binaries/memory_probe`、`binaries/libactor_runtime.a`：本机留存的对应 ELF/静态库，由现有 .gitignore 排除，不上传仓库。

注意：`counts_observed=0` 表示内部表项未读取，其他 count 字段中的 0 只是占位。`post_stop_observer` 通过同一 worker 上的新 Actor 读取旧 timer 队列，包含这个小型观察 Actor 的开销。`trimmed_diagnostic` 只在系统析构后调用 malloc_trim，不参与业务阶段基线。

## WSL 复现

以下在 Ubuntu-24.04 bash 中执行，输出目录必须不存在。运行器仅依赖 Python 标准库；需要 g++、cmake、pthread 和提供 mallinfo2 的 glibc。

```bash
src='/mnt/c/Users/icema/Documents/ChatGPT/下一代ipc框架-dev'
build=$(mktemp -d /tmp/actor-memory-build.XXXXXX)
cmake -S "$src/actor_runtime" -B "$build" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DACTOR_RUNTIME_SANITIZERS=OFF -DACTOR_RUNTIME_COVERAGE=OFF
cmake --build "$build" --target actor_runtime -j4
g++ -std=c++17 -O2 -g -fno-omit-frame-pointer -pthread \
  -I"$src/actor_runtime/include" -I"$src/actor_runtime/src" \
  "$src/actor_runtime/benchmarks/memory_probe.cpp" \
  "$build/libactor_runtime.a" -o "$build/memory_probe"
python3 "$src/actor_runtime/benchmarks/run_memory.py" \
  --binary "$build/memory_probe" --build "$build" --output "$build/results" \
  --revision ad2389b10794b7b84ae0b103f58182a9fe7e7007
```

上述 revision 只适用于对应源码；复测新版本必须由宿主 `git rev-parse HEAD` 替换该值。Linux 原生 Git checkout 可省略 `--revision` 由脚本读取。脚本始终记录实际 src/include 哈希，不能只凭传入 revision 认定代码一致。

单场景接口：`memory_probe mode count payloadBytes stackKiB touchKiB workers rounds`。例如 `memory_probe fibers 1000 0 256 64 1 1`。除 idle/actors 外，内部检查要求 workers=1。资源保护限制默认最大 10 万计数、3000 fibers、128 MiB 测试 payload/额外触栈预算；运行器对每进程设 120 秒超时。

该探针会明确验证本版本的保留行为，**不是修复后的内存有界性回归门禁**。修改结果/等待生命周期后，应按新的 API 契约更新相应数量断言。
