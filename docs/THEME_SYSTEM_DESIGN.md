# 主题系统设计

## 配色方案

### 当前深色主题配色
```cpp
struct dark_theme {
    // 用户消息
    user_message: #00d4ff (青色加粗)
    
    // AI 回复
    assistant_message: #d4d4d4 (浅灰)
    
    // 工具颜色
    tool_read: #4fc1ff (蓝)
    tool_write: #ce9178 (橙)
    tool_edit: #dcdcaa (黄)
    tool_bash: #4ec9b0 (绿)
    tool_search: #4ec9b0 (青)
    
    // UI 元素
    prompt: #00d4ff (青)
    border: #6a6a6a (灰)
    dim: #6a6a6a (暗灰)
    accent: #00d4ff (青)
    error: #f48771 (红)
    
    // 状态
    success: #4ec9b0 (绿)
    warning: #ce9178 (橙)
    info: #4fc1ff (蓝)
};
```

### 可选主题

#### 1. Light Theme (浅色)
- 背景：白色
- 文字：深灰
- 强调：深蓝

#### 2. Solarized Dark
- 经典的 Solarized 配色

#### 3. Dracula
- 流行的紫色主题

#### 4. Nord
- 冷色调主题

#### 5. Monokai
- 经典编辑器主题

## 实现计划

### Phase 1: 主题结构 (30分钟)
- 创建 theme.hpp/cpp
- 定义主题数据结构
- 加载/保存主题配置

### Phase 2: /theme 命令 (1小时)
- 列出可用主题
- 交互式主题选择
- 实时预览（如果可能）
- 保存选择

### Phase 3: 自定义配色编辑器 (2小时)
- 交互式颜色编辑
- RGB 值调整
- 实时预览
- 保存自定义主题

### Phase 4: 应用主题到所有 UI (1小时)
- 替换硬编码颜色
- 使用主题系统
- 测试所有场景
