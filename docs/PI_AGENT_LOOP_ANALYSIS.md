# Pi Agent 循环和工具系统深度分析

## 🏗️ Pi 的核心架构

### 三层架构

```
┌─────────────────────────────────────┐
│   Harness (Durable Framework)      │  ← Agent 循环、任务调度
├─────────────────────────────────────┤
│   Chord (State Management)          │  ← 文档事务、状态同步
├─────────────────────────────────────┤
│   Storage (SQLite/Memory)           │  ← 持久化
└─────────────────────────────────────┘
```

---

## 🔄 Agent 循环设计

### Generation Task 状态机

Pi 的 Agent 循环是一个**状态机**，通过 `GenerationCheckpoint` 定义：

```typescript
type GenerationCheckpoint =
  | { phase: "prepare" }     // 1. 准备系统提示和工具
  | { phase: "request" }     // 2. 发送 API 请求
  | { phase: "retry" }       // 3. 重试（如果失败）
  | { phase: "poll" }        // 4. 轮询延迟响应
  | { phase: "tools" }       // 5. 执行工具调用
  | { phase: "finish" }      // 6. 完成并触发钩子
  | { phase: "drain" }       // 7. 等待后续任务
```

### 核心流程

```
用户输入
  ↓
[prepare] ────────────→ 渲染系统提示
  │                     检查上下文大小
  │                     准备工具列表
  ↓
[request] ────────────→ 调用 Anthropic API
  │                     流式响应
  │                     处理延迟响应
  ↓
失败? ──Yes─→ [retry] ──→ 延迟重试
  │No
  ↓
延迟? ──Yes─→ [poll] ───→ 轮询结果
  │No
  ↓
工具调用? ──Yes─→ [tools] → 执行工具
  │No                        ↓
  ↓                     并行/串行
  ↓                          ↓
[finish] ─────────────→ 保存答案
  ↓                     触发钩子
[drain] ──────────────→ 等待钩子任务
  ↓
结束
```

---

## 🛠️ 工具系统设计

### 1. 工具定义（Tool Registration）

```typescript
interface ToolRegistration {
  name: string;
  description: string;
  inputSchema: JSONSchema;
  
  // 执行函数
  execute(args, api, context): ToolExecutionResult;
  
  // 访问控制
  origin?: "any" | "owner";       // 谁可以调用
  scope?: "any" | "main" | "subagent";  // 在哪里可用
  locked?: "pass" | "never" | LockCapability;  // 锁定模式
  
  // 执行模式
  callers?: Array<"model" | "user">;  // 谁可以触发
  deferred?: boolean;                 // 是否延迟执行
}
```

### 2. 工具访问控制（guardTool）

**三重检查机制：**

```typescript
async function guardTool(tool: P2Tool): Promise<ToolExecutionResult> {
  // 1. Scope 检查
  if (tool.scope === "main" && !isTopLevel(context)) {
    return refusal("Tool only works in main conversation");
  }
  
  // 2. Origin 检查
  if (tool.origin === "owner" && await originOf(context) !== "owner") {
    return refusal("Tool needs owner-started turn");
  }
  
  // 3. Lock 检查
  const lock = await sessionLock(context);
  if (lock && tool.locked === "never") {
    return refusal("Tool not available in locked session");
  }
  
  // 执行工具
  return await tool.execute(args, api, context);
}
```

### 3. 工具执行模式

**并行 vs 串行：**

```typescript
// 并行执行（默认）
toolExecution: "parallel"
// 所有工具同时开始

// 串行执行
toolExecution: "sequential"
// 一个完成后再开始下一个
```

**工具调用流程：**

```
Model 返回 tool_use
  ↓
创建 ToolTask
  ↓
检查访问权限 (guardTool)
  ↓
并行/串行执行
  ↓
收集结果
  ↓
追加 tool_result 到对话
  ↓
下一轮 Generation
```

---

## 📦 Durable 框架核心概念

### 1. Document-Based State

**所有状态存储在文档中：**

```typescript
// Agent 配置文档
const AgentDoc = defineDoc<AgentState>({
  kind: "pi.agent",
  scope: "conversation",
  history: "rewindable",  // 可回溯
  fork: "asOf",           // Fork 时复制
});

// Live 状态文档
const LiveDoc = defineDoc<LiveState>({
  kind: "pi.live",
  scope: "conversation",
  // 当前运行的状态
});
```

### 2. Transaction System

**所有修改通过事务：**

```typescript
await runtime.commit(async (tx) => {
  // 读取文档
  const agent = await tx.doc(AgentDoc, conversationId);
  
  // 修改状态
  agent.model = newModel;
  
  // 自动提交
}, context);
```

**事务保证：**
- ✅ ACID 属性
- ✅ 崩溃恢复
- ✅ 并发控制
- ✅ 增量更新

### 3. Task System

**所有异步工作都是 Task：**

```typescript
const GenerationTask = defineTask({
  name: "pi.generation",
  
  initial: () => ({ phase: "prepare", attempt: 1 }),
  
  phases: {
    prepare: async (task, runtime, context) => {
      // 准备工作
      return { status: "waiting", checkpoint, on: [compactionTask] };
    },
    
    request: async (task, runtime, context) => {
      // 发送请求
      return { status: "waiting", checkpoint, on: [toolTasks] };
    },
    
    tools: async (task, runtime, context) => {
      // 执行工具
      return { status: "completed", result };
    }
  }
});
```

**Task 特点：**
- ✅ 可暂停/恢复
- ✅ 崩溃后恢复
- ✅ 依赖关系管理
- ✅ 超时和重试

---

## 🆚 与 Ode 的对比

| 特性 | Pi (Durable) | Ode (C++) | 评价 |
|------|--------------|-----------|------|
| **架构** | 状态机 + 文档存储 | 直接循环 | Pi 更复杂但更健壮 |
| **状态管理** | 事务性文档 | 内存变量 | Pi 可恢复，Ode 更快 |
| **工具执行** | 异步 Task | 同步调用 | Pi 可并行，可恢复 |
| **错误处理** | 自动重试 + 恢复 | 手动处理 | Pi 更可靠 |
| **并发** | 完整支持 | 有限支持 | Pi 更强大 |
| **持久化** | SQLite | 文本日志 | Pi 更结构化 |
| **崩溃恢复** | ✅ 完整 | ❌ 无 | Pi 优势明显 |

---

## 💡 Pi 的优秀设计

### 1. 状态机模式

**优点：**
- 每个阶段明确定义
- 可以在任意点暂停/恢复
- 崩溃后从 checkpoint 继续
- 易于测试和调试

**示例：**
```typescript
// Pi: 明确的状态转换
prepare → request → tools → finish

// Ode: 隐式循环
while (!done) {
  response = api_call();
  if (tool_calls) execute_tools();
}
```

### 2. 访问控制系统

**三维权限模型：**

| 维度 | 选项 | 用途 |
|------|------|------|
| origin | any/owner | 谁可以调用 |
| scope | any/main/subagent | 在哪里可用 |
| locked | pass/never/capability | 锁定会话行为 |

这让工具可以精细控制使用场景。

### 3. 延迟工具（Deferred Tools）

```typescript
tool: {
  name: "long_running_task",
  deferred: true,  // 标记为延迟
  execute: async () => {
    // 返回一个 handle
    return { handle: { jobId: "123", pollAfterMs: 5000 } };
  }
}
```

**流程：**
```
1. Model 调用工具
2. 工具返回 handle（不阻塞）
3. Generation 进入 "poll" 状态
4. 定期轮询直到完成
5. 继续对话
```

这避免了长时间阻塞！

### 4. 工具搜索（Tool Search）

```typescript
// 自动添加工具搜索能力
if (hasToolSearchExtension) {
  tools.add({
    name: "toolSearch",
    description: "Search available tools by capability",
    execute: (query) => {
      // 语义搜索工具列表
      return relevantTools;
    }
  });
}
```

模型可以自己发现需要的工具！

### 5. Compaction（上下文压缩）

**智能压缩策略：**

```typescript
thresholdCompaction(view, entries, contextWindow, policy) {
  const estimated = estimateContext(view, entries);
  
  if (estimated + policy.reserveTokens > contextWindow) {
    // 阈值压缩：先压缩再发送
    return "blocking";
  }
  
  if (estimated + policy.backgroundTokens > contextWindow) {
    // 背景压缩：发送后异步压缩
    return "background";
  }
  
  return "none";
}
```

**两种模式：**
- **blocking**: 超出阈值，必须先压缩
- **background**: 接近上限，异步压缩

---

## 📋 Ode 可以借鉴的设计

### 高优先级

#### 1. 工具访问控制
```cpp
struct ToolAccess {
    enum Origin { Any, Owner };
    enum Scope { Any, Main, Subagent };
    
    Origin origin = Any;
    Scope scope = Any;
    
    bool check(const Context& ctx) const {
        if (scope == Main && !ctx.is_top_level()) return false;
        if (origin == Owner && ctx.origin() != "owner") return false;
        return true;
    }
};
```

#### 2. 工具并行执行
```cpp
class ToolExecutor {
    // 当前：串行
    for (auto& call : tool_calls) {
        result = execute_tool(call);
        results.push_back(result);
    }
    
    // 改进：并行
    std::vector<std::future<ToolResult>> futures;
    for (auto& call : tool_calls) {
        futures.push_back(std::async(std::launch::async, 
            [&]() { return execute_tool(call); }));
    }
    for (auto& f : futures) {
        results.push_back(f.get());
    }
};
```

#### 3. 状态检查点（简化版）
```cpp
struct GenerationState {
    enum Phase { Prepare, Request, Tools, Finish };
    Phase phase = Prepare;
    int attempt = 0;
    std::vector<ToolCall> pending_tools;
    
    // 保存状态
    json to_json() const;
    
    // 恢复状态
    static GenerationState from_json(const json& j);
};
```

### 中优先级

#### 4. 工具重试机制
```cpp
ToolResult execute_with_retry(const ToolCall& call, int max_retries = 3) {
    for (int i = 0; i < max_retries; ++i) {
        try {
            return execute_tool(call);
        } catch (const RetryableError& e) {
            if (i == max_retries - 1) throw;
            std::this_thread::sleep_for(std::chrono::seconds(1 << i));
        }
    }
}
```

#### 5. 工具超时
```cpp
ToolResult execute_with_timeout(const ToolCall& call, int timeout_ms = 30000) {
    auto future = std::async(std::launch::async, 
        [&]() { return execute_tool(call); });
    
    if (future.wait_for(std::chrono::milliseconds(timeout_ms)) 
        == std::future_status::timeout) {
        return ToolResult::error("Tool execution timeout");
    }
    
    return future.get();
}
```

### 低优先级

#### 6. 延迟工具支持
```cpp
struct DeferredHandle {
    std::string job_id;
    int poll_after_ms = 5000;
};

struct ToolResult {
    bool is_deferred = false;
    DeferredHandle handle;
    std::string output;
};
```

#### 7. 工具搜索
```cpp
// 语义搜索工具
Tool tool_search_tool = {
    .name = "search_tools",
    .description = "Find tools by capability",
    .execute = [](const json& args) {
        auto query = args["query"].get<std::string>();
        return search_tools_by_description(query);
    }
};
```

---

## 🎯 实施建议

### Phase 1：立即可做（1-2天）

**1.1 工具访问控制**
- 添加 origin/scope 检查
- 实现 `guardTool()` 函数
- 在每个工具执行前检查

**1.2 并行工具执行**
- 使用 `std::async` 并行执行
- 收集所有结果后再继续
- 添加超时保护

**1.3 基础重试机制**
- 识别可重试错误
- 指数退避重试
- 最大重试次数限制

### Phase 2：中期优化（1周）

**2.1 状态检查点**
- 保存 generation 状态
- 实现恢复逻辑
- 处理中断场景

**2.2 工具超时和取消**
- 每个工具有超时
- 支持用户取消
- 清理资源

**2.3 错误分类和处理**
- 区分重试/失败/致命错误
- 更好的错误消息
- 错误恢复策略

### Phase 3：长期愿景（1个月+）

**3.1 完整状态机**
- 实现 Pi 的 7 阶段状态机
- 完整的崩溃恢复
- 状态持久化

**3.2 Durable 风格框架**
- 文档存储系统
- 事务性更新
- Task 调度器

**3.3 高级工具特性**
- 延迟工具
- 工具搜索
- 工具组合

---

## 📝 总结

### Pi 的核心优势

1. **状态机设计** - 清晰、可恢复、可测试
2. **文档事务** - ACID、崩溃恢复、并发安全
3. **访问控制** - 精细的三维权限模型
4. **异步 Task** - 并行执行、依赖管理
5. **延迟执行** - 长时间任务不阻塞

### Ode 的优势

1. **原生性能** - C++ 更快
2. **简单直接** - 更易理解
3. **低延迟** - 无额外抽象层
4. **小内存** - 无 Node.js 开销

### 建议的融合策略

**保持 Ode 的优势：**
- C++ 实现
- 简单架构
- 快速启动

**借鉴 Pi 的设计：**
- 工具访问控制
- 并行执行
- 重试机制
- 状态检查点（简化版）

**不必完全复制：**
- 完整 Durable 框架（太重）
- 文档存储（文本日志足够）
- Task 系统（直接 async 更简单）

---

## 🔗 关键代码位置

### Pi 源码

| 功能 | 文件 |
|------|------|
| Agent 循环 | `packages/durable/src/harness/generation.ts` |
| Agent 配置 | `packages/durable/src/harness/agent.ts` |
| 工具访问 | `apps/p2/src/runtime/tool-access.ts` |
| 工具执行 | `packages/durable/src/harness/tool.ts` |
| 状态管理 | `packages/durable/src/harness/live.ts` |
| 上下文压缩 | `packages/durable/src/harness/compaction.ts` |

### Ode 对应

| 功能 | 文件 |
|------|------|
| Agent 循环 | `src/core/agent.cpp` |
| 工具执行 | `src/tools/*.cpp` |
| 权限系统 | `src/core/permission.cpp` |
| 会话管理 | `src/core/session.cpp` |

---

**下一步：实施 Phase 1 的三个改进？**
