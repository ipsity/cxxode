// 集成计划：Agent::run_tools() 改造
// 
// 目标：使用 SmartToolScheduler 替换现有的串行/简单并行逻辑
//
// 当前逻辑（agent.cpp:150-200）：
// 1. 连续的只读工具并行执行
// 2. 其他工具串行执行
// 3. 检查取消和 steering
//
// 新逻辑（使用 SmartToolScheduler）：
// 1. 收集所有工具调用
// 2. 分析依赖关系（自动）
// 3. 智能调度执行
// 4. 保持 steering 和 cancel 支持
//
// 实施步骤：
// 1. 创建 SmartToolScheduler 实例
// 2. 转换 tool_use 到 ToolCall 格式
// 3. 设置 steering 回调
// 4. 执行并收集结果
// 5. 转换结果到 message blocks
//
// 兼容性：
// - 保持现有的 agent_events 回调
// - 保持 cancel_token 支持
// - 保持 turn_backup 机制
// - 保持 steering 功能

void agent::run_tools(const message& reply, agent_events& events, cancel_token& cancel, message& results,
                      turn_backup& backup) {
    const auto calls = reply.tool_uses();
    
    // TODO: 创建 SmartToolScheduler
    // SmartToolScheduler scheduler([this, &events, &cancel, &backup](const ToolCall& call) {
    //     // 执行单个工具
    //     return this->execute_tool_internal(call, events, cancel, backup);
    // }, SmartToolScheduler::Strategy::Smart);
    
    // TODO: 设置 steering
    // scheduler.set_steering([&events]() {
    //     return events.take_steering ? events.take_steering() : std::vector<std::string>{};
    // });
    
    // 当前保持原有逻辑（向后兼容）
    std::size_t i = 0;
    while (i < calls.size()) {
        if (cancel.cancelled()) {
            results.blocks.push_back(block::make_tool_result(calls[i]->id, "[Interrupted by the user]", true));
            ++i;
            continue;
        }
        // ... 现有逻辑
    }
}
