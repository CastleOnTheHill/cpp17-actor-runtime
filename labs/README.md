# 前三章实验

这是独立教学工程，不修改 Actor runtime。所有命令在 WSL 的仓库根目录执行：

```bash
cd '/mnt/c/Users/icema/Documents/ChatGPT/下一代ipc框架'
cmake -S labs -B labs/build
cmake --build labs/build -j4
ctest --test-dir labs/build --output-on-failure
./labs/build/ch01_call_stack
./labs/build/ch02_state_machine
./labs/build/ch03_ucontext
```

先写预测再运行。参考实现自带轨迹断言，`[PASS]` 只表示该程序的这些断言通过，不代表你已掌握，也不证明普遍线程安全。

| 变式 | 要验证的差异 |
|---|---|
| `./labs/build/ch01_call_stack --throw` | 正常离开与异常展开都结束相应局部对象生命周期 |
| `./labs/build/ch02_state_machine 10` | 持久值跨两次 next 从 10 到 11，不依赖常量 41 |
| `./labs/build/ch03_ucontext --throw` | fiber 内展开并保存异常，返回 scheduler 后再重抛 |

第 2 章自建练习：修改 `exercises/ch02_build_it.cpp`，只填写 TODO；测试用例已给出。

```bash
cmake --build labs/build --target ch02_build_it
./labs/build/ch02_build_it
```

未填写时会输出 TODO 并返回非零，这是有意保留的学习起点，不纳入默认六个参考测试。先别看 `ch02_state_machine.cpp` 的答案。完成后将你修改的 diff、运行前预测及解释提交给导师。

第 1 章构建任务：先从空文件写 Guard 的构造/析构日志，再写两层调用；第 3 章构建任务：在安全参考外壳内，把 `inner()` 里的 yield 移到自己新增的 `helper()`，预测并验证暂停时两层都未返回。两章的具体边界与通过标准见课文。

安全范围：第 3 章仅一个线程、一个活动 Demo，main 总会恢复到结束；不是通用协程库。禁止把 demo 移动到别的地址、跨线程 resume、恢复已完成上下文或暂停后直接释放栈。保护页不是任意栈溢出的可靠恢复方案。不要删掉错误检查、入口异常捕获和结束状态保护来“精简”代码。
