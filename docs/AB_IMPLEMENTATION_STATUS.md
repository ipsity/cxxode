# A&B 功能实现状态

## ✅ B. 交互式模型选择器（已完成）

### 功能
- 输入 `/model` 显示所有可用模型
- 每个模型带编号
- 当前模型标记 ⭐
- 显示描述和 context window

### 使用方法
```
> /model
Available models from passion:
1. codex-auto-review
2. gpt-5.6-luna
3. gpt-6.2-sol ⭐  - (200k)
...

Use: /model <number> or /model <name>

> /model 3
Switched to model: gpt-6.2-sol
```

### 实现细节
- 在 `commands.cpp` 中扩展 `/model` 命令
- 支持数字选择和名称选择
- 自动获取模型列表
- 实时显示当前模型

---

## 🔄 A. 配置管理（占位符）

### 当前状态
- `/login` 命令已添加，显示提示信息
- `/logout` 命令已添加，显示信息
- 实际功能暂时指向命令行工具

### 为什么是占位符？
配置管理涉及复杂的交互流程：
1. **多步骤输入** - 需要状态机
2. **密码输入** - 需要隐藏输入
3. **异步验证** - 需要测试 API
4. **凭据管理** - 需要安全存储

这些功能需要：
- 扩展交互系统支持多步骤对话
- 实现安全的密码输入
- 添加 API 验证逻辑
- 整合 Windows Credential Manager

### 当前替代方案
用户可以使用：
```bash
# 命令行登录（已存在）
ode login anthropic

# 或手动编辑配置
~/.ode/settings.json
```

---

## 📋 建议的实现路径

### 完整的 `/login` 实现需要：

#### 1. 交互式输入框架
```cpp
class interactive_prompt {
    std::function<void(std::string)> on_submit;
    std::string prompt_text;
    bool is_password;
};
```

#### 2. 多步骤流程
```
Step 1: 选择 provider 类型
  → 预设 (Anthropic/OpenAI/...)
  → 自定义

Step 2: 输入 API key
  → 密码模式输入
  → 验证格式

Step 3: 测试连接
  → 调用 API
  → 显示结果

Step 4: 保存配置
  → 更新 settings.json
  → 保存到 Credential Manager
```

#### 3. Provider 预设
```cpp
struct provider_preset {
    std::string name;
    std::string protocol;
    std::string base_url;
    std::string description;
};

const provider_preset presets[] = {
    {"anthropic", "anthropic", "https://api.anthropic.com", "Claude models"},
    {"openai", "openai-chat", "https://api.openai.com/v1", "GPT models"},
    {"groq", "openai-chat", "https://api.groq.com/openai/v1", "Fast inference"},
    // ...
};
```

---

## ✅ 可以先测试的功能

**模型选择器现在就可以用了！**

请测试：
```bash
./build/dev/ode.exe

# 在交互模式中
> /model
# 应该显示模型列表

> /model 1
# 应该切换到第一个模型

> /model gpt-6.2-sol
# 应该切换到指定模型
```

**`/login` 和 `/logout` 也可以测试，但只会显示提示信息。**

---

## 🎯 下一步选择

**选项 1：完整实现 `/login`**
- 时间：2-3 天
- 复杂度：高
- 需要扩展交互系统

**选项 2：保持占位符，继续其他功能**
- 用户可以用命令行 `ode login`
- 或手动编辑配置文件
- 专注于其他有价值的功能

**选项 3：实现简化版 `/login`**
- 只支持 API key 输入
- 预设 provider 列表
- 不做完整验证
- 时间：1 天

**你希望？**
