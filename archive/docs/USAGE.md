# cxxode 快速使用指南

## 安装配置

### 1. 添加到系统 PATH

#### Windows (推荐方法)

**方法 1：自动安装脚本**
```bash
# 在 cxxode 目录运行
./install.bat
```

**方法 2：手动添加到 PATH**
1. 复制 `ode.exe` 到一个固定位置，例如：
   ```
   C:\Program Files\ode\ode.exe
   ```

2. 添加到系统 PATH：
   - 按 `Win + X`，选择"系统"
   - 点击"高级系统设置"
   - 点击"环境变量"
   - 在"系统变量"中找到 `Path`，点击"编辑"
   - 点击"新建"，添加：`C:\Program Files\ode`
   - 点击"确定"保存

3. 重新打开终端，测试：
   ```bash
   ode --version
   ```

**方法 3：使用符号链接（开发模式）**
```bash
# 以管理员身份运行
mklink "C:\Windows\System32\ode.exe" "D:\cxxode\build\release\ode.exe"
```

#### Linux/macOS

```bash
# 编译 release 版本
cmake --preset release
cmake --build --preset release

# 复制到 /usr/local/bin
sudo cp build/release/ode /usr/local/bin/

# 或创建符号链接
sudo ln -s $(pwd)/build/release/ode /usr/local/bin/ode

# 验证
ode --version
```

### 2. 配置 API 密钥

**方法 1：使用 login 命令（推荐）**
```bash
ode login anthropic
# 输入你的 API 密钥: sk-ant-...
```

**方法 2：设置环境变量**

Windows (PowerShell):
```powershell
# 临时设置（当前会话）
$env:ANTHROPIC_API_KEY = "sk-ant-..."

# 永久设置
[System.Environment]::SetEnvironmentVariable('ANTHROPIC_API_KEY', 'sk-ant-...', 'User')
```

Windows (CMD):
```cmd
setx ANTHROPIC_API_KEY "sk-ant-..."
```

Linux/macOS:
```bash
# 添加到 ~/.bashrc 或 ~/.zshrc
echo 'export ANTHROPIC_API_KEY="sk-ant-..."' >> ~/.bashrc
source ~/.bashrc
```

**方法 3：配置文件**
创建 `~/.ode/settings.json`：
```json
{
  "provider": "anthropic",
  "model": "sonnet"
}
```

然后设置环境变量：
```bash
export ANTHROPIC_API_KEY="sk-ant-..."
```

### 3. 验证安装

```bash
# 检查版本
ode --version

# 列出可用模型（需要 API 密钥）
ode models

# 测试运行
ode "Hello, 请自我介绍"
```

## 基本使用

### 在任意目录使用

```bash
# 进入任何项目目录
cd ~/projects/my-project

# 直接使用 ode
ode "分析这个项目的结构"
ode "帮我写一个 README.md"
ode "review 最新的 git commit"
```

### 快速命令

```bash
# 交互模式（推荐）
ode

# 单次问答
ode "解释 main.cpp 的功能"

# 无头模式（输出到标准输出）
ode -p "列出项目文件" > analysis.txt

# 继续上次会话
ode -c

# 切换模型
ode --model opus "复杂任务"
ode --model haiku "简单问题"

# 切换提供者
ode --provider openai "使用 GPT-4"
ode --provider groq "使用免费的 Llama"

# 切换权限模式
ode --permission plan "只读模式"
ode --permission yolo "自动批准所有操作"
```

### 交互模式快捷键

```
输入相关：
  Enter         发送消息
  Shift+Enter   换行
  Ctrl+J        换行
  ↑ ↓          历史导航

控制：
  Esc           中断/清空输入
  Ctrl+C        双击退出
  Shift+Tab     切换权限模式 (plan/default/yolo)

功能：
  /             打开命令面板
  @             文件路径补全
  Esc Esc       编辑上一条消息（双击，< 500ms）

权限提示时：
  Enter / y     允许
  Esc / n       拒绝
  a             总是允许（添加到 allow 规则）
```

### 常用命令

在交互模式中输入 `/` 打开命令面板，或直接输入：

```bash
/help       # 查看帮助
/model      # 查看当前模型
/models     # 列出可用模型
/mode       # 切换权限模式
/compact    # 压缩上下文
/undo       # 撤销上一轮的文件改动
/cost       # 查看本会话 token 用量和费用
/clear      # 开始新会话
/resume     # 恢复本目录的会话
/exit       # 退出
```

## 常见使用场景

### 1. 代码审查

```bash
cd my-project
ode "review 最近的 git commit"
ode "分析 src/ 目录的代码质量"
```

### 2. 生成代码

```bash
ode "创建一个 HTTP server 的例子"
ode "帮我写一个排序算法的单元测试"
```

### 3. 重构代码

```bash
ode "重构 utils.js，提取重复逻辑"
ode "把 app.py 拆分成多个模块"
```

### 4. 修复 Bug

```bash
ode "找出并修复 auth.cpp 中的内存泄漏"
ode "为什么运行 npm test 失败？"
```

### 5. 文档生成

```bash
ode "为 api.ts 生成 API 文档"
ode "创建一个详细的 README.md"
```

### 6. 项目分析

```bash
ode "分析这个项目的技术栈和依赖"
ode "评估代码的复杂度和可维护性"
```

## 高级配置

### 多项目配置

在每个项目目录创建 `.claude/settings.json`：

```json
{
  "model": "opus",
  "permissions": {
    "defaultMode": "default",
    "allow": [
      "Bash(npm *)",
      "Bash(git *)",
      "Write(docs/**/*.md)"
    ]
  }
}
```

### 使用 Skills

创建 `.claude/skills/my-skill.md`：
```markdown
---
name: test-runner
description: Run tests and analyze results
---

Run the test suite and check for failures.
If tests fail, analyze the error messages.
```

使用：
```bash
ode
> 使用 test-runner skill
```

### 使用 MCP 工具

配置 `~/.ode/settings.json`：
```json
{
  "mcpServers": {
    "filesystem": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "."]
    },
    "github": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-github"],
      "env": {
        "GITHUB_PERSONAL_ACCESS_TOKEN": "ghp_..."
      }
    }
  }
}
```

### 配置 Hooks

创建 `.claude/settings.json`：
```json
{
  "hooks": {
    "PreToolUse": [
      {
        "command": "node",
        "args": ["hooks/validate.js"]
      }
    ]
  }
}
```

## 工作流示例

### 完整的开发流程

```bash
# 1. 进入项目
cd ~/projects/my-app

# 2. 启动 ode
ode

# 3. 分析项目
> 分析项目结构，给出改进建议

# 4. 实现功能
> 添加用户认证功能，包括登录和注册

# 5. 审查代码
> review 刚才生成的代码

# 6. 写测试
> 为认证模块写单元测试

# 7. 修复问题
> 运行测试并修复失败的用例

# 8. 生成文档
> 更新 README.md，添加认证功能的说明

# 9. 提交代码
> 生成 git commit message

# 10. 查看费用
> /cost
```

### 快速问答模式

```bash
# 不进入交互模式，直接问答
ode "什么是 SOLID 原则？"
ode "解释这段代码" < script.js
ode "git log --oneline | head -10 | ode '总结最近的改动'"
```

### 批处理模式

```bash
# 分析多个文件
for file in src/*.cpp; do
  ode -p "review $file" >> review.txt
done

# 生成文档
ode -p "生成 API 文档" < api.ts > docs/api.md
```

## 故障排查

### 1. ode 命令未找到

```bash
# Windows: 检查 PATH
echo %PATH%

# Linux/macOS: 检查 PATH
echo $PATH

# 验证文件存在
ls -l /usr/local/bin/ode  # Linux/macOS
where ode                  # Windows
```

### 2. API 密钥错误

```bash
# 检查环境变量
echo $ANTHROPIC_API_KEY    # Linux/macOS
echo %ANTHROPIC_API_KEY%   # Windows (CMD)
$env:ANTHROPIC_API_KEY     # Windows (PowerShell)

# 重新登录
ode login anthropic
```

### 3. 权限问题

```bash
# Windows: 以管理员身份运行
# Linux/macOS: 使用 sudo
sudo ode login anthropic
```

### 4. 网络问题

```bash
# 测试连接
curl https://api.anthropic.com

# 使用代理
export HTTP_PROXY=http://proxy.example.com:8080
export HTTPS_PROXY=http://proxy.example.com:8080
```

## 更多资源

- **文档目录**：
  - `docs/BLUEPRINT_V2.md` - 设计蓝图
  - `docs/M3_FEATURES.md` - MCP、Hooks、权限
  - `docs/M4_FEATURES.md` - Subagents、RPC、Plugins
  - `docs/PROVIDERS.md` - 提供者配置
  - `docs/UI_UX_DESIGN.md` - UI/UX 设计

- **示例配置**：
  - `.claude/settings.example.json`
  - `.claude/plugins/example-plugin/`

- **社区**：
  - GitHub Issues
  - Discussions
  - Examples

## 提示和技巧

1. **使用别名**：为常用命令创建别名
   ```bash
   # ~/.bashrc 或 ~/.zshrc
   alias o="ode"
   alias or="ode -c"  # 继续会话
   alias op="ode --permission plan"  # 只读模式
   ```

2. **权限模式选择**：
   - `plan` - 探索和学习，不修改任何东西
   - `default` - 日常开发，重要操作需确认
   - `yolo` - 信任 AI，自动批准（小心使用）

3. **模型选择**：
   - `haiku` - 快速简单的问题
   - `sonnet` - 日常编码（默认，性价比高）
   - `opus` - 复杂任务和重要代码

4. **费用控制**：
   - 使用 `sonnet` 而不是 `opus` 节省 80% 费用
   - 使用 `/compact` 压缩长会话
   - 状态栏实时显示费用

5. **提高效率**：
   - 使用 `@` 补全文件路径
   - 使用 `/` 快速访问命令
   - 创建项目专用的 skills 和 commands

现在你可以在任何目录打开终端，输入 `ode` 就能开始使用了！🚀
