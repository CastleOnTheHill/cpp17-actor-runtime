# 课程交付验证 · 2026-09-05

## 已执行

- 本机 Ubuntu-24.04 WSL / GNU C++ 13.3，C++17：配置、构建成功。
- CTest 6/6 通过：普通返回、异常展开、两段任务默认/变式输入、有栈暂停恢复、fiber 入口异常桥接。
- 显式构建 `ch02_build_it` 成功；运行返回 1、输出 `TODO: implement Task::next`。这是留给学习者的未完成练习，符合预期，不纳入参考实验的通过率。
- `python3 labs/check_course.py`：5 个 HTML、47 个本地引用目标/锚点通过。
- `node --check assets/course.js`：语法检查通过。
- `node assets/course.test.cjs`：步骤切换的边界/可见状态、空答案提示、导出内容与下载链接生命周期的 DOM 替身单元测试通过；不是实际浏览器下载验证。

## 未完成的验证

内置浏览器调用的自动权限审批连续两次超时，未得到可用的页面截图或 DOM。没有完成实际浏览器视觉、响应式布局和下载行为验收。静态链接检查与 DOM 替身单元测试不能替代这一步。

## 可复现命令

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
