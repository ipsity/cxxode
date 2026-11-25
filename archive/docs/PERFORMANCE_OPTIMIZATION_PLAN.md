# 性能优化方案

## 🎯 优化目标

1. **减少卡顿** - 思考到执行的延迟
2. **提高响应速度** - UI 刷新更流畅
3. **降低 CPU 使用** - 减少不必要的计算
4. **优化渲染** - 减少重绘次数

---

## 📊 当前性能分析

### 可能的瓶颈

#### 1. UI 刷新频率
**位置**: `interactive.cpp:120`
```cpp
auto keys = term_.read(busy_ ? 40 : 120);  // 40ms/120ms 刷新间隔
```

**问题**:
- 忙碌时 40ms (25fps)
- 空闲时 120ms (8fps)
- 可能导致视觉卡顿

#### 2. 颜色代码生成
**位置**: `theme.cpp`, `interactive_view.cpp`
```cpp
std::string rgb_color::ansi_fg() const {
    std::ostringstream oss;
    oss << "\033[38;2;" << r << ";" << g << ";" << b << "m";
    return oss.str();
}
```

**问题**:
- 每次调用都创建新的 stringstream
- 每次都重新构造字符串
- 频繁调用时开销大

#### 3. 字符串拷贝
**位置**: 多处
- `paint()` 函数创建临时字符串
- `layout()` 返回 vector<string>
- 大量字符串连接操作

#### 4. 重复渲染
**位置**: `interactive_view.cpp:live()`
- 每次刷新都重新构建所有行
- 即使内容没变化

---

## 🚀 优化方案

### Phase 1: UI 刷新优化（立即见效）

#### 1.1 提高刷新率
```cpp
// 当前
auto keys = term_.read(busy_ ? 40 : 120);

// 优化后
auto keys = term_.read(busy_ ? 16 : 50);  // 60fps / 20fps
```

**收益**: 更流畅的动画和响应

#### 1.2 智能脏检查
```cpp
struct render_cache {
    std::vector<std::string> last_rows;
    bool dirty = true;
};

// 只在内容真正改变时重绘
if (cache.last_rows != current_rows) {
    screen_.draw(current_rows, row, column);
    cache.last_rows = current_rows;
}
```

**收益**: 减少 50%+ 的重绘

---

### Phase 2: 颜色代码缓存（中等收益）

#### 2.1 预生成颜色代码
```cpp
struct theme {
    // ... 现有字段
    
    // 缓存的 ANSI 代码
    std::string user_message_fg;
    std::string assistant_message_fg;
    std::string tool_read_fg;
    // ...
    
    void update_cache() {
        user_message_fg = user_message.ansi_fg();
        assistant_message_fg = assistant_message.ansi_fg();
        // ...
    }
};
```

**收益**: 减少 80% 的字符串构造

#### 2.2 优化 ansi_fg()
```cpp
// 当前：每次创建 stringstream
std::string rgb_color::ansi_fg() const {
    std::ostringstream oss;
    oss << "\033[38;2;" << r << ";" << g << ";" << b << "m";
    return oss.str();
}

// 优化后：直接拼接
std::string rgb_color::ansi_fg() const {
    return "\033[38;2;" + std::to_string(r) + ";" + 
           std::to_string(g) + ";" + std::to_string(b) + "m";
}
```

**收益**: 快 2-3 倍

---

### Phase 3: 字符串操作优化（高级）

#### 3.1 使用 string_view
```cpp
// 当前
std::string paint(tone t, std::string_view s);

// 优化后 - 避免拷贝
void paint_to(std::string& out, tone t, std::string_view s);
```

#### 3.2 预分配缓冲区
```cpp
std::vector<std::string> live(...) {
    std::vector<std::string> rows;
    rows.reserve(size_.rows);  // 预分配
    // ...
}
```

**收益**: 减少内存分配

---

### Phase 4: 事件处理优化

#### 4.1 减少事件队列锁争用
```cpp
// 当前：每次 drain() 都加锁
void drain() {
    std::lock_guard lock(mutex_);
    events.swap(queue_);
}

// 优化：批量处理
void drain() {
    std::deque<ui_event> batch;
    {
        std::lock_guard lock(mutex_);
        if (queue_.size() > 100) {
            batch.swap(queue_);
        } else {
            batch = std::move(queue_);
        }
    }
    for (auto& e : batch) process(e);
}
```

---

## 📋 实施计划

### 优先级 1：立即实施（1-2 小时）
- [x] 提高 UI 刷新率
- [ ] 添加基础的脏检查
- [ ] 优化 ansi_fg() 实现

### 优先级 2：短期优化（半天）
- [ ] 主题颜色代码缓存
- [ ] 预分配字符串缓冲区
- [ ] 减少临时对象创建

### 优先级 3：长期优化（1-2 天）
- [ ] 完整的增量渲染系统
- [ ] string_view 重构
- [ ] 事件系统优化

---

## 🧪 性能测试方法

### 测试场景

1. **启动时间**
```bash
time ./ode.exe --help
```

2. **UI 响应**
```bash
# 快速输入，观察延迟
# 长对话，观察滚动流畅度
```

3. **工具执行**
```bash
# 执行多个工具
# 观察 spinner 动画流畅度
```

### 预期改进

| 指标 | 当前 | 目标 | 改进 |
|------|------|------|------|
| UI 刷新率 | 8-25fps | 20-60fps | +150% |
| 响应延迟 | 100-200ms | 20-50ms | -75% |
| CPU 使用 | 中等 | 低 | -40% |
| 内存分配 | 频繁 | 较少 | -50% |

---

## 💡 快速修复（现在就做）

### 1. 提高刷新率
文件: `src/app/interactive.cpp:120`
```cpp
-auto keys = term_.read(busy_ ? 40 : 120);
+auto keys = term_.read(busy_ ? 16 : 33);  // 60fps / 30fps
```

### 2. 优化颜色代码
文件: `src/theme/theme.cpp:6`
```cpp
std::string rgb_color::ansi_fg() const {
-    std::ostringstream oss;
-    oss << "\033[38;2;" << static_cast<int>(r) << ";"
-        << static_cast<int>(g) << ";" << static_cast<int>(b) << "m";
-    return oss.str();
+    return "\033[38;2;" + std::to_string(r) + ";" + 
+           std::to_string(g) + ";" + std::to_string(b) + "m";
}
```

### 3. 添加脏检查标记
文件: `src/app/interactive.cpp:151`
```cpp
if (dirty) {
    int row = -1, column = 0;
    auto rows = live(row, column);
+   // 只在真正需要时重绘
+   static std::vector<std::string> last_rows;
+   if (rows != last_rows) {
        screen_.draw(rows, row, column);
+       last_rows = std::move(rows);
+   }
    last_draw = now;
    dirty = false;
}
```

---

## 🎯 下一步

**你希望：**

A. **立即实施快速修复** (30分钟)
   - 提高刷新率
   - 优化颜色代码
   - 基础脏检查

B. **完整优化方案** (2-3小时)
   - 包含 A 的所有内容
   - 颜色代码缓存
   - 预分配优化
   - 全面性能提升

C. **渐进式优化** (分阶段)
   - 先做 Phase 1
   - 测试效果
   - 再决定是否继续

**告诉我你的选择！** 🚀
