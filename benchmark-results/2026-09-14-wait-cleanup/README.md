# 等待登记清理：2026-09-14 原子验收

恢复时 CMake 已引用 wait_cleanup_tests，但文件缺失。本轮新增该测试，配置 src 诊断头文件 include 路径，未修改运行库实现。此前未提交的结果回收和 timer 改动保持原样。

- 冻结旧基线：`/tmp/actor-p0-20260913/baseline-source` 与 `baseline-build`，源自 ad2389b。
- 同一测试用基线头文件和静态库构建（不混用新旧内部布局），退出码 1，输出 `message registration retained`，证明测试能识别旧缺陷。
- 当前版完整回归：`full-ctest.log`，14/14。
- 当前版专项重复：`repeat-ctest.log`，wait_cleanup_tests 连续 20 次通过。
- 每次专项在 1/4 worker 分别检查 10000 次即时 Any、1000 次部分安装失败回滚、真实消息胜出和真实 timeout 胜出；读取发生在 owner worker 和 Actor 执行权范围内。
- 临时输出 `/tmp/actor-p0-20260914-wait-cleanup`，候选构建 `/tmp/actor-p0-20260913/candidate-build`。

重跑：`cmake --build /tmp/actor-p0-20260913/candidate-build -j4`；`ctest --test-dir /tmp/actor-p0-20260913/candidate-build --output-on-failure`；专项可加 `-R wait_cleanup_tests --repeat until-fail:20`。

本轮没有测试完整内存容量/峰值、timer 大规模乱序撤销、全部跨 worker 取消竞态或性能 A/B，不能据此宣布四项 P0 或“不劣化”完成。完整目标仍保留；未提交、推送或改写历史。
