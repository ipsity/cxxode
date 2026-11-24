# 任务动效实现方案（最终版）

## 确认的设计
- ✅ **扫描动画** - 用于 Read/Grep/Glob（信息获取）
- ✅ **脉冲动画** - 用于 Write/Edit（文件修改）
- ✅ **旋转动画** - 用于 Bash（执行命令）
- ✅ **波浪动画** - 用于 Thinking（AI思考）
- ✅ **配色方案** - 蓝色/橙色/绿色/紫色等

## 动画帧设计

### 扫描动画（4帧，150ms/帧）
```cpp
"⎯", "⎯⎯", "⎯⎯⎯", "⎯⎯"
```
用于：Read, Grep, Glob

### 脉冲动画（3帧，180ms/帧）
```cpp
"◦", "●", "◉"
```
用于：Write, Edit

### 旋转动画（6帧，100ms/帧）
```cpp
"⠁", "⠂", "⠄", "⡀", "⢀", "⠠"
```
用于：Bash

### 波浪动画（6帧，120ms/帧）
```cpp
"⠁", "⠂", "⠄", "⠠", "⡀", "⢀"
```
用于：Thinking

## 实现代码

```cpp
struct tool_animation {
    const char** frames;
    int frame_count;
    int delay_ms;
};

// 扫描
const char* scan_frames[] = {"⎯", "⎯⎯", "⎯⎯⎯", "⎯⎯"};
tool_animation scan_anim = {scan_frames, 4, 150};

// 脉冲
const char* pulse_frames[] = {"◦", "●", "◉"};
tool_animation pulse_anim = {pulse_frames, 3, 180};

// 旋转
const char* rotate_frames[] = {"⠁", "⠂", "⠄", "⡀", "⢀", "⠠"};
tool_animation rotate_anim = {rotate_frames, 6, 100};

// 波浪
const char* wave_frames[] = {"⠁", "⠂", "⠄", "⠠", "⡀", "⢀"};
tool_animation wave_anim = {wave_frames, 6, 120};
```

## 颜色映射

```cpp
struct tool_color {
    uint8_t r, g, b;
};

std::map<std::string, tool_color> colors = {
    {"Read",  {79, 193, 255}},   // 蓝
    {"Write", {206, 145, 120}},  // 橙
    {"Edit",  {220, 220, 170}},  // 黄
    {"Bash",  {78, 201, 176}},   // 绿
    {"Grep",  {78, 201, 176}},   // 青
    {"Glob",  {78, 201, 176}},   // 青
};
```
