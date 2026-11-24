# Lay Code

用 C++20 写的终端 coding agent。目标：启动 < 20 ms，空闲内存 < 15 MB，单个 exe，兼容 Claude Code 的 skills、命令、MCP、hooks 和插件格式。

当前状态：**V2 M0**，只有骨架。完整设计见 [docs/BLUEPRINT_V2.md](docs/BLUEPRINT_V2.md)。

V1（P0–P28）的最后状态保存在 tag `v1-final` 和分支 `archive/v1-p28`。V1 源码暂存在 `legacy/`，供 V2 回收代码。

## 构建

需要 C++20 编译器、CMake 3.24+ 和 Ninja。

```
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/lay --version
```
