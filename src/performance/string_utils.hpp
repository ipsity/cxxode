#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <sstream>
#include <algorithm>

namespace ode {
namespace perf {

/**
 * 字符串优化工具
 * Week 3 Day 16: 性能优化 - 字符串操作优化
 *
 * 功能：
 * - 高效字符串拼接
 * - 避免不必要的拷贝
 * - 常用字符串操作优化
 */
class string_utils {
public:
    // 高效的字符串拼接（预分配空间）
    template<typename... Args>
    static std::string concat(Args&&... args) {
        std::string result;
        // 计算总大小
        size_t total_size = (string_length(args) + ...);
        result.reserve(total_size);
        // 拼接
        (append_to(result, std::forward<Args>(args)), ...);
        return result;
    }

    // 字符串构建器（避免多次拷贝）
    class builder {
    public:
        explicit builder(size_t reserve_size = 256) {
            buffer_.reserve(reserve_size);
        }

        builder& append(std::string_view sv) {
            buffer_.append(sv);
            return *this;
        }

        builder& append(char c) {
            buffer_.push_back(c);
            return *this;
        }

        builder& append(int value) {
            buffer_.append(std::to_string(value));
            return *this;
        }

        builder& append(double value) {
            buffer_.append(std::to_string(value));
            return *this;
        }

        template<typename T>
        builder& operator<<(T&& value) {
            append(std::forward<T>(value));
            return *this;
        }

        std::string str() const { return buffer_; }
        std::string&& take() { return std::move(buffer_); }

        void clear() { buffer_.clear(); }
        void reserve(size_t size) { buffer_.reserve(size); }
        size_t size() const { return buffer_.size(); }
        bool empty() const { return buffer_.empty(); }

    private:
        std::string buffer_;
    };

    // 分割字符串（避免拷贝，返回 string_view）
    static std::vector<std::string_view> split_view(std::string_view str, char delimiter) {
        std::vector<std::string_view> result;
        size_t start = 0;
        size_t end = str.find(delimiter);

        while (end != std::string_view::npos) {
            result.push_back(str.substr(start, end - start));
            start = end + 1;
            end = str.find(delimiter, start);
        }

        if (start < str.size()) {
            result.push_back(str.substr(start));
        }

        return result;
    }

    // 分割字符串（返回拷贝）
    static std::vector<std::string> split(std::string_view str, char delimiter) {
        auto views = split_view(str, delimiter);
        std::vector<std::string> result;
        result.reserve(views.size());
        for (auto view : views) {
            result.emplace_back(view);
        }
        return result;
    }

    // 修剪空白
    static std::string_view trim(std::string_view str) {
        const char* whitespace = " \t\n\r";
        size_t start = str.find_first_not_of(whitespace);
        if (start == std::string_view::npos) return {};
        size_t end = str.find_last_not_of(whitespace);
        return str.substr(start, end - start + 1);
    }

    // 大小写转换（in-place）
    static void to_lower_inplace(std::string& str) {
        std::transform(str.begin(), str.end(), str.begin(),
                      [](unsigned char c) { return std::tolower(c); });
    }

    static void to_upper_inplace(std::string& str) {
        std::transform(str.begin(), str.end(), str.begin(),
                      [](unsigned char c) { return std::toupper(c); });
    }

    // 大小写转换（返回新字符串）
    static std::string to_lower(std::string_view str) {
        std::string result(str);
        to_lower_inplace(result);
        return result;
    }

    static std::string to_upper(std::string_view str) {
        std::string result(str);
        to_upper_inplace(result);
        return result;
    }

    // 检查前缀/后缀
    static bool starts_with(std::string_view str, std::string_view prefix) {
        return str.size() >= prefix.size() &&
               str.substr(0, prefix.size()) == prefix;
    }

    static bool ends_with(std::string_view str, std::string_view suffix) {
        return str.size() >= suffix.size() &&
               str.substr(str.size() - suffix.size()) == suffix;
    }

    // 替换所有出现
    static std::string replace_all(std::string_view str, std::string_view from, std::string_view to) {
        std::string result(str);
        if (from.empty()) return result;

        size_t start_pos = 0;
        while ((start_pos = result.find(from, start_pos)) != std::string::npos) {
            result.replace(start_pos, from.length(), to);
            start_pos += to.length();
        }
        return result;
    }

    // 连接字符串数组
    static std::string join(const std::vector<std::string>& parts, std::string_view delimiter) {
        if (parts.empty()) return {};

        size_t total_size = 0;
        for (const auto& part : parts) {
            total_size += part.size();
        }
        total_size += delimiter.size() * (parts.size() - 1);

        std::string result;
        result.reserve(total_size);

        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) result.append(delimiter);
            result.append(parts[i]);
        }

        return result;
    }

private:
    // 辅助函数：获取字符串长度
    static size_t string_length(std::string_view sv) { return sv.size(); }
    static size_t string_length(const std::string& s) { return s.size(); }
    static size_t string_length(const char* s) { return std::strlen(s); }
    static size_t string_length(char) { return 1; }

    // 辅助函数：追加到字符串
    static void append_to(std::string& target, std::string_view sv) {
        target.append(sv);
    }
    static void append_to(std::string& target, const std::string& s) {
        target.append(s);
    }
    static void append_to(std::string& target, const char* s) {
        target.append(s);
    }
    static void append_to(std::string& target, char c) {
        target.push_back(c);
    }
};

} // namespace perf
} // namespace ode
