# cxxode UI/UX 设计文档

## 设计原则

### 核心理念
1. **极简主义**：只显示必要信息，避免视觉噪音
2. **即时反馈**：所有操作都有清晰的视觉反馈
3. **渐进式揭示**：初学者友好，专家高效
4. **性能优先**：流畅的 60fps 渲染，< 5ms 按键响应
5. **终端原生**：充分利用终端能力，不过度依赖花哨效果

### 视觉语言
- **简洁**：单一强调色（青色 #00d4ff），配合灰度
- **清晰**：明确的视觉层次和分组
- **一致**：统一的交互模式和视觉反馈
- **国际化**：中英文混排优化

## 当前实现分析

### ✅ 做得好的地方

1. **状态栏设计**
   ```
   ode  ~/projects/cxxode  main    default · claude-sonnet-4-6 · 42k/200k · $0.21
   ```
   - 信息密度合理
   - 实时费用显示
   - 清晰的层次

2. **命令面板**
   - `/` 触发，模态显示
   - 分类标记（skill/command）
   - 模糊搜索友好

3. **Spinner 动画**
   - Braille 字符（⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏）
   - 流畅的 100ms 间隔
   - 低视觉干扰

4. **权限提示**
   ```
   ? bash rm file.txt
     ⏎ 允许   esc 拒绝   a 总是允许 Bash(rm *)
   ```
   - 清晰的操作选项
   - 显示规则预览

### ⚠️ 需要改进的地方

1. **颜色硬编码**
   - RGB 值直接写在代码中
   - 无法适应不同终端背景
   - 缺少主题系统

2. **缺少欢迎流程**
   - 首次运行没有引导
   - 不知道有哪些功能
   - 快捷键不明显

3. **错误信息不够友好**
   - 技术性错误直接显示
   - 缺少建议性的解决方案
   - HTTP 错误码对用户不友好

4. **长时间操作缺少反馈**
   - MCP 初始化时无提示
   - 大文件读取无进度
   - 网络请求超时不明显

5. **视觉分隔不够**
   - 多个工具调用挤在一起
   - thinking 过程难以区分
   - 对话轮次不明显

## 改进方案

### 1. 主题系统

#### 1.1 颜色方案定义

**配置格式（settings.json）：**
```json
{
  "theme": {
    "name": "default-dark",
    "colors": {
      "accent": "#00d4ff",
      "accent_dim": "#0088aa",
      "error": "#ff5f5f",
      "warning": "#ffaa00",
      "success": "#00ff88",
      "dim": "#6c7086",
      "border": "#3c3c3c"
    }
  }
}
```

**内置主题：**
1. **default-dark**（当前）
   - 青色强调 (#00d4ff)
   - 适合深色终端

2. **default-light**
   - 蓝色强调 (#0066cc)
   - 适合浅色终端

3. **minimal**
   - 只用粗体/斜体
   - 完全单色
   - 适合单色终端或截图

4. **nord**
   - Nord 配色方案
   - 柔和的蓝绿色调

5. **github**
   - GitHub 风格配色
   - 蓝色/绿色/红色

#### 1.2 自动检测
```cpp
// 检测终端背景色
bool is_dark_background() {
    // 通过 OSC 11 查询
    // 或检测 COLORFGBG 环境变量
    // 失败时默认深色
}

// 自动选择主题
std::string auto_theme() {
    if (!color_enabled()) return "minimal";
    return is_dark_background() ? "default-dark" : "default-light";
}
```

### 2. 欢迎屏幕

#### 2.1 首次运行
```
╭─ 欢迎使用 ode ─────────────────────────────────────────────╮
│                                                              │
│  ode 是一个终端 coding agent，可以：                          │
│                                                              │
│  • 读写文件、运行命令、搜索代码                                 │
│  • 使用 Skills、MCP 工具、自定义插件                           │
│  • 实时显示费用，三档权限控制                                   │
│                                                              │
│  快速开始：                                                   │
│    ode "帮我分析 main.cpp"                                   │
│    ode --model opus "code review"                           │
│                                                              │
│  快捷键：                                                     │
│    /help    查看所有命令                                      │
│    @        文件补全                                          │
│    Shift+Tab 切换权限模式                                     │
│                                                              │
│  文档：https://github.com/user/cxxode                        │
│                                                              │
╰──────────────────────────────────────────────────────────────╯

继续输入开始对话...
```

#### 2.2 实现
```cpp
// .ode/welcome_shown 标记文件
bool should_show_welcome() {
    return !fs::exists(ode_home() / "welcome_shown");
}

void mark_welcome_shown() {
    std::ofstream(ode_home() / "welcome_shown");
}
```

### 3. 改进的视觉分隔

#### 3.1 对话轮次分隔
```
─────────────────────────────────────────── Turn 1 ───

👤 用户
帮我分析 main.cpp 的结构

🤖 助手 (thinking 2.3s, tokens: 1.2k → $0.004)
让我先读取这个文件...

─────────────────────────────────────────── Turn 2 ───
```

#### 3.2 工具调用组
```
╭─ Tools ─────────────────────────────────────╮
│ ⠋ read main.cpp                        0.1s │
│ ✓ bash git log --oneline -10          0.3s │
│ ⠋ grep "TODO" *.cpp                    0.2s │
╰─────────────────────────────────────────────╯
```

#### 3.3 Thinking 过程
```
💭 Thinking...
   First, I need to understand the structure...
   Then I'll analyze the dependencies...
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 2.3s
```

### 4. 增强的反馈系统

#### 4.1 加载状态
```cpp
// 阶段性加载提示
enum class loading_phase {
    initializing,     // "初始化..."
    loading_config,   // "加载配置..."
    connecting_mcp,   // "连接 MCP servers..."
    loading_skills,   // "加载 skills..."
    ready            // "就绪"
};
```

显示：
```
ode v0.3.0
├─ 加载配置... ✓ (3 files)
├─ 连接 MCP... ✓ (2 servers, 15 tools)
├─ 加载 Skills... ✓ (5 skills)
└─ 就绪

~/projects/cxxode  main
>
```

#### 4.2 错误信息改进
**Before:**
```
HTTP 401: {"error":{"type":"authentication_error","message":"invalid_api_key"}}
```

**After:**
```
❌ API 密钥无效

无法连接到 Anthropic API，请检查：
  • ANTHROPIC_API_KEY 环境变量是否设置
  • API 密钥是否有效（以 sk-ant- 开头）
  • 网络连接是否正常

运行 'ode login anthropic' 重新输入密钥
文档：https://docs.anthropic.com/api/authentication
```

#### 4.3 进度指示
```
⠋ 读取大文件 large.cpp (2.3 MB) ━━━━━━━━━━░░░░░░░░░░ 45%
```

### 5. 交互增强

#### 5.1 内联帮助
按 `?` 时显示上下文帮助：
```
╭─ 可用操作 ────────────────────────────────╮
│                                            │
│  ⏎ Enter    发送消息                       │
│  Esc        取消/中断                       │
│  /          命令面板                        │
│  @          文件补全                        │
│  ↑ ↓        历史导航                        │
│  Shift+Tab  切换权限 (当前: default)         │
│                                            │
│  输入 /help 查看完整命令列表                 │
│                                            │
╰────────────────────────────────────────────╯
```

#### 5.2 命令建议
输入错误命令时：
```
> /mode

未知命令 '/mode'

你是否想要：
  /model  - 查看或切换模型
  /mode   - 没有此命令，使用 Shift+Tab 切换权限模式

输入 / 查看所有可用命令
```

#### 5.3 快捷键提示
空闲时在底部显示（3 秒后淡出）：
```
提示: 按 / 打开命令面板，@ 补全文件路径
```

### 6. 性能优化的渲染

#### 6.1 增量渲染
```cpp
class incremental_renderer {
    std::vector<std::string> cached_rows;
    std::size_t last_rendered = 0;

    void render_delta() {
        // 只重绘变化的行
        for (size_t i = last_rendered; i < current_rows.size(); ++i) {
            render_row(i, current_rows[i]);
        }
        last_rendered = current_rows.size();
    }
};
```

#### 6.2 帧合并
```cpp
// 流式输出按帧合并（60fps = 16ms）
class frame_buffer {
    std::chrono::steady_clock::time_point last_flush;
    std::string buffer;

    void maybe_flush() {
        auto now = std::chrono::steady_clock::now();
        if (now - last_flush >= std::chrono::milliseconds(16)) {
            flush();
        }
    }
};
```

### 7. 响应式布局

#### 7.1 窄终端适配
```cpp
// < 80 列时简化显示
if (columns < 80) {
    // 状态栏只显示核心信息
    status = "ode · " + model_short_name() + " · " + cost_str;
    
    // 工具标签截断
    tool_label = truncate(tool_label, 20);
}

// < 40 列时最小化
if (columns < 40) {
    status = short_count(tokens);
    // 只显示进度，不显示详情
}
```

#### 7.2 宽终端利用
```cpp
// > 120 列时显示更多
if (columns > 120) {
    // 显示完整的文件路径
    // 显示详细的 token 分解
    // 并排显示多个工具调用
}
```

### 8. 可访问性

#### 8.1 屏幕阅读器支持
```cpp
// 通过 aria-live 区域通知
void announce(std::string_view message) {
    if (screen_reader_mode) {
        std::cout << "\x1b]777;notify;" << message << "\x07";
    }
}
```

#### 8.2 高对比度模式
```cpp
// 检测 NO_COLOR 或高对比度设置
bool high_contrast_mode() {
    return std::getenv("ODE_HIGH_CONTRAST") != nullptr;
}
```

#### 8.3 可调字体大小
```
# settings.json
{
  "ui": {
    "compact": false,     # 紧凑模式（更小行间距）
    "verbose": true,      # 详细模式（显示更多信息）
    "ascii_only": false   # 只用 ASCII 字符
  }
}
```

## 实现优先级

### P0 - 核心改进（立即）
1. ✅ 主题系统基础（已有 color_enabled）
2. ⬜ 友好的错误信息
3. ⬜ 欢迎屏幕
4. ⬜ 内联帮助（`?` 键）

### P1 - 视觉增强（短期）
1. ⬜ 对话轮次分隔
2. ⬜ 工具调用分组
3. ⬜ Thinking 过程优化
4. ⬜ 进度指示器

### P2 - 高级功能（中期）
1. ⬜ 主题配置系统
2. ⬜ 响应式布局
3. ⬜ 命令建议
4. ⬜ 加载阶段显示

### P3 - 打磨（长期）
1. ⬜ 可访问性完整支持
2. ⬜ 多主题支持
3. ⬜ 动画效果
4. ⬜ 自定义 UI 组件

## 设计规范

### 颜色使用指南

| 场景 | 颜色 | 用途 |
|------|------|------|
| 强调 | accent | 命令、工具名、链接 |
| 成功 | success | ✓ 完成的操作 |
| 警告 | warning | ⚠ 需要注意 |
| 错误 | error | ❌ 失败的操作 |
| 辅助 | dim | 次要信息、时间戳 |
| 正常 | normal | 主要文本 |

### 图标使用

```
操作状态：
  ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏  加载中（动画）
  ✓             成功
  ❌            失败
  ⚠             警告
  ℹ             信息

实体：
  👤            用户
  🤖            助手
  🛠             工具
  📁            文件
  🔧            配置
  💭            Thinking
  🎯            目标
```

### 间距规范

```
紧凑模式（compact: true）：
  - 行间距：0
  - 段落间距：1 行
  - 分隔线：单行

正常模式（默认）：
  - 行间距：0
  - 段落间距：1-2 行
  - 分隔线：带文字

宽松模式（verbose: true）：
  - 行间距：0-1
  - 段落间距：2 行
  - 分隔线：双行
```

### 文本规范

```
标题：粗体 + accent
副标题：粗体
强调：accent 或粗体
代码：code 颜色 + 等宽
文件路径：dim + 斜体
时间戳：dim + 右对齐
错误：error + ❌
成功：success + ✓
```

## A/B 测试计划

### 测试场景
1. **欢迎屏幕** vs **直接开始**
   - 指标：新用户完成首个任务的时间
   
2. **详细分隔** vs **紧凑显示**
   - 指标：用户满意度、可读性评分

3. **颜色强调** vs **单色显示**
   - 指标：信息识别速度

### 收集反馈
```bash
# 匿名使用统计（可选）
ode --telemetry on

# 反馈命令
ode feedback "建议..."
```

## 技术实现

### 文件结构
```
src/ui/
  ├── theme.hpp/cpp       # 主题系统
  ├── components.hpp/cpp  # UI 组件库
  ├── layout.hpp/cpp      # 响应式布局
  ├── feedback.hpp/cpp    # 反馈系统
  └── welcome.hpp/cpp     # 欢迎屏幕
```

### 配置示例
```json
{
  "ui": {
    "theme": "default-dark",
    "compact": false,
    "show_welcome": true,
    "animations": true,
    "icons": "unicode"
  }
}
```

## 参考设计

### 灵感来源
- **GitHub Copilot CLI** - 简洁的命令面板
- **Warp Terminal** - 流畅的 AI 集成
- **Zed Editor** - 优雅的 inline assistant
- **Claude.ai** - 清晰的对话界面
- **Rich (Python)** - 丰富的终端 UI 库

### 避免的陷阱
1. ❌ 过度使用颜色和图标
2. ❌ 动画导致的性能问题
3. ❌ 在不同终端上显示不一致
4. ❌ 信息过载
5. ❌ 牺牲性能换取美观

## 总结

cxxode 的 UI/UX 设计理念是**简洁、高效、专业**。通过渐进式的改进，在保持终端原生感的同时，提供现代化的用户体验。

优先级：先解决痛点（错误信息、欢迎流程），再优化视觉（分隔、主题），最后打磨细节（动画、响应式）。
