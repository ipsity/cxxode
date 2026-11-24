# Lay Code V2

原生 C++20 终端 coding agent。设计依据：`docs/BLUEPRINT_V2.md`。有冲突可以提出修改，蓝图不是不可讨论的。

## 构建与测试

Windows 用 MSYS2 UCRT64 的 g++，cmake/ninja 可用 `py -m pip install --user cmake ninja` 安装。

```
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

发布构建把 `dev` 换成 `release`。构建目录只有 `build/dev` 和 `build/release` 两个。

## 目录

```
src/main.cpp     参数解析与前端分发
src/core/        loop、context、permission、session(jsonl)、compact
src/provider/    http(libcurl)、anthropic、openai
src/tools/       read write edit bash grep glob
src/ext/         skills commands hooks mcp agents plugins
src/ui/          term render input unicode
src/config/      settings.json、凭据
tests/           doctest 单元测试 + 回放夹具
third_party/     单头文件依赖
legacy/          V1 源码，只用于回收代码，不参与构建
```

## 完成定义

一个改动满足以下三条即可提交：

1. 编译通过，无新警告。
2. `ctest --preset dev` 通过，全部离线，总时长 < 60 秒。
3. 本人实际运行一次，行为正确。

## 不要做

- 不要创建验收 JSON、人工验收手册、RC 编号。
- 不要为改动启动虚拟机、WSL、讲述人或多窗口尺寸人工矩阵。
- 不要在 `build/` 下为每个阶段新建目录。
- 不要往 `legacy/` 里加东西。从 legacy 搬代码时，搬到 `src/` 下对应模块后再改。
- 不要新增 Python 测试，测试统一用 C++ + doctest。

## 代码风格

- 命名空间 `lay`，`snake_case` 函数和变量，类型也用 `snake_case`，与 V1 一致。
- 头文件用 `#pragma once`。
- 只在必要的边界引入抽象。JSON 类型不出协议层。
- 注释写"为什么"，不写"做了什么"。

## 提交

Conventional Commits：`feat:`、`fix:`、`test:`、`chore:`、`docs:`。一次提交只做一件事。
