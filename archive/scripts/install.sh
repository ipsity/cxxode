#!/bin/bash
# cxxode Linux/macOS 安装脚本

set -e

echo "正在安装 cxxode..."
echo

# 检查是否有 sudo 权限
if [ "$EUID" -ne 0 ]; then
    echo "需要 sudo 权限。请使用: sudo ./install.sh"
    exit 1
fi

# 检查构建目录
if [ ! -f "build/release/ode" ]; then
    echo "错误: 未找到 build/release/ode"
    echo "请先运行: cmake --preset release && cmake --build --preset release"
    exit 1
fi

# 复制到 /usr/local/bin
echo "复制文件到 /usr/local/bin..."
cp build/release/ode /usr/local/bin/ode
chmod +x /usr/local/bin/ode

echo
echo "✓ 安装完成！"
echo
echo "运行以下命令开始使用:"
echo "  ode --version"
echo "  ode login anthropic"
echo
