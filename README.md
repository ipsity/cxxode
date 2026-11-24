# cxxode

用 C++20 写的终端 coding agent。cxx (C++) + ode (颂诗) = code。

**当前状态：V2 M5** — 费用统计、图片输入、性能优化已完成。完整设计见 [docs/BLUEPRINT_V2.md](docs/BLUEPRINT_V2.md)。

V1（P0–P28）的最后状态保存在 tag `v1-final` 和分支 `archive/v1-p28`。

## 快速开始

### 安装

**Windows:**
```bash
# 1. 编译 release 版本
cmake --preset release
cmake --build --preset release

# 2. 运行安装脚本（以管理员身份）
install.bat

# 3. 重启终端，配置 API 密钥
ode login anthropic
```

**Linux/macOS:**
```bash
# 1. 编译 release 版本
cmake --preset release
cmake --build --preset release

# 2. 运行安装脚本
sudo ./install.sh

# 3. 配置 API 密钥
ode login anthropic
```

**手动安装（所有平台）:**
1. 将 `build/release/ode.exe` (Windows) 或 `build/release/ode` (Linux/macOS) 复制到 PATH 中的任意目录
2. 或者添加 `build/release` 目录到系统 PATH

详细安装和配置说明见 [docs/USAGE.md](docs/USAGE.md)

### 基本使用

```bash
# 在任意目录启动
cd ~/my-project
ode

# 单次问答
ode "分析这个项目的结构"

# 切换模型
ode --model opus "复杂任务"
ode --model haiku "简单问题"

# 列出可用模型
ode models

# 查看帮助
ode --help
```

## 功能

**M5（当前）**
- **费用统计**：实时显示 token 使用成本，支持多种模型定价
- **图片输入**：基础图片支持（base64 编码，符合 Anthropic Vision API）
- **性能优化**：exe 大小 3.2MB，启动 ~50ms，代码 6,914 行
- 详细总结：[docs/M5_SUMMARY.md](docs/M5_SUMMARY.md)

**M4**
- **Subagents**：独立 agent 实例，支持专门系统提示（`task` 工具）
- **RPC 协议**：JSONL over stdin/stdout，供编辑器和 IDE 嵌入
- **Plugins**：自动加载 `~/.ode/plugins/` 和 `~/.claude/plugins/` 中的扩展
- 详细文档：[docs/M4_FEATURES.md](docs/M4_FEATURES.md)

**M3**
- **MCP 客户端**：通过 JSON-RPC 2.0 连接外部工具服务器（stdio 传输）
- **Hooks**：在工具执行前后、用户提交消息、agent 停止时运行自定义脚本
- **权限规则**：settings.json 中配置 allow/deny 规则，支持通配符和前缀匹配
- 详细文档：[docs/M3_FEATURES.md](docs/M3_FEATURES.md)

**M2**
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
- 配置：多级 settings.json（~/.claude, ~/.ode, .claude, .ode）
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
./build/dev/ode.exe --version
```
