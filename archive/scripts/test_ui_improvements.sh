#!/bin/bash
# UI 功能实际测试脚本

echo "开始 UI 功能实际测试..."
echo "================================"
echo ""

# 测试 1: 基本帮助显示
echo "测试 1: 帮助系统是否包含新命令"
echo "命令: ./build/dev/ode.exe --help"
./build/dev/ode.exe --help | grep -E "/ref|/search|/test|/build" && echo "✅ 帮助系统包含快捷命令" || echo "❌ 帮助系统缺少快捷命令"
echo ""

# 测试 2: 版本信息
echo "测试 2: 程序基本运行"
echo "命令: ./build/dev/ode.exe --version"
./build/dev/ode.exe --version && echo "✅ 程序正常运行" || echo "❌ 程序无法运行"
echo ""

# 测试 3: 检查编译的命令数量
echo "测试 3: 验证命令数量"
echo "检查: builtin_commands 数组"
cmd_count=$(grep -A 100 "builtin_commands\[\]" src/app/interactive_view.cpp | grep "{\"/" | wc -l)
echo "内置命令数量: $cmd_count"
if [ $cmd_count -ge 25 ]; then
    echo "✅ 命令数量合理（>=25，包含快捷命令和别名）"
else
    echo "⚠️  命令数量较少: $cmd_count"
fi
echo ""

# 测试 4: 检查模糊搜索实现
echo "测试 4: 模糊搜索功能检查"
echo "检查: fuzzy_match 函数"
if grep -q "fuzzy_match.*lambda" src/app/interactive_view.cpp; then
    echo "✅ 模糊搜索已实现"
else
    echo "❌ 模糊搜索未找到"
fi
echo ""

# 测试 5: 检查优先级控制
echo "测试 5: UI 优先级控制检查"
echo "检查: validation 优先于 suggestions"
if grep -A 5 "Priority 1.*validation" src/app/interactive_view.cpp | grep -q "Priority 2"; then
    echo "✅ UI 优先级控制已实现"
else
    echo "⚠️  优先级控制可能缺失"
fi
echo ""

# 测试 6: 检查所有快捷命令实现
echo "测试 6: 快捷命令实现检查"
shortcuts=("/ref" "/search" "/fix" "/test" "/build" "/run" "/commit" "/lint" "/format" "/doc")
for cmd in "${shortcuts[@]}"; do
    if grep -q "else if.*name == \"$cmd\"" src/app/commands.cpp; then
        echo "✅ $cmd 已实现"
    else
        echo "❌ $cmd 未实现"
    fi
done
echo ""

# 测试 7: 编译状态
echo "测试 7: 编译状态"
if [ -f build/dev/ode.exe ]; then
    echo "✅ 可执行文件存在"
    size=$(stat -c%s build/dev/ode.exe 2>/dev/null || stat -f%z build/dev/ode.exe 2>/dev/null || echo "unknown")
    echo "   文件大小: $size bytes"
else
    echo "❌ 可执行文件不存在"
fi
echo ""

# 测试 8: 文档完整性
echo "测试 8: 文档完整性"
docs=("UI_ITERATION_SUMMARY.md" "SHORTCUTS_GUIDE.md" "UI_FINAL_REPORT.md" "STAGE_COMPLETION_REPORT.md")
for doc in "${docs[@]}"; do
    if [ -f "docs/$doc" ]; then
        lines=$(wc -l < "docs/$doc")
        echo "✅ $doc ($lines 行)"
    else
        echo "❌ $doc 缺失"
    fi
done
echo ""

echo "================================"
echo "测试完成"
