# M3 功能说明

## Hooks

Hooks 允许在特定事件发生时运行外部脚本。支持的 hook 类型：

### PreToolUse
在工具执行前调用。如果脚本返回退出码 2，工具调用将被阻止。

### PostToolUse
在工具执行后调用。用于日志记录或通知。

### UserPromptSubmit
当用户提交消息时调用。可用于验证或预处理。

### Stop
在 agent 停止前调用（正常或错误）。

### 配置示例

```json
{
  "hooks": {
    "PreToolUse": [
      {
        "command": "node",
        "args": ["hooks/validate.js"],
        "cwd": ""
      }
    ],
    "PostToolUse": [
      {
        "command": "python",
        "args": ["hooks/log.py"]
      }
    ]
  }
}
```

### Hook 输入

Hooks 通过 stdin 接收 JSON 输入：

**PreToolUse:**
```json
{
  "type": "PreToolUse",
  "tool": "bash",
  "input": {"command": "rm -rf /"}
}
```

**PostToolUse:**
```json
{
  "type": "PostToolUse",
  "tool": "read",
  "output": "file contents...",
  "isError": false
}
```

**UserPromptSubmit:**
```json
{
  "type": "UserPromptSubmit",
  "prompt": "user's message"
}
```

**Stop:**
```json
{
  "type": "Stop",
  "reason": "Cancelled."
}
```

### Hook 返回值

- 退出码 0：允许继续
- 退出码 2：阻止操作（仅对 PreToolUse 和 UserPromptSubmit 有效）
- 其他退出码：视为错误，但不阻止

## MCP (Model Context Protocol)

MCP 允许连接外部工具服务器，扩展 agent 的能力。

### 配置示例

```json
{
  "mcpServers": {
    "filesystem": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "."],
      "env": {}
    },
    "github": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-github"],
      "env": {
        "GITHUB_PERSONAL_ACCESS_TOKEN": "ghp_..."
      }
    },
    "custom": {
      "command": "/path/to/custom-mcp-server",
      "args": ["--arg1", "value1"],
      "env": {
        "API_KEY": "secret"
      }
    }
  }
}
```

### 工具命名

MCP 工具在 agent 中的名称格式为：`mcp__servername__toolname`

例如，filesystem server 的 `read_file` 工具会变成 `mcp__filesystem__read_file`

### 可用的 MCP Servers

- **@modelcontextprotocol/server-filesystem** - 文件系统操作
- **@modelcontextprotocol/server-github** - GitHub API 操作
- **@modelcontextprotocol/server-git** - Git 仓库操作
- **@modelcontextprotocol/server-postgres** - PostgreSQL 数据库
- **@modelcontextprotocol/server-sqlite** - SQLite 数据库

更多：https://github.com/modelcontextprotocol/servers

### 启动行为

- MCP servers 在 ode 启动时初始化
- 初始化失败的 server 会显示警告但不会阻止启动
- Server 崩溃时工具调用会返回错误

## 权限规则

在 `settings.json` 中配置工具权限：

```json
{
  "permissions": {
    "defaultMode": "default",
    "allow": [
      "Bash(git *)",
      "Bash(npm install*)",
      "Bash(cmake *)",
      "Write(*.md)",
      "Edit(src/**/*.cpp)"
    ],
    "deny": [
      "Bash(rm -rf*)",
      "Write(/etc/*)"
    ]
  }
}
```

### 规则语法

- `Bash(pattern)` - 匹配命令前缀
- `Write(pattern)` - 匹配文件路径（支持通配符）
- `Edit(pattern)` - 匹配文件路径
- `Read(pattern)` - 匹配文件路径
- `Grep(pattern)` - 匹配搜索模式
- `Glob(pattern)` - 匹配 glob 模式

### 权限模式

- **plan** - 只读模式，拒绝所有写操作
- **default** - 默认模式，写操作需要确认（除非在 allow 列表中）
- **yolo** - 自动允许所有操作

在运行时用 `Shift+Tab` 切换模式，或用 `--permission` 参数指定。

## 配置文件优先级

从低到高：
1. `~/.claude/CLAUDE.md`
2. `~/.ode/settings.json`
3. `~/.claude/settings.json`
4. `<workspace>/.ode/settings.json`
5. `<workspace>/.claude/settings.json`

后加载的配置会覆盖或合并先加载的配置。
