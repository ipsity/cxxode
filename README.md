# Lay Code

Lay Code 是一个用 C++20 实现的本地代码助手。当前处于 P0，CLI 接收一行问题，由脚本化模型请求读取内存夹具，再根据读取结果生成回答。

## 构建与运行

需要 C++20 编译器、CMake 3.24+ 和 Ninja。Windows 使用 MSVC 时，在 x64 开发者终端运行。

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release

'Can the parser accept an empty configuration?' | ./build/release/lay-code.exe
'Can the parser accept an empty configuration?' | ./build/release/lay-code.exe --truncated
```

Linux 使用 `./build/release/lay-code`。也可以直接启动程序，在提示符后输入一行。正常轨迹退出码为 `0`。截断轨迹在一个完整读取调用之后丢失第二个调用的末尾和回复终止事件，退出码为 `1`，派发次数为 `0`。

| 契约轨迹 | 停止原因 | 模型请求 | 读取派发 |
| --- | --- | ---: | ---: |
| 正常结束 | `end_turn` | 2 | 1 |
| 半截调用 | `protocol_error` | 1 | 0 |
| 调用途中取消 | `cancelled` | 1 | 0 |
| 首次读取后耗尽请求预算 | `request_limit` | 1 | 1 |

契约测试对每条固定轨迹重放 100 次，逐次比较终态、已提交回复、调用和结果。CLI 端到端测试核对退出码和终端输出。

## P0 基准与质量题集

[`bench/windows-p0.json`](bench/windows-p0.json) 保存 Windows 11 x64 参考机的 100 次热启动样本。MSYS2 UCRT64 GCC 15.2.0 Release 构建从创建进程到收到提示符的 P95 为 **14.99 ms**，等输入时的最大私有内存为 **1.12 MiB**。首次观测为 **15.87 ms**，测量使用管道接收 stdout，系统缓存状态未控制。

质量题集包含 4 道代码定位、4 道单文件修复、2 道保留未提交改动、2 道验证环境异常。`prepare` 把题目复制为独立 Git 仓库，目录须为新路径，隐藏断言保留在题集目录。

```powershell
python bench/startup.py ./build/release/lay-code.exe --compiler "MSYS2 UCRT64 GCC 15.2.0 (Rev13)"
python bench/quality.py list
python bench/quality.py verify
python bench/quality.py prepare fix-empty-config ./build/fix-empty-config
```
