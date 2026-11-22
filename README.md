# Lay Code

Lay Code 是一个用 C++20 实现的本地代码助手。P0 提供离线夹具闭环，P1 提供基于 Anthropic Messages 的工作区只读模式。

## 离线演示

需要 C++20 编译器、CMake 3.24+ 和 Ninja。Windows 使用 MSVC 时，在 x64 开发者终端运行。

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release

'Can the parser accept an empty configuration?' | ./build/release/lay-code.exe
'Can the parser accept an empty configuration?' | ./build/release/lay-code.exe --truncated
```

Linux 使用 `./build/release/lay-code`。也可以直接启动程序，在提示符后输入一行 UTF-8 问题。正常轨迹退出码为 `0`。`--truncated` 模拟宿主收到一个完整读取提案后丢失回复终止事件，退出码为 `1`，派发次数为 `0`。

| 契约轨迹 | 停止原因 | 模型请求 | 读取派发 |
| --- | --- | ---: | ---: |
| 正常结束 | `end_turn` | 2 | 1 |
| 半截调用 | `protocol_error` | 1 | 0 |
| 回复提交前取消 | `cancelled` | 1 | 0 |
| 首次读取后耗尽请求预算 | `request_limit` | 1 | 1 |

契约测试对每条固定轨迹重放 100 次，逐次比较终态、已提交回复、调用和结果。CLI 端到端测试核对退出码和终端输出。

## 工作区只读模式

额外依赖 libcurl 7.85+ 和 nlohmann-json 3.11+。Windows 可用 vcpkg manifest，在 MSVC x64 开发者终端配置 vcpkg 工具链。Ubuntu 24.04 可使用 `libcurl4-openssl-dev` 和 `nlohmann-json3-dev`，然后直接配置同名 preset。契约测试需要 Python 3。

```powershell
cmake --preset readonly-release "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build --preset readonly-release
ctest --preset readonly-release

./build/readonly-release/lay-code.exe --workspace . --model YOUR_MODEL_ID --exclude private
```

将 `YOUR_MODEL_ID` 替换为账户可调用的模型 ID，并在本地环境设置 `ANTHROPIC_API_KEY`。Linux 可执行文件为 `./build/readonly-release/lay-code`。程序使用 Anthropic Messages API 的 `2023-06-01` 协议版本与文本、客户端工具子集。

`--workspace` 选定文件范围，`--exclude` 可重复指定要排除的工作区相对路径。问题和工具选中的摘录会发给 `api.anthropic.com`。`read` 列举一层目录或读取带行号的 UTF-8 文本，`search` 返回字面匹配的相对路径和行号。链接、重解析点、多硬链接文件、版本库元数据及常见凭据路径会被排除。工具各自报告截断或覆盖范围，`Ctrl+C` 取消当前请求。Windows 工作区使用本地 NTFS，Linux 需要 `openat2` 和 `/proc/self/fd`。

本地回环契约测试覆盖逐字节 SSE、检索和读取回填、批次预检、429 重试、断流及取消。真实服务检索验收需可用的 Anthropic 凭据与模型 ID。

## P0 基准与质量题集

[`bench/windows-p0.json`](bench/windows-p0.json) 保存 Windows 11 x64 参考机的 100 次热启动样本。MSYS2 UCRT64 GCC 15.2.0 Release 构建从创建进程到收到提示符的 P95 为 **14.99 ms**，等输入时的最大私有内存为 **1.12 MiB**。首次观测为 **15.87 ms**，测量使用管道接收 stdout，系统缓存状态未控制。

质量题集包含 4 道代码定位、4 道单文件修复、2 道保留未提交改动、2 道验证环境异常。`prepare` 把题目复制为独立 Git 仓库，目录须为新路径，隐藏断言保留在题集目录。

```powershell
python bench/startup.py ./build/release/lay-code.exe --compiler "MSYS2 UCRT64 GCC 15.2.0 (Rev13)"
python bench/quality.py list
python bench/quality.py verify
python bench/quality.py prepare fix-empty-config ./build/fix-empty-config
```
