# Changelog

## 0.2.0（开发中）

- V2 重建开始。V1 冻结为 tag `v1-final`，源码移入 `legacy/`。
- 新骨架：`ode` 可执行文件、doctest 测试、`dev`/`release` 两个 preset。
- CI 改为 Windows MinGW 单作业。
- 项目改名 cxxode，命令 `ode`，命名空间 `ode`。配置目录 `~/.ode/`、`.ode/`，记忆文件 `ODE.md`，环境变量 `ODE_SHELL`、`ODE_CJK_WIDTH`，凭据目标名 `ode/<provider>`。旧的 `~/.lay`、`~/.cxxode` 不再读取。
