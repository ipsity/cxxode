# cxxode V2 UI & UX 重设计方案

## 🔴 Critical Bug 修复

### 命令调色板 Bug
**问题：** 使用方向键选择 `/model` 后按回车，命令被重复处理
**根因：** `interactive_input.cpp:19` 的条件判断错误
**修复：** 检查 `palette_index_ > 0` 来判断是否从调色板选择

---

## 📐 UI 重设计方案

### 1. 输入框设计（核心改动）

#### 当前问题
- 输入框位置不固定
- 多行输入体验不佳
- 视觉边界不清晰

#### 新设计：固定底部输入框

```
┌─────────────────────────────────────────────┐
│                                             │
│  [对话历史区域 - 可滚动]                      │
│                                             │
│                                             │
│                                             │
│                                             │
├─────────────────────────────────────────────┤  ← 上横线（可移动）
│ 输入内容...                                  │
│ 可以换行...                                  │
├─────────────────────────────────────────────┤  ← 下横线（固定底部）
│ ode ~/project  ⚡ gpt-6.2-sol  🎫 15k/200k  │  ← 状态栏
└─────────────────────────────────────────────┘
```

**实现要点：**
- 下横线固定在终端底部（倒数第二行）
- 状态栏固定在最底行
- 上横线随输入内容行数变化
- 单行时：上下横线重合，显示为一条
- 多行时：上横线上移，形成输入框

**代码结构：**
```cpp
class fixed_input_box {
    int min_height = 1;  // 最小高度（单行）
    int max_height = 10; // 最大高度（10行）
    int current_lines = 1;
    
    void render(int terminal_height) {
        // 计算位置
        int bottom_line = terminal_height - 1;  // 状态栏
        int input_bottom = terminal_height - 2;  // 下横线
        int input_top = input_bottom - current_lines;  // 上横线
        
        // 绘制
        draw_top_border(input_top);
        draw_input_content(input_top + 1, input_bottom - 1);
        draw_bottom_border(input_bottom);
        draw_status_bar(bottom_line);
    }
};
```

---

### 2. 任务执行显示（工具调用）

#### 当前问题
- 显示"● 写 src/file.cpp"，中文动词不专业
- 工具名不清晰

#### 新设计：使用英文工具名

```
当前：● 写 src/app/interactive.cpp
改为：● Write src/app/interactive.cpp

当前：● 跑 npm test
改为：● Bash npm test

当前：● 读 package.json
改为：● Read package.json
```

**工具名映射：**
```cpp
const std::map<std::string, std::string> tool_display_names = {
    {"read", "Read"},
    {"write", "Write"},
    {"edit", "Edit"},
    {"bash", "Bash"},
    {"grep", "Grep"},
    {"glob", "Glob"},
    // ... 其他工具
};
```

**显示格式：**
```
● Read src/config.hpp
  ↓ [进度指示]
✓ Read src/config.hpp (142 lines)

● Bash npm test
  ↓ [实时输出]
✓ Bash npm test (exit 0)

✗ Write failed.txt
  ↓ Permission denied
```

---

### 3. 配色方案设计

#### 配色原则
- **高对比度**：确保可读性
- **语义化**：不同状态用不同颜色
- **护眼**：避免过亮或刺眼的颜色

#### 配色方案 A（深色主题）

```
背景：     #1e1e1e  (深灰)
文本：     #d4d4d4  (浅灰)
横线：     #3c3c3c  (中灰)
强调：     #4fc1ff  (蓝色)
成功：     #4ec9b0  (青色)
警告：     #ce9178  (橙色)
错误：     #f48771  (红色)
次要文本：  #6a6a6a  (暗灰)
```

#### 配色方案 B（护眼绿）

```
背景：     #002b36  (深蓝绿)
文本：     #839496  (灰绿)
横线：     #073642  (深青)
强调：     #268bd2  (蓝)
成功：     #859900  (绿)
警告：     #b58900  (黄)
错误：     #dc322f  (红)
次要文本：  #586e75  (暗绿)
```

**实现方式：**
```cpp
struct theme {
    struct color {
        uint8_t r, g, b;
    };
    
    color background;
    color text;
    color border;
    color accent;
    color success;
    color warning;
    color error;
    color dim;
};

// 预设主题
const theme dark_theme = { ... };
const theme solarized_theme = { ... };
const theme light_theme = { ... };
```

---

### 4. 动效设计

#### 加载动画

**当前：** 旋转的点
```
⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏
```

**可选方案：**

A. 进度条
```
[========>       ] 45%
```

B. 脉冲点
```
● Working...
◐ Working...
◑ Working...
◒ Working...
```

C. 波浪
```
▁▂▃▄▅▆▇█ Processing...
```

#### 过渡动画

**输入框展开：**
```
━━━━━━━━━━━━━━━━━  (单行)
  ↓ 
┌─────────────────┐
│                 │  (展开到多行)
└─────────────────┘
```

**工具执行：**
```
● Read file.cpp          (开始)
  ↓
● Read file.cpp [=====>  (进行中，带进度)
  ↓
✓ Read file.cpp (142 lines)  (完成)
```

---

### 5. 字体建议

#### 推荐字体
1. **Cascadia Code** - Microsoft 官方，支持连字
2. **JetBrains Mono** - 专为编程设计
3. **Fira Code** - 开源，连字支持好
4. **Consolas** - Windows 经典

#### 中英文混排
- 英文：等宽字体
- 中文：等宽中文字体（如更纱黑体 Mono）

**代码中不控制字体，由终端设置决定**

---

## 🔐 配置管理改进方案

### 新增命令

#### `/login` 命令

**交互流程：**

```
> /login

选择登录方式：
1. Official Account  (官方账号)
2. API Key           (API 密钥)
3. Custom Provider   (自定义服务商)

输入选项 [1-3]: _
```

##### 选项 1：官方账号登录
```
> /login
> 1

正在打开浏览器进行授权...
[浏览器打开 OAuth 页面]

等待授权... ⠋

✓ 登录成功！
  账号：user@example.com
  Token 已保存到 Windows Credential Manager
```

##### 选项 2：API Key 登录
```
> /login
> 2

选择服务商：
1. Anthropic (claude.ai)
2. OpenAI (openai.com)
3. Groq (groq.com)
4. DeepSeek (deepseek.com)
5. Custom (自定义)

输入选项 [1-5]: 2

请输入 OpenAI API Key:
sk-************************************************

验证中... ⠋

✓ 验证成功！
  Provider: openai
  可用模型: gpt-4o, gpt-4o-mini, ...
  API Key 已保存

是否设置为默认 provider? [Y/n]: y
✓ 已设置 openai 为默认 provider
```

##### 选项 3：自定义 Provider
```
> /login
> 2
> 5 (Custom)

请输入配置信息：

Provider 名称: my-api
Base URL: https://api.example.com/v1
API Key: sk-********************************
协议类型:
  1. anthropic
  2. openai-chat
  3. openai-responses
选择 [1-3]: 3

测试连接中... ⠋

✓ 连接成功！
  获取到 23 个可用模型

选择默认模型:
1. gpt-6.2-sol
2. gpt-6.2-astra
3. gpt-6.1-sol
...
输入序号 [1-23]: 1

✓ 配置已保存！
  Provider: my-api
  默认模型: gpt-6.2-sol
  
是否立即切换到此 provider? [Y/n]: y
✓ 已切换到 my-api
```

#### `/logout` 命令

```
> /logout

当前登录：
  Provider: passion
  Model: gpt-6.2-sol
  
确认退出登录? [y/N]: y

✓ 已退出登录
  凭据已从 Credential Manager 移除
  
下次使用请先 /login
```

---

### 配置存储结构

**配置文件自动管理：**
```
~/.ode/
  ├── credentials/          # 凭据（敏感信息）
  │   ├── anthropic.json
  │   ├── openai.json
  │   └── passion.json
  ├── settings.json         # 全局设置
  └── providers.json        # Provider 配置
```

**Windows Credential Manager：**
```
目标名称: ode/anthropic
用户名: api_key
密码: sk-ant-...

目标名称: ode/openai
用户名: api_key
密码: sk-...
```

**自动生成 settings.json：**
```json
{
  "provider": "passion",
  "model": "gpt-6.2-sol",
  "providers": {
    "passion": {
      "protocol": "openai-responses",
      "base_url": "https://api.passionapi.com/v1",
      "api_key_env": "ODE_PASSION_KEY",
      "model": "gpt-6.2-sol"
    }
  }
}
```

---

### 首次使用体验

```
$ ode

欢迎使用 cxxode！

检测到这是首次运行，需要配置 API 访问。

选择配置方式：
1. 快速开始 - 使用 Anthropic Claude (推荐)
2. 自定义配置 - 选择其他服务商
3. 稍后配置 - 手动编辑配置文件

输入选项 [1-3]: _
```

---

## 📋 实现计划

### Phase 1: Bug 修复 + 命令改进（1-2天）
- [x] 修复调色板选择 bug
- [ ] 工具名改为英文
- [ ] 测试所有命令功能

### Phase 2: 输入框重设计（3-5天）
- [ ] 设计固定底部输入框
- [ ] 实现动态边框
- [ ] 多行输入支持
- [ ] 滚动区域分离

### Phase 3: 配色与动效（2-3天）
- [ ] 实现主题系统
- [ ] 添加 2-3 个预设主题
- [ ] 改进加载动画
- [ ] 添加过渡效果

### Phase 4: 配置管理（3-4天）
- [ ] 实现 `/login` 命令
- [ ] 实现 `/logout` 命令
- [ ] Provider 在线配置
- [ ] 首次使用向导

### Phase 5: 模型选择器（2天）
- [ ] 交互式模型列表
- [ ] 方向键选择
- [ ] 模型信息显示
- [ ] 快速切换

---

## 🎯 下一步

请确认：
1. **UI 设计方向**是否符合你的预期？
2. **配色方案** A 还是 B？或者其他建议？
3. **实现优先级**：你最希望先看到哪个功能？

我们可以先做一个小的 prototype 让你看看效果再继续。
