# Benchmark 交付验收记录

2026-09-08，WSL Ubuntu-24.04。开发 worktree 基线 `5acad1251c7dd52ffbceae028889d896717cdd2e`。

| 要求 | 实际证据 |
|---|---|
| 基础 echo、多场景、真实执行 | suite/summary.json 53行，每行3轮；suite/raw.jsonl 保留命令、输出、退出码、校准和正式记录 |
| 主流框架同机对照 | 六个可执行文件，CAF 0.19.6 / SO5 5.8.3；固定源归档SHA与实际编译flags |
| 延长测量/检查波动 | confirm/summary.json 12行，每行5轮；同拓扑固定请求数 |
| 差距与建议 | BENCHMARK_REPORT.md；区分实测事实、代码路径、唤醒配对证据、尚未实施优化 |
| 真实火焰图与原始数据 | profiles 下8组，含 perf.data/script/folded/SVG；保存调试二进制 |
| 唤醒假设实验 | probes/results.json 共6次，3组有无pulse配对，全部40,000往返、40,000 RTT样本 |
| 回归 | reproduction/ctest.log 的11项 Test Passed；涵盖10项既有测试/例子及新增harness自检 |
| 不改变运行库基线 | Windows Git diff 对 src/include/tests/actor_runtime根CMake与上述基线完全一致 |

结果根目录：`benchmark-results/2026-09-08`。
归档脚本报告：131个证据文件，209.5 MiB。随后从Windows实际逐文件读取并复核：

```text
PASS: 131 artifact hashes, six measured binary hashes, eight SVGs and sample conservation,
53 matrix + 12 confirmation + six diagnostic results
CTest Test Passed count: 11
```

复核内容不是仅检查文件存在：

- SHA256.json 每个文件的字节数及SHA-256均匹配。
- suite/environment.json 的六个测量二进制SHA，与归档的六个带符号二进制SHA均匹配。
- 每个 perf.script 的 cpu-clock:u 事件数 = folded末列权重总和 = analysis/profile记录的样本数。
- 八个SVG均由禁用外部解析器的XML解析器读取成功，根元素为svg。
- 读取当前汇总验证53/12/6/8组结果齐全；CTest日志实际包含11次通过。

运行库未改，故不声称本轮修好了唤醒或扩展性问题，也没有重算覆盖率。
高波动的fiber和低样本CPU图已在报告中降级解读；用户态采样不能替代off-CPU/内核拆账。
三方消息拥有方式和线程结构并非相同，报告不将该实验称为最新版本的普适框架排行榜。

大体积原始数据/二进制受.gitignore排除但仍在磁盘，没有删除；没有自动提交/推送本轮改动。
备份整个结果目录才能保住全部原始证据，不能仅依赖未来的Git push。
