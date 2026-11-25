# 快捷命令使用指南

## 🚀 7 个工作流命令，覆盖开发全流程

### 📖 命令概览

| 命令 | 别名 | 功能 | 自动化流程 |
|------|------|------|-----------|
| `/ref` | - | 查看编辑文件 | Read → Edit → Write |
| `/search` | `/s` | 搜索代码 | Grep → Read |
| `/fix` | - | 智能修复 | 分析 → 修复 |
| `/test` | `/t` | 运行测试 | 检测 → 执行 → 报告 |
| `/build` | `/b` | 构建项目 | 检测 → 构建 |
| `/run` | `/r` | 运行程序 | 编译 → 执行 |
| `/commit` | `/c` | 智能提交 | 分析 → 提交 |

---

## 💡 实战场景

### 场景 1：快速修改文件
```
❌ 以前（3 步）：
  1. Read config.json
  2. Edit config.json [修改内容]
  3. Write config.json

✅ 现在（1 步）：
  /ref config.json
  
提示词示例：
"Change the port from 8080 to 3000"
```

### 场景 2：搜索并修复 bug
```
❌ 以前（4+ 步）：
  1. Grep "TODO"
  2. Read file1.cpp
  3. Read file2.cpp
  4. Edit ...

✅ 现在（2 步）：
  /s TODO          # 搜索所有 TODO
  /fix file.cpp    # 修复具体文件
```

### 场景 3：测试驱动开发
```
❌ 以前：
  Bash "npm test"  或  Bash "pytest"
  需要记住测试命令

✅ 现在：
  /t              # 运行所有测试
  /t login        # 只测试登录相关
  /t "user.*api"  # 正则匹配测试
```

### 场景 4：构建和运行
```
❌ 以前：
  Bash "cmake --build ..."
  Bash "npm run build"
  需要知道具体命令

✅ 现在：
  /b              # 自动检测并构建
  /r              # 自动运行
  /r --port 8080  # 带参数运行
```

### 场景 5：提交代码
```
❌ 以前（3 步）：
  1. 查看更改
  2. 思考提交消息
  3. Git commit

✅ 现在（1 步）：
  /c              # 智能分析并提交
  /c "fix bug"    # 使用指定消息
```

---

## 🎯 最佳实践

### 1. 使用短别名提升效率
```
/s pattern      替代  /search pattern
/t              替代  /test
/b              替代  /build
/r              替代  /run
/c              替代  /commit
```

### 2. 组合使用工作流
```
开发流程：
/s "bug"        # 1. 找到问题
/fix main.cpp   # 2. 修复问题
/t              # 3. 运行测试
/c              # 4. 提交更改

功能开发：
/ref feature.h  # 1. 编辑头文件
/ref feature.cpp # 2. 实现功能
/t feature      # 3. 测试功能
/b              # 4. 构建项目
/r              # 5. 运行验证
```

### 3. 善用参数
```
# 有参数 = 精确操作
/ref main.cpp           # 编辑特定文件
/s "TODO"               # 搜索特定内容
/t login                # 测试特定功能

# 无参数 = 智能分析
/fix                    # 分析最近错误并修复
/t                      # 运行所有测试
/c                      # 智能生成提交消息
```

---

## 🔍 命令面板技巧

### 模糊搜索
```
输入 /rf      → 匹配 /ref
输入 /sr      → 匹配 /search
输入 /cm      → 匹配 /commit
输入 /test    → 完全匹配
```

### 快速导航
```
1. 输入 /
2. 看到分类列表：
   ── Commands ──
   📖 /help
   🔄 /ref
   🔍 /search
   ...
   
3. 继续输入筛选
4. ↑↓ 选择
5. Enter 确认
```

### 查看所有命令
```
/              # 只输入 / 显示所有命令
/help          # 查看完整帮助
```

---

## ⚡ 效率对比

### 传统方式 vs 快捷命令

**修改 3 个文件：**
```
传统：9 条命令
Read file1 → Edit file1 → Write file1
Read file2 → Edit file2 → Write file2
Read file3 → Edit file3 → Write file3

快捷：3 条命令
/ref file1
/ref file2
/ref file3

节省：67% 命令数量！
```

**搜索并修复：**
```
传统：5+ 条命令
Grep pattern → Read file1 → Read file2 → Edit → Write

快捷：2 条命令
/s pattern → /fix file

节省：60% 命令数量！
```

**测试和提交：**
```
传统：3 条命令
Bash "test command" → 检查结果 → Git commit

快捷：2 条命令
/t → /c

节省：33% 命令数量！
```

---

## 🎓 进阶技巧

### 1. 结合自然语言
```
/ref config.json
提示："Change port to 3000 and enable debug mode"

AI 会理解你的意图，自动完成所有步骤
```

### 2. 迭代式开发
```
/s "login"           # 找到登录相关代码
/ref auth.cpp        # 编辑认证模块
/t login             # 测试登录功能
/fix auth.cpp        # 修复发现的问题
/t login             # 再次测试
/c "improve login"   # 提交更改
```

### 3. 错误修复循环
```
/b                   # 构建失败
/fix                 # 自动分析并修复编译错误
/b                   # 再次构建
/t                   # 运行测试
/fix                 # 修复测试失败
/c                   # 提交修复
```

---

## 📊 时间节省计算

### 单次操作
- 传统 3 命令 → 快捷 1 命令：**节省 67% 时间**
- 传统 5 命令 → 快捷 2 命令：**节省 60% 时间**

### 每天开发
假设每天 20 次文件操作：
- 传统：20 × 3 = 60 条命令
- 快捷：20 × 1 = 20 条命令
- **每天节省 40 条命令输入！**

### 每周开发
- 传统：300 条命令
- 快捷：100 条命令
- **每周节省 200 条命令！**

---

## 💪 实战演练

### 练习 1：修改配置
```
任务：将端口从 8080 改为 3000

快捷方式：
/ref config.json

提示词：
"Change port from 8080 to 3000"
```

### 练习 2：搜索并修复 TODO
```
任务：找到所有 TODO 并完成

快捷方式：
/s TODO
/fix [文件名]
```

### 练习 3：完整开发流程
```
任务：添加新功能并测试

步骤：
1. /s "feature"      # 了解现有功能
2. /ref new.cpp      # 创建新文件
3. /t                # 运行测试
4. /fix new.cpp      # 修复问题
5. /b                # 构建项目
6. /r                # 运行验证
7. /c                # 提交代码
```

---

## 🎉 总结

### 核心价值
✅ **更少命令** - 1 条替代 3-5 条
✅ **更快速度** - 短别名 + 自动化
✅ **更智能** - AI 理解意图
✅ **更简单** - 不需要记住复杂命令

### 记住这些
1. `/ref` - 最常用，查看编辑文件
2. `/s` - 搜索代码（s = search）
3. `/t` - 测试（t = test）
4. `/b` - 构建（b = build）
5. `/r` - 运行（r = run）
6. `/c` - 提交（c = commit）

### 开始使用
```
现在就试试：
/help           # 查看完整帮助
/ref README.md  # 编辑 README
/t              # 运行测试

享受高效开发！🚀
```
