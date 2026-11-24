# M4 功能说明

## Subagents

Subagents 允许创建具有专门系统提示的独立 agent 实例，用于特定任务。

### 配置文件格式

在 `.claude/agents/` 或 `~/.claude/agents/` 目录中创建 `.md` 文件：

```markdown
---
name: code-reviewer
description: Reviews code for bugs, style, and best practices
---

You are an expert code reviewer. When given code:

1. Check for bugs and logic errors
2. Verify adherence to best practices
3. Suggest improvements for readability
4. Flag security concerns

Provide specific, actionable feedback with line numbers.
```

### 使用方式

Agent 通过 `task` 工具调用 subagents：

```json
{
  "agent": "code-reviewer",
  "task": "Review the authentication logic in auth.cpp"
}
```

### 特性

- **隔离上下文**：每个 subagent 有独立的消息历史
- **相同工具集**：继承主 agent 的所有工具（builtin + skills + MCP）
- **自动批准**：Subagent 的工具调用自动批准（无需用户确认）
- **返回文本**：只返回最终输出文本，不包括思考过程

## RPC 协议

RPC 模式允许其他程序（编辑器、IDE 插件）通过 stdin/stdout 嵌入 cxxode。

### 启动 RPC 模式

```bash
ode --rpc
```

### 协议格式

使用 JSONL（JSON Lines）格式，每行一个 JSON 对象。

### 请求格式（客户端 → 服务器）

**运行任务：**
```json
{"id": 1, "method": "run", "params": {"prompt": "Fix the bug in main.cpp"}}
```

**中断：**
```json
{"id": 2, "method": "interrupt", "params": {}}
```

**撤销：**
```json
{"id": 3, "method": "undo", "params": {}}
```

**压缩上下文：**
```json
{"id": 4, "method": "compact", "params": {}}
```

### 事件格式（服务器 → 客户端）

**文本输出：**
```json
{"id": 1, "event": "text", "data": {"text": "Let me check that file..."}}
```

**思考过程：**
```json
{"id": 1, "event": "thinking", "data": {"text": "analyzing...", "signature": "sig"}}
```

**工具开始：**
```json
{"id": 1, "event": "tool_started", "data": {"tool": "read", "input": {"file_path": "main.cpp"}}}
```

**工具完成：**
```json
{"id": 1, "event": "tool_finished", "data": {"tool": "read", "input": {...}, "output": "...", "is_error": false}}
```

**Token 使用：**
```json
{"id": 1, "event": "usage", "data": {"input": 1000, "output": 500, "cache_read": 0, "cache_write": 0}}
```

**权限请求：**
```json
{"id": 1, "event": "ask", "data": {"tool": "bash", "input": {"command": "rm file"}}}
```

客户端应回复：
```json
{"answer": "yes"}  // 或 "no", "always"
```

**完成：**
```json
{"id": 1, "event": "done", "data": {"stop": "end_turn", "error": ""}}
```

**错误：**
```json
{"id": 1, "event": "error", "data": {"message": "Parse error: invalid JSON"}}
```

### 示例客户端（Python）

```python
import json
import subprocess

proc = subprocess.Popen(
    ["ode", "--rpc"],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    text=True
)

# 发送请求
request = {"id": 1, "method": "run", "params": {"prompt": "List files"}}
proc.stdin.write(json.dumps(request) + "\n")
proc.stdin.flush()

# 读取事件
for line in proc.stdout:
    event = json.loads(line)
    if event["event"] == "text":
        print(event["data"]["text"], end="")
    elif event["event"] == "done":
        break
```

## Plugins

Plugins 是包含 skills、commands、agents 等的打包目录，安装到 `~/.ode/plugins/` 或 `~/.claude/plugins/`。

### Plugin 目录结构

```
~/.ode/plugins/
  my-plugin/
    .claude-plugin/
      plugin.json          # 元数据（必需）
    skills/                # Skills（可选）
      my-skill.md
    commands/              # Commands（可选）
      my-command.md
    agents/                # Subagents（可选）
      my-agent.md
    README.md              # 文档
```

### plugin.json 格式

```json
{
  "name": "my-plugin",
  "version": "1.0.0",
  "description": "My useful plugin for cxxode",
  "author": "Your Name",
  "repository": "https://github.com/user/my-plugin"
}
```

### 安装 Plugin

**手动安装：**

```bash
# 解压或克隆到 plugins 目录
cd ~/.ode/plugins
git clone https://github.com/user/my-plugin
```

**验证安装：**

重启 ode，plugin 中的 skills、commands 和 agents 会自动加载。

### 加载顺序

1. 用户级配置（`~/.ode`, `~/.claude`）
2. **Plugin 配置**（`~/.ode/plugins/*/`, `~/.claude/plugins/*/`）
3. 工作区配置（`workspace/.ode`, `workspace/.claude`）

工作区配置优先级最高，可覆盖 plugin 中的同名项。

### 创建 Plugin

1. 创建目录结构
2. 添加 `.claude-plugin/plugin.json`
3. 添加 skills、commands 或 agents
4. 测试：复制到 `~/.ode/plugins/` 并运行 ode
5. 分享：发布到 GitHub 或其他平台

### 示例 Plugin

**simple-plugin/.claude-plugin/plugin.json:**
```json
{
  "name": "simple-plugin",
  "version": "1.0.0",
  "description": "A simple example plugin",
  "author": "Example Author"
}
```

**simple-plugin/skills/hello.md:**
```markdown
---
name: hello
description: Says hello
---

Say "Hello from the plugin!"
```

安装后，agent 可以调用 `skill(name="hello")`。

### 未来功能

- `ode plugin install <url>` 命令自动安装
- Marketplace 支持（从 registry 浏览和安装）
- 依赖管理（plugin 依赖其他 plugins）
- 版本检查和更新

当前版本需要手动安装，自动安装功能将在后续版本实现。
