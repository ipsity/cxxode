// Agent::run_tools 实现改造
// 目标：集成 SmartToolScheduler

void agent::run_tools(const message& reply, agent_events& events, cancel_token& cancel, message& results,
                      turn_backup& backup) {
    const auto calls = reply.tool_uses();
    if (calls.empty()) return;

    // 检查 steering
    std::vector<std::string> steering = events.take_steering ? events.take_steering() : std::vector<std::string>{};

    // 准备工具调用
    std::vector<SmartToolScheduler::ToolCall> tool_calls;
    for (const auto& call : calls) {
        const auto* t = find_tool(call->name);
        if (!t) continue;

        SmartToolScheduler::ToolCall tc;
        tc.name = call->name;
        tc.input = call->input.dump();
        tc.access = t->access_control;

        // 分析依赖（简化版）
        if (t->access == tool_access::read) {
            // 读操作
        } else if (t->access == tool_access::write) {
            // 写操作
            tc.writes_to.push_back("file");
        }

        tool_calls.push_back(tc);
    }

    // 创建调度器
    auto executor = [this, &events, &cancel, &backup](const SmartToolScheduler::ToolCall& call) {
        // 执行单个工具
        SmartToolScheduler::ToolResult result;

        // 找到工具
        const auto* t = find_tool(call.name);
        if (!t) {
            result.success = false;
            result.error = "Tool not found";
            return result;
        }

        // 解析输入
        json input = json::parse(call.input);

        // 执行工具
        auto out = run_one(*calls[0], events, cancel, backup, seen_);

        result.output = out.text;
        result.success = !out.error;
        result.error = out.error ? "Error" : "";

        return result;
    };

    SmartToolScheduler scheduler(executor, SmartToolScheduler::Strategy::Smart);

    // 设置 steering
    scheduler.set_steering([&events]() {
        return events.take_steering ? events.take_steering() : std::vector<std::string>{};
    });

    // 执行
    auto tool_results = scheduler.execute(tool_calls);

    // 收集结果
    for (size_t i = 0; i < tool_results.size() && i < calls.size(); ++i) {
        const auto& tr = tool_results[i];
        results.blocks.push_back(block::make_tool_result(
            calls[i]->id,
            tr.output,
            !tr.success
        ));
    }

    // 处理 steering 消息
    for (auto& s : steering) {
        results.blocks.push_back(block::make_text(std::move(s)));
    }
}
