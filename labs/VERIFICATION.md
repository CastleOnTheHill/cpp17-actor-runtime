# 课程交付验证

## 2026-09-06 · 第 4–8 章追加

- 分支：teaching。没有修改 actor_runtime 源码、用户第二章练习或未跟踪的 docs/BUSINESS_TEST_PLAN.md。
- 同一本机 Ubuntu-24.04 WSL，C++17：八个参考程序配置与编译通过，CTest 22/22 通过（原 6 + 新增 16）。
- 新增 16 个场景各重复 20 次：共 320 次运行通过。覆盖 FIFO/不 yield/空队列、截止前/到期/重复扫描/同截止/零时长、正常/异常/64 栈回收、1/4 worker 与失败 handler、类型错误/满邮箱/关闭拒绝。
- HTML 结构检查：11 页、117 个本地引用有效；每课一道过关题，答题导出目标与 label 完整性由脚本检查。
- node 语法检查及 assets/course.test.cjs 的 DOM 替身逻辑测试通过。
- 浏览器连接尝试失败：当前原生浏览器桥接不可用。未取得截图/DOM，实际视觉、响应式与浏览器下载行为仍未验收；不是单元测试的通过范围。
- 本轮第 4–8 章新增与导航改动未提交、未推送。学习记录没有标记用户已通过新章节。

新增实验与教学边界：

| 章 | 验证载体 | 刻意不作保证 |
|---|---|---|
| 04 | 真实 ucontext、单线程 FIFO | 通用公平/抢占/多 worker 全局顺序 |
| 05 | 手动模拟 tick + 真实暂停 | 真实计时精度/事件竞赛/取消 |
| 06 | 对象数、栈数、完成记录数 | RSS 等同/任意暂停栈销毁/资源耗尽容错 |
| 07 | std::thread + mutex | 公平份额/跨 Actor 并行度/事务回滚 |
| 08 | 值载荷邮箱、预先完成发送 | 真实框架固定容量/close-drain/可靠消息协议 |

重复验证命令（WSL 仓库根目录）：

```bash
ctest --test-dir labs/build -R 'ch0[4-8]_' --repeat until-fail:20 --output-on-failure
python3 labs/check_course.py
```

## 2026-09-05 · 前三章首次交付（历史记录）

### 已执行

- 本机 Ubuntu-24.04 WSL / GNU C++ 13.3，C++17：配置、构建成功。
- CTest 6/6 通过：普通返回、异常展开、两段任务默认/变式输入、有栈暂停恢复、fiber 入口异常桥接。
- 显式构建 `ch02_build_it` 成功；运行返回 1、输出 `TODO: implement Task::next`。这是留给学习者的未完成练习，符合预期，不纳入参考实验的通过率。
- `python3 labs/check_course.py`：5 个 HTML、47 个本地引用目标/锚点通过。
- `node --check assets/course.js`：语法检查通过。
- `node assets/course.test.cjs`：步骤切换的边界/可见状态、空答案提示、导出内容与下载链接生命周期的 DOM 替身单元测试通过；不是实际浏览器下载验证。

### 未完成的验证

内置浏览器调用的自动权限审批连续两次超时，未得到可用的页面截图或 DOM。没有完成实际浏览器视觉、响应式布局和下载行为验收。静态链接检查与 DOM 替身单元测试不能替代这一步。

### 可复现命令

```bash
# WSL，仓库根目录
cmake -S labs -B labs/build
cmake --build labs/build -j4
ctest --test-dir labs/build --output-on-failure
python3 labs/check_course.py
```

```powershell
# Windows，仓库根目录；只测试脚本逻辑，不控制浏览器
node --check assets/course.js
node assets/course.test.cjs
```

范围：本轮没有修改 runtime 源码，不将已有框架覆盖率归功于教学实验。上述验证不是学习者过关记录，也不是生产安全证明。教学文件尚未提交或上传 GitHub。
