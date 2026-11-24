#include "core/tool_access.hpp"

namespace ode {

ToolAccess::CheckResult ToolAccess::check(const Context& ctx) const {
    CheckResult result;

    // 1. Origin 检查
    switch (origin_) {
        case Origin::Owner:
            if (!ctx.is_owner) {
                result.reason = "Tool requires owner permission";
                return result;
            }
            break;
        case Origin::Guest:
            if (ctx.is_owner) {
                result.reason = "Tool only available to guests";
                return result;
            }
            break;
        case Origin::Any:
            // 允许所有人
            break;
    }

    // 2. Scope 检查
    switch (scope_) {
        case Scope::Main:
            if (!ctx.is_main_session) {
                result.reason = "Tool only works in main session";
                return result;
            }
            break;
        case Scope::Subagent:
            if (ctx.is_main_session) {
                result.reason = "Tool only available in subagents";
                return result;
            }
            break;
        case Scope::Local:
            if (ctx.is_remote) {
                result.reason = "Tool not available in remote sessions";
                return result;
            }
            break;
        case Scope::Any:
            // 任何范围都允许
            break;
    }

    // 3. Mode 检查
    switch (mode_) {
        case Mode::Never:
            result.reason = "Tool is disabled";
            return result;

        case Mode::Ask:
            result.allowed = false;
            result.should_ask = true;
            result.reason = "User permission required";
            return result;

        case Mode::Cached:
            // TODO: 检查缓存的用户决定
            result.allowed = false;
            result.should_ask = true;
            result.reason = "User permission required (will be cached)";
            return result;

        case Mode::Pass:
            // 无条件通过
            break;
    }

    // 4. Resource 检查在执行时进行（见 check_resource_limits）

    result.allowed = true;
    return result;
}

bool ToolAccess::check_resource_limits(const ResourceUsage& usage) const {
    // 检查执行时间
    if (usage.elapsed.count() > resource_.max_time_ms) {
        return false;
    }

    // 检查内存使用
    if (usage.memory_bytes > static_cast<size_t>(resource_.max_memory_mb) * 1024 * 1024) {
        return false;
    }

    // 检查输出大小
    if (usage.output_bytes > static_cast<size_t>(resource_.max_output_kb) * 1024) {
        return false;
    }

    return true;
}

} // namespace ode
