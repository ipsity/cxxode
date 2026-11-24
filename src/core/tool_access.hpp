#pragma once

#include <string>
#include <set>
#include <chrono>

namespace ode {

/**
 * 四维工具访问控制系统
 * 超越 Pi 的三维权限模型
 */
class ToolAccess {
public:
    // 1. Origin: 谁可以调用
    enum class Origin {
        Any,      // 任何人
        Owner,    // 仅所有者
        Guest     // 仅访客
    };

    // 2. Scope: 在哪里可用
    enum class Scope {
        Any,       // 任何地方
        Main,      // 仅主会话
        Subagent,  // 仅子 agent
        Local      // 仅本地（非远程）
    };

    // 3. Mode: 权限模式
    enum class Mode {
        Pass,      // 无限制通过
        Ask,       // 每次询问
        Never,     // 永不允许
        Cached     // 缓存用户决定
    };

    // 4. Resource: 资源限制（Pi 没有的新维度）
    struct Resource {
        bool network = false;           // 需要网络访问
        bool filesystem = false;        // 需要文件系统
        bool execution = false;         // 需要执行命令
        int max_time_ms = 30000;       // 最大执行时间（默认30秒）
        int max_memory_mb = 512;       // 最大内存使用（默认512MB）
        int max_output_kb = 1024;      // 最大输出大小（默认1MB）
    };

    // 构造函数
    ToolAccess(
        Origin origin = Origin::Any,
        Scope scope = Scope::Any,
        Mode mode = Mode::Ask,
        Resource resource = Resource{}
    ) : origin_(origin), scope_(scope), mode_(mode), resource_(resource) {}

    // 检查访问权限
    struct Context {
        std::string user;              // 当前用户
        bool is_owner = true;          // 是否所有者
        bool is_main_session = true;   // 是否主会话
        bool is_remote = false;        // 是否远程会话
        std::string session_id;        // 会话ID
    };

    struct CheckResult {
        bool allowed = false;
        std::string reason;            // 拒绝原因
        bool should_ask = false;       // 是否需要询问用户
    };

    CheckResult check(const Context& ctx) const;

    // 检查资源限制
    struct ResourceUsage {
        std::chrono::milliseconds elapsed;
        size_t memory_bytes = 0;
        size_t output_bytes = 0;
    };

    bool check_resource_limits(const ResourceUsage& usage) const;

    // Getters
    Origin origin() const { return origin_; }
    Scope scope() const { return scope_; }
    Mode mode() const { return mode_; }
    const Resource& resource() const { return resource_; }

    // 便捷工厂方法
    static ToolAccess unrestricted() {
        return ToolAccess(Origin::Any, Scope::Any, Mode::Pass, {});
    }

    static ToolAccess safe_default() {
        return ToolAccess(Origin::Owner, Scope::Main, Mode::Ask,
            {.filesystem = true, .max_time_ms = 30000});
    }

    static ToolAccess dangerous() {
        return ToolAccess(Origin::Owner, Scope::Main, Mode::Cached,
            {.network = true, .filesystem = true, .execution = true});
    }

private:
    Origin origin_;
    Scope scope_;
    Mode mode_;
    Resource resource_;
};

} // namespace ode
