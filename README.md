# cxxode

用 C++20 写的终端 coding agent。cxx (C++) + ode (颂诗) = code。

**当前状态：V2 M2** — Skills、用户命令、@文件补全、Esc Esc 分叉已完成。完整设计见 [docs/BLUEPRINT_V2.md](docs/BLUEPRINT_V2.md)。

V1（P0–P28）的最后状态保存在 tag `v1-final` 和分支 `archive/v1-p28`。

## 快速开始

```bash
# 交互模式
lay

# 带初始提示
lay "解释 main.cpp 的流程"

# 无头模式（输出到 stdout）
lay -p "README.md 里的构建步骤过时了吗？"

# 继续最近的会话
lay -c

# 登录（保存 API key）
lay login anthropic
```

## 功能

**M2（当前）**
- **Skills**：从 `.claude/skills/` 和 `~/.claude/skills/` 加载 SKILL.md，通过 `skill` 工具调用
- **用户斜杠命令**：从 `.claude/commands/` 加载自定义命令，支持 $ARGUMENTS、$WORKSPACE 变量
- **扩展命令面板**：`/` 触发，显示内置命令 + skills（标记 skill）+ 用户命令
- **@文件补全**：输入 `@` 后显示工作区文件，Tab 补全路径
- **Esc Esc 分叉**：双击 Esc（< 500ms）编辑上一条消息，创建分支会话

**M1**
- 交互式 TUI：内联渲染、权限提示、斜杠命令（/help /compact /undo /resume）
- 工具：Read/Write/Edit/Bash/Grep/Glob，支持并行读取
- 权限系统：plan/default/yolo 三档，allow/deny 规则，Shift+Tab 切换
- 会话：JSONL 日志，自动压缩（85% 上下文触发）
- 配置：多级 settings.json（~/.claude, ~/.lay, .claude, .lay）
- 模型别名：opus/sonnet/haiku/fable → 通过 `ANTHROPIC_DEFAULT_*_MODEL` 解析

**按键**
- `Enter` 发送，`Shift+Enter`/`Ctrl+J` 换行
- `Esc` 中断/清空，`Ctrl+C` 双击退出
- `Shift+Tab` 切换权限档
- `↑` `↓` 历史/编辑导航
- 权限提示时：`Enter` 允许，`Esc` 拒绝，`a` 总是允许

**平台支持**
- Windows：完整支持（WinHTTP, ConPTY, Job Object 进程隔离）
- Linux：暂未实现（需要 libcurl, termios, fork/exec）

## 构建

需要 C++20 编译器、CMake 3.24+ 和 Ninja。

**Windows**
```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/lay.exe --version
```
