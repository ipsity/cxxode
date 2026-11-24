# Lay Code 发布记录

## V2 M2 (2024-11-24)

Skills、用户命令、文件补全、消息分叉。

**新功能**
- **Skills 系统**：
  - 扫描 `~/.claude/skills/` 和 `.claude/skills/` 目录
  - 解析 YAML frontmatter（name, description）
  - 通过 `skill` 工具调用，系统提示列出可用 skills
  - 6 个单元测试（frontmatter 解析、skill 加载）
- **用户斜杠命令**：
  - 扫描 `.claude/commands/*.md` 自定义命令
  - 变量展开：`$ARGUMENTS`（命令参数）、`$WORKSPACE`（工作区路径）
  - 命令内容作为用户消息发送
  - 5 个单元测试（变量展开、命令加载）
- **扩展命令面板**：
  - 合并内置命令、skills、用户命令
  - Skills 标记 `(skill)` 区分
  - 前缀匹配，最多显示 8 项
  - Tab 补全，Enter 执行
- **@文件补全**：
  - 检测 `@mention` 模式
  - 递归搜索工作区文件（忽略 .git、node_modules 等）
  - 子字符串匹配，显示相对路径
  - Tab 补全文件路径
- **Esc Esc 分叉**：
  - 双击 Esc（间隔 < 500ms）编辑上一条用户消息
  - 创建分支会话（新 session id，如 `xxx-fork`）
  - 截断消息到分叉点
  - 编辑模式提示：`编辑中 · Esc 取消 · Enter 创建分支`

**测试**
- 38 单元测试通过（+11 个新测试）
- 330 个断言（+14 个新断言）

**已知限制**
- 分叉会话 id 简单追加 `-fork`，不支持多级分叉计数
- @文件补全暂无模糊匹配评分，仅子字符串匹配
- Skills 变量展开未实现（计划 M3）

## V2 M1 (2024-11-24)

首个可用版本，实现交互式 TUI 和无头模式。

**新功能**
- 交互式终端界面：内联渲染，实时输入框，状态栏显示路径/分支/模式/模型/token 用量
- 权限系统：plan（只读）、default（允许工作区编辑）、yolo（全部放行）三档
  - Shift+Tab 实时切换
  - 命令确认提示：Enter 允许，Esc 拒绝，a 总是允许
  - 规则持久化到 allow/deny 列表
- 工具集：
  - Read：按行读取，支持 offset/limit 分页
  - Write：创建或覆盖文件
  - Edit：精确字符串替换，CRLF 保持，要求唯一匹配或 replace_all
  - Bash：Git Bash 优先，回退 PowerShell，超时 600s
  - Grep：ECMAScript 正则，尊重 .gitignore
  - Glob：文件名模式匹配，按修改时间排序
- 会话管理：
  - JSONL 格式，每个工作区独立 sessions 目录
  - `-c` 继续最近会话，`-r` 恢复指定会话
  - 自动压缩：上下文超过 85% 时触发
  - /resume 命令：列出和恢复历史会话
- 斜杠命令：/help /clear /compact /model /mode /undo /resume /cost /exit
- 配置：
  - 多级 settings.json：~/.claude、~/.lay、.claude、.lay
  - 模型别名解析：opus/sonnet/haiku/fable
  - 环境变量支持：ANTHROPIC_DEFAULT_*_MODEL、ANTHROPIC_API_KEY、ANTHROPIC_AUTH_TOKEN
- 凭据管理：`lay login <provider>` 保存到 Windows Credential Manager
- Markdown 渲染：标题加粗，代码块 dim，列表项转换
- Unicode 17.0：CJK 宽度识别，字素边界正确处理
- 并行工具执行：连续只读调用并发运行，混合调用串行执行

**无头模式**
- `-p` 或 `--print`：文本流向 stdout，工具日志到 stderr
- 管道支持：`git diff | lay -p "review this"`
- 默认 plan 模式，ask 自动拒绝

**平台**
- Windows：WinHTTP、ConPTY、Job Object 进程隔离、CREATE_NO_WINDOW 子进程
- Git Bash 检测：遍历祖先目录查找 bin/bash.exe
- 原始终端模式：关闭 line/echo/processed input，启用 VT 处理
- 粘贴检测：批量 Enter 识别为换行而非提交

**测试**
- 27 单元测试：agent 循环、工具、权限、会话、编辑器、Unicode 布局
- 无头烟测：真实 API 调用，文件编辑+验证轮次
- PTY 烟测：交互渲染、命令补全、权限确认

**已知限制**
- 仅支持 Windows（Linux 需要 libcurl、termios、fork/exec）
- 无 `--output json` JSONL 事件流（推迟到 M4）
- 无 `lay models` 命令
- Thinking 文本不显示（仅显示"思考中"）
- Markdown 渲染简化（无内联代码高亮）

## V2 M0 (2024-11-23)

初始骨架，仅包含 HTTP、消息编解码和版本信息。

---

V1 最终版本：tag `v1-final`、分支 `archive/v1-p28`
