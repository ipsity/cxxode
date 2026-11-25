# cxxode

Windows 原生 C++20 终端 coding agent，命令 `ode`。设计依据：`docs/ARCHITECTURE.md`，它是唯一有效的设计文档。有冲突先讨论，再改文档或代码。`archive/` 是旧文档和旧脚本，只读，不作为依据。

## 平台

只支持 Windows x64。不写 POSIX 分支，不做其他系统验证。

## 构建与测试

MSYS2 UCRT64 的 g++，cmake/ninja 可用 `py -m pip install --user cmake ninja` 安装。

```
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

发布构建把 `dev` 换成 `release`。构建目录只有 `build/dev` 和 `build/release`。

## 目录

见 `docs/ARCHITECTURE.md` 第 3 节。分层：`base → platform → ui/config → provider → tools → core → ext → app`，只能向下依赖，同层互不依赖，`main.cpp` 负责装配。

## 完成定义

一个改动满足以下三条即可提交：

1. 编译通过，无新警告。
2. `ctest --preset dev` 通过，全部离线，总时长 < 60 秒。
3. 本人实际运行一次，行为正确。

## 不要做

- 不要写阶段报告、进度文档、完成声明（DAY/WEEK/PHASE/COMPLETE 之类）。设计变化直接改 `docs/ARCHITECTURE.md`。
- 不要创建验收 JSON、人工验收手册、RC 编号。
- 不要写安装脚本。发布物就是单个 `ode.exe`。
- 不要加没有调用方的模块，"以后会用"不算调用方。
- 不要在 `build/` 下为每个阶段新建目录。
- 不要往 `legacy/`、`archive/` 里加东西。需要旧代码时搬到 `src/` 对应模块再改。
- 不要新增 Python 测试，测试统一用 C++ + doctest。
- 不要把密钥写进任何被跟踪的文件或提交。

## 代码风格

- 命名空间 `ode`，函数、变量、类型都用 `snake_case`。
- 头文件用 `#pragma once`。
- 只在必要的边界引入抽象。JSON 类型不出协议层（见架构文档第 3 节）。
- 注释写"为什么"，不写"做了什么"。注释里不写 emoji、"新增"、"Day N"。

## 提交

Conventional Commits：`feat:`、`fix:`、`refactor:`、`test:`、`chore:`、`docs:`。一次提交只做一件事。
